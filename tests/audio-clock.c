/* Test-only audio device: clocked callbacks without opening speakers. Scrubbing
 * must keep one device alive instead of repeatedly closing and reopening it. */
#include "dm.h"
int dm_audio_failed(dm_audio *a) { (void)a; return 0; }
#include <pthread.h>
#include <stdlib.h>

struct dm_audio {
  pthread_t thread;
  pthread_mutex_t mu;
  dm_audio_cb callback;
  void *user;
  int stop, sr;
  uint64_t played;
};
static int opened;
static void *pump(void *user) {
  dm_audio *a = user;
  float pcm[2048];
  for (;;) {
    pthread_mutex_lock(&a->mu); int stop = a->stop; pthread_mutex_unlock(&a->mu);
    if (stop) break;
    a->callback(a->user, pcm, 1024);
    dm_sleep(1024.0 / a->sr);
    pthread_mutex_lock(&a->mu); a->played += 1024; pthread_mutex_unlock(&a->mu);
  }
  return NULL;
}
dm_audio *dm_audio_open(int sr, dm_audio_cb cb, void *user) {
  if (++opened > 1) { fprintf(stderr, "audio device reopened during scrub\n"); abort(); }
  dm_audio *a = calloc(1, sizeof *a);
  if (!a) return NULL;
  a->sr = sr; a->callback = cb; a->user = user;
  if (pthread_mutex_init(&a->mu, NULL)) { free(a); return NULL; }
  if (pthread_create(&a->thread, NULL, pump, a)) { pthread_mutex_destroy(&a->mu); free(a); return NULL; }
  return a;
}
uint64_t dm_audio_played(dm_audio *a) {
  pthread_mutex_lock(&a->mu); uint64_t p = a->played; pthread_mutex_unlock(&a->mu); return p;
}
void dm_audio_close(dm_audio *a) {
  if (!a) return;
  pthread_mutex_lock(&a->mu); a->stop = 1; pthread_mutex_unlock(&a->mu);
  pthread_join(a->thread, NULL); pthread_mutex_destroy(&a->mu); free(a);
}
const char *dm_audio_backend(dm_audio *a) { (void)a; return "test clock"; }
