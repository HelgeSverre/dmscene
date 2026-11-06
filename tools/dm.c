/* dm - the dmscene command line: compile, check, disasm, play, render, watch, new.
 * Inputs may be .dmt (compiled in memory) or .dms. */
#include "dm.h"
#include "dm_mov.h"
#include "dm_seek.h"
#include "dm_banner.h"
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

/* Binding changes only before workers start or after they have all stopped. */
static void bind_native(const dm_module *m) {
#ifdef DM_AOT
  extern int dm_aot_bind(const dm_module *);
  (void)dm_aot_bind(m); /* edited / different demos use the interpreter */
#else
  (void)m;
#endif
}

static char *slurp(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END)) { fclose(f); return NULL; }
  long n = ftell(f);
  if (n < 0 || fseek(f, 0, SEEK_SET)) { fclose(f); return NULL; }
  char *s = malloc((size_t)n + 1);
  if (!s) { fclose(f); return NULL; }
  size_t got = fread(s, 1, (size_t)n, f);
  int failed = ferror(f); fclose(f);
  if (failed || got != (size_t)n) { free(s); return NULL; }
  s[got] = 0;
  return s;
}
static int ends_with(const char *s, const char *suf) { size_t a = strlen(s), b = strlen(suf); return a >= b && !strcmp(s + a - b, suf); }
typedef struct { int exists; dev_t dev; ino_t ino; off_t size; struct timespec time; } file_stamp;
static file_stamp mtime(const char *path) {
  struct stat st; file_stamp v = {0};
  if (stat(path, &st)) return v;
  v.exists = 1; v.dev = st.st_dev; v.ino = st.st_ino; v.size = st.st_size;
#ifdef __APPLE__
  v.time = st.st_mtimespec;
#else
  v.time = st.st_mtim;
#endif
  return v;
}
static int same_stamp(file_stamp a, file_stamp b) {
  return a.exists == b.exists && a.dev == b.dev && a.ino == b.ino && a.size == b.size &&
    a.time.tv_sec == b.time.tv_sec && a.time.tv_nsec == b.time.tv_nsec;
}

/* load a .dmt or .dms into m; returns 0 on success */
static int load_any(const char *path, dm_module *m) {
  dm_module *next = calloc(1, sizeof *next);
  if (!next) { dm_set_error("out of memory loading %s", path); return -1; }
  int rc;
  if (ends_with(path, ".dms")) rc = dm_module_read(next, path);
  else {
    char *src = slurp(path);
    if (!src) { dm_set_error("cannot read %s", path); free(next); return -1; }
    rc = dm_compile(src, path, next);
    free(src);
  }
  if (rc) dm_module_free(next);
  else { dm_module_free(m); *m = *next; }
  free(next);
  return rc;
}

static void usage(int code) {
  fprintf(code ? stderr : stdout, DM_BANNER
    "usage: dm COMMAND file [options]\n\n"
    "  compile IN [-o OUT.dms] [--disasm]   text -> byte-packed module\n"
    "  check   IN [--json]                  compile and verify only\n"
    "  stats   IN [--json]                  module and per-program resource usage\n"
    "  disasm  IN                           print every program's bytecode\n"
    "  play    IN [play options]            terminal + audio (IN may be .dmt or .dms)\n"
    "  watch   IN [play options]            play, recompiling whenever the .dmt changes\n"
    "  render  IN --mp4 F | --avi F | --mov F | --png DIR | --wav F [--seconds S] [--size WxH] [--fps N] [--render-scale N]\n"
    "  new     NAME                         write NAME.dmt, a minimal starting point\n\n"
    "  --version                            engine and module format version\n\n"
    "play options:\n"
    "  --fps N       target frame rate (default 30)      --size WxH   force framebuffer size\n"
    "  --mute        no audio, wall-clock visuals         --start S    start S seconds in\n"
    "  --solo C      hear only channel C                  --threads N  pixel worker threads\n"
    "  --graphics auto|kitty|text   terminal images (default: probe, then text fallback)\n"
    "  --render-scale N            override demo pixel scale (1 = full detail)\n"
    "  --native                    require compiled shaders (play/watch try this automatically)\n"
    "  --interpreted               use the VM; skip native shader builds\n"
    "  --adaptive                  trade image detail for frame rate (off by default; --size stays fixed)\n"
    "  keys: q quit, left/right seek 5s, mouse wheel/trackpad smooth scrub, up/down speed, space pause\n"
    "        1..8 toggle channel mute, 0 unmute all, r reload (watch), Ctrl-Z suspend\n"
    "env: DM_THREADS, DM_AUDIO_CMD, DM_AUDIO_LATENCY (pipe backend)\n");
  exit(code);
}

/* ------------------------------------------------------------ compile / check / disasm */
static void disasm_all(const dm_module *m) {
  for (int i = 0; i < m->ninst; i++) { printf("inst %s  (locals %d, state %d%s)\n", m->inst[i].name, m->inst[i].prog.nlocals, m->inst[i].prog.nstate, m->inst[i].flags & DM_INST_NNA ? ", nna" : ""); dm_disasm(m, &m->inst[i].prog, stdout); }
  for (int c = 0; c < DM_MAX_CHANNELS; c++) if ((m->has_chanfx >> c) & 1) { printf("chan %d\n", c); dm_disasm(m, &m->chanfx[c], stdout); }
  if (m->has_master) { printf("master (delay lines %d, verbs %d)\n", m->master.ndelay, m->master.nverb); dm_disasm(m, &m->master, stdout); }
  if (m->has_frame) { printf("frame\n"); dm_disasm(m, &m->frame, stdout); }
  for (int i = 0; i < m->npixel; i++) { printf("pixel %s\n", m->pixelname[i]); dm_disasm(m, &m->pixel[i], stdout); }
  if (m->has_post) { printf("post\n"); dm_disasm(m, &m->post, stdout); }
}
static void stats(FILE *out, const char *in, const dm_module *m) {
  int npat = 0, cells = 0;
  for (int i = 0; i < DM_MAX_PATTERNS; i++) if (m->pat[i].cells) { npat++; cells += m->pat[i].rows * m->pat[i].channels; }
  fprintf(out, "%s: %d inst, %d pattern(s), %d cells, %d pixel program(s), %d palette(s), %d text(s), %d sample(s), %d texture(s), %d event(s), %u bytes of code\n",
          in, m->ninst, npat, cells, m->npixel, m->npal, m->ntext, m->nsmp, m->ntex, m->nev, m->codelen);
}

/* JSON is a separate stdout protocol: no banner or human diagnostics. Preserve
 * UTF-8; encode control characters and invalid filename bytes as escapes. */
static void json_string(FILE *out, const char *s) {
  if (!s) { fputs("null", out); return; }
  fputc('"', out);
  const unsigned char *p = (const unsigned char *)s;
  while (*p) {
    unsigned c = *p;
    if (c == '"' || c == '\\') { fputc('\\', out); fputc(c, out); p++; }
    else if (c < 32) { fprintf(out, "\\u%04x", c); p++; }
    else if (c < 128) { fputc(c, out); p++; }
    else {
      int n = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
      unsigned cp = n ? c & ((1u << (7 - n)) - 1) : 0;
      int valid = n != 0;
      for (int i = 1; i < n; i++) {
        if ((p[i] & 0xc0) != 0x80) { valid = 0; break; }
        cp = (cp << 6) | (p[i] & 63);
      }
      if (valid && (cp < (n == 2 ? 0x80u : n == 3 ? 0x800u : 0x10000u) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))) valid = 0;
      if (valid) { fwrite(p, 1, (size_t)n, out); p += n; }
      else { fprintf(out, "\\u%04x", c); p++; }
    }
  }
  fputc('"', out);
}

