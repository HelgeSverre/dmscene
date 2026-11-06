/* dm_audio.c - audio output.
 *   macOS : AudioQueue (AudioToolbox, a C API) with sample-accurate playback position.
 *   other : a pipe to an external player (DM_AUDIO_CMD, default aplay) with estimated latency.
 */
#include "dm.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(__APPLE__) && !defined(DM_AUDIO_PIPE)
#include <AudioToolbox/AudioToolbox.h>
#define NBUF 3
#define BUFFRAMES 1024

struct dm_audio {
  AudioQueueRef q;
  AudioQueueBufferRef b[NBUF];
  dm_audio_cb cb; void *user;
  int sr;
  int started;
};

static void aq_cb(void *user, AudioQueueRef q, AudioQueueBufferRef b) {
  dm_audio *a = user;
  a->cb(a->user, (float *)b->mAudioData, BUFFRAMES);
  b->mAudioDataByteSize = BUFFRAMES * 8;
  AudioQueueEnqueueBuffer(q, b, 0, NULL);
}

dm_audio *dm_audio_open(int sr, dm_audio_cb cb, void *user) {
  dm_audio *a = calloc(1, sizeof *a);
  if (!a) return NULL;
  a->cb = cb; a->user = user; a->sr = sr;
  AudioStreamBasicDescription d;
  memset(&d, 0, sizeof d);
  d.mSampleRate = sr; d.mFormatID = kAudioFormatLinearPCM;
  d.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
  d.mBytesPerPacket = 8; d.mFramesPerPacket = 1; d.mBytesPerFrame = 8; d.mChannelsPerFrame = 2; d.mBitsPerChannel = 32;
  if (AudioQueueNewOutput(&d, aq_cb, a, NULL, NULL, 0, &a->q) != noErr) { free(a); return NULL; }
  for (int i = 0; i < NBUF; i++) {
    if (AudioQueueAllocateBuffer(a->q, BUFFRAMES * 8, &a->b[i]) != noErr) { AudioQueueDispose(a->q, true); free(a); return NULL; }
    aq_cb(a, a->q, a->b[i]);
  }
  if (AudioQueueStart(a->q, NULL) != noErr) { AudioQueueDispose(a->q, true); free(a); return NULL; }
  a->started = 1;
  return a;
}

uint64_t dm_audio_played(dm_audio *a) {
  AudioTimeStamp ts;
  if (!a->started || AudioQueueGetCurrentTime(a->q, NULL, &ts, NULL) != noErr) return 0;
  if (!(ts.mFlags & kAudioTimeStampSampleTimeValid) || ts.mSampleTime < 0) return 0;
  return (uint64_t)ts.mSampleTime;
}

void dm_audio_close(dm_audio *a) {
  if (!a) return;
  AudioQueueStop(a->q, true);
  AudioQueueDispose(a->q, true);
  free(a);
}
int dm_audio_failed(dm_audio *a) { (void)a; return 0; }
const char *dm_audio_backend(dm_audio *a) { (void)a; return "coreaudio"; }

#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;

struct dm_audio {
  int fd;
  pid_t child;
  pthread_t th;
  pthread_mutex_t mu;
  dm_audio_cb cb; void *user;
  int sr, stop, failed;
  uint64_t written;
  double t0, latency;
};

static int stopping(dm_audio *a) {
  pthread_mutex_lock(&a->mu); int stop = a->stop; pthread_mutex_unlock(&a->mu);
  return stop;
}
static void *pump(void *p) {
  dm_audio *a = p;
  /* SIGPIPE belongs to this writer, not the application. Leave it blocked
   * until the thread exits so a pending signal cannot kill the terminal. */
  sigset_t blocked; sigemptyset(&blocked); sigaddset(&blocked, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &blocked, NULL);
  enum { N = 1024 };
  float buf[N * 2];
  uint8_t pcm[N * 4];
  while (!stopping(a)) {
    a->cb(a->user, buf, N);
    for (int i = 0; i < N * 2; i++) {
      float x = buf[i];
      if (!isfinite(x)) x = 0;
      if (x > 1) x = 1; if (x < -1) x = -1;
      int v = (int)(x * 32767.0f);
      pcm[i * 2] = (uint8_t)v; pcm[i * 2 + 1] = (uint8_t)((unsigned)v >> 8);
    }
    size_t at = 0;
    while (at < sizeof pcm) {
      if (stopping(a)) return NULL;
      ssize_t n = write(a->fd, pcm + at, sizeof pcm - at);
      if (n > 0) { at += (size_t)n; continue; }
      if (n < 0 && errno == EINTR) continue;
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        struct pollfd fd = {a->fd, POLLOUT, 0};
        if (poll(&fd, 1, 20) >= 0 || errno == EINTR) continue;
      }
      pthread_mutex_lock(&a->mu); a->failed = 1; pthread_mutex_unlock(&a->mu);
      return NULL;
    }
    pthread_mutex_lock(&a->mu);
    a->written += N;
    if (!a->t0) a->t0 = dm_now();
    pthread_mutex_unlock(&a->mu);
  }
  return NULL;
}

