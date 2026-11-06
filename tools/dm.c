/* dm - the dmscene command line: compile, check, disasm, play, render, watch, new.
 * Inputs may be .dmt (compiled in memory) or .dms. */
#include "dm.h"
#include "dm_seek.h"
#include "dm_banner.h"
#include <pthread.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

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
    "  check   IN                           compile and verify only\n"
    "  disasm  IN                           print every program's bytecode\n"
    "  play    IN [play options]            terminal + audio (IN may be .dmt or .dms)\n"
    "  watch   IN [play options]            play, recompiling whenever the .dmt changes\n"
    "  render  IN --mp4 F | --png DIR | --wav F [--seconds S] [--size WxH] [--fps N] [--render-scale N]\n"
    "  new     NAME                         write NAME.dmt, a minimal starting point\n\n"
    "play options:\n"
    "  --fps N       target frame rate (default 30)      --size WxH   force framebuffer size\n"
    "  --mute        no audio, wall-clock visuals         --start S    start S seconds in\n"
    "  --solo C      hear only channel C                  --threads N  pixel worker threads\n"
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
static void stats(const char *in, const dm_module *m) {
  int npat = 0, cells = 0;
  for (int i = 0; i < DM_MAX_PATTERNS; i++) if (m->pat[i].cells) { npat++; cells += m->pat[i].rows * m->pat[i].channels; }
  fprintf(stderr, "%s: %d inst, %d pattern(s), %d cells, %d pixel program(s), %d palette(s), %d text(s), %d sample(s), %d texture(s), %d event(s), %u bytes of code\n",
          in, m->ninst, npat, cells, m->npixel, m->npal, m->ntext, m->nsmp, m->ntex, m->nev, m->codelen);
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
  stats(in, &m);
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
  const char *file, *png, *wav, *mp4;
  int fps, W, H, mute, threads, solo, watch, render_scale;
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
    else if (!strcmp(argv[i], "--threads") && i + 1 < argc) o->threads = parse_int(argv[++i]);
    else if (!strcmp(argv[i], "--render-scale") && i + 1 < argc) { o->render_scale = parse_int(argv[++i]); if (o->render_scale < 1) usage(2); }
    else if (!strcmp(argv[i], "--start") && i + 1 < argc) o->start = parse_double(argv[++i]);
    else if (!strcmp(argv[i], "--solo") && i + 1 < argc) o->solo = parse_int(argv[++i]);
    else if (!strcmp(argv[i], "--watch")) o->watch = 1;
    else if (argv[i][0] == '-') usage(2);
    else o->file = argv[i];
  }
  if (!o->file || o->fps < 1 || o->fps > 240 || o->W < 0 || o->H < 0 ||
      (o->W && (o->W < 2 || o->W > 16384)) || (o->H && (o->H < 2 || o->H > 16384)) ||
      !isfinite(o->seconds) || o->seconds < 0 || !isfinite(o->start) || o->start < 0 ||
      o->start > (double)UINT64_MAX / DM_SR || o->seconds > (double)(INT_MAX - 1) / o->fps ||
      o->render_scale < 0 || o->render_scale > 64) usage(2);
}

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