static int inspect_error(const char *file, int json, const char *kind, const char *message, int status) {
  if (json) {
    fputs("{\"schema_version\":1,\"ok\":false,\"file\":", stdout); json_string(stdout, file);
    fputs(",\"error\":{\"kind\":", stdout); json_string(stdout, kind);
    fputs(",\"message\":", stdout); json_string(stdout, message); fputs("}}\n", stdout);
    fflush(stdout);
  } else fprintf(stderr, "dm: %s\n", message);
  return status;
}

static void program_stats(const dm_prog *p, const char *kind, int index, const char *name, int json, int *first) {
  if (json) {
    if (!*first) fputc(',', stdout);
    fputs("{\"kind\":", stdout); json_string(stdout, kind);
    fprintf(stdout, ",\"index\":%d,\"name\":", index); json_string(stdout, name);
    printf(",\"offset\":%u,\"bytes\":%u,\"locals\":%u,\"state\":%u,\"delays\":%u,\"reverbs\":%u}",
      p->off, p->len, p->nlocals, p->nstate, p->ndelay, p->nverb);
  } else printf("%-7s %3d %-16s %6u %6u %6u %6u %6u\n", kind, index, name,
    p->len, p->nlocals, p->nstate, p->ndelay, p->nverb);
  *first = 0;
}

static void module_stats(const char *file, const dm_module *m, int json) {
  unsigned patterns = 0, cells = 0;
  unsigned long long sample_bytes = 0, texture_bytes = 0;
  for (int i = 0; i < DM_MAX_PATTERNS; i++) if (m->pat[i].cells) { patterns++; cells += m->pat[i].rows * m->pat[i].channels; }
  for (int i = 0; i < m->nsmp; i++) sample_bytes += m->smp[i].len;
  for (int i = 0; i < m->ntex; i++) texture_bytes += (unsigned long long)m->tex[i].w * m->tex[i].h * 3;
  if (json) {
    fputs("{\"schema_version\":1,\"ok\":true,\"file\":", stdout); json_string(stdout, file);
    fputs(",\"title\":", stdout); json_string(stdout, m->title);
    printf(",\"module_version\":%d,\"song\":{\"bpm\":%u,\"speed\":%u,\"rows_per_beat\":%u,\"channels\":%u,\"orders\":%u,\"loop_order\":%u}",
      DM_VERSION, m->bpm, m->speed, m->rpb, m->channels, m->norder, m->loop);
    printf(",\"resources\":{\"code_bytes\":%u,\"instruments\":%d,\"patterns\":%u,\"cells\":%u,\"uniforms\":%u,\"pixel_programs\":%d,\"palettes\":%d,\"texts\":%d,\"samples\":%d,\"sample_bytes\":%llu,\"textures\":%d,\"texture_bytes\":%llu,\"envelopes\":%d,\"event_names\":%d,\"events\":%d},\"programs\":[",
      m->codelen, m->ninst, patterns, cells, m->nuniform, m->npixel, m->npal, m->ntext,
      m->nsmp, sample_bytes, m->ntex, texture_bytes, m->nvenv, m->nevname, m->nev);
  } else {
    printf("%s\n", m->title[0] ? m->title : file);
    stats(stdout, file, m);
    printf("%u BPM, %u channels, %u orders, %u uniforms; sample PCM %llu bytes, texture RGB %llu bytes\n",
      m->bpm, m->channels, m->norder, m->nuniform, sample_bytes, texture_bytes);
    puts("kind    idx name              bytes locals  state delays reverbs");
  }
  int first = 1;
  for (int i = 0; i < m->ninst; i++) program_stats(&m->inst[i].prog, "inst", i + 1, m->inst[i].name, json, &first);
  for (int i = 0; i < DM_MAX_CHANNELS; i++) if ((m->has_chanfx >> i) & 1) program_stats(&m->chanfx[i], "chan", i, "", json, &first);
  if (m->has_master) program_stats(&m->master, "master", 0, "master", json, &first);
  if (m->has_frame) program_stats(&m->frame, "frame", 0, "frame", json, &first);
  for (int i = 0; i < m->npixel; i++) program_stats(&m->pixel[i], "pixel", i, m->pixelname[i], json, &first);
  if (m->has_post) program_stats(&m->post, "post", 0, "post", json, &first);
  if (json) fputs("]}\n", stdout);
}

static int wants_json(int argc, char **argv) {
  for (int i = 0; i < argc && strcmp(argv[i], "--"); i++) if (!strcmp(argv[i], "--json")) return 1;
  return 0;
}

static int cmd_inspect(int argc, char **argv, int report) {
  int json = wants_json(argc, argv), positional = 0;
  const char *file = NULL;
  for (int i = 0; i < argc; i++) {
    if (!positional && !strcmp(argv[i], "--json")) continue;
    if (!positional && !strcmp(argv[i], "--")) { positional = 1; continue; }
    if ((!positional && argv[i][0] == '-') || file)
      return inspect_error(file, json, "usage", "expected one input file and optional --json", 2);
    file = argv[i];
  }
  if (!file) return inspect_error(NULL, json, "usage", "missing input file", 2);
  dm_module *m = calloc(1, sizeof *m);
  if (!m) return inspect_error(file, json, "load", "out of memory", 1);
  int rc = load_any(file, m);
  if (rc) { free(m); return inspect_error(file, json, "load", dm_error(), 1); }
  if (report) module_stats(file, m, json);
  else {
    fputs("{\"schema_version\":1,\"ok\":true,\"file\":", stdout); json_string(stdout, file);
    fputs(",\"error\":null}\n", stdout);
  }
  dm_module_free(m); free(m);
  return fflush(stdout) || ferror(stdout) ? 1 : 0;
}

static int cmd_compile(int argc, char **argv, int check, int disasm) {
  const char *in = NULL, *out = NULL;
  for (int i = 0; i < argc; i++) {
    if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
    else if (!strcmp(argv[i], "--disasm")) disasm = 1;
    else if (!strcmp(argv[i], "--check")) check = 1;
    else if (argv[i][0] == '-') usage(2);
    else in = argv[i];
  }
  if (!in) usage(2);
  static dm_module m;
  if (load_any(in, &m)) { fprintf(stderr, "%s\n", dm_error()); return 1; }
  stats(stderr, in, &m);
  if (disasm) disasm_all(&m);
  if (check || disasm) return 0;
  char def[1024];
  if (!out) {
    snprintf(def, sizeof def, "%s", in);
    char *dot = strrchr(def, '.');
    if (dot && !strchr(dot, '/')) *dot = 0;
    strncat(def, ".dms", sizeof def - strlen(def) - 1);
    out = def;
  }
  if (dm_module_write(&m, out)) { fprintf(stderr, "%s\n", dm_error()); return 1; }
  FILE *f = fopen(out, "rb"); long sz = 0;
  if (f) { fseek(f, 0, SEEK_END); sz = ftell(f); fclose(f); }
  fprintf(stderr, "wrote %s (%ld bytes)\n", out, sz);
  dm_module_free(&m);
  return 0;
}

