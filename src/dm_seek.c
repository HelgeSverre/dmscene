#include "dm_seek.h"
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define SEEK_BLOCK 1024u
#define CHECKPOINT_STEP (43u * SEEK_BLOCK) /* about one second, on a block boundary */
#define CHECKPOINT_BYTES (32u * 1024u * 1024u)
#define CHECKPOINT_MAX 64

struct dm_seek {
  const dm_module *m;
  pthread_mutex_t mu;
  pthread_cond_t wake;
  pthread_t thread;
  uint64_t generation, ready_generation, target;
  uint8_t mute[DM_MAX_CHANNELS];
  int stop, failed, prepare_audio;
  float env[DM_MAX_CHANNELS];
  dm_player *ready;
  /* Owned exclusively by the worker. The memory budget includes delay/reverb
   * buffers; dense effects cannot turn a long session into an unbounded cache. */
  dm_player *checkpoint[CHECKPOINT_MAX];
  unsigned cache_at, cache_count, cache_limit;
};

static void player_delete(dm_player *p) {
  if (p) { dm_player_free(p); free(p); }
}

static dm_player *player_new(const dm_module *m) {
  dm_player *p = calloc(1, sizeof *p);
  if (p && dm_player_init(p, m, DM_SR)) { player_delete(p); return NULL; }
  return p;
}

static size_t player_bytes(const dm_player *p) {
  size_t bytes = sizeof *p + (size_t)p->synth.mixcap * sizeof(float);
  for (int c = -1; c < DM_MAX_CHANNELS; c++) {
    const dm_fxunit *fx = c < 0 ? &p->synth.master : &p->synth.chan[c];
    for (int i = 0; i < DM_MAX_DELAY_LINES; i++) if (fx->dly[i]) bytes += p->synth.dlylen * sizeof(float);
    for (int i = 0; i < DM_MAX_VERBS; i++) if (fx->verb[i].buf) bytes += DM_VERB_BUF * sizeof(float);
  }
  return bytes;
}

/* Synth state contains owned pointers. Copies must never share effect buffers
 * with the active audio callback or another checkpoint. */
static dm_player *player_copy(const dm_player *src) {
  dm_player *dst = malloc(sizeof *dst);
  if (!dst) return NULL;
  *dst = *src;
  memset(&dst->synth.vm, 0, sizeof dst->synth.vm); /* rebound by synth_render */
  dst->synth.mix = NULL;
  dst->synth.mixcap = 0; /* scratch is not persistent synth state */
  for (int c = -1; c < DM_MAX_CHANNELS; c++) {
    dm_fxunit *fx = c < 0 ? &dst->synth.master : &dst->synth.chan[c];
    for (int i = 0; i < DM_MAX_DELAY_LINES; i++) fx->dly[i] = NULL;
    for (int i = 0; i < DM_MAX_VERBS; i++) fx->verb[i].buf = NULL;
  }
  for (int c = -1; c < DM_MAX_CHANNELS; c++) {
    const dm_fxunit *from = c < 0 ? &src->synth.master : &src->synth.chan[c];
    dm_fxunit *to = c < 0 ? &dst->synth.master : &dst->synth.chan[c];
    for (int i = 0; i < DM_MAX_DELAY_LINES; i++) if (from->dly[i]) {
      size_t n = src->synth.dlylen * sizeof(float);
      to->dly[i] = malloc(n);
      if (!to->dly[i]) { player_delete(dst); return NULL; }
      memcpy(to->dly[i], from->dly[i], n);
    }
    for (int i = 0; i < DM_MAX_VERBS; i++) if (from->verb[i].buf) {
      to->verb[i].buf = malloc(DM_VERB_BUF * sizeof(float));
      if (!to->verb[i].buf) { player_delete(dst); return NULL; }
      memcpy(to->verb[i].buf, from->verb[i].buf, DM_VERB_BUF * sizeof(float));
    }
  }
  return dst;
}

