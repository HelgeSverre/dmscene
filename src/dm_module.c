/* dm_module.c - the .dms byte-packed container.
 *
 *   "DMS" 0x01
 *   chunk*   := tag(u8) len(varint) payload
 *     'T' title      utf8
 *     'G' song       bpm speed rpb channels loop norder nuniform vol(u8/255) order[norder]
 *     'C' code       raw bytecode, shared by every program
 *     'S' scene      flags(bit0 frame, bit2 master, bit3 post) [frame] [master] [post]
 *                    npixel (namelen name prog)*  chanmask (prog per set bit)
 *                    prog := off u16 len u16 nlocals nstate ndelay nverb
 *     'I' inst       id namelen name prog flags
 *     'X' text       id namelen name len(u16) bytes
 *     'W' sample     id namelen name rate(u16) len(u32) int8 pcm
 *     'V' envelope   id namelen name npts sustain (ms u16, level u8)*
 *     'E' timeline   nnames (namelen name)* nev(u16) (order row id f32)*
 *     'P' pattern    id rows(u16) channels cells*  (cell := mask fields | 0x80|skip)
 *     'L' palette    id namelen name nkeys (pos r g b)*   all u8
 */
#include "dm.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static char g_err[512];
const char *dm_error(void) { return g_err; }
void dm_set_error(const char *fmt, ...) {
  va_list ap; va_start(ap, fmt); vsnprintf(g_err, sizeof g_err, fmt, ap); va_end(ap);
}

void dm_module_free(dm_module *m) {
  for (int i = 0; i < DM_MAX_PATTERNS; i++) { free(m->pat[i].cells); m->pat[i].cells = NULL; }
  for (int i = 0; i < DM_MAX_TEXTS; i++) { free(m->text[i].s); m->text[i].s = NULL; }
  for (int i = 0; i < DM_MAX_SAMPLES; i++) { free(m->smp[i].pcm); m->smp[i].pcm = NULL; }
}

void dm_palette_build(dm_palette *p) {
  /* keys sorted by pos; LUT = piecewise linear */
  for (int i = 0; i < 256; i++) {
    float t = i / 255.0f;
    if (p->nkeys == 0) { p->lut[i][0] = p->lut[i][1] = p->lut[i][2] = t; continue; }
    int k = 0;
    while (k + 1 < p->nkeys && p->keys[k + 1][0] <= t) k++;
    float *a = p->keys[k], *b = p->keys[k + 1 < p->nkeys ? k + 1 : k];
    float span = b[0] - a[0];
    float f = span > 1e-6f ? (t - a[0]) / span : 0.0f;
    if (f < 0) f = 0; if (f > 1) f = 1;
    for (int c = 0; c < 3; c++) p->lut[i][c] = a[1 + c] + (b[1 + c] - a[1 + c]) * f;
  }
}

int dm_module_verify(dm_module *m) {
  if (m->channels < 1 || m->channels > DM_MAX_CHANNELS) { dm_set_error("bad channel count %d", m->channels); return -1; }
  if (m->norder < 1) { dm_set_error("empty order list"); return -1; }
  for (int i = 0; i < m->norder; i++) {
    if (m->order[i] >= DM_MAX_PATTERNS || !m->pat[m->order[i]].cells) { dm_set_error("order %d refers to missing pattern %d", i, m->order[i]); return -1; }
  }
  if (m->loop >= m->norder) m->loop = 0;
  for (int i = 0; i < DM_MAX_PATTERNS; i++) {
    const dm_pattern *p = &m->pat[i];
    if (!p->cells) continue;
    for (int k = 0; k < p->rows * p->channels; k++)
      if (p->cells[k].inst > m->ninst) { dm_set_error("pattern %d uses instrument %d but only %d defined", i, p->cells[k].inst, m->ninst); return -1; }
  }
  for (int i = 0; i < m->npal; i++) dm_palette_build(&m->pal[i]);
  m->cumrows[0] = 0;
  for (int i = 0; i < m->norder; i++) m->cumrows[i + 1] = m->cumrows[i] + m->pat[m->order[i]].rows;
  if (m->scale < 1) m->scale = 1;
  for (int i = 0; i < m->ninst; i++) if (dm_prog_verify(m, &m->inst[i].prog, m->inst[i].name)) return -1;
  if (m->has_frame && dm_prog_verify(m, &m->frame, "frame")) return -1;
  for (int i = 0; i < m->npixel; i++) if (dm_prog_verify(m, &m->pixel[i], m->pixelname[i])) return -1;
  if (m->has_master && dm_prog_verify(m, &m->master, "master")) return -1;
  if (m->has_post && dm_prog_verify(m, &m->post, "post")) return -1;
  for (int c = 0; c < DM_MAX_CHANNELS; c++) if ((m->has_chanfx >> c) & 1) { char nm[16]; snprintf(nm, sizeof nm, "chan %d", c); if (dm_prog_verify(m, &m->chanfx[c], nm)) return -1; }
  return 0;
}

