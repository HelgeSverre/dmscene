/* dm_audio.c - audio output.
 *   macOS : AudioQueue (AudioToolbox, a C API) with sample-accurate playback position.
 *   other : a pipe to an external player (DM_AUDIO_CMD, default aplay) with estimated latency.
 */
#include "dm.h"
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
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
const char *dm_audio_backend(dm_audio *a) { (void)a; return "coreaudio"; }

#else
#include <pthread.h>
#include <stdio.h>
#include <time.h>

struct dm_audio {
  FILE *pipe;
  pthread_t th;
  dm_audio_cb cb; void *user;
  int sr, stop;
  uint64_t written;
  double t0;
  double latency;
};

static void *pump(void *p) {
  dm_audio *a = p;
  enum { N = 1024 };
  float buf[N * 2];
  int16_t pcm[N * 2];
  while (!a->stop) {
    a->cb(a->user, buf, N);
    for (int i = 0; i < N * 2; i++) { float x = buf[i]; pcm[i] = (int16_t)(x * 32767.0f); }
    if (fwrite(pcm, 4, N, a->pipe) != N) break;
    fflush(a->pipe);
    a->written += N;
    if (!a->t0) a->t0 = dm_now();
  }
  return NULL;
}

dm_audio *dm_audio_open(int sr, dm_audio_cb cb, void *user) {
  const char *cmd = getenv("DM_AUDIO_CMD");
  char def[128];
  if (!cmd) { snprintf(def, sizeof def, "aplay -q -t raw -f S16_LE -r %d -c 2", sr); cmd = def; }
  FILE *f = popen(cmd, "w");
  if (!f) return NULL;
  dm_audio *a = calloc(1, sizeof *a);
  a->pipe = f; a->cb = cb; a->user = user; a->sr = sr;
  const char *lat = getenv("DM_AUDIO_LATENCY");
  a->latency = lat ? atof(lat) / 1000.0 : 0.25;
  pthread_create(&a->th, NULL, pump, a);
  return a;
}
uint64_t dm_audio_played(dm_audio *a) {
  if (!a->t0) return 0;
  double el = dm_now() - a->t0 - a->latency;
  if (el < 0) el = 0;
  uint64_t by_clock = (uint64_t)(el * a->sr);
  return by_clock < a->written ? by_clock : a->written;
}
void dm_audio_close(dm_audio *a) {
  if (!a) return;
  a->stop = 1;
  pthread_join(a->th, NULL);
  pclose(a->pipe);
  free(a);
}
const char *dm_audio_backend(dm_audio *a) { (void)a; return "pipe"; }
#endif
