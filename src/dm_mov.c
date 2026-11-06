/* Experimental dependency-free QuickTime Animation (qtrle) MOV writer. */
#include "dm_mov.h"
#include <stdlib.h>
#include <string.h>

struct dm_mov {
  FILE *f; int w, h, fps, sr; long mdat_size;
  uint8_t *prev, *pkt; size_t pkt_cap;
  uint32_t *voff, *vsize, *aoff, *acount;
  size_t nv, cv, na, ca; uint32_t audio_frames, keyint; int failed;
};

static void be16(FILE *f, uint16_t v) { uint8_t b[2] = {v >> 8, v}; fwrite(b, 1, 2, f); }
static void be32(FILE *f, uint32_t v) { uint8_t b[4] = {v >> 24, v >> 16, v >> 8, v}; fwrite(b, 1, 4, f); }
static void tag(FILE *f, const char *s) { fwrite(s, 1, 4, f); }
static long atom(FILE *f, const char *type) { long p = ftell(f); be32(f, 0); tag(f, type); return p; }
static void end_atom(dm_mov *m, long p) {
  FILE *f = m->f; long e = ftell(f), here = e;
  if (e < p || fseek(f, p, SEEK_SET) || ferror(f)) { m->failed = 1; return; }
  be32(f, (uint32_t)(e - p));
  if (fseek(f, here, SEEK_SET) || ferror(f)) m->failed = 1;
}
static int grow_pair(uint32_t **a, uint32_t **b, size_t *cap, size_t n) {
  if (n <= *cap) return 0;
  size_t c = *cap ? *cap : 256; while (c < n) { if (c > SIZE_MAX / 2) return -1; c *= 2; }
  if (c > SIZE_MAX / sizeof(uint32_t)) return -1;
  uint32_t *x = malloc(c * sizeof *x), *y = malloc(c * sizeof *y);
  if (!x || !y) { free(x); free(y); return -1; }
  if (*cap) { memcpy(x, *a, *cap * sizeof *x); memcpy(y, *b, *cap * sizeof *y); }
  free(*a); free(*b); *a = x; *b = y; *cap = c; return 0;
}
static int put(dm_mov *m, uint8_t v, size_t *n) { if (*n >= m->pkt_cap) return -1; m->pkt[(*n)++] = v; return 0; }
static int rgb_eq(const uint8_t *a, const uint8_t *b) { return !memcmp(a, b, 3); }
static int room(dm_mov *m, uint64_t bytes) {
  long p = ftell(m->f);
  if (p < 0 || (uint64_t)p > UINT32_MAX || bytes > UINT32_MAX - (uint64_t)p) { m->failed = 1; return -1; }
  return 0;
}

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
  if (!path || w < 1 || h < 1 || w > 65535 || h > 65535 || fps < 1 || sr < 1 || sr > 65535) return NULL;
  uint64_t pixels = (uint64_t)w * (uint64_t)h * 3, packet = pixels * 4 + (uint64_t)h * 4 + 64;
  if (pixels > SIZE_MAX || packet > SIZE_MAX) return NULL;
  dm_mov *m = calloc(1, sizeof *m); if (!m) return NULL;
  m->f = fopen(path, "wb"); m->w = w; m->h = h; m->fps = fps; m->sr = sr; m->keyint = (uint32_t)fps * 2;
  m->prev = malloc((size_t)pixels); m->pkt_cap = (size_t)packet; m->pkt = malloc(m->pkt_cap);
  if (!m->f || !m->prev || !m->pkt) { if (m->f) fclose(m->f); free(m->prev); free(m->pkt); free(m); return NULL; }
  be32(m->f, 20); tag(m->f, "ftyp"); tag(m->f, "qt  "); be32(m->f, 0x00020000); tag(m->f, "qt  ");
  be32(m->f, 8); tag(m->f, "wide"); m->mdat_size = ftell(m->f); be32(m->f, 0); tag(m->f, "mdat");
  if (m->mdat_size < 0 || ferror(m->f)) { fclose(m->f); free(m->prev); free(m->pkt); free(m); return NULL; }
  return m;
}