static int cmd_new(const char *name) {
  char path[1024]; snprintf(path, sizeof path, "%s%s", name, ends_with(name, ".dmt") ? "" : ".dmt");
  FILE *f = fopen(path, "wx");
  if (!f) { fprintf(stderr, "dm: cannot create %s (exists?)\n", path); return 1; }
  fprintf(f,
    "# %s - a dmscene demo.  dm watch %s\n"
    "# @description A short description of this demo.\n"
    "title %s\nsong bpm=125 speed=6 rpb=4 channels=3 loop=0 volume=0.7\norder 0\n\n"
    "palette glow {\n  0.0  0.0 0.0 0.1\n  0.5  1.0 0.3 0.0\n  1.0  1.0 1.0 0.8\n}\n\n"
    "inst kick {\n  f = 50 + 400 * exp(-t * 35)\n  $p = $p + f / sr\n  out(dist(sin($p * tau) * exp(-t * 6) * 2, 1.4) * 0.8)\n}\n"
    "inst bass {\n  e = env(0.003, 0.2, 0.4, 0.1)\n  s = svf(bsaw(ph, freq), 200 + 2500 * e, 0.4)\n  out(s * e * 0.5)\n}\n"
    "inst hat {\n  out(hp(noise(), 8000) * exp(-t * 50) * 0.25)\n}\n\n"
    "master {\n  d = dly(l * 0.5 + r * 0.5, 0.36, 0.35)\n  out(dist(l + d * 0.2, 1.2), dist(r + d * 0.2, 1.2))\n}\n\n"
    "# channels: 0 kick  1 bass  2 hat\npattern 0 rows=16 {\n"
    "  C-2 1 40 | A-1 2 40 | .\n  .        | .        | .\n  .        | .        | C-6 3 20\n  .        | A-1 2 30 | .\n"
    "  C-2 1 40 | .        | .\n  .        | .        | .\n  .        | C-2 2 36 | C-6 3 20\n  .        | .        | .\n"
    "  C-2 1 40 | A-1 2 40 | .\n  .        | .        | .\n  .        | .        | C-6 3 20\n  .        | G-1 2 30 | .\n"
    "  C-2 1 40 | .        | .\n  .        | .        | .\n  .        | G-1 2 36 | C-6 3 20\n  .        | .        | C-6 3 14\n}\n\n"
    "uniform kickE\n\nframe {\n  kickE = exp(-trig(0) * 8)\n}\n\n"
    "pixel {\n  a = beat * 0.1\n  it = julia(u * 1.4, v * 1.4, 0.7885 * cos(a), 0.7885 * sin(a), 48)\n  k = log(1 + it) / log(49)\n"
    "  pal(glow, k * 2 + bar * 0.05)\n  cmul((0.1 + 1.4 * k * k) * (1 - step(48, it)) + kickE * 0.3)\n}\n", name, path, name);
  fclose(f);
  fprintf(stderr, "wrote %s\n", path);
  return 0;
}

/* ------------------------------------------------------------ play / render / watch */
typedef struct {
  dm_player p;
  pthread_mutex_t mu;
  double rate, phase;
  float a[2], b[2], source[2 * 1024];
  uint32_t source_at, source_n;
  int ready, paused;
} audio_ctx;

/* Keep the synth's normal 1024-sample render grouping.  In particular, do not
 * render it one sample at a time: several instrument effects are block-aware. */
static void source_next(audio_ctx *ac, float out[2]) {
  if (ac->source_at == ac->source_n) {
    dm_player_render(&ac->p, ac->source, 1024);
    ac->source_at = 0;
    ac->source_n = 1024;
  }
  out[0] = ac->source[2 * ac->source_at];
  out[1] = ac->source[2 * ac->source_at + 1];
  ac->source_at++;
}

static void audio_cb(void *user, float *lr, uint32_t frames) {
  audio_ctx *ac = user;
  pthread_mutex_lock(&ac->mu);
  if (ac->paused) memset(lr, 0, sizeof(float) * 2 * frames);
  else if (ac->rate == 1.0) dm_player_render(&ac->p, lr, frames);
  else for (uint32_t i = 0; i < frames; i++) {
    if (!ac->ready) { source_next(ac, ac->a); source_next(ac, ac->b); ac->phase = 0; ac->ready = 1; }
    lr[2 * i] = ac->a[0] + (ac->b[0] - ac->a[0]) * (float)ac->phase;
    lr[2 * i + 1] = ac->a[1] + (ac->b[1] - ac->a[1]) * (float)ac->phase;
    ac->phase += ac->rate;
    while (ac->phase >= 1.0) { ac->a[0] = ac->b[0]; ac->a[1] = ac->b[1]; source_next(ac, ac->b); ac->phase -= 1.0; }
  }
  pthread_mutex_unlock(&ac->mu);
}

typedef struct {
  const char *file, *png, *wav, *mp4, *avi, *mov;
  int fps, W, H, mute, threads, solo, watch, render_scale, graphics, native, adaptive;
  double seconds, start;
} opts;

static int parse_int(const char *s) {
  char *end;
  errno = 0;
  long value = strtol(s, &end, 10);
  if (errno || end == s || *end || value < INT_MIN || value > INT_MAX) usage(2);
  return (int)value;
}

static double parse_double(const char *s) {
  char *end;
  errno = 0;
  double value = strtod(s, &end);
  if (errno || end == s || *end || !isfinite(value)) usage(2);
  return value;
}

static void parse_play_opts(int argc, char **argv, opts *o) {
  for (int i = 0; i < argc; i++) {
    if (!strcmp(argv[i], "--fps") && i + 1 < argc) o->fps = parse_int(argv[++i]);
    else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
      int end = 0;
      const char *size = argv[++i];
      if (sscanf(size, "%dx%d%n", &o->W, &o->H, &end) != 2 || !end || size[end] || o->W < 2 || o->H < 2) usage(2);
    }
    else if (!strcmp(argv[i], "--mute")) o->mute = 1;
    else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) o->seconds = parse_double(argv[++i]);
    else if (!strcmp(argv[i], "--png") && i + 1 < argc) o->png = argv[++i];
    else if (!strcmp(argv[i], "--wav") && i + 1 < argc) o->wav = argv[++i];
    else if (!strcmp(argv[i], "--mp4") && i + 1 < argc) o->mp4 = argv[++i];
    else if (!strcmp(argv[i], "--avi") && i + 1 < argc) o->avi = argv[++i];
    else if (!strcmp(argv[i], "--mov") && i + 1 < argc) o->mov = argv[++i];
    else if (!strcmp(argv[i], "--threads") && i + 1 < argc) o->threads = parse_int(argv[++i]);
    else if (!strcmp(argv[i], "--render-scale") && i + 1 < argc) { o->render_scale = parse_int(argv[++i]); if (o->render_scale < 1) usage(2); }
    else if (!strcmp(argv[i], "--start") && i + 1 < argc) o->start = parse_double(argv[++i]);
    else if (!strcmp(argv[i], "--solo") && i + 1 < argc) o->solo = parse_int(argv[++i]);
    else if (!strcmp(argv[i], "--watch")) o->watch = 1;
    else if (!strcmp(argv[i], "--native")) o->native = 1;
    else if (!strcmp(argv[i], "--interpreted")) o->native = -1;
    else if (!strcmp(argv[i], "--adaptive")) o->adaptive = 1;
    else if (!strcmp(argv[i], "--graphics") && i + 1 < argc) {
      const char *mode = argv[++i];
      if (!strcmp(mode, "auto")) o->graphics = DM_TERM_AUTO;
      else if (!strcmp(mode, "text")) o->graphics = DM_TERM_TEXT;
      else if (!strcmp(mode, "kitty")) o->graphics = DM_TERM_KITTY;
      else usage(2);
    }
    else if (argv[i][0] == '-') usage(2);
    else o->file = argv[i];
  }
  if (!o->file || o->fps < 1 || o->fps > 240 || o->W < 0 || o->H < 0 ||
      (o->W && (o->W < 2 || o->W > 16384)) || (o->H && (o->H < 2 || o->H > 16384)) ||
      !isfinite(o->seconds) || o->seconds < 0 || !isfinite(o->start) || o->start < 0 ||
      o->start > (double)UINT64_MAX / DM_SR || o->seconds > (double)(INT_MAX - 1) / o->fps ||
      o->render_scale < 0 || o->render_scale > 64 ||
      (!!o->mp4 + !!o->avi + !!o->mov > 1)) usage(2);
  const char *out[] = {o->mp4, o->avi, o->mov, o->wav};
  for (size_t i = 0; i < sizeof out / sizeof *out; i++) for (size_t j = i + 1; j < sizeof out / sizeof *out; j++)
    if (out[i] && out[j] && !strcmp(out[i], out[j])) usage(2);
}

