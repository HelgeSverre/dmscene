/* Scene worker state belongs to each scene and each render pass. */
#include "dm.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  static dm_module m;
  const char *src =
    "song channels=1\n"
    "order 0\n"
    "pattern 0 rows=1 {\n .\n}\n"
    "uniform shared\nframe {\n shared = 0\n}\n"
    "pixel {\n $q = $q + 0.001\n shared = shared + 0.01\n rgb($q + shared, 0, 0)\n}\n"
    "post {\n $q = $q + 0.001\n cadd($q, 0, 0)\n}\n";
  if (dm_compile(src, "scene-workers", &m)) { fprintf(stderr, "%s\n", dm_error()); return 1; }
  dm_scene a, b, ref;
  dm_scene_init(&a, &m); dm_scene_init(&b, &m); dm_scene_init(&ref, &m);
  a.nthreads = b.nthreads = ref.nthreads = 2;
  dm_fb fa = {0}, fb = {0}, fr = {0};
  if (dm_fb_alloc(&fa, 4, 4) || dm_fb_alloc(&fb, 4, 4) || dm_fb_alloc(&fr, 4, 4)) return 1;
  dm_seq clk; dm_seq_init(&clk, &m, DM_SR);
  dm_scene_render(&a, &clk, NULL, &fa);
  dm_scene_render(&b, &clk, NULL, &fb);
  dm_scene_render(&b, &clk, NULL, &fb);
  dm_scene_render(&a, &clk, NULL, &fa);
  dm_scene_render(&ref, &clk, NULL, &fr);
  dm_scene_render(&ref, &clk, NULL, &fr);
  if (memcmp(fa.rgb, fr.rgb, 4 * 4 * 3) ||
      memcmp(fa.rgb, fa.rgb + 4 * 3, 4 * 3) || a.uni[0] != 0 ||
      fabsf(a.worker_state[0][1][0] - 0.016f) > 0.0001f ||
      fabsf(a.worker_state[1][1][0] - 0.016f) > 0.0001f) {
    fprintf(stderr, "scene worker state leaked between scenes or passes\n"); return 1;
  }
  uint8_t *old = fa.rgb;
  if (dm_fb_alloc(&fa, 0, 4) == 0 || fa.rgb != old || fa.w != 4 || fa.h != 4) {
    fprintf(stderr, "invalid framebuffer allocation destroyed the old buffer\n"); return 1;
  }
  dm_fb_free(&fa); dm_fb_free(&fb); dm_fb_free(&fr);
  dm_scene_free(&a); dm_scene_free(&b); dm_scene_free(&ref);
  dm_module_free(&m);
  puts("ok: scene worker isolation and framebuffer allocation");
  return 0;
}