/* The command owns a separate process group; shutdown never waits on an
 * uncooperative player indefinitely. Stop the writer before closing its fd. */
static void stop_child(pid_t child) {
  kill(-child, SIGTERM);
  for (int i = 0; i < 20; i++) {
    /* Keep the leader unreaped until the final group kill: a shell can exit
     * before its helpers, and its PID must not be reused in the meantime. */
    siginfo_t status; memset(&status, 0, sizeof status);
    int rc = waitid(P_PID, (id_t)child, &status, WEXITED | WNOHANG | WNOWAIT);
    if (rc < 0 && errno == ECHILD) return;
    if (!rc && status.si_pid == child) break;
    dm_sleep(.01);
  }
  kill(-child, SIGKILL);
  while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
}

dm_audio *dm_audio_open(int sr, dm_audio_cb cb, void *user) {
  if (sr <= 0 || !cb) return NULL;
  dm_audio *a = calloc(1, sizeof *a);
  if (!a) return NULL;
  if (pthread_mutex_init(&a->mu, NULL)) { free(a); return NULL; }
  a->cb = cb; a->user = user; a->sr = sr;
  const char *lat = getenv("DM_AUDIO_LATENCY");
  a->latency = lat ? atof(lat) / 1000.0 : .25;
  if (!isfinite(a->latency) || a->latency < 0) a->latency = .25;
  const char *cmd = getenv("DM_AUDIO_CMD");
  char def[128];
  if (!cmd) { snprintf(def, sizeof def, "aplay -q -t raw -f S16_LE -r %d -c 2", sr); cmd = def; }
  int fds[2];
  if (pipe(fds)) goto fail;
  if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(fds[1], F_SETFD, FD_CLOEXEC) < 0 ||
      fcntl(fds[1], F_SETFL, O_NONBLOCK) < 0) goto fail_pipe;
#ifdef F_SETNOSIGPIPE
  /* Darwin can deliver a pipe's SIGPIPE to another unblocked thread. */
  if (fcntl(fds[1], F_SETNOSIGPIPE, 1) < 0) goto fail_pipe;
#endif
  posix_spawn_file_actions_t actions;
  posix_spawnattr_t attr;
  if (posix_spawn_file_actions_init(&actions)) goto fail_pipe;
  if (posix_spawnattr_init(&attr)) { posix_spawn_file_actions_destroy(&actions); goto fail_pipe; }
  int err = posix_spawn_file_actions_adddup2(&actions, fds[0], STDIN_FILENO);
  if (!err && fds[0] != STDIN_FILENO) err = posix_spawn_file_actions_addclose(&actions, fds[0]);
  if (!err) err = posix_spawn_file_actions_addclose(&actions, fds[1]);
  if (!err) err = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
  if (!err) err = posix_spawnattr_setpgroup(&attr, 0);
  char *const args[] = {"sh", "-c", (char *)cmd, NULL};
  if (!err) err = posix_spawn(&a->child, "/bin/sh", &actions, &attr, args, environ);
  posix_spawn_file_actions_destroy(&actions); posix_spawnattr_destroy(&attr);
  if (err) goto fail_pipe;
  close(fds[0]); a->fd = fds[1];
  if (pthread_create(&a->th, NULL, pump, a)) {
    close(a->fd); stop_child(a->child); goto fail;
  }
  return a;
 fail_pipe:
  close(fds[0]); close(fds[1]);
 fail:
  pthread_mutex_destroy(&a->mu); free(a); return NULL;
}
uint64_t dm_audio_played(dm_audio *a) {
  pthread_mutex_lock(&a->mu);
  double t0 = a->t0; uint64_t written = a->written;
  pthread_mutex_unlock(&a->mu);
  if (!t0) return 0;
  double el = dm_now() - t0 - a->latency;
  if (el <= 0) return 0;
  double frames = el * a->sr;
  return frames >= (double)written ? written : (uint64_t)frames;
}
int dm_audio_failed(dm_audio *a) {
  pthread_mutex_lock(&a->mu); int failed = a->failed; pthread_mutex_unlock(&a->mu);
  return failed;
}
void dm_audio_close(dm_audio *a) {
  if (!a) return;
  pthread_mutex_lock(&a->mu); a->stop = 1; pthread_mutex_unlock(&a->mu);
  pthread_join(a->th, NULL);
  close(a->fd); stop_child(a->child);
  pthread_mutex_destroy(&a->mu); free(a);
}
const char *dm_audio_backend(dm_audio *a) { (void)a; return "pipe"; }
#endif