#ifndef DM_AOT
/* Optional source-checkout accelerator. Pass argv directly to make/exec: no
 * shell interpolation, generated source from verified bytecode only. */
static int run_native(char **argv, const opts *o) {
  const char *name = strrchr(o->file, '/'); name = name ? name + 1 : o->file;
  size_t n = strlen(name);
  if (n <= 4 || n > 128 || !ends_with(name, ".dmt")) goto unavailable;
  for (size_t i = 0; i < n - 4; i++)
    if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= 'A' && name[i] <= 'Z') ||
          (name[i] >= '0' && name[i] <= '9') || name[i] == '_' || name[i] == '-')) goto unavailable;
  char source[160], target[192];
  snprintf(source, sizeof source, "demos/%s", name);
  struct stat requested, local, compiler;
  if (stat(o->file, &requested) || stat(source, &local) || stat("tools/dm-aot.c", &compiler) ||
      requested.st_dev != local.st_dev || requested.st_ino != local.st_ino) goto unavailable;
  snprintf(target, sizeof target, "build/native-player/%.*s", (int)n - 4, name);
  fprintf(stderr, "dm: preparing native shaders for %s (cached between runs)\n", name);
  if (mkdir("build", 0777) && errno != EEXIST) { perror("dm: build directory"); return o->native > 0 ? 1 : -1; }
  int lock = open("build/.native-build.lock", O_CREAT | O_RDWR, 0600);
  if (lock < 0) { perror("dm: native build lock"); return o->native > 0 ? 1 : -1; }
  while (flock(lock, LOCK_EX) < 0) if (errno != EINTR) { perror("dm: native build lock"); close(lock); return o->native > 0 ? 1 : -1; }
  /* One cache build at a time, even for different demos sharing core objects.
   * The make child inherits the lock until all compiler children have exited. */
  pid_t child = fork();
  if (child < 0) { perror("dm: native build"); close(lock); return o->native > 0 ? 1 : -1; }
  if (!child) {
    dup2(2, 1);
    execlp("make", "make", "--no-print-directory", "-s", target, (char *)NULL);
    perror("dm: make"); _exit(127);
  }
  int status;
  while (waitpid(child, &status, 0) < 0) if (errno != EINTR) { perror("dm: wait for native build"); close(lock); return o->native > 0 ? 1 : -1; }
  if (!WIFEXITED(status) || WEXITSTATUS(status)) {
    close(lock);
    fprintf(stderr, "dm: native build failed%s\n", o->native > 0 ? "; check the compiler/make output above" : "; using interpreter");
    return o->native > 0 ? 1 : -1;
  }
  /* Release atomically with exec, not before: another launch must not rebuild
   * this cache entry between our successful build and executable load. */
  if (fcntl(lock, F_SETFD, FD_CLOEXEC) < 0) { perror("dm: native lock flags"); close(lock); return o->native > 0 ? 1 : -1; }
  execv(target, argv);
  perror("dm: launch native player"); close(lock); return o->native > 0 ? 1 : -1;
 unavailable:
  if (o->native > 0) fprintf(stderr, "dm: --native needs a demos/NAME.dmt source and the repository working directory; use normal playback elsewhere\n");
  return o->native > 0 ? 2 : -1;
}
#endif

static int mkdir_p(const char *path) {
  char *copy = strdup(path);
  if (!copy) return -1;
  for (char *p = copy + (copy[0] == '/'); ; p++) {
    if (*p != '/' && *p != 0) continue;
    char c = *p; *p = 0;
    if (*copy && mkdir(copy, 0777) && errno != EEXIST) { free(copy); return -1; }
    if (*copy) { struct stat st; if (stat(copy, &st) || !S_ISDIR(st.st_mode)) { free(copy); return -1; } }
    *p = c;
    if (!c) break;
  }
  free(copy);
  return 0;
}

static int ffmpeg_available(void) {
  const char *path = getenv("PATH");
  if (!path) path = "/bin:/usr/bin";
  const char *p = path;
  do {
    const char *end = strchr(p, ':');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    char *name = malloc(len + sizeof "/ffmpeg");
    if (!name) return 0;
    if (len) memcpy(name, p, len);
    else { name[0] = '.'; len = 1; }
    memcpy(name + len, "/ffmpeg", sizeof "/ffmpeg");
    int found = access(name, X_OK) == 0;
    free(name);
    if (found) return 1;
    p = end ? end + 1 : NULL;
  } while (p);
  return 0;
}

static FILE *start_ffmpeg(int w, int h, int fps, const char *wav, const char *mp4, pid_t *pid) {
  int fds[2];
  if (pipe(fds)) return NULL;
  char size[64], rate[32];
  snprintf(size, sizeof size, "%dx%d", w, h);
  snprintf(rate, sizeof rate, "%d", fps);
  char *const args[] = {"ffmpeg", "-y", "-hide_banner", "-loglevel", "error", "-f", "rawvideo",
    "-pix_fmt", "rgb24", "-s", size, "-r", rate, "-i", "-", "-i", (char *)wav,
    "-c:v", "libx264", "-preset", "medium", "-crf", "18", "-pix_fmt", "yuv420p",
    "-c:a", "aac", "-b:a", "192k", "-shortest", "-movflags", "+faststart", (char *)mp4, NULL};
  *pid = fork();
  if (*pid == 0) {
    close(fds[1]);
    if (dup2(fds[0], STDIN_FILENO) < 0) _exit(127);
    if (fds[0] != STDIN_FILENO) close(fds[0]);
    execvp("ffmpeg", args);
    _exit(127);
  }
  close(fds[0]);
  if (*pid < 0) { close(fds[1]); return NULL; }
  FILE *out = fdopen(fds[1], "wb");
  if (!out) {
    close(fds[1]); kill(*pid, SIGTERM);
    while (waitpid(*pid, NULL, 0) < 0 && errno == EINTR) {}
    *pid = -1;
  }
  return out;
}

