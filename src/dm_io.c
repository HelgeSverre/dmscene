/* dm_io.c - PNG (stored deflate), PPM and WAV writers.  No zlib. */
#include "dm.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

static uint32_t crc_tab[256];
static pthread_once_t crc_once = PTHREAD_ONCE_INIT;
static void crc_init(void) {
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
  pthread_once(&crc_once, crc_init);
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  fwrite(sig, 1, 8, f);
  uint8_t ihdr[13]; be32(ihdr, fb->w); be32(ihdr + 4, fb->h); ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
  chunk(f, "IHDR", ihdr, 13);
  size_t stride = (size_t)fb->w * 3 + 1, raw_n = stride * fb->h;
  uint8_t *raw = malloc(raw_n);
  if (!raw) { fclose(f); return -1; }
  for (int y = 0; y < fb->h; y++) { raw[y * stride] = 0; memcpy(raw + y * stride + 1, fb->rgb + (size_t)y * fb->w * 3, (size_t)fb->w * 3); }
  size_t nblk = (raw_n + 65534) / 65535, zn = 2 + raw_n + nblk * 5 + 4;
  uint8_t *z = zn <= UINT32_MAX ? malloc(zn) : NULL, *p = z;
  if (!z) { free(raw); fclose(f); return -1; }
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
  int failed = ferror(f);
  if (fclose(f)) failed = 1;
  free(raw); free(z);
  return failed ? -1 : 0;
}

int dm_ppm_write(const dm_fb *fb, const char *path) {
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  fprintf(f, "P6\n%d %d\n255\n", fb->w, fb->h);
  fwrite(fb->rgb, 1, (size_t)fb->w * fb->h * 3, f);
  int failed = ferror(f);
  if (fclose(f)) failed = 1;
  return failed ? -1 : 0;
}

static void le32(FILE *f, uint32_t v) { uint8_t b[4] = {v, v >> 8, v >> 16, v >> 24}; fwrite(b, 1, 4, f); }
static void le16(FILE *f, uint32_t v) { uint8_t b[2] = {v, v >> 8}; fwrite(b, 1, 2, f); }
static void wav_header(dm_wav *w) {
  uint32_t data = w->frames * 4;
  if (fseek(w->f, 0, SEEK_SET)) { w->failed = 1; return; }
  fwrite("RIFF", 1, 4, w->f); le32(w->f, 36 + data); fwrite("WAVEfmt ", 1, 8, w->f);
  le32(w->f, 16); le16(w->f, 1); le16(w->f, 2); le32(w->f, w->sr); le32(w->f, w->sr * 4); le16(w->f, 4); le16(w->f, 16);
  fwrite("data", 1, 4, w->f); le32(w->f, data);
  if (ferror(w->f)) w->failed = 1;
}
int dm_wav_open(dm_wav *w, const char *path, int sr) {
  w->f = fopen(path, "wb"); w->frames = 0; w->sr = sr; w->failed = 0;
  if (!w->f) return -1;
  wav_header(w);
  if (fflush(w->f)) w->failed = 1;
  if (w->failed) { fclose(w->f); w->f = NULL; return -1; }
  return 0;
}
int dm_wav_write(dm_wav *w, const float *lr, uint32_t frames) {
  if (!w->f || w->failed) return -1;
  if (frames > (UINT32_MAX - 36) / 4 - w->frames) { w->failed = 1; return -1; }
  uint8_t pcm[8192];
  while (frames) {
    uint32_t n = frames > 2048 ? 2048 : frames;
    for (uint32_t i = 0; i < n * 2; i++) {
      float x = lr[i]; if (!isfinite(x)) x = 0; if (x > 1) x = 1; if (x < -1) x = -1;
      int v = (int)(x * 32767.0f);
      pcm[2 * i] = (uint8_t)v; pcm[2 * i + 1] = (uint8_t)(v >> 8);
    }
    if (fwrite(pcm, 4, n, w->f) != n) { w->failed = 1; return -1; }
    lr += (size_t)n * 2;
    frames -= n;
    w->frames += n;
  }
  return 0;
}
int dm_wav_close(dm_wav *w) {
  if (w->f) {
    if (!w->failed) wav_header(w);
    if (fclose(w->f)) w->failed = 1;
    w->f = NULL;
  }
  return w->failed ? -1 : 0;
}

/* Experimental raw AVI: BGR frames and PCM audio, no external encoder. */
typedef struct { uint32_t id, flags, off, size; } avi_idx;
struct dm_avi {
  FILE *f; int w, h, fps, sr; uint32_t frames, audio_frames;
  long riff_size, avih_frames, vstrh_frames, astrh_frames, movi;
  avi_idx *idx; size_t nidx, cap; uint8_t *pixels;
  uint32_t stride, image;
  int failed;
};

