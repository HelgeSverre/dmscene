/* dm_io.c - PNG (stored deflate), PPM and WAV writers.  No zlib. */
#include "dm.h"
#include <stdlib.h>
#include <string.h>

static uint32_t crc_tab[256];
static void crc_init(void) {
  if (crc_tab[1]) return;
  for (uint32_t n = 0; n < 256; n++) {
    uint32_t c = n;
    for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
    crc_tab[n] = c;
  }
}
static uint32_t crc32_upd(uint32_t c, const uint8_t *p, size_t n) {
  c = ~c;
  while (n--) c = crc_tab[(c ^ *p++) & 255] ^ (c >> 8);
  return ~c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
  uint8_t hdr[8]; be32(hdr, n); memcpy(hdr + 4, type, 4);
  fwrite(hdr, 1, 8, f);
  if (n) fwrite(data, 1, n, f);
  uint32_t c = crc32_upd(0, hdr + 4, 4); if (n) c = crc32_upd(c, data, n);
  uint8_t cb[4]; be32(cb, c); fwrite(cb, 1, 4, f);
}

int dm_png_write(const dm_fb *fb, const char *path) {
  crc_init();
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  fwrite(sig, 1, 8, f);
  uint8_t ihdr[13]; be32(ihdr, fb->w); be32(ihdr + 4, fb->h); ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
  chunk(f, "IHDR", ihdr, 13);
  size_t stride = (size_t)fb->w * 3 + 1, raw_n = stride * fb->h;
  uint8_t *raw = malloc(raw_n);
  for (int y = 0; y < fb->h; y++) { raw[y * stride] = 0; memcpy(raw + y * stride + 1, fb->rgb + (size_t)y * fb->w * 3, (size_t)fb->w * 3); }
  size_t nblk = (raw_n + 65534) / 65535, zn = 2 + raw_n + nblk * 5 + 4;
  uint8_t *z = malloc(zn), *p = z;
  *p++ = 0x78; *p++ = 0x01;
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
  for (size_t off = 0; off < raw_n; off += 65535) {
    size_t len = raw_n - off < 65535 ? raw_n - off : 65535;
    *p++ = off + len >= raw_n ? 1 : 0;
    *p++ = len & 255; *p++ = len >> 8; *p++ = ~len & 255; *p++ = (~len >> 8) & 255;
    memcpy(p, raw + off, len); p += len;
  }
  be32(p, (b << 16) | a); p += 4;
  chunk(f, "IDAT", z, (uint32_t)(p - z));
  chunk(f, "IEND", NULL, 0);
  fclose(f); free(raw); free(z);
  return 0;
}

int dm_ppm_write(const dm_fb *fb, const char *path) {
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  fprintf(f, "P6\n%d %d\n255\n", fb->w, fb->h);
  fwrite(fb->rgb, 1, (size_t)fb->w * fb->h * 3, f);
  fclose(f);
  return 0;
}

static void le32(FILE *f, uint32_t v) { uint8_t b[4] = {v, v >> 8, v >> 16, v >> 24}; fwrite(b, 1, 4, f); }
static void le16(FILE *f, uint32_t v) { uint8_t b[2] = {v, v >> 8}; fwrite(b, 1, 2, f); }
static void wav_header(dm_wav *w) {
  uint32_t data = w->frames * 4;
  fseek(w->f, 0, SEEK_SET);
  fwrite("RIFF", 1, 4, w->f); le32(w->f, 36 + data); fwrite("WAVEfmt ", 1, 8, w->f);
  le32(w->f, 16); le16(w->f, 1); le16(w->f, 2); le32(w->f, w->sr); le32(w->f, w->sr * 4); le16(w->f, 4); le16(w->f, 16);
  fwrite("data", 1, 4, w->f); le32(w->f, data);
}
int dm_wav_open(dm_wav *w, const char *path, int sr) {
  w->f = fopen(path, "wb"); w->frames = 0; w->sr = sr;
  if (!w->f) return -1;
  wav_header(w);
  return 0;
}
void dm_wav_write(dm_wav *w, const float *lr, uint32_t frames) {
  for (uint32_t i = 0; i < frames * 2; i++) {
    float x = lr[i]; if (x > 1) x = 1; if (x < -1) x = -1;
    int v = (int)(x * 32767.0f);
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    fwrite(b, 1, 2, w->f);
  }
  w->frames += frames;
}
void dm_wav_close(dm_wav *w) { if (!w->f) return; wav_header(w); fclose(w->f); w->f = NULL; }