static void print_shell_arg(FILE *f, const char *s) {
  fputc('\'', f);
  for (; *s; s++) {
    if (*s == '\'') fputs("'\\''", f);
    else fputc(*s, f);
  }
  fputc('\'', f);
}

static char *native_temp_path(const char *path) {
  size_t n = strlen(path) + sizeof ".tmp.XXXXXX";
  char *tmp = malloc(n);
  if (!tmp) return NULL;
  snprintf(tmp, n, "%s.tmp.XXXXXX", path);
  int fd = mkstemp(tmp);
  if (fd < 0) { free(tmp); return NULL; }
  close(fd);
  return tmp;
}

static int render_offline(dm_module *m, opts *o) {
  static audio_ctx ac;
  dm_scene scene; dm_scene_init(&scene, m); scene.nthreads = o->threads;
  dm_fb fb = {0}, big = {0};
  dm_avi *avi = NULL;
  dm_mov *movie = NULL;
  dm_wav wv = {0};
  int have_wav = 0;
  const char *avi_path = o->avi, *mov_path = o->mov;
  char *avi_tmp = NULL, *mov_tmp = NULL;
  char tmpwav[1100] = {0};
  FILE *vpipe = NULL;
  pid_t encoder = -1;
  int rc = 1, saved_pipe = 0, tmp_owned = 0;
  struct sigaction old_pipe;
  double seconds = o->seconds > 0 ? o->seconds : 8;
  int W = o->W ? o->W : 640, H = o->H ? o->H : 400, fps = o->fps;
  const char *png = o->png, *wav = o->wav, *mp4 = o->mp4;
  if (mp4) { W &= ~1; H &= ~1; }
  uint64_t total = (uint64_t)(seconds * DM_SR);
  int nframes = (int)(seconds * fps + 0.5);
  uint64_t stride = (((uint64_t)W * 3 + 3) & ~3ull), image = stride * (uint64_t)H;
  if ((avi_path || mov_path) && (nframes < 1 || total > UINT32_MAX)) { fprintf(stderr, "dm: export duration exceeds native container limits\n"); goto done; }
  if (avi_path && (image > UINT32_MAX || (uint64_t)nframes * (image + 24) + total * 4 + ((total + 4095) / 4096) * 24 + 1024 > UINT32_MAX)) {
    fprintf(stderr, "dm: AVI exceeds its 4 GiB container limit\n"); goto done;
  }
  if (dm_player_init(&ac.p, m, DM_SR)) { fprintf(stderr, "dm: out of memory\n"); goto done; }
  if (o->solo >= 0) dm_player_solo(&ac.p, o->solo);
  dm_seq clk; dm_seq_init(&clk, m, DM_SR);
  uint64_t start_sample = (uint64_t)(o->start * DM_SR);
  if (start_sample) { float tmp[2 * 1024]; uint64_t left = start_sample; while (left) { uint32_t k = left > 1024 ? 1024 : (uint32_t)left; dm_player_render(&ac.p, tmp, k); left -= k; } }
  int sc = o->render_scale ? o->render_scale : (m->scale > 1 ? m->scale : 1);
  if (dm_fb_alloc(&fb, W / sc > 1 ? W / sc : 1, H / sc > 1 ? H / sc : 1) || dm_fb_alloc(&big, W, H)) goto done;
  static char fbwav[1100], dir[1100];
  int have_ffmpeg = 0;
  if (mp4) {
    have_ffmpeg = ffmpeg_available();
    if (!have_ffmpeg) {
      if (snprintf(dir, sizeof dir, "%s.frames", mp4) >= (int)sizeof dir || mkdir_p(dir)) {
        fprintf(stderr, "dm: cannot create %s.frames\n", mp4); goto done;
      }
      png = dir;
      if (!wav) { if (snprintf(fbwav, sizeof fbwav, "%s.wav", mp4) >= (int)sizeof fbwav) goto done; wav = fbwav; }
      fprintf(stderr, "dm: ffmpeg not found; writing %s/ and %s instead\n", dir, wav);
    } else if (!wav) {
      if (snprintf(tmpwav, sizeof tmpwav, "%s.tmp.XXXXXX", mp4) >= (int)sizeof tmpwav) goto done;
      int fd = mkstemp(tmpwav);
      if (fd < 0) { fprintf(stderr, "dm: cannot create temporary audio for %s\n", mp4); goto done; }
      close(fd); tmp_owned = 1; wav = tmpwav;
    }
  }
  if (avi_path) { avi_tmp = native_temp_path(avi_path); if (!avi_tmp) goto done; avi = dm_avi_open(avi_tmp, W, H, fps, DM_SR); }
  if (mov_path) { mov_tmp = native_temp_path(mov_path); if (!mov_tmp) goto done; movie = dm_mov_open(mov_tmp, W, H, fps, DM_SR); }
  if ((avi_path && !avi) || (mov_path && !movie)) { fprintf(stderr, "dm: cannot create native video output\n"); goto done; }
  if (wav) {
    if (total > (UINT32_MAX - 36) / 4) { fprintf(stderr, "dm: audio exceeds WAV size limit\n"); goto done; }
    if (dm_wav_open(&wv, wav, DM_SR)) { fprintf(stderr, "dm: cannot write %s\n", wav); goto done; }
    have_wav = 1;
  }
  if (have_wav || avi || movie) {
    float *buf = malloc(sizeof(float) * 2 * 4096);
    int failed = !buf;
    for (uint64_t done = 0; !failed && done < total;) {
      uint32_t n = total - done > 4096 ? 4096 : (uint32_t)(total - done);
      dm_player_render(&ac.p, buf, n);
      if (have_wav && dm_wav_write(&wv, buf, n)) failed = 1;
      if (avi && dm_avi_audio(avi, buf, n)) failed = 1;
      if (movie && dm_mov_audio(movie, buf, n)) failed = 1;
      done += n;
    }
    if (have_wav && dm_wav_close(&wv)) failed = 1;
    have_wav = 0;
    free(buf);
    if (failed) { fprintf(stderr, "dm: cannot finish audio output\n"); goto done; }
  }
  if (mp4 && have_ffmpeg) {
    struct sigaction ignore = {0}; ignore.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &ignore, &old_pipe)) goto done;
    saved_pipe = 1;
    vpipe = start_ffmpeg(W, H, fps, wav, mp4, &encoder);
    if (!vpipe) { fprintf(stderr, "dm: cannot start ffmpeg\n"); goto done; }
  }
  int frame = 0, render_error = 0;
  if (png || vpipe || avi || movie) {
    /* offline the audio was rendered first, so level followers are replayed from a second player for the visuals */
    static audio_ctx vis;
    int vis_ready = dm_player_init(&vis.p, m, DM_SR) == 0;
    if (vis_ready && o->solo >= 0) dm_player_solo(&vis.p, o->solo);
    float *scratch = vis_ready ? malloc(sizeof(float) * 2 * 8192) : NULL;
    if (!scratch) { fprintf(stderr, "dm: out of memory\n"); render_error = 1; }
    uint64_t vdone = 0;
    for (frame = 0; !render_error && frame < nframes; frame++) {
      uint64_t target = start_sample + (uint64_t)((double)frame * DM_SR / fps);
      while (vdone < target) { uint32_t k = target - vdone > 8192 ? 8192 : (uint32_t)(target - vdone); dm_player_render(&vis.p, scratch, k); vdone += k; }
      dm_seq_seek(&clk, target);
      dm_scene_render(&scene, &clk, vis.p.synth.cenv, &fb);
      dm_fb *outfb = &fb;
      if (sc > 1) { dm_fb_upscale(&fb, &big); outfb = &big; }
      if (png) {
        char path[1200];
        if (snprintf(path, sizeof path, "%s/frame_%04d.png", png, frame) >= (int)sizeof path ||
            dm_png_write(outfb, path)) { fprintf(stderr, "dm: cannot write frame %d in %s\n", frame, png); render_error = 1; break; }
      }
      if (vpipe && fwrite(outfb->rgb, 1, (size_t)outfb->w * outfb->h * 3, vpipe) != (size_t)outfb->w * outfb->h * 3) { render_error = 1; break; }
      if (avi && dm_avi_video(avi, outfb)) { render_error = 1; break; }
      if (movie && dm_mov_video(movie, outfb)) { render_error = 1; break; }
      if (vpipe && (frame % fps) == 0) { fprintf(stderr, "\r%s: %d/%d frames", mp4, frame, nframes); fflush(stderr); }
    }
    free(scratch); if (vis_ready) dm_player_free(&vis.p);
  }
  if (vpipe) {
    int write_error = fclose(vpipe); vpipe = NULL;
    int status = 0;
    pid_t waited;
    do { waited = waitpid(encoder, &status, 0); } while (waited < 0 && errno == EINTR);
    encoder = -1;
    if (render_error || write_error || waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      fprintf(stderr, "\ndm: ffmpeg failed or stopped before all frames were written\n"); goto done;
    }
    fprintf(stderr, "\rwrote %s: %d frames, %dx%d @ %d fps, %.1fs            \n", mp4, frame, W, H, fps, seconds);
  } else if (render_error) goto done;
  else if (mp4) {
    size_t len = strlen(png) + sizeof "/frame_%04d.png";
    char *pattern = malloc(len);
    if (!pattern) goto done;
    snprintf(pattern, len, "%s/frame_%%04d.png", png);
    fprintf(stderr, "to finish:\n  ffmpeg -framerate %d -i ", fps);
    print_shell_arg(stderr, pattern);
    fputs(" -i ", stderr); print_shell_arg(stderr, wav);
    fputs(" -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest ", stderr);
    print_shell_arg(stderr, mp4); fputc('\n', stderr);
    free(pattern);
  }
  if (avi && dm_avi_close(avi)) { avi = NULL; fprintf(stderr, "dm: AVI finalize failed\n"); goto done; }
  avi = NULL;
  if (avi_tmp && rename(avi_tmp, avi_path)) { fprintf(stderr, "dm: cannot publish %s\n", avi_path); goto done; }
  free(avi_tmp); avi_tmp = NULL;
  if (movie && dm_mov_close(movie)) { movie = NULL; fprintf(stderr, "dm: MOV finalize failed\n"); goto done; }
  movie = NULL;
  if (mov_tmp && rename(mov_tmp, mov_path)) { fprintf(stderr, "dm: cannot publish %s\n", mov_path); goto done; }
  free(mov_tmp); mov_tmp = NULL;
  if (!mp4) fprintf(stderr, "rendered %.2fs: %d frame(s)%s%s\n", seconds, frame, wav ? ", wav " : "", wav ? wav : "");
  rc = 0;
 done:
  if (have_wav) dm_wav_close(&wv);
  if (vpipe) fclose(vpipe);
  if (encoder > 0) { kill(encoder, SIGTERM); while (waitpid(encoder, NULL, 0) < 0 && errno == EINTR) {} }
  if (tmp_owned) remove(tmpwav);
  if (saved_pipe) sigaction(SIGPIPE, &old_pipe, NULL);
  if (avi) dm_avi_close(avi);
  if (movie) dm_mov_close(movie);
  if (avi_tmp) remove(avi_tmp);
  if (mov_tmp) remove(mov_tmp);
  free(avi_tmp); free(mov_tmp);
  dm_player_free(&ac.p); dm_scene_free(&scene); dm_fb_free(&fb); dm_fb_free(&big);
  return rc;
}