/* ------------------------------------------------------------ writer */
typedef struct { uint8_t *d; size_t n, cap; } buf;
static void bput(buf *b, const void *p, size_t n) {
  if (b->n + n > b->cap) { b->cap = (b->cap + n) * 2 + 64; b->d = realloc(b->d, b->cap); }
  memcpy(b->d + b->n, p, n); b->n += n;
}
static void b8(buf *b, unsigned v) { uint8_t x = (uint8_t)v; bput(b, &x, 1); }
static void b16(buf *b, unsigned v) { b8(b, v & 255); b8(b, v >> 8); }
static void bvar(buf *b, size_t v) { while (v >= 0x80) { b8(b, (v & 0x7f) | 0x80); v >>= 7; } b8(b, v); }
static void bprog(buf *b, const dm_prog *p) { b16(b, p->off); b16(b, p->len); b8(b, p->nlocals); b8(b, p->nstate); b8(b, p->ndelay); b8(b, p->nverb); }
static void bchunk(buf *out, char tag, const buf *payload) { b8(out, tag); bvar(out, payload->n); bput(out, payload->d, payload->n); }

static void pack_pattern(buf *b, const dm_pattern *p) {
  int total = p->rows * p->channels, skip = 0;
  for (int i = 0; i < total; i++) {
    const dm_cell *c = &p->cells[i];
    uint8_t mask = (c->note ? 1 : 0) | (c->inst ? 2 : 0) | (c->vol ? 4 : 0) | (c->fx ? 8 : 0) | (c->fxp ? 16 : 0);
    if (!mask) { skip++; if (skip == 128) { b8(b, 0x80 | 127); skip = 0; } continue; }
    if (skip) { b8(b, 0x80 | (skip - 1)); skip = 0; }
    b8(b, mask);
    if (c->note) b8(b, c->note);
    if (c->inst) b8(b, c->inst);
    if (c->vol) b8(b, c->vol);
    if (c->fx) b8(b, c->fx);
    if (c->fxp) b8(b, c->fxp);
  }
  if (skip) b8(b, 0x80 | (skip - 1));
}