static void cache_clear(dm_seek *s) {
  for (unsigned i = 0; i < s->cache_count; i++) player_delete(s->checkpoint[i]);
  memset(s->checkpoint, 0, sizeof s->checkpoint);
  s->cache_count = s->cache_at = 0;
}

static void cache_save(dm_seek *s, const dm_player *p) {
  if (!s->cache_limit) {
    size_t n = CHECKPOINT_BYTES / player_bytes(p);
    s->cache_limit = n > CHECKPOINT_MAX ? CHECKPOINT_MAX : (unsigned)n;
    if (!s->cache_limit) return;
  }
  for (unsigned i = 0; i < s->cache_count; i++)
    if (s->checkpoint[i]->seq.sample == p->seq.sample) return;
  dm_player *copy = player_copy(p);
  if (!copy) return; /* caching is optional */
  player_delete(s->checkpoint[s->cache_at]);
  s->checkpoint[s->cache_at] = copy;
  s->cache_at = (s->cache_at + 1) % s->cache_limit;
  if (s->cache_count < s->cache_limit) s->cache_count++;
}

static dm_player *cache_restore(dm_seek *s, uint64_t target) {
  dm_player *best = NULL;
  for (unsigned i = 0; i < s->cache_count; i++) {
    dm_player *p = s->checkpoint[i];
    if (p->seq.sample <= target && (!best || p->seq.sample > best->seq.sample)) best = p;
  }
  return best ? player_copy(best) : player_new(s->m);
}

static void *seek_run(void *user) {
  dm_seek *s = user;
  dm_player *p = player_new(s->m);
  uint64_t completed = 0;
  uint8_t mute[DM_MAX_CHANNELS] = {0};
  float pcm[2 * SEEK_BLOCK];
  while (p) {
    pthread_mutex_lock(&s->mu);
    while (!s->stop && completed == s->generation) pthread_cond_wait(&s->wake, &s->mu);
    uint64_t generation = s->generation, target = s->target;
    int stop = s->stop, prepare = s->prepare_audio;
    int changed_mute = memcmp(mute, s->mute, sizeof mute) != 0;
    memcpy(mute, s->mute, sizeof mute);
    pthread_mutex_unlock(&s->mu);
    if (stop) break;

    if (changed_mute) { cache_clear(s); player_delete(p); p = player_new(s->m); }
    /* Restore before the target's source block. Advancing arbitrary partial
     * blocks repeatedly changes block-aware instruments, so only the final
     * published copy gets the partial block. The worker stays block-aligned. */
    uint64_t aligned = target / SEEK_BLOCK * SEEK_BLOCK;
    if (p && p->seq.sample > aligned) { player_delete(p); p = cache_restore(s, aligned); }
    if (!p) break;
    memcpy(p->synth.mute, mute, sizeof mute);
    int stale = 0;
    while (p->seq.sample < aligned) {
      dm_player_render(p, pcm, SEEK_BLOCK);
      if (p->seq.sample % CHECKPOINT_STEP == 0) cache_save(s, p);
      pthread_mutex_lock(&s->mu);
      stale = s->stop || generation != s->generation;
      pthread_mutex_unlock(&s->mu);
      if (stale) break;
    }
    if (stale) continue;
    dm_player *result = prepare ? player_copy(p) : NULL;
    if (prepare && !result) { player_delete(p); p = NULL; break; }
    if (result && target > aligned) dm_player_render(result, pcm, (uint32_t)(target - aligned));
    pthread_mutex_lock(&s->mu);
    if (!s->stop && generation == s->generation) {
      memcpy(s->env, result ? result->synth.cenv : p->synth.cenv, sizeof s->env);
      dm_player *old = s->ready;
      s->ready = result;
      s->ready_generation = generation;
      completed = generation;
      result = old;
    }
    pthread_mutex_unlock(&s->mu);
    player_delete(result);
  }
  pthread_mutex_lock(&s->mu);
  if (!p) s->failed = 1;
  pthread_mutex_unlock(&s->mu);
  player_delete(p);
  return NULL;
}

