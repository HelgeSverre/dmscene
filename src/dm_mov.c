/* Experimental dependency-free QuickTime Animation (qtrle) MOV writer. */
#include "dm_mov.h"
#include <stdlib.h>
#include <string.h>

struct dm_mov {
  FILE *f; int w, h, fps, sr; long mdat_size;
  uint8_t *prev, *pkt; size_t pkt_cap;
  uint32_t *voff, *vsize, *aoff, *acount;
  size_t nv, cv, na, ca; uint32_t audio_frames;
};

static void be16(FILE *f, uint16_t v) { uint8_t b[2] = {v >> 8, v}; fwrite(b, 1, 2, f); }
static void be32(FILE *f, uint32_t v) { uint8_t b[4] = {v >> 24, v >> 16, v >> 8, v}; fwrite(b, 1, 4, f); }
static void tag(FILE *f, const char *s) { fwrite(s, 1, 4, f); }
static long atom(FILE *f, const char *type) { long p = ftell(f); be32(f, 0); tag(f, type); return p; }
static void end_atom(FILE *f, long p) { long e = ftell(f), here = e; fseek(f, p, SEEK_SET); be32(f, (uint32_t)(e - p)); fseek(f, here, SEEK_SET); }
static int grow_pair(uint32_t **a, uint32_t **b, size_t *cap, size_t n) {
  if (n <= *cap) return 0;
  size_t c = *cap ? *cap * 2 : 256; while (c < n) c *= 2;
  uint32_t *x = malloc(c * sizeof *x), *y = malloc(c * sizeof *y);
  if (!x || !y) { free(x); free(y); return -1; }
  if (*cap) { memcpy(x, *a, *cap * sizeof *x); memcpy(y, *b, *cap * sizeof *y); }
  free(*a); free(*b); *a = x; *b = y; *cap = c; return 0;
}
static int put(dm_mov *m, uint8_t v, size_t *n) { if (*n >= m->pkt_cap) return -1; m->pkt[(*n)++] = v; return 0; }
static int rgb_eq(const uint8_t *a, const uint8_t *b) { return !memcmp(a, b, 3); }

static int put_line(dm_mov *m, const uint8_t *cur, const uint8_t *old, size_t *n) {
  int x = 0, w = m->w;
  if (old) { int skip = 0; while (skip < w && skip < 254 && rgb_eq(cur + skip * 3, old + skip * 3)) skip++; if (put(m, (uint8_t)(skip + 1), n)) return -1; x = skip; }
  else if (put(m, 1, n)) return -1;
  while (x < w) {
    if (old && rgb_eq(cur + x * 3, old + x * 3)) {
      int run = 0; while (x + run < w && run < 254 && rgb_eq(cur + (x + run) * 3, old + (x + run) * 3)) run++;
      if (put(m, 0, n) || put(m, (uint8_t)(run + 1), n)) return -1; x += run; continue;
    }
    int rep = 1; while (x + rep < w && rep < 128 && rgb_eq(cur + x * 3, cur + (x + rep) * 3)) rep++;
    if (rep >= 2) { if (put(m, (uint8_t)(256 - rep), n)) return -1; for (int k = 0; k < 3; k++) if (put(m, cur[x * 3 + k], n)) return -1; x += rep; continue; }
    int start = x++; while (x < w && x - start < 127) { if (old && rgb_eq(cur + x * 3, old + x * 3)) break; int r = 1; while (x + r < w && r < 2 && rgb_eq(cur + x * 3, cur + (x + r) * 3)) r++; if (r >= 2) break; x++; }
    if (put(m, (uint8_t)(x - start), n)) return -1;
    for (int p = start; p < x; p++) for (int k = 0; k < 3; k++) if (put(m, cur[p * 3 + k], n)) return -1;
  }
  return put(m, 255, n);
}

dm_mov *dm_mov_open(const char *path, int w, int h, int fps, int sr) {
  if (w < 1 || h < 1 || fps < 1 || sr < 1 || (uint64_t)w * h * 4 > SIZE_MAX) return NULL;
  dm_mov *m = calloc(1, sizeof *m); if (!m) return NULL;
  m->f = fopen(path, "wb"); m->w = w; m->h = h; m->fps = fps; m->sr = sr;
  size_t pixels = (size_t)w * h * 3; m->prev = malloc(pixels); m->pkt_cap = pixels * 4 + (size_t)h * 4 + 64; m->pkt = malloc(m->pkt_cap);
  if (!m->f || !m->prev || !m->pkt) { if (m->f) fclose(m->f); free(m->prev); free(m->pkt); free(m); return NULL; }
  be32(m->f, 20); tag(m->f, "ftyp"); tag(m->f, "qt  "); be32(m->f, 0x00020000); tag(m->f, "qt  ");
  be32(m->f, 8); tag(m->f, "wide"); m->mdat_size = ftell(m->f); be32(m->f, 0); tag(m->f, "mdat");
  return m;
}