int dm_module_write(const dm_module *m, const char *path) {
  buf out = {0}, c = {0};
  bput(&out, "DMS", 3); b8(&out, DM_VERSION);
  if (m->title[0]) { c.n = 0; bput(&c, m->title, strlen(m->title)); bchunk(&out, 'T', &c); }
  c.n = 0; b8(&c, m->bpm); b8(&c, m->speed); b8(&c, m->rpb); b8(&c, m->channels); b8(&c, m->loop);
  b8(&c, m->norder); b8(&c, m->nuniform); b8(&c, (unsigned)(m->volume * 255.0f + 0.5f)); bput(&c, m->order, m->norder);
  b8(&c, m->scale ? m->scale : 1); b8(&c, m->dither);
  for (int i = 0; i < DM_MAX_CHANNELS; i++) b8(&c, (unsigned)(m->pan[i] * 255.0f + 0.5f));
  bchunk(&out, 'G', &c);
  c.n = 0; bput(&c, m->code, m->codelen); bchunk(&out, 'C', &c);
  c.n = 0; b8(&c, (m->has_frame ? 1 : 0) | (m->has_master ? 4 : 0) | (m->has_post ? 8 : 0));
  if (m->has_frame) bprog(&c, &m->frame);
  if (m->has_master) bprog(&c, &m->master);
  if (m->has_post) bprog(&c, &m->post);
  b8(&c, m->npixel);
  for (int i = 0; i < m->npixel; i++) { b8(&c, (unsigned)strlen(m->pixelname[i])); bput(&c, m->pixelname[i], strlen(m->pixelname[i])); bprog(&c, &m->pixel[i]); }
  b8(&c, m->has_chanfx);
  for (int i = 0; i < DM_MAX_CHANNELS; i++) if ((m->has_chanfx >> i) & 1) bprog(&c, &m->chanfx[i]);
  bchunk(&out, 'S', &c);
  for (int i = 0; i < m->ninst; i++) {
    c.n = 0; b8(&c, i + 1); b8(&c, (unsigned)strlen(m->inst[i].name)); bput(&c, m->inst[i].name, strlen(m->inst[i].name));
    bprog(&c, &m->inst[i].prog); b8(&c, m->inst[i].flags); bchunk(&out, 'I', &c);
  }
  for (int i = 0; i < m->ntext; i++) {
    const dm_text *t = &m->text[i];
    c.n = 0; b8(&c, i); b8(&c, (unsigned)strlen(t->name)); bput(&c, t->name, strlen(t->name)); b16(&c, t->len); bput(&c, t->s, t->len); bchunk(&out, 'X', &c);
  }
  for (int i = 0; i < m->nsmp; i++) {
    const dm_sample *sm = &m->smp[i];
    c.n = 0; b8(&c, i); b8(&c, (unsigned)strlen(sm->name)); bput(&c, sm->name, strlen(sm->name)); b16(&c, sm->rate);
    b16(&c, sm->len & 0xffff); b16(&c, sm->len >> 16); bput(&c, sm->pcm, sm->len); bchunk(&out, 'W', &c);
  }
  for (int i = 0; i < m->nvenv; i++) {
    const dm_venv *v = &m->venv[i];
    c.n = 0; b8(&c, i); b8(&c, (unsigned)strlen(v->name)); bput(&c, v->name, strlen(v->name)); b8(&c, v->npts); b8(&c, v->sustain);
    for (int k = 0; k < v->npts; k++) { float ms = v->t[k] * 1000.0f; b16(&c, (unsigned)(ms < 0 ? 0 : ms > 65535 ? 65535 : ms + 0.5f)); float lv = v->v[k]; b8(&c, (unsigned)((lv < 0 ? 0 : lv > 1 ? 1 : lv) * 255.0f + 0.5f)); }
    bchunk(&out, 'V', &c);
  }
  if (m->nevname) {
    c.n = 0; b8(&c, m->nevname);
    for (int i = 0; i < m->nevname; i++) { b8(&c, (unsigned)strlen(m->evname[i])); bput(&c, m->evname[i], strlen(m->evname[i])); }
    b16(&c, m->nev);
    for (int i = 0; i < m->nev; i++) { b8(&c, m->ev[i].order); b8(&c, m->ev[i].row); b8(&c, m->ev[i].id); bput(&c, &m->ev[i].value, 4); }
    bchunk(&out, 'E', &c);
  }
  for (int i = 0; i < DM_MAX_PATTERNS; i++) {
    if (!m->pat[i].cells) continue;
    c.n = 0; b8(&c, i); b16(&c, m->pat[i].rows); b8(&c, m->pat[i].channels); pack_pattern(&c, &m->pat[i]); bchunk(&out, 'P', &c);
  }
  for (int i = 0; i < m->npal; i++) {
    const dm_palette *p = &m->pal[i];
    c.n = 0; b8(&c, i); b8(&c, (unsigned)strlen(p->name)); bput(&c, p->name, strlen(p->name)); b8(&c, p->nkeys);
    for (int k = 0; k < p->nkeys; k++) for (int j = 0; j < 4; j++) {
      float v = p->keys[k][j]; if (v < 0) v = 0; if (v > 1) v = 1; b8(&c, (unsigned)(v * 255.0f + 0.5f));
    }
    bchunk(&out, 'L', &c);
  }
  FILE *f = fopen(path, "wb");
  int rc = -1;
  if (f) { rc = fwrite(out.d, 1, out.n, f) == out.n ? 0 : -1; fclose(f); }
  if (rc) dm_set_error("cannot write %s", path);
  free(out.d); free(c.d);
  return rc;
}

