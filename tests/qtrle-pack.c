/* Pack pre-rendered RGB24 and stereo s16le PCM into a qtrle MOV. */
#include "dm_mov.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  if (argc != 8) { fprintf(stderr, "usage: qtrle-pack RGB PCM OUT W H FPS FRAMES\\n"); return 2; }
  int w = atoi(argv[4]), h = atoi(argv[5]), fps = atoi(argv[6]), frames = atoi(argv[7]);
  FILE *rgb = fopen(argv[1], "rb"), *pcm = fopen(argv[2], "rb");
  if (!rgb || !pcm || w < 1 || h < 1 || fps < 1 || frames < 1) return 1;
  dm_mov *mov = dm_mov_open(argv[3], w, h, fps, DM_SR);
  dm_fb fb = {w, h, malloc((size_t)w * h * 3)};
  float *audio = malloc(sizeof(float) * 2 * 8192); int16_t *raw = malloc(sizeof(int16_t) * 2 * 8192);
  if (!mov || !fb.rgb || !audio || !raw) return 1;
  for (int i = 0; i < frames; i++) if (fread(fb.rgb, 1, (size_t)w * h * 3, rgb) != (size_t)w * h * 3 || dm_mov_video(mov, &fb)) return 1;
  for (;;) {
    size_t n = fread(raw, sizeof(int16_t) * 2, 8192, pcm); if (!n) break;
    for (size_t i = 0; i < n * 2; i++) audio[i] = raw[i] / 32767.0f;
    if (dm_mov_audio(mov, audio, (uint32_t)n)) return 1;
  }
  fclose(rgb); fclose(pcm); free(fb.rgb); free(audio); free(raw);
  return dm_mov_close(mov) ? 1 : 0;
}