int dm_mov_video(dm_mov *m, const dm_fb *fb) {
  if (!m || !fb || !fb->rgb || m->failed || fb->w != m->w || fb->h != m->h || m->nv == UINT32_MAX) return -1;
  const size_t stride = (size_t)m->w * 3; int start = 0, end = m->h;
  const uint8_t *old = m->nv && m->nv % m->keyint ? m->prev : NULL;
  if (old) { while (start < m->h && !memcmp(fb->rgb + (size_t)start * stride, old + (size_t)start * stride, stride)) start++; while (end > start && !memcmp(fb->rgb + (size_t)(end - 1) * stride, old + (size_t)(end - 1) * stride, stride)) end--; }
  size_t n = 4; if (n + 10 > m->pkt_cap) return -1;
  if ((start == 0 && end == m->h) || start == m->h) { m->pkt[n++] = 0; m->pkt[n++] = 0; }
  else { m->pkt[n++] = 0; m->pkt[n++] = 8; m->pkt[n++] = start >> 8; m->pkt[n++] = start; m->pkt[n++] = 0; m->pkt[n++] = 0; m->pkt[n++] = (end - start) >> 8; m->pkt[n++] = end - start; m->pkt[n++] = 0; m->pkt[n++] = 0; }
  for (int y = start; y < end; y++) if (put_line(m, fb->rgb + (size_t)y * stride, old ? old + (size_t)y * stride : NULL, &n)) return -1;
  if (put(m, 0, &n) || n > UINT32_MAX || room(m, n) || grow_pair(&m->voff, &m->vsize, &m->cv, m->nv + 1)) return -1;
  m->pkt[0] = n >> 24; m->pkt[1] = n >> 16; m->pkt[2] = n >> 8; m->pkt[3] = n;
  long off = ftell(m->f); if (off < 0) { m->failed = 1; return -1; }
  m->voff[m->nv] = (uint32_t)off; m->vsize[m->nv] = (uint32_t)n;
  if (fwrite(m->pkt, 1, n, m->f) != n || ferror(m->f)) { m->failed = 1; return -1; }
  memcpy(m->prev, fb->rgb, stride * m->h); m->nv++; return 0;
}

int dm_mov_audio(dm_mov *m, const float *lr, uint32_t frames) {
  uint8_t pcm[8192];
  if (!m || !lr || !frames || m->failed || frames > UINT32_MAX - m->audio_frames || frames > UINT32_MAX / 4 || room(m, (uint64_t)frames * 4) || grow_pair(&m->aoff, &m->acount, &m->ca, m->na + 1)) return -1;
  long off = ftell(m->f); if (off < 0) { m->failed = 1; return -1; }
  m->aoff[m->na] = (uint32_t)off; m->acount[m->na] = frames;
  for (uint32_t done = 0; done < frames;) {
    uint32_t n = frames - done > 2048 ? 2048 : frames - done;
    for (uint32_t i = 0; i < n * 2; i++) { float x = lr[(size_t)done * 2 + i]; if (x != x) x = 0; if (x > 1) x = 1; if (x < -1) x = -1; int v = (int)(x * 32767.0f); pcm[2 * i] = (uint8_t)v; pcm[2 * i + 1] = (uint8_t)(v >> 8); }
    if (fwrite(pcm, 1, (size_t)n * 4, m->f) != (size_t)n * 4) { m->failed = 1; return -1; }
    done += n;
  }
  if (ferror(m->f)) { m->failed = 1; return -1; }
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
  end_atom(m, p);
}
static void write_stts(dm_mov *m, int video) { FILE *f = m->f; long p = atom(f, "stts"); be32(f, 0); be32(f, 1); be32(f, video ? (uint32_t)m->nv : m->audio_frames); be32(f, 1); end_atom(m, p); }
static void write_stsc(dm_mov *m, int video) {
  FILE *f = m->f; long p = atom(f, "stsc"); be32(f, 0);
  if (video) { be32(f, 1); be32(f, 1); be32(f, 1); be32(f, 1); }
  else {
    uint32_t runs = 1; for (size_t i = 1; i < m->na; i++) if (m->acount[i] != m->acount[i - 1]) runs++;
    be32(f, runs); be32(f, 1); be32(f, m->acount[0]); be32(f, 1);
    for (size_t i = 1; i < m->na; i++) if (m->acount[i] != m->acount[i - 1]) { be32(f, (uint32_t)i + 1); be32(f, m->acount[i]); be32(f, 1); }
  }
  end_atom(m, p);
}
static void write_stsz(dm_mov *m, int video) {
  FILE *f = m->f; long p = atom(f, "stsz"); be32(f, 0); be32(f, video ? 0 : 4); be32(f, video ? (uint32_t)m->nv : m->audio_frames);
  if (video) for (size_t i = 0; i < m->nv; i++) be32(f, m->vsize[i]); end_atom(m, p);
}
static void write_stco(dm_mov *m, int video) { FILE *f = m->f; size_t n = video ? m->nv : m->na; uint32_t *o = video ? m->voff : m->aoff; long p = atom(f, "stco"); be32(f, 0); be32(f, (uint32_t)n); for (size_t i = 0; i < n; i++) be32(f, o[i]); end_atom(m, p); }
static void write_stbl(dm_mov *m, int video) {
  FILE *f = m->f; long p = atom(f, "stbl"); write_stsd(m, video); write_stts(m, video); write_stsc(m, video); write_stsz(m, video); write_stco(m, video);
  if (video) { long q = atom(f, "stss"); uint32_t keys = (uint32_t)(((uint64_t)m->nv + m->keyint - 1) / m->keyint); be32(f, 0); be32(f, keys); for (uint32_t i = 0; i < keys; i++) be32(f, 1 + i * m->keyint); end_atom(m, q); } end_atom(m, p);
}
static void write_dinf(dm_mov *m) { FILE *f = m->f; long p = atom(f, "dinf"), q = atom(f, "dref"); be32(f, 0); be32(f, 1); be32(f, 12); tag(f, "url "); be32(f, 1); end_atom(m, q); end_atom(m, p); }
static void write_trak(dm_mov *m, int video, uint32_t id) {
  FILE *f = m->f; uint32_t dur = video ? (uint32_t)m->nv : m->audio_frames; long p = atom(f, "trak"), q = atom(f, "tkhd");
  be32(f, 7); be32(f, 0); be32(f, 0); be32(f, id); be32(f, 0); be32(f, video ? (uint32_t)m->nv : (uint32_t)((uint64_t)m->audio_frames * m->fps / m->sr)); be32(f, 0); be32(f, 0); be16(f, 0); be16(f, 0); be16(f, video ? 0 : 0x0100); be16(f, 0);
  be32(f, 0x00010000); be32(f, 0); be32(f, 0); be32(f, 0); be32(f, 0x00010000); be32(f, 0); be32(f, 0); be32(f, 0); be32(f, 0x40000000); be32(f, video ? (uint32_t)m->w << 16 : 0); be32(f, video ? (uint32_t)m->h << 16 : 0); end_atom(m, q);
  q = atom(f, "mdia"); long r = atom(f, "mdhd"); be32(f, 0); be32(f, 0); be32(f, 0); be32(f, video ? m->fps : m->sr); be32(f, dur); be16(f, 0x55c4); be16(f, 0); end_atom(m, r);
  r = atom(f, "hdlr"); be32(f, 0); be32(f, 0); tag(f, video ? "vide" : "soun"); tag(f, video ? "rle " : "sowt"); be32(f, 0); be32(f, 0); be32(f, 0); fputc(0, f); end_atom(m, r);
  r = atom(f, "minf"); long s = atom(f, video ? "vmhd" : "smhd"); be32(f, video ? 1 : 0); if (video) { be16(f, 0); be16(f, 0); be16(f, 0); be16(f, 0); } else { be16(f, 0); be16(f, 0); } end_atom(m, s); write_dinf(m); write_stbl(m, video); end_atom(m, r); end_atom(m, q); end_atom(m, p);
}