static void hold_audio(audio_ctx *ac, int paused) {
  pthread_mutex_lock(&ac->mu);
  ac->paused = paused;
  pthread_mutex_unlock(&ac->mu);
}

static void accept_audio(audio_ctx *ac, dm_player *next, double rate, int paused) {
  pthread_mutex_lock(&ac->mu);
  dm_player old = ac->p;
  ac->p = *next;
  ac->rate = rate;
  ac->phase = 0;
  ac->ready = 0;
  ac->source_at = ac->source_n = 0;
  ac->paused = paused;
  pthread_mutex_unlock(&ac->mu);
  free(next);
  dm_player_free(&old);
}

static uint64_t advance_samples(uint64_t base, double samples) {
  if (samples <= 0) return base;
  if (samples >= (double)(UINT64_MAX - base)) return UINT64_MAX;
  return base + (uint64_t)samples;
}

static void copy_mute(audio_ctx *ac, uint8_t *mute) {
  pthread_mutex_lock(&ac->mu);
  memcpy(mute, ac->p.synth.mute, DM_MAX_CHANNELS);
  pthread_mutex_unlock(&ac->mu);
}

static void reset_scene_after_seek(dm_scene *scene, const dm_module *m, const opts *o, uint64_t pos) {
  uint64_t frame = pos / DM_SR > (uint64_t)INT_MAX / (uint64_t)o->fps ? INT_MAX : pos * (uint64_t)o->fps / DM_SR;
  dm_scene_free(scene);
  dm_scene_init(scene, m);
  scene->nthreads = o->threads;
  scene->frame = (int)frame;
}

typedef struct { double scale, full_cost; unsigned settled; } play_budget;
static void budget_observe(play_budget *b, double cost, int fps) {
  if (!(cost > 0) || !isfinite(cost) || fps < 1) return;
  double full = cost / (b->scale * b->scale), target = .8 / fps;
  b->full_cost = b->full_cost ? .75 * b->full_cost + .25 * full : full;
  double wanted = sqrt(target / b->full_cost);
  if (wanted < .25) wanted = .25;
  if (wanted > 1) wanted = 1;
  b->settled++;
  if (b->settled >= 3 && wanted < b->scale * .90) { b->scale = wanted; b->full_cost = 0; b->settled = 0; }
  else if (b->settled >= 60 && (wanted > b->scale * 1.15 || (wanted == 1 && b->scale < 1))) {
    b->scale = fmin(wanted, b->scale * 1.10); b->full_cost = 0; b->settled = 0;
  }
}