int dm_mov_video(dm_mov *m, const dm_fb *fb) {
  if (!m || !fb || fb->w != m->w || fb->h != m->h || m->nv == UINT32_MAX) return -1;
  const size_t stride = (size_t)m->w * 3; int start = 0, end = m->h;
  const uint8_t *old = m->nv ? m->prev : NULL;
  if (old) { while (start < m->h && !memcmp(fb->rgb + (size_t)start * stride, old + (size_t)start * stride, stride)) start++; while (end > start && !memcmp(fb->rgb + (size_t)(end - 1) * stride, old + (size_t)(end - 1) * stride, stride)) end--; }
  size_t n = 4; if (n + 10 > m->pkt_cap) return -1;
  if ((start == 0 && end == m->h) || start == m->h) { m->pkt[n++] = 0; m->pkt[n++] = 0; }
  else { m->pkt[n++] = 0; m->pkt[n++] = 8; m->pkt[n++] = start >> 8; m->pkt[n++] = start; m->pkt[n++] = 0; m->pkt[n++] = 0; m->pkt[n++] = (end - start) >> 8; m->pkt[n++] = end - start; m->pkt[n++] = 0; m->pkt[n++] = 0; }
  for (int y = start; y < end; y++) if (put_line(m, fb->rgb + (size_t)y * stride, old ? old + (size_t)y * stride : NULL, &n)) return -1;
  if (put(m, 0, &n)) return -1;
  m->pkt[0] = n >> 24; m->pkt[1] = n >> 16; m->pkt[2] = n >> 8; m->pkt[3] = n;
  if (grow_pair(&m->voff, &m->vsize, &m->cv, m->nv + 1)) return -1;
  m->voff[m->nv] = (uint32_t)ftell(m->f); m->vsize[m->nv] = (uint32_t)n;
  if (fwrite(m->pkt, 1, n, m->f) != n) return -1;
  memcpy(m->prev, fb->rgb, stride * m->h); m->nv++; return 0;
}

int dm_mov_audio(dm_mov *m, const float *lr, uint32_t frames) {
  if (!m || !lr || !frames || frames > UINT32_MAX - m->audio_frames || grow_pair(&m->aoff, &m->acount, &m->ca, m->na + 1)) return -1;
  m->aoff[m->na] = (uint32_t)ftell(m->f); m->acount[m->na] = frames;
  for (uint32_t i = 0; i < frames * 2; i++) { float x = lr[i]; if (x > 1) x = 1; if (x < -1) x = -1; int v = (int)(x * 32767.0f); uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; if (fwrite(b, 1, 2, m->f) != 2) return -1; }
  m->audio_frames += frames; m->na++; return 0;
}

