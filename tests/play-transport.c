/* Audio transport regression: keep 1x bit-exact and make rate conversion
 * deterministic across audio callback boundaries. */
#define main dm_tool_main
#include "../tools/dm.c"
#undef main

#include <stdio.h>

static int fail(const char *what) { fprintf(stderr, "FAIL: %s\n", what); return 1; }

static int init_ctx(audio_ctx *ac, dm_module *m, double rate) {
  memset(ac, 0, sizeof *ac);
  if (pthread_mutex_init(&ac->mu, NULL) || dm_player_init(&ac->p, m, DM_SR)) return -1;
  ac->rate = rate;
  return 0;
}

/* This deliberately does not mirror audio_cb's rolling-buffer state.  It
 * renders an independent contiguous source timeline in fixed 1024-frame
 * synth calls, then samples absolute source coordinates. */
static float *make_source(dm_module *m, uint64_t needed) {
  uint64_t frames = ((needed + 1023) / 1024) * 1024;
  if (frames > SIZE_MAX / (2 * sizeof(float))) return NULL;
  float *pcm = malloc((size_t)frames * 2 * sizeof(float));
  dm_player p;
  if (!pcm || dm_player_init(&p, m, DM_SR)) { free(pcm); return NULL; }
  for (uint64_t at = 0; at < frames; at += 1024) dm_player_render(&p, pcm + at * 2, 1024);
  dm_player_free(&p);
  return pcm;
}

static void absolute_resample(const float *source, double rate, uint64_t out_at, float *out, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    double where = (out_at + i) * rate;
    uint64_t base = (uint64_t)where;
    float mix = (float)(where - base);
    out[2 * i] = source[2 * base] + (source[2 * base + 2] - source[2 * base]) * mix;
    out[2 * i + 1] = source[2 * base + 1] + (source[2 * base + 3] - source[2 * base + 1]) * mix;
  }
}

static int rate_case(dm_module *m, double rate) {
  audio_ctx ac;
  const uint32_t chunks[] = {17, 257, 1, 1024, 63, 411};
  uint64_t total = 0, out_at = 0;
  for (size_t i = 0; i < sizeof chunks / sizeof *chunks; i++) total += chunks[i];
  float *source = make_source(m, (uint64_t)((total - 1) * rate) + 2);
  if (!source || init_ctx(&ac, m, rate)) { free(source); return fail("player init"); }
  for (size_t i = 0; i < sizeof chunks / sizeof *chunks; i++) {
    float actual[2 * 1024], expected[2 * 1024];
    audio_cb(&ac, actual, chunks[i]);
    absolute_resample(source, rate, out_at, expected, chunks[i]);
    if (memcmp(actual, expected, sizeof(float) * 2 * chunks[i])) return fail("rate conversion differs from 1024-frame reference");
    out_at += chunks[i];
  }
  free(source); dm_player_free(&ac.p); pthread_mutex_destroy(&ac.mu);
  return 0;
}

int main(void) {
  /* A slow 640x400 shader should converge to its frame budget without
   * oscillating resolution. A lighter scene may gradually regain detail. */
  play_budget budget = {1, 0, 0};
  for (int i = 0; i < 90; i++) budget_observe(&budget, .108 * budget.scale * budget.scale + .001, 30);
  if (budget.scale < .35 || budget.scale > .55) return fail("adaptive playback missed its frame budget");
  double settled = budget.scale;
  for (int i = 0; i < 90; i++) budget_observe(&budget, .108 * budget.scale * budget.scale + .001, 30);
  if (fabs(budget.scale - settled) > .01) return fail("adaptive resolution oscillates at steady load");
  for (int i = 0; i < 900; i++) budget_observe(&budget, .008 * budget.scale * budget.scale + .001, 30);
  if (budget.scale < .95 || budget.scale > 1) return fail("adaptive playback never restores detail");
  budget_observe(&budget, INFINITY, 30);
  if (!isfinite(budget.scale)) return fail("invalid timing corrupted adaptive resolution");
  dm_module m = {0};
  if (load_any("demos/pulse.dmt", &m)) return fail("load pulse");

  audio_ctx direct, paused;
  dm_player expected;
  float actual[2 * 1024], reference_pcm[2 * 1024], silence[2 * 127];
  if (init_ctx(&direct, &m, 1.0) || dm_player_init(&expected, &m, DM_SR)) return fail("direct init");
  audio_cb(&direct, actual, 1024);
  dm_player_render(&expected, reference_pcm, 1024);
  if (memcmp(actual, reference_pcm, sizeof actual)) return fail("1x must be bit-identical to dm_player_render");
  dm_player_free(&direct.p); dm_player_free(&expected); pthread_mutex_destroy(&direct.mu);

  float *paused_source = make_source(&m, 1282);
  if (!paused_source || init_ctx(&paused, &m, 1.25)) return fail("pause init");
  uint64_t sample_before = paused.p.seq.sample;
  double phase_before = paused.phase;
  uint32_t at_before = paused.source_at, n_before = paused.source_n;
  int ready_before = paused.ready;
  float a_before[2] = {paused.a[0], paused.a[1]}, b_before[2] = {paused.b[0], paused.b[1]};
  paused.paused = 1; memset(silence, 1, sizeof silence); audio_cb(&paused, silence, 127);
  for (size_t i = 0; i < sizeof silence / sizeof *silence; i++) if (silence[i] != 0) return fail("paused callback is not silent");
  if (paused.p.seq.sample != sample_before || paused.phase != phase_before || paused.source_at != at_before || paused.source_n != n_before || paused.ready != ready_before || memcmp(paused.a, a_before, sizeof a_before) || memcmp(paused.b, b_before, sizeof b_before)) return fail("paused callback advanced transport state");
  paused.paused = 0; audio_cb(&paused, actual, 1024);
  absolute_resample(paused_source, 1.25, 0, reference_pcm, 1024);
  if (memcmp(actual, reference_pcm, sizeof actual)) return fail("paused callback advanced source samples");
  free(paused_source); dm_player_free(&paused.p); pthread_mutex_destroy(&paused.mu);

  if (rate_case(&m, 0.5) || rate_case(&m, 1.25) || rate_case(&m, 2.0)) { dm_module_free(&m); return 1; }
  dm_module_free(&m);
  puts("ok: play transport 1x, resampling and pause");
  return 0;
}