static int render_offline(dm_module *m, opts *o) {
  static audio_ctx ac;
  dm_scene scene; dm_scene_init(&scene, m); scene.nthreads = o->threads;
  dm_fb fb = {0}, big = {0};
  char tmpwav[1100] = {0};
  FILE *vpipe = NULL;
  pid_t encoder = -1;
  int rc = 1, saved_pipe = 0, tmp_owned = 0;
  struct sigaction old_pipe;
  if (dm_player_init(&ac.p, m, DM_SR)) { fprintf(stderr, "dm: out of memory\n"); goto done; }
  if (o->solo >= 0) dm_player_solo(&ac.p, o->solo);
  dm_seq clk; dm_seq_init(&clk, m, DM_SR);
  uint64_t start_sample = (uint64_t)(o->start * DM_SR);
  if (start_sample) { float tmp[2 * 1024]; uint64_t left = start_sample; while (left) { uint32_t k = left > 1024 ? 1024 : (uint32_t)left; dm_player_render(&ac.p, tmp, k); left -= k; } }
  double seconds = o->seconds > 0 ? o->seconds : 8;
  int W = o->W ? o->W : 640, H = o->H ? o->H : 400, fps = o->fps;
  const char *png = o->png, *wav = o->wav, *mp4 = o->mp4;
  if (mp4) { W &= ~1; H &= ~1; }
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
  uint64_t total = (uint64_t)(seconds * DM_SR);
  if (wav) {
    if (total > (UINT32_MAX - 36) / 4) { fprintf(stderr, "dm: audio exceeds WAV size limit\n"); goto done; }
    dm_wav wv = {0};
    if (dm_wav_open(&wv, wav, DM_SR)) { fprintf(stderr, "dm: cannot write %s\n", wav); goto done; }
    float *buf = malloc(sizeof(float) * 2 * 4096);
    int failed = !buf;
    for (uint64_t done = 0; !failed && done < total;) {
      uint32_t n = total - done > 4096 ? 4096 : (uint32_t)(total - done);
      dm_player_render(&ac.p, buf, n); failed = dm_wav_write(&wv, buf, n); done += n;
    }
    if (dm_wav_close(&wv)) failed = 1;
    free(buf);
    if (failed) { fprintf(stderr, "dm: cannot finish %s\n", wav); goto done; }
  }
  if (mp4 && have_ffmpeg) {
    struct sigaction ignore = {0}; ignore.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &ignore, &old_pipe)) goto done;
    saved_pipe = 1;
    vpipe = start_ffmpeg(W, H, fps, wav, mp4, &encoder);
    if (!vpipe) { fprintf(stderr, "dm: cannot start ffmpeg\n"); goto done; }
  }
  int frame = 0, nframes = (int)(seconds * fps + 0.5), render_error = 0;
  if (png || vpipe) {
    /* offline the audio was rendered first, so level followers are replayed from a second player for the visuals */
    static audio_ctx vis;
    int vis_ready = dm_player_init(&vis.p, m, DM_SR) == 0;
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
  } else fprintf(stderr, "rendered %.2fs: %d frame(s)%s%s\n", seconds, frame, wav ? ", wav " : "", wav ? wav : "");
  rc = 0;
 done:
  if (vpipe) fclose(vpipe);
  if (encoder > 0) { kill(encoder, SIGTERM); while (waitpid(encoder, NULL, 0) < 0 && errno == EINTR) {} }
  if (tmp_owned) remove(tmpwav);
  if (saved_pipe) sigaction(SIGPIPE, &old_pipe, NULL);
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
  double epoch = dm_now(), last = epoch, fps_est = 0, lastcheck = epoch, last_draw = 0;
  file_stamp srcm = mtime(o->file);
  char note[160] = ""; double note_until = 0;
  int frames = 0, frame_dirty = 1, restore_tstp = 0;
  struct sigaction saved_tstp;
  if (!term) { fprintf(stderr, "dm: cannot open terminal\n"); rc = 1; goto done; }
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
    int cols, rows;
    if (!o->W) { dm_term_size(term, &cols, &rows); rows = rows > 1 ? rows - 1 : 1; }
    else { cols = o->W; rows = (o->H + 1) / 2; }
    int sc = m->scale > 1 ? m->scale : 1;
    int fw = cols / sc > 1 ? cols / sc : 1, fh = rows * 2 / sc > 1 ? rows * 2 / sc : 2;
    if (fb.w != fw || fb.h != fh) frame_dirty = 1;
    if (dm_fb_alloc(&fb, fw, fh)) { rc = 1; break; }
    float env[DM_MAX_CHANNELS];
    if (au && !waiting && !scrub.active) {
      pthread_mutex_lock(&ac.mu); memcpy(env, ac.p.synth.cenv, sizeof env); pthread_mutex_unlock(&ac.mu);
    } else dm_seek_env(seeker, env);
    if ((!paused && !waiting) || frame_dirty) {
      dm_scene_render(&scene, &clk, env, &fb); frame_dirty = 0;
    }
    char chs[DM_MAX_CHANNELS + 1];
    for (int c = 0; c < m->channels; c++) chs[c] = mute[c] ? '.' : (char)('1' + c);
    chs[m->channels] = 0;
    char status[320];
    snprintf(status, sizeof status, " %s  %s%.3gx  %.2fs  beat %6.2f  %.0ffps  ch %s  %s",
      m->title[0] ? m->title : o->file, scrub.active ? "scrub @ " : waiting ? "seeking @ " : paused ? "pause @ " : "speed ",
      speeds[speed_i], (double)pos / DM_SR, dm_seq_beat(&clk), fps_est, chs, now < note_until ? note : "");
    dm_term_blit(term, &fb, cols, rows, status);
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
  if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help") || !strcmp(cmd, "help")) usage(0);
  for (int i = base; i < argc; i++) if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) usage(0);
  if (!strcmp(cmd, "compile")) return cmd_compile(argc - base, argv + base, 0, 0);
  if (!strcmp(cmd, "check")) return cmd_compile(argc - base, argv + base, 1, 0);
  if (!strcmp(cmd, "disasm")) return cmd_compile(argc - base, argv + base, 1, 1);
  if (!strcmp(cmd, "new")) { if (argc - base < 1) usage(2); return cmd_new(argv[base]); }
  if (!strcmp(cmd, "play") || !strcmp(cmd, "render") || !strcmp(cmd, "watch")) {
    opts o; memset(&o, 0, sizeof o); o.fps = 30; o.solo = -1;
    parse_play_opts(argc - base, argv + base, &o);
    if (!strcmp(cmd, "watch")) o.watch = 1;
    static dm_module mods[2];
    if (load_any(o.file, &mods[0])) { fprintf(stderr, "dm: %s\n", dm_error()); return 1; }
    if (o.png || o.wav || o.mp4) return render_offline(&mods[0], &o);
    if (!strcmp(cmd, "render")) { fprintf(stderr, "dm render: give --mp4 FILE, --png DIR or --wav FILE\n"); return 2; }
    if (o.watch && !ends_with(o.file, ".dmt")) fprintf(stderr, "dm: --watch reloads a .dmt; a .dms will not change\n");
    return play_realtime(mods, &o);
  }
  usage(2);
  return 2;
}
