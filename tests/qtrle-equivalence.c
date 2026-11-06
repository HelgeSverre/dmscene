/* Generates a qtrle MOV and exact RGB/PCM references for decode comparison. */
#include "dm_mov.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void put_pcm(FILE *f, float x) {
  if (x != x) x = 0; if (x > 1) x = 1; if (x < -1) x = -1;
  int v = (int)(x * 32767.0f); uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
  fwrite(b, 1, 2, f);
}

int main(int argc, char **argv) {
  if (argc != 4) return 2;
  const int w = 65, h = 9, fps = 10, nframes = 25;
  FILE *rgb = fopen(argv[2], "wb"), *pcm = fopen(argv[3], "wb");
  dm_mov *mov = dm_mov_open(argv[1], w, h, fps, DM_SR);
  dm_fb fb = {w, h, malloc((size_t)w * h * 3)};
  if (!rgb || !pcm || !mov || !fb.rgb) return 1;
  for (int frame = 0; frame < nframes; frame++) {
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
      uint8_t *p = fb.rgb + ((size_t)y * w + x) * 3;
      p[0] = (uint8_t)((x / 2) * 7); p[1] = (uint8_t)(y * 21); p[2] = (uint8_t)(x ^ y);
    }
    if (frame == 2) for (int x = 11; x < 54; x++) { uint8_t *p = fb.rgb + ((size_t)4 * w + x) * 3; p[0] = 240; p[1] = 17; p[2] = 91; }
    if (frame >= 20) for (int x = 0; x < w; x++) fb.rgb[((size_t)7 * w + x) * 3 + 1] ^= 0x55;
    if (fwrite(fb.rgb, 1, (size_t)w * h * 3, rgb) != (size_t)w * h * 3 || dm_mov_video(mov, &fb)) return 1;
  }
  const uint32_t chunks[] = {7, 4096, 8192, 97955};
  for (size_t c = 0; c < sizeof chunks / sizeof *chunks; c++) {
    float *lr = malloc(sizeof(float) * chunks[c] * 2); if (!lr) return 1;
    for (uint32_t i = 0; i < chunks[c] * 2; i++) { float x = ((int)((i + c * 73) % 257) - 128) / 128.0f; if (c == 1 && i == 3) x = NAN; lr[i] = x; put_pcm(pcm, x); }
    if (dm_mov_audio(mov, lr, chunks[c])) return 1; free(lr);
  }
  free(fb.rgb); fclose(rgb); fclose(pcm);
  return dm_mov_close(mov) ? 1 : 0;
}