static void avi_u32(FILE *f, uint32_t v) { le32(f, v); }
static void avi_id(FILE *f, const char *s) { fwrite(s, 1, 4, f); }
static void avi_chunk(FILE *f, const char *id, uint32_t n) { avi_id(f, id); avi_u32(f, n); }
static int avi_patch(dm_avi *a, long at, uint32_t v) {
  long here = ftell(a->f);
  if (here < 0 || at < 0 || fseek(a->f, at, SEEK_SET) || ferror(a->f)) { a->failed = 1; return -1; }
  avi_u32(a->f, v);
  if (fseek(a->f, here, SEEK_SET) || ferror(a->f)) { a->failed = 1; return -1; }
  return 0;
}
static int avi_index(dm_avi *a, const char *id, uint32_t flags, uint32_t size) {
  if (a->failed) return -1;
  long pos = ftell(a->f);
  if (pos < 0 || (uint64_t)pos + size + 16 * (uint64_t)(a->nidx + 1) + 16 > UINT32_MAX) { a->failed = 1; return -1; }
  if (a->nidx == a->cap) {
    size_t cap = a->cap ? a->cap * 2 : 256;
    avi_idx *p = realloc(a->idx, cap * sizeof *p);
    if (!p) { a->failed = 1; return -1; }
    a->idx = p; a->cap = cap;
  }
  avi_idx *e = &a->idx[a->nidx++];
  memcpy(&e->id, id, 4); e->flags = flags; e->off = (uint32_t)(pos - a->movi); e->size = size;
  return 0;
}

dm_avi *dm_avi_open(const char *path, int w, int h, int fps, int sr) {
  if (w < 1 || h < 1 || fps < 1 || sr < 1 || (((uint64_t)w * 3 + 3) & ~3ull) * h > UINT32_MAX) return NULL;
  dm_avi *a = calloc(1, sizeof *a);
  if (!a) return NULL;
  a->f = fopen(path, "wb"); a->w = w; a->h = h; a->fps = fps; a->sr = sr;
  a->stride = ((uint32_t)w * 3 + 3) & ~3u; a->image = a->stride * h;
  a->pixels = calloc(a->image, 1);
  if (!a->f || !a->pixels) { if (a->f) fclose(a->f); free(a->pixels); free(a); return NULL; }
  setvbuf(a->f, NULL, _IOFBF, 1024 * 1024);
  FILE *f = a->f; uint32_t image = a->image;
  avi_id(f, "RIFF"); a->riff_size = ftell(f); avi_u32(f, 0); avi_id(f, "AVI ");
  avi_id(f, "LIST"); long hdrl = ftell(f); avi_u32(f, 0); avi_id(f, "hdrl");
  avi_chunk(f, "avih", 56);
  avi_u32(f, 1000000u / (uint32_t)fps); avi_u32(f, image * (uint32_t)fps + (uint32_t)sr * 4); avi_u32(f, 0); avi_u32(f, 0x10);
  a->avih_frames = ftell(f); avi_u32(f, 0); avi_u32(f, 0); avi_u32(f, 2); avi_u32(f, image); avi_u32(f, w); avi_u32(f, h);
  for (int i = 0; i < 4; i++) avi_u32(f, 0);
  avi_id(f, "LIST"); long vlist = ftell(f); avi_u32(f, 0); avi_id(f, "strl");
  avi_chunk(f, "strh", 56); avi_id(f, "vids"); avi_id(f, "DIB "); avi_u32(f, 0); avi_u32(f, 0);
  avi_u32(f, 0); avi_u32(f, 1); avi_u32(f, fps); avi_u32(f, 0); a->vstrh_frames = ftell(f); avi_u32(f, 0);
  avi_u32(f, image); avi_u32(f, UINT32_MAX); avi_u32(f, 0); avi_u32(f, 0); avi_u32(f, ((uint32_t)h << 16) | (uint16_t)w);
  avi_chunk(f, "strf", 40); avi_u32(f, 40); avi_u32(f, w); avi_u32(f, h); le16(f, 1); le16(f, 24); avi_u32(f, 0); avi_u32(f, image);
  avi_u32(f, 0); avi_u32(f, 0); avi_u32(f, 0); avi_u32(f, 0); avi_patch(a, vlist, (uint32_t)(ftell(f) - vlist - 4));
  avi_id(f, "LIST"); long alist = ftell(f); avi_u32(f, 0); avi_id(f, "strl");
  avi_chunk(f, "strh", 56); avi_id(f, "auds"); avi_u32(f, 1); avi_u32(f, 0); avi_u32(f, 0); avi_u32(f, 0);
  avi_u32(f, 1); avi_u32(f, sr); avi_u32(f, 0); a->astrh_frames = ftell(f); avi_u32(f, 0); avi_u32(f, 4096); avi_u32(f, UINT32_MAX); avi_u32(f, 4); avi_u32(f, 0); avi_u32(f, 0);
  avi_chunk(f, "strf", 16); le16(f, 1); le16(f, 2); avi_u32(f, sr); avi_u32(f, sr * 4); le16(f, 4); le16(f, 16);
  avi_patch(a, alist, (uint32_t)(ftell(f) - alist - 4)); avi_patch(a, hdrl, (uint32_t)(ftell(f) - hdrl - 4));
  avi_id(f, "LIST"); long movi_size = ftell(f); avi_u32(f, 0); avi_id(f, "movi"); a->movi = ftell(f); (void)movi_size;
  if (a->failed || a->movi < 0 || ferror(f)) { fclose(f); free(a->idx); free(a->pixels); free(a); return NULL; }
  return a;
}

