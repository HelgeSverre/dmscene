/* PROTOTYPE: dependency-free qtrle MOV export experiment.
 * QuickTime Animation RGB video + PCM audio. Intentionally separate from dm's public CLI. */
#include "dm.h"
#include "dm_mov.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *slurp(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END)) { fclose(f); return NULL; }
  long n = ftell(f); if (n < 0 || fseek(f, 0, SEEK_SET)) { fclose(f); return NULL; }
  char *s = malloc((size_t)n + 1);
  if (!s) { fclose(f); return NULL; }
  size_t got = fread(s, 1, (size_t)n, f); fclose(f); s[got] = 0;
  return s;
}

static void usage(void) {
  fprintf(stderr, "usage: qtrle-prototype IN.dmt OUT.mov [--seconds S] [--size WxH] [--fps N] [--threads N] [--scale N]\n");
  exit(2);
}

int main(int argc, char **argv) {
  if (argc < 3) usage();
  const char *in = argv[1], *out = argv[2];
  int w = 960, h = 600, fps = 30, threads = 0, scale_override = 0;
  double seconds = 3.0;
  for (int i = 3; i < argc; i++) {
    if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
    else if (!strcmp(argv[i], "--size") && i + 1 < argc && sscanf(argv[++i], "%dx%d", &w, &h) == 2) {}
    else if (!strcmp(argv[i], "--fps") && i + 1 < argc) fps = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--threads") && i + 1 < argc) threads = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale_override = atoi(argv[++i]);
    else usage();
  }
  if (w < 2 || h < 2 || fps < 1 || seconds <= 0 || scale_override < 0) usage();
  char *src = slurp(in);
  if (!src) { fprintf(stderr, "cannot read %s\n", in); return 1; }
  dm_module m;
  if (dm_compile(src, in, &m)) { fprintf(stderr, "%s\n", dm_error()); free(src); return 1; }
  free(src);
#ifdef DM_AOT
  extern int dm_aot_bind(const dm_module *m);
  if (dm_aot_bind(&m)) { fprintf(stderr, "native code does not match this demo; rebuild the exporter\n"); return 1; }
#endif
  int nframes = (int)(seconds * fps + 0.5);
  uint64_t nsamp = (uint64_t)(seconds * DM_SR);
  if ((uint64_t)w * h * 3 * nframes + nsamp * 4 > UINT32_MAX) {
    fprintf(stderr, "prototype AVI is limited to 4 GiB; shorten or reduce dimensions\n"); return 1;
  }
  int scale = scale_override ? scale_override : (m.scale > 1 ? m.scale : 1);
  dm_fb fb = {0}, big = {0};
  if (dm_fb_alloc(&fb, w / scale > 1 ? w / scale : 1, h / scale > 1 ? h / scale : 1) || dm_fb_alloc(&big, w, h)) {
    fprintf(stderr, "framebuffer allocation failed\n"); return 1;
  }
  dm_mov *avi = dm_mov_open(out, w, h, fps, DM_SR);
  if (!avi) { fprintf(stderr, "cannot create %s\n", out); return 1; }
  dm_scene scene; dm_scene_init(&scene, &m); scene.nthreads = threads;
  dm_seq clk; dm_seq_init(&clk, &m, DM_SR);
  dm_player vis;
  if (dm_player_init(&vis, &m, DM_SR)) { fprintf(stderr, "audio allocation failed\n"); return 1; }
  float *scratch = malloc(sizeof(float) * 2 * 8192);
  if (!scratch) return 1;
  uint64_t done = 0;
  for (int frame = 0; frame < nframes; frame++) {
    uint64_t target = (uint64_t)((double)frame * DM_SR / fps);
    while (done < target) { uint32_t n = target - done > 8192 ? 8192 : (uint32_t)(target - done); dm_player_render(&vis, scratch, n); done += n; }
    dm_seq_seek(&clk, target);
    dm_scene_render(&scene, &clk, vis.synth.cenv, &fb);
    if (scale > 1) dm_fb_upscale(&fb, &big);
    if (dm_mov_video(avi, scale > 1 ? &big : &fb)) { fprintf(stderr, "video write failed\n"); return 1; }
  }
  dm_player_free(&vis); dm_player aud;
  if (dm_player_init(&aud, &m, DM_SR)) return 1;
  for (uint64_t left = nsamp; left;) {
    uint32_t n = left > 8192 ? 8192 : (uint32_t)left;
    dm_player_render(&aud, scratch, n);
    if (dm_mov_audio(avi, scratch, n)) { fprintf(stderr, "audio write failed\n"); return 1; }
    left -= n;
  }
  free(scratch); dm_player_free(&aud); dm_scene_free(&scene); dm_fb_free(&fb); dm_fb_free(&big); dm_module_free(&m);
  if (dm_mov_close(avi)) { fprintf(stderr, "AVI finalize failed\n"); return 1; }
  fprintf(stderr, "wrote %s: %d frames, %dx%d @ %d fps, %.1fs (qtrle MOV prototype)\n", out, nframes, w, h, fps, seconds);
  return 0;
}