static int play_realtime(dm_module *mods, opts *o) {
  static const double speeds[] = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
  dm_module *m = &mods[0];
  int which = 0, paused = 0, speed_i = 2, rc = 0;
  static audio_ctx ac;
  pthread_mutex_init(&ac.mu, NULL);
  if (dm_player_init(&ac.p, m, DM_SR)) {
    fprintf(stderr, "dm: out of memory\n");
    dm_player_free(&ac.p); pthread_mutex_destroy(&ac.mu); dm_module_free(m);
    return 1;
  }
  if (o->solo >= 0) dm_player_solo(&ac.p, o->solo);
  uint8_t mute[DM_MAX_CHANNELS];
  copy_mute(&ac, mute);
  dm_seek *seeker = dm_seek_open(m);
  if (!seeker) {
    dm_player_free(&ac.p); pthread_mutex_destroy(&ac.mu); dm_module_free(m);
    return 1;
  }
  uint64_t base = (uint64_t)(o->start * DM_SR), audio_origin = 0;
  ac.rate = speeds[speed_i]; ac.paused = base != 0;
  dm_audio *au = o->mute ? NULL : dm_audio_open(DM_SR, audio_cb, &ac);
  int waiting = au && base != 0;
  dm_seek_request(seeker, base, mute, waiting);
  dm_seq clk; dm_seq_init(&clk, m, DM_SR);
  dm_scene scene; dm_scene_init(&scene, m); scene.nthreads = o->threads;
  dm_term *term = dm_term_open();
  dm_fb fb = {0}; dm_scrub scrub = {0};
  play_budget budget = {1, 0, 0};
  double epoch = dm_now(), last = epoch, fps_est = 0, lastcheck = epoch, last_draw = 0;
  file_stamp srcm = mtime(o->file);
  char note[160] = ""; double note_until = 0;
  int frames = 0, frame_dirty = 1, restore_tstp = 0;
  struct sigaction saved_tstp;
  if (!term) { fprintf(stderr, "dm: cannot open terminal\n"); rc = 1; goto done; }
  dm_term_graphics(term, o->graphics);
  if (!au && !o->mute) {
    snprintf(note, sizeof note, "audio unavailable; wall-clock playback");
    note_until = epoch + 3;
  }
  for (;;) {
    double now = dm_now();
    uint64_t pos = base;
    if (!paused && !waiting && !scrub.active) {
      uint64_t heard = au ? dm_audio_played(au) : 0;
      double elapsed = au ? (double)(heard > audio_origin ? heard - audio_origin : 0) : (now - epoch) * DM_SR;
      pos = advance_samples(base, elapsed * speeds[speed_i]);
    }
    if (au && dm_audio_failed(au)) {
      hold_audio(&ac, 1); dm_audio_close(au); au = NULL;
      base = pos; waiting = 0; epoch = dm_now();
      snprintf(note, sizeof note, "audio stopped; wall-clock playback");
      note_until = epoch + 3; frame_dirty = 1;
    }

    /* Consume the whole scroll burst before drawing. Keep ordinary keys queued
     * for the next iteration instead of discarding them behind the first key. */
    int k = -1, delta = 0;
    for (int inputs = 0; inputs < 4096; inputs++) {
      int in = dm_term_key(term);
      if (in < 0) break;
      if (in == DM_KEY_SCRUB_BACK) delta--;
      else if (in == DM_KEY_SCRUB_FORWARD) delta++;
      else { k = in; break; }
    }
    if (k == 'q') break;
    if (delta) {
      if (!scrub.active) reset_scene_after_seek(&scene, m, o, pos);
      dm_scrub_add(&scrub, delta, now, (double)pos / DM_SR);
      base = advance_samples(0, scrub.target * DM_SR);
      hold_audio(&ac, 1);
      waiting = au != NULL;
      dm_seek_request(seeker, base, mute, waiting);
      frame_dirty = 1;
    }
    int was_scrubbing = scrub.active;
    if (scrub.active) {
      pos = advance_samples(0, dm_scrub_tick(&scrub, now) * DM_SR);
      frame_dirty = 1;
      if (!scrub.active && !waiting) { base = pos; epoch = now; }
    }

    if (k == DM_KEY_SUSPEND) {
      base = pos; scrub.active = 0;
      hold_audio(&ac, 1);
      dm_audio_close(au); au = NULL;
      dm_term_close(term); term = NULL;
      if (!restore_tstp) { sigaction(SIGTSTP, NULL, &saved_tstp); restore_tstp = 1; }
      signal(SIGTSTP, SIG_DFL); raise(SIGTSTP);
      term = dm_term_open();
      if (!term) { fprintf(stderr, "dm: cannot restore terminal\n"); rc = 1; break; }
      dm_term_graphics(term, o->graphics);
      au = o->mute ? NULL : dm_audio_open(DM_SR, audio_cb, &ac);
      waiting = au != NULL;
      dm_seek_request(seeker, base, mute, waiting);
      audio_origin = au ? dm_audio_played(au) : 0; epoch = dm_now();
      continue;
    }

    if (k >= '1' && k <= '8' && k - '1' < m->channels) mute[k - '1'] ^= 1;
    if (k == '0') memset(mute, 0, sizeof mute);
    pthread_mutex_lock(&ac.mu);
    memcpy(ac.p.synth.mute, mute, sizeof mute);
    pthread_mutex_unlock(&ac.mu);
    if ((k == '0' || (k >= '1' && k <= '8')) && (waiting || scrub.active))
      dm_seek_request(seeker, base, mute, waiting);

    int seeked = 0, changed = 0;
    if (k == DM_KEY_LEFT) { pos = pos > 5 * DM_SR ? pos - 5 * DM_SR : 0; seeked = 1; }
    if (k == DM_KEY_RIGHT) { pos = advance_samples(pos, 5 * DM_SR); seeked = 1; }
    if (k == DM_KEY_UP && speed_i + 1 < (int)(sizeof speeds / sizeof *speeds)) { speed_i++; changed = 1; }
    if (k == DM_KEY_DOWN && speed_i > 0) { speed_i--; changed = 1; }
    if (k == ' ') { paused ^= 1; changed = 1; }
    if (seeked || changed) {
      base = pos; scrub.active = 0; epoch = now;
      hold_audio(&ac, 1);
      waiting = au != NULL;
      dm_seek_request(seeker, base, mute, waiting);
      if (seeked) { reset_scene_after_seek(&scene, m, o, base); frame_dirty = 1; }
    }

    if (waiting && !scrub.active) {
      dm_player *ready = dm_seek_take(seeker);
      if (ready) {
        base = pos = ready->seq.sample;
        accept_audio(&ac, ready, speeds[speed_i], paused);
        waiting = 0;
        audio_origin = dm_audio_played(au); epoch = dm_now();
        frame_dirty = 1;
      }
    }
    if (!waiting && !scrub.active) dm_seek_request(seeker, pos, mute, 0);
    if (dm_seek_failed(seeker)) { fprintf(stderr, "dm: audio seek worker failed\n"); rc = 1; break; }

    /* Input can wake a slow playback timer immediately. Coalesce rapid input
     * into at most 120 drawings/s instead of repainting once per wheel report. */
    double minimum_frame_time = 1.0 / (o->fps > 120 ? o->fps : 120);
    if (k < 0 && now < last_draw + minimum_frame_time) {
      dm_term_wait(term, last_draw + minimum_frame_time - dm_now());
      continue;
    }

    /* The sequencer's seek function only advances. Explicitly rewind the cheap
     * tracker clock when scrubbing backwards; never replay audio on this path. */
    if (pos < clk.sample) dm_seq_init(&clk, m, DM_SR);
    dm_seq_seek(&clk, pos);
    int cols, rows, pw, ph, kitty = dm_term_is_kitty(term);
    if (kitty || !o->W) { dm_term_size(term, &cols, &rows); rows = rows > 1 ? rows - 1 : 1; }
    else { cols = o->W; rows = (o->H + 1) / 2; }
    pw = cols; ph = rows * 2;
    if (kitty) {
      if (o->W) { pw = o->W; ph = o->H; }
      else {
        dm_term_pixels(term, &pw, &ph);
        ph = (int)((int64_t)ph * rows / (rows + 1)); /* reserve status row */
        double down = fmax((double)pw / 640, (double)ph / 400);
        if (down > 1) { pw = (int)(pw / down); ph = (int)(ph / down); }
      }
    }
    int sc = o->render_scale ? o->render_scale : m->scale > 1 ? m->scale : 1;
    int fw = pw / sc > 1 ? pw / sc : 1, fh = ph / sc > 1 ? ph / sc : 2;
    if (kitty && !o->W && o->adaptive) {
      fw = (int)(fw * budget.scale); if (fw < 1) fw = 1;
      fh = (int)(fh * budget.scale); if (fh < 2) fh = 2;
    }
    if (fb.w != fw || fb.h != fh) frame_dirty = 1;
    if (dm_fb_alloc(&fb, fw, fh)) { rc = 1; break; }
    float env[DM_MAX_CHANNELS];
    if (au && !waiting && !scrub.active) {
      pthread_mutex_lock(&ac.mu); memcpy(env, ac.p.synth.cenv, sizeof env); pthread_mutex_unlock(&ac.mu);
    } else dm_seek_env(seeker, env);
    int rendered = (!paused && !waiting) || frame_dirty;
    double render_start = dm_now();
    if (rendered) {
      dm_scene_render(&scene, &clk, env, &fb); frame_dirty = 0;
    }
    char chs[DM_MAX_CHANNELS + 1];
    for (int c = 0; c < m->channels; c++) chs[c] = mute[c] ? '.' : (char)('1' + c);
    chs[m->channels] = 0;
    char status[320];
    snprintf(status, sizeof status, " %s  %s%.3gx  %.2fs  beat %6.2f  %.0ffps  ch %s  %s %dx%d  %s",
      m->title[0] ? m->title : o->file, scrub.active ? "scrub @ " : waiting ? "seeking @ " : paused ? "pause @ " : "speed ",
      speeds[speed_i], (double)pos / DM_SR, dm_seq_beat(&clk), fps_est, chs, kitty ? "kitty" : "text", fw, fh, now < note_until ? note : "");
    dm_term_blit(term, &fb, cols, rows, status);
    if (rendered && kitty && !o->W && o->adaptive) budget_observe(&budget, dm_now() - render_start, o->fps);
    last_draw = now;

    int reload = k == 'r';
    if (o->watch && now - lastcheck > .5) {
      lastcheck = now;
      file_stamp mt = mtime(o->file);
      if (!same_stamp(mt, srcm)) { srcm = mt; reload = 1; }
    }
    if (reload && o->watch) {
      dm_module *nm = &mods[which ^ 1];
      if (load_any(o->file, nm) == 0) {
        dm_seek *next_seeker = dm_seek_open(nm);
        dm_player *next = calloc(1, sizeof *next);
        if (!next_seeker || !next || dm_player_init(next, nm, DM_SR)) {
          if (next) { dm_player_free(next); free(next); }
          dm_seek_close(next_seeker); dm_module_free(nm); rc = 1; break;
        }
        memcpy(next->synth.mute, mute, sizeof mute);
        hold_audio(&ac, 1);
        dm_audio_close(au); au = NULL;
        dm_seek_close(seeker); seeker = next_seeker;
        accept_audio(&ac, next, speeds[speed_i], 1);
        dm_scene_free(&scene); dm_module_free(m);
        which ^= 1; m = nm;
        if (o->native >= 0) bind_native(m);
        budget = (play_budget){1, 0, 0};
        dm_scene_init(&scene, m); scene.nthreads = o->threads;
        dm_seq_init(&clk, m, DM_SR);
        base = pos; scrub.active = 0; frame_dirty = 1;
        au = o->mute ? NULL : dm_audio_open(DM_SR, audio_cb, &ac);
        waiting = au != NULL;
        dm_seek_request(seeker, base, mute, waiting);
        audio_origin = au ? dm_audio_played(au) : 0; epoch = dm_now();
        snprintf(note, sizeof note, "reloaded"); note_until = epoch + 2;
      } else { snprintf(note, sizeof note, "%s", dm_error()); note_until = now + 8; }
    }
    frames++;
    double t1 = dm_now();
    if (t1 - last >= 1) { fps_est = frames / (t1 - last); frames = 0; last = t1; }
    int fps = was_scrubbing && o->fps < 60 ? 60 : o->fps;
    dm_term_wait(term, now + 1.0 / fps - dm_now());
  }
 done:
  dm_term_close(term);
  if (restore_tstp) sigaction(SIGTSTP, &saved_tstp, NULL);
  dm_audio_close(au); dm_seek_close(seeker);
  dm_player_free(&ac.p); pthread_mutex_destroy(&ac.mu);
  dm_scene_free(&scene); dm_fb_free(&fb); dm_module_free(m);
  return rc;
}