int dm_avi_video(dm_avi *a, const dm_fb *fb) {
  if (!a || !fb || !fb->rgb || a->failed || fb->w != a->w || fb->h != a->h || a->frames == UINT32_MAX) return -1;
  uint32_t bytes = a->image;
  if (avi_index(a, "00db", 0x10, bytes)) return -1;
  avi_chunk(a->f, "00db", bytes);
  for (int y = a->h - 1; y >= 0; y--) {
    const uint8_t *src = fb->rgb + (size_t)y * a->w * 3;
    uint8_t *dst = a->pixels + (size_t)(a->h - 1 - y) * a->stride;
    for (int x = 0; x < a->w; x++) { dst[x * 3] = src[x * 3 + 2]; dst[x * 3 + 1] = src[x * 3 + 1]; dst[x * 3 + 2] = src[x * 3]; }
  }
  if (fwrite(a->pixels, 1, bytes, a->f) != bytes || ferror(a->f)) { a->failed = 1; return -1; }
  a->frames++;
  return 0;
}

int dm_avi_audio(dm_avi *a, const float *lr, uint32_t frames) {
  if (!a || !lr || a->failed || frames > UINT32_MAX - a->audio_frames || frames > UINT32_MAX / 4) return -1;
  uint32_t bytes = frames * 4;
  if (avi_index(a, "01wb", 0, bytes)) return -1;
  avi_chunk(a->f, "01wb", bytes);
  uint8_t pcm[8192];
  for (uint32_t off = 0; off < frames;) {
    uint32_t n = frames - off > 2048 ? 2048 : frames - off;
    for (uint32_t i = 0; i < n * 2; i++) {
      float x = lr[(size_t)off * 2 + i]; if (x != x) x = 0; if (x > 1) x = 1; if (x < -1) x = -1;
      int v = (int)(x * 32767.0f); pcm[i*2] = (uint8_t)v; pcm[i*2+1] = (uint8_t)(v >> 8);
    }
    if (fwrite(pcm, 4, n, a->f) != n) { a->failed = 1; return -1; }
    off += n;
  }
  if (ferror(a->f)) { a->failed = 1; return -1; }
  a->audio_frames += frames;
  return 0;
}

int dm_avi_close(dm_avi *a) {
  if (!a) return -1;
  FILE *f = a->f;
  if (a->failed) { fclose(f); free(a->idx); free(a->pixels); free(a); return -1; }
  long movi_end = ftell(f);
  if (movi_end < 0) a->failed = 1;
  avi_patch(a, a->movi - 8, (uint32_t)(movi_end - (a->movi - 8) - 4));
  avi_chunk(f, "idx1", (uint32_t)(a->nidx * sizeof *a->idx));
  for (size_t i = 0; i < a->nidx; i++) { fwrite(&a->idx[i].id, 1, 4, f); avi_u32(f, a->idx[i].flags); avi_u32(f, a->idx[i].off); avi_u32(f, a->idx[i].size); }
  long end = ftell(f);
  avi_patch(a, a->riff_size, (uint32_t)(end - 8));
  avi_patch(a, a->avih_frames, a->frames); avi_patch(a, a->vstrh_frames, a->frames); avi_patch(a, a->astrh_frames, a->audio_frames);
  int bad = a->failed || ferror(f) ? -1 : 0;
  if (fclose(f)) bad = -1;
  free(a->idx); free(a->pixels); free(a);
  return bad;
}