static void zeros(FILE *f, int n) { while (n--) fputc(0, f); }
static void write_stsd(dm_mov *m, int video) {
  FILE *f = m->f; long p = atom(f, "stsd"); be32(f, 0); be32(f, 1);
  if (video) {
    be32(f, 86); tag(f, "rle "); zeros(f, 6); be16(f, 1); be16(f, 0); be16(f, 0); be32(f, 0); be32(f, 0); be32(f, 0); be16(f, m->w); be16(f, m->h); be32(f, 0x00480000); be32(f, 0x00480000); be32(f, 0); be16(f, 1); zeros(f, 32); be16(f, 24); be16(f, 0xffff);
  } else {
    be32(f, 36); tag(f, "sowt"); zeros(f, 6); be16(f, 1); be16(f, 0); be16(f, 0); be32(f, 0); be16(f, 2); be16(f, 16); be16(f, 0); be16(f, 0); be32(f, (uint32_t)m->sr << 16);
  }
  end_atom(f, p);
}
static void write_stts(dm_mov *m, int video) { FILE *f = m->f; long p = atom(f, "stts"); be32(f, 0); be32(f, 1); be32(f, video ? (uint32_t)m->nv : m->audio_frames); be32(f, 1); end_atom(f, p); }
static void write_stsc(dm_mov *m, int video) {
  FILE *f = m->f; long p = atom(f, "stsc"); be32(f, 0);
  uint32_t n = video || m->na < 2 || m->acount[0] == m->acount[m->na - 1] ? 1 : 2; be32(f, n);
  be32(f, 1); be32(f, video ? 1 : m->acount[0]); be32(f, 1);
  if (n == 2) { be32(f, (uint32_t)m->na); be32(f, m->acount[m->na - 1]); be32(f, 1); }
  end_atom(f, p);
}
static void write_stsz(dm_mov *m, int video) {
  FILE *f = m->f; long p = atom(f, "stsz"); be32(f, 0); be32(f, video ? 0 : 4); be32(f, video ? (uint32_t)m->nv : m->audio_frames);
  if (video) for (size_t i = 0; i < m->nv; i++) be32(f, m->vsize[i]); end_atom(f, p);
}
static void write_stco(dm_mov *m, int video) { FILE *f = m->f; size_t n = video ? m->nv : m->na; uint32_t *o = video ? m->voff : m->aoff; long p = atom(f, "stco"); be32(f, 0); be32(f, (uint32_t)n); for (size_t i = 0; i < n; i++) be32(f, o[i]); end_atom(f, p); }
static void write_stbl(dm_mov *m, int video) {
  FILE *f = m->f; long p = atom(f, "stbl"); write_stsd(m, video); write_stts(m, video); write_stsc(m, video); write_stsz(m, video); write_stco(m, video);
  if (video) { long q = atom(f, "stss"); be32(f, 0); be32(f, 1); be32(f, 1); end_atom(f, q); } end_atom(f, p);
}
static void write_dinf(FILE *f) { long p = atom(f, "dinf"), q = atom(f, "dref"); be32(f, 0); be32(f, 1); be32(f, 12); tag(f, "url "); be32(f, 1); end_atom(f, q); end_atom(f, p); }
static void write_trak(dm_mov *m, int video, uint32_t id) {
  FILE *f = m->f; uint32_t dur = video ? (uint32_t)m->nv : m->audio_frames; long p = atom(f, "trak"), q = atom(f, "tkhd");
  be32(f, 7); be32(f, 0); be32(f, 0); be32(f, id); be32(f, 0); be32(f, video ? (uint32_t)m->nv : (uint32_t)((uint64_t)m->audio_frames * m->fps / m->sr)); be32(f, 0); be32(f, 0); be16(f, 0); be16(f, 0); be16(f, video ? 0 : 0x0100); be16(f, 0);
  be32(f, 0x00010000); be32(f, 0); be32(f, 0); be32(f, 0); be32(f, 0x00010000); be32(f, 0); be32(f, 0); be32(f, 0); be32(f, 0x40000000); be32(f, video ? (uint32_t)m->w << 16 : 0); be32(f, video ? (uint32_t)m->h << 16 : 0); end_atom(f, q);
  q = atom(f, "mdia"); long r = atom(f, "mdhd"); be32(f, 0); be32(f, 0); be32(f, 0); be32(f, video ? m->fps : m->sr); be32(f, dur); be16(f, 0x55c4); be16(f, 0); end_atom(f, r);
  r = atom(f, "hdlr"); be32(f, 0); be32(f, 0); tag(f, video ? "vide" : "soun"); tag(f, video ? "rle " : "sowt"); be32(f, 0); be32(f, 0); be32(f, 0); fputc(0, f); end_atom(f, r);
  r = atom(f, "minf"); long s = atom(f, video ? "vmhd" : "smhd"); be32(f, video ? 1 : 0); if (video) { be16(f, 0); be16(f, 0); be16(f, 0); be16(f, 0); } else { be16(f, 0); be16(f, 0); } end_atom(f, s); write_dinf(f); write_stbl(m, video); end_atom(f, r); end_atom(f, q); end_atom(f, p);
}

int dm_mov_close(dm_mov *m) {
  if (!m) return -1; FILE *f = m->f; long end_mdat = ftell(f), here = end_mdat;
  fseek(f, m->mdat_size, SEEK_SET); be32(f, (uint32_t)(end_mdat - m->mdat_size)); fseek(f, here, SEEK_SET);
  long moov = atom(f, "moov"), mvhd = atom(f, "mvhd");
  be32(f, 0); be32(f, 0); be32(f, 0); be32(f, m->fps); be32(f, (uint32_t)m->nv); be32(f, 0x00010000); be16(f, 0x0100); be16(f, 0); be32(f, 0); be32(f, 0);
  be32(f, 0x00010000); be32(f, 0); be32(f, 0); be32(f, 0); be32(f, 0x00010000); be32(f, 0); be32(f, 0); be32(f, 0); be32(f, 0x40000000);
  for (int i = 0; i < 6; i++) be32(f, 0); be32(f, 3); end_atom(f, mvhd);
  write_trak(m, 1, 1); if (m->audio_frames) write_trak(m, 0, 2); end_atom(f, moov);
  int bad = fclose(f) ? -1 : 0;
  free(m->prev); free(m->pkt); free(m->voff); free(m->vsize); free(m->aoff); free(m->acount); free(m);
  return bad;
}