int main(int argc, char **argv) {
  if (argc < 2) usage(2);
  const char *cmd = argv[1];
  const int base = 2;
  if (!strcmp(cmd, "--version") || !strcmp(cmd, "-V")) {
    if (argc != 2) usage(2);
    printf("dmscene v%d (DMS format %d)\n", DM_VERSION, DM_VERSION);
    return fflush(stdout) || ferror(stdout) ? 1 : 0;
  }
  if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help") || !strcmp(cmd, "help")) usage(0);
  for (int i = base; i < argc && strcmp(argv[i], "--"); i++) if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) usage(0);
  if (!strcmp(cmd, "compile")) return cmd_compile(argc - base, argv + base, 0, 0);
  if (!strcmp(cmd, "check")) return wants_json(argc - base, argv + base) ? cmd_inspect(argc - base, argv + base, 0) : cmd_compile(argc - base, argv + base, 1, 0);
  if (!strcmp(cmd, "stats")) return cmd_inspect(argc - base, argv + base, 1);
  if (!strcmp(cmd, "disasm")) return cmd_compile(argc - base, argv + base, 1, 1);
  if (!strcmp(cmd, "new")) { if (argc - base < 1) usage(2); return cmd_new(argv[base]); }
  if (!strcmp(cmd, "play") || !strcmp(cmd, "render") || !strcmp(cmd, "watch")) {
    opts o; memset(&o, 0, sizeof o); o.fps = 30; o.solo = -1;
    parse_play_opts(argc - base, argv + base, &o);
#ifndef DM_AOT
    if (o.native > 0 || (!o.native && strcmp(cmd, "render"))) {
      int result = run_native(argv, &o);
      if (result >= 0) return result;
    }
#endif
    if (!strcmp(cmd, "watch")) o.watch = 1;
    static dm_module mods[2];
    if (load_any(o.file, &mods[0])) { fprintf(stderr, "dm: %s\n", dm_error()); return 1; }
    if (o.native >= 0) bind_native(&mods[0]);
    if (o.png || o.wav || o.mp4 || o.avi || o.mov) return render_offline(&mods[0], &o);
    if (!strcmp(cmd, "render")) { fprintf(stderr, "dm render: give --mp4 FILE, --avi FILE, --mov FILE, --png DIR or --wav FILE\n"); return 2; }
    if (o.watch && !ends_with(o.file, ".dmt")) fprintf(stderr, "dm: --watch reloads a .dmt; a .dms will not change\n");
    return play_realtime(mods, &o);
  }
  usage(2);
  return 2;
}