/* ------------------------------------------------------------ reader */
typedef struct { const uint8_t *p, *end; int err; } rd;
static unsigned r8(rd *r) { if (r->p >= r->end) { r->err = 1; return 0; } return *r->p++; }
static unsigned r16(rd *r) { unsigned a = r8(r); return a | (r8(r) << 8); }
static size_t rvar(rd *r) { size_t v = 0; int sh = 0; for (;;) { unsigned b = r8(r); v |= (size_t)(b & 0x7f) << sh; if (!(b & 0x80) || r->err) break; sh += 7; } return v; }
static void rprog(rd *r, dm_prog *p) { p->off = r16(r); p->len = r16(r); p->nlocals = r8(r); p->nstate = r8(r); p->ndelay = r8(r); p->nverb = r8(r); }
static void rname(rd *r, char *dst, size_t cap) {
  unsigned n = r8(r); size_t i;
  for (i = 0; i < n; i++) { unsigned c = r8(r); if (i + 1 < cap) dst[i] = (char)c; }
  dst[n < cap - 1 ? n : cap - 1] = 0;
}

int dm_module_load(dm_module *m, const uint8_t *data, size_t len) {
  memset(m, 0, sizeof *m);
  m->volume = 1.0f; m->rpb = 4; m->scale = 1;
  for (int i = 0; i < DM_MAX_CHANNELS; i++) m->pan[i] = 0.5f;
  if (len < 4 || memcmp(data, "DMS", 3) != 0) { dm_set_error("not a DMS file"); return -1; }
  if (data[3] != DM_VERSION) { dm_set_error("DMS version %d, this build reads version %d: recompile the .dmt", data[3], DM_VERSION); return -1; }
  rd r = { data + 4, data + len, 0 };
  while (r.p < r.end && !r.err) {
    unsigned tag = r8(&r);
    size_t n = rvar(&r);
    if (r.err || r.p + n > r.end) { dm_set_error("truncated chunk '%c'", tag); return -1; }
    rd c = { r.p, r.p + n, 0 };
    r.p += n;
    switch (tag) {
      case 'T': { size_t k = n < sizeof m->title - 1 ? n : sizeof m->title - 1; memcpy(m->title, c.p, k); m->title[k] = 0; break; }
      case 'G':
        m->bpm = r8(&c); m->speed = r8(&c); m->rpb = r8(&c); m->channels = r8(&c); m->loop = r8(&c);
        m->norder = r8(&c); m->nuniform = r8(&c); m->volume = r8(&c) / 255.0f;
        for (int i = 0; i < m->norder; i++) m->order[i] = r8(&c);
        if (c.p < c.end) { m->scale = r8(&c); m->dither = r8(&c); for (int i = 0; i < DM_MAX_CHANNELS; i++) m->pan[i] = r8(&c) / 255.0f; }
        break;
      case 'C':
        if (n > DM_MAX_CODE) { dm_set_error("code too large"); return -1; }
        memcpy(m->code, c.p, n); m->codelen = (uint32_t)n; break;
      case 'S': { unsigned fl = r8(&c);
        if (fl & 1) { rprog(&c, &m->frame); m->has_frame = 1; }
        if (fl & 4) { rprog(&c, &m->master); m->has_master = 1; }
        if (fl & 8) { rprog(&c, &m->post); m->has_post = 1; }
        unsigned np = r8(&c);
        if (np > DM_MAX_PIXEL) { dm_set_error("too many pixel programs"); return -1; }
        for (unsigned i = 0; i < np; i++) { rname(&c, m->pixelname[i], DM_NAME_LEN); rprog(&c, &m->pixel[i]); }
        m->npixel = np;
        m->has_chanfx = r8(&c);
        for (int i = 0; i < DM_MAX_CHANNELS; i++) if ((m->has_chanfx >> i) & 1) rprog(&c, &m->chanfx[i]);
        break; }
      case 'I': { unsigned id = r8(&c);
        if (id < 1 || id > DM_MAX_INST) { dm_set_error("bad instrument id %u", id); return -1; }
        rname(&c, m->inst[id - 1].name, DM_NAME_LEN); rprog(&c, &m->inst[id - 1].prog);
        m->inst[id - 1].flags = c.p < c.end ? r8(&c) : 0;
        if ((int)id > m->ninst) m->ninst = id; break; }
      case 'X': { unsigned id = r8(&c);
        if (id >= DM_MAX_TEXTS) { dm_set_error("bad text id"); return -1; }
        dm_text *t = &m->text[id]; rname(&c, t->name, DM_NAME_LEN); unsigned len = r16(&c);
        if (c.p + len > c.end) { dm_set_error("truncated text"); return -1; }
        free(t->s); t->s = malloc(len + 1); memcpy(t->s, c.p, len); t->s[len] = 0; t->len = len; c.p += len;
        if ((int)id + 1 > m->ntext) m->ntext = id + 1; break; }
      case 'W': { unsigned id = r8(&c);
        if (id >= DM_MAX_SAMPLES) { dm_set_error("bad sample id"); return -1; }
        dm_sample *sm = &m->smp[id]; rname(&c, sm->name, DM_NAME_LEN); sm->rate = r16(&c);
        uint32_t len = r16(&c); len |= (uint32_t)r16(&c) << 16;
        if (c.p + len > c.end || len < 2) { dm_set_error("truncated sample"); return -1; }
        free(sm->pcm); sm->pcm = malloc(len); memcpy(sm->pcm, c.p, len); sm->len = len; c.p += len;
        if ((int)id + 1 > m->nsmp) m->nsmp = id + 1; break; }
      case 'V': { unsigned id = r8(&c);
        if (id >= DM_MAX_VENVS) { dm_set_error("bad envelope id"); return -1; }
        dm_venv *v = &m->venv[id]; rname(&c, v->name, DM_NAME_LEN); v->npts = r8(&c); v->sustain = r8(&c);
        if (v->npts > DM_MAX_VENV_PTS) { dm_set_error("too many envelope points"); return -1; }
        for (int k = 0; k < v->npts; k++) { v->t[k] = r16(&c) / 1000.0f; v->v[k] = r8(&c) / 255.0f; }
        if ((int)id + 1 > m->nvenv) m->nvenv = id + 1; break; }
      case 'E': { unsigned nn = r8(&c);
        if (nn > DM_MAX_EVNAMES) { dm_set_error("too many event names"); return -1; }
        for (unsigned i = 0; i < nn; i++) rname(&c, m->evname[i], DM_NAME_LEN);
        m->nevname = nn;
        unsigned ne = r16(&c);
        if (ne > DM_MAX_EVENTS) { dm_set_error("too many events"); return -1; }
        for (unsigned i = 0; i < ne; i++) { dm_event *e = &m->ev[i]; e->order = r8(&c); e->row = r8(&c); e->id = r8(&c);
          if (c.p + 4 > c.end) { c.err = 1; break; } memcpy(&e->value, c.p, 4); c.p += 4; }
        m->nev = ne; break; }
      case 'P': { unsigned id = r8(&c); unsigned rows = r16(&c), chn = r8(&c);
        if (id >= DM_MAX_PATTERNS || rows < 1 || rows > 256 || chn < 1 || chn > DM_MAX_CHANNELS) { dm_set_error("bad pattern header"); return -1; }
        dm_pattern *p = &m->pat[id];
        free(p->cells);
        p->rows = rows; p->channels = chn; p->cells = calloc(rows * chn, sizeof(dm_cell));
        int total = rows * chn, i = 0;
        while (i < total && !c.err && c.p < c.end) {
          unsigned mk = r8(&c);
          if (mk & 0x80) { i += (mk & 0x7f) + 1; continue; }
          dm_cell *cell = &p->cells[i++];
          if (mk & 1) cell->note = r8(&c);
          if (mk & 2) cell->inst = r8(&c);
          if (mk & 4) cell->vol = r8(&c);
          if (mk & 8) cell->fx = r8(&c);
          if (mk & 16) cell->fxp = r8(&c);
        }
        break; }
      case 'L': { unsigned id = r8(&c);
        if (id >= DM_MAX_PAL) { dm_set_error("bad palette id"); return -1; }
        dm_palette *p = &m->pal[id];
        rname(&c, p->name, DM_NAME_LEN); p->nkeys = r8(&c);
        if (p->nkeys > DM_MAX_PALKEYS) { dm_set_error("too many palette keys"); return -1; }
        for (int k = 0; k < p->nkeys; k++) for (int j = 0; j < 4; j++) p->keys[k][j] = r8(&c) / 255.0f;
        if ((int)id + 1 > m->npal) m->npal = id + 1; break; }
      default: break;   /* unknown chunks are skipped: forward compatible */
    }
    if (c.err) { dm_set_error("corrupt chunk '%c'", tag); return -1; }
  }
  return dm_module_verify(m);
}

int dm_module_read(dm_module *m, const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) { dm_set_error("cannot open %s", path); return -1; }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  if (n < 0) { fclose(f); dm_set_error("cannot read %s", path); return -1; }
  uint8_t *d = malloc((size_t)n + 1);
  size_t got = fread(d, 1, (size_t)n, f); fclose(f);
  int rc = dm_module_load(m, d, got);
  free(d);
  return rc;
}