dm_seek *dm_seek_open(const dm_module *m) {
  dm_seek *s = calloc(1, sizeof *s);
  if (!s) return NULL;
  s->m = m;
  if (pthread_mutex_init(&s->mu, NULL)) { free(s); return NULL; }
  if (pthread_cond_init(&s->wake, NULL)) { pthread_mutex_destroy(&s->mu); free(s); return NULL; }
  if (pthread_create(&s->thread, NULL, seek_run, s)) {
    pthread_cond_destroy(&s->wake); pthread_mutex_destroy(&s->mu); free(s); return NULL;
  }
  return s;
}

void dm_seek_request(dm_seek *s, uint64_t sample, const uint8_t *mute, int prepare_audio) {
  pthread_mutex_lock(&s->mu);
  if (!s->generation || s->target != sample || s->prepare_audio != prepare_audio || memcmp(s->mute, mute, sizeof s->mute)) {
    s->target = sample;
    memcpy(s->mute, mute, sizeof s->mute);
    s->prepare_audio = prepare_audio;
    s->generation++;
    pthread_cond_signal(&s->wake);
  }
  pthread_mutex_unlock(&s->mu);
}

dm_player *dm_seek_take(dm_seek *s) {
  pthread_mutex_lock(&s->mu);
  dm_player *p = s->ready_generation == s->generation ? s->ready : NULL;
  if (p) s->ready = NULL;
  pthread_mutex_unlock(&s->mu);
  return p;
}

void dm_seek_env(dm_seek *s, float *env) {
  pthread_mutex_lock(&s->mu); memcpy(env, s->env, sizeof s->env); pthread_mutex_unlock(&s->mu);
}
int dm_seek_failed(dm_seek *s) {
  pthread_mutex_lock(&s->mu); int failed = s->failed; pthread_mutex_unlock(&s->mu); return failed;
}
void dm_seek_close(dm_seek *s) {
  if (!s) return;
  pthread_mutex_lock(&s->mu); s->stop = 1; pthread_cond_signal(&s->wake); pthread_mutex_unlock(&s->mu);
  pthread_join(s->thread, NULL);
  cache_clear(s); player_delete(s->ready);
  pthread_cond_destroy(&s->wake); pthread_mutex_destroy(&s->mu); free(s);
}

void dm_scrub_add(dm_scrub *s, double delta, double now, double position) {
  if (!delta || !isfinite(delta)) return;
  int direction = delta > 0 ? 1 : -1;
  if (!s->active) {
    memset(s, 0, sizeof *s);
    s->position = s->target = position;
    s->last_tick = s->last_event = now;
  } else dm_scrub_tick(s, now);
  if (s->direction && s->direction != direction) {
    s->target = s->position; s->density = 0; /* reverse immediately */
  }
  double dt = fmax(0, now - s->last_event);
  s->density = s->density * exp(-dt / .080) + fabs(delta) / .080;
  double gain = 1.0 + 3.0 * s->density / (s->density + 80.0);
  double previous_target = s->target;
  s->target = fmax(0, s->target + delta * .030 * gain);
  /* Directly follow part of new motion so the first input/reversal is visible
   * in this frame; interpolate the remainder rather than delaying all motion. */
  s->position += .25 * (s->target - previous_target);
  s->last_event = now; s->direction = direction; s->active = 1;
}

double dm_scrub_tick(dm_scrub *s, double now) {
  if (!s->active) return s->position;
  double dt = fmax(0, now - s->last_tick);
  s->position += (s->target - s->position) * -expm1(-dt / .025);
  s->last_tick = now;
  if (now - s->last_event >= .120 && fabs(s->target - s->position) < .001) {
    s->position = s->target; s->active = 0;
  }
  return s->position;
}