int dm_mov_close(dm_mov *m) {
  if (!m) return -1;
  FILE *f = m->f; long end_mdat = ftell(f);
  uint64_t metadata = 2048 + (uint64_t)m->nv * 12 + (uint64_t)m->na * 16;
  if (!m->nv || end_mdat < 0 || (uint64_t)end_mdat > UINT32_MAX || metadata > UINT32_MAX - (uint64_t)end_mdat) m->failed = 1;
  if (!m->failed && (fseek(f, m->mdat_size, SEEK_SET) || ferror(f))) m->failed = 1;
  if (!m->failed) be32(f, (uint32_t)(end_mdat - m->mdat_size));
  if (!m->failed && (fseek(f, end_mdat, SEEK_SET) || ferror(f))) m->failed = 1;
  if (!m->failed) {
    long moov = atom(f, "moov"), mvhd = atom(f, "mvhd");
    be32(f, 0); be32(f, 0); be32(f, 0); be32(f, m->fps); be32(f, (uint32_t)m->nv); be32(f, 0x00010000); be16(f, 0x0100); be16(f, 0); be32(f, 0); be32(f, 0);
    be32(f, 0x00010000); be32(f, 0); be32(f, 0); be32(f, 0); be32(f, 0x00010000); be32(f, 0); be32(f, 0); be32(f, 0); be32(f, 0x40000000);
    for (int i = 0; i < 6; i++) be32(f, 0); be32(f, m->audio_frames ? 3 : 2); end_atom(m, mvhd);
    write_trak(m, 1, 1); if (m->audio_frames) write_trak(m, 0, 2); end_atom(m, moov);
  }
  int bad = m->failed || ferror(f);
  if (fclose(f)) bad = 1;
  free(m->prev); free(m->pkt); free(m->voff); free(m->vsize); free(m->aoff); free(m->acount); free(m);
  return bad ? -1 : 0;
}
