/* Native export experiment: raw AVI or lossless qtrle MOV, both with PCM.
 * Optionally linked with specialized pixel/frame/post programs (DM_AOT). */
#include "dm.h"
#include "dm_mov.h"
#include <limits.h>
#include <math.h>
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
  fprintf(stderr, "usage: avi-prototype IN.dmt OUT.avi|OUT.mov [--seconds S] [--size WxH] [--fps N] [--threads N] [--scale N]\n");
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
  if (w < 2 || w > 16384 || h < 2 || h > 16384 || fps < 1 || fps > 240 || !isfinite(seconds) || seconds <= 0 || seconds > (INT_MAX-1.0)/fps || scale_override < 0 || scale_override > 64) usage();
  size_t outlen = strlen(out);
  int mov = outlen >= 4 && !strcmp(out + outlen - 4, ".mov");
  if (!mov && (outlen < 4 || strcmp(out + outlen - 4, ".avi"))) usage();
  double started = dm_now(), render_time = 0, scale_time = 0, write_time = 0, audio_time = 0;
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
  if (nframes < 1) usage();
  uint64_t nsamp = (uint64_t)(seconds * DM_SR);
  uint64_t raw_estimate = ((((uint64_t)w * 3 + 3) & ~3ull) * h * nframes) + nsamp * 4 + (uint64_t)nframes * 24 + 65536;
  if (nsamp > UINT32_MAX || (!mov && raw_estimate > UINT32_MAX)) {
    fprintf(stderr, "native prototype exceeds its 32-bit container limits; shorten the export\n"); return 1;
  }
  int scale = scale_override ? scale_override : (m.scale > 1 ? m.scale : 1);
  dm_fb fb = {0}, big = {0};
  if (dm_fb_alloc(&fb, w / scale > 1 ? w / scale : 1, h / scale > 1 ? h / scale : 1) || dm_fb_alloc(&big, w, h)) {
    fprintf(stderr, "framebuffer allocation failed\n"); return 1;
  }
  dm_avi *avi = mov ? NULL : dm_avi_open(out, w, h, fps, DM_SR);
  dm_mov *movie = mov ? dm_mov_open(out, w, h, fps, DM_SR) : NULL;
  if (!avi && !movie) { fprintf(stderr, "cannot create %s\n", out); return 1; }
  dm_scene scene; dm_scene_init(&scene, &m); scene.nthreads = threads;
  dm_seq clk; dm_seq_init(&clk, &m, DM_SR);
  dm_player vis;
  if (dm_player_init(&vis, &m, DM_SR)) { fprintf(stderr, "audio allocation failed\n"); return 1; }
  float *scratch = malloc(sizeof(float) * 2 * 8192);
  if (!scratch) return 1;
  uint64_t done = 0;
  for (int frame = 0; frame < nframes; frame++) {
    uint64_t target = (uint64_t)((double)frame * DM_SR / fps);
    double t = dm_now();
    while (done < target) { uint32_t n = target - done > 8192 ? 8192 : (uint32_t)(target - done); dm_player_render(&vis, scratch, n); done += n; }
    audio_time += dm_now() - t;
    dm_seq_seek(&clk, target);
    t = dm_now();
    dm_scene_render(&scene, &clk, vis.synth.cenv, &fb);
    render_time += dm_now() - t; t = dm_now();
    if (scale > 1) dm_fb_upscale(&fb, &big);
    scale_time += dm_now() - t; t = dm_now();
    if (mov ? dm_mov_video(movie, scale > 1 ? &big : &fb) : dm_avi_video(avi, scale > 1 ? &big : &fb)) { fprintf(stderr, "video write failed\n"); return 1; }
    write_time += dm_now() - t;
  }
  dm_player_free(&vis); dm_player aud;
  if (dm_player_init(&aud, &m, DM_SR)) return 1;
  for (uint64_t left = nsamp; left;) {
    uint32_t n = left > 8192 ? 8192 : (uint32_t)left;
    double t = dm_now();
    dm_player_render(&aud, scratch, n);
    audio_time += dm_now() - t; t = dm_now();
    if (mov ? dm_mov_audio(movie, scratch, n) : dm_avi_audio(avi, scratch, n)) { fprintf(stderr, "audio write failed\n"); return 1; }
    write_time += dm_now() - t;
    left -= n;
  }
  free(scratch); dm_player_free(&aud); dm_scene_free(&scene); dm_fb_free(&fb); dm_fb_free(&big); dm_module_free(&m);
  if (mov ? dm_mov_close(movie) : dm_avi_close(avi)) { fprintf(stderr, "video finalize failed\n"); return 1; }
  fprintf(stderr, "wrote %s: %d frames, %dx%d @ %d fps, %.1fs\n", out, nframes, w, h, fps, seconds);
  fprintf(stderr, "timing: total %.3fs, pixels %.3fs, upscale %.3fs, audio %.3fs, encode/write %.3fs\n", dm_now()-started, render_time, scale_time, audio_time, write_time);
  return 0;
}
