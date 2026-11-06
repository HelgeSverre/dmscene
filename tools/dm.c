/* dm - the dmscene command line: compile, check, disasm, play, render, watch, new.
 * Inputs may be .dmt (compiled in memory) or .dms. */
#include "dm.h"
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
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  char *s = malloc((size_t)n + 1);
  size_t got = fread(s, 1, (size_t)n, f); fclose(f);
  s[got] = 0;
  return s;
}
static int ends_with(const char *s, const char *suf) { size_t a = strlen(s), b = strlen(suf); return a >= b && !strcmp(s + a - b, suf); }
static double mtime(const char *path) { struct stat st; return stat(path, &st) == 0 ? (double)st.st_mtime : 0; }

/* load a .dmt or .dms into m; returns 0 on success */
static int load_any(const char *path, dm_module *m) {
  if (ends_with(path, ".dms")) return dm_module_read(m, path);
  char *src = slurp(path);
  if (!src) { dm_set_error("cannot read %s", path); return -1; }
  int rc = dm_compile(src, path, m);
  free(src);
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
    "  keys: q/esc quit, 1..8 toggle channel mute, 0 unmute all, r reload (watch)\n"
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
typedef struct { dm_player p; pthread_mutex_t mu; } audio_ctx;
static void audio_cb(void *user, float *lr, uint32_t frames) {
  audio_ctx *ac = user;
  pthread_mutex_lock(&ac->mu);
  dm_player_render(&ac->p, lr, frames);
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
    close(fds[0]);
    execvp("ffmpeg", args);
    _exit(127);
  }
  close(fds[0]);
  if (*pid < 0) { close(fds[1]); return NULL; }
  FILE *out = fdopen(fds[1], "wb");
  if (!out) { close(fds[1]); kill(*pid, SIGTERM); waitpid(*pid, NULL, 0); }
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
  if (dm_player_init(&ac.p, m, DM_SR)) { fprintf(stderr, "dm: out of memory\n"); return 1; }
  if (o->solo >= 0) dm_player_solo(&ac.p, o->solo);
  dm_seq clk; dm_seq_init(&clk, m, DM_SR);
  dm_scene scene; dm_scene_init(&scene, m); scene.nthreads = o->threads;
  uint64_t start_sample = (uint64_t)(o->start * DM_SR);
  if (start_sample) { float tmp[2 * 1024]; uint64_t left = start_sample; while (left) { uint32_t k = left > 1024 ? 1024 : (uint32_t)left; dm_player_render(&ac.p, tmp, k); left -= k; } }
  double seconds = o->seconds > 0 ? o->seconds : 8;
  int W = o->W ? o->W : 640, H = o->H ? o->H : 400, fps = o->fps;
  const char *png = o->png, *wav = o->wav, *mp4 = o->mp4;
  if (mp4) { W &= ~1; H &= ~1; }
  int sc = o->render_scale ? o->render_scale : (m->scale > 1 ? m->scale : 1);
  dm_fb fb = {0}, big = {0};
  if (dm_fb_alloc(&fb, W / sc > 1 ? W / sc : 1, H / sc > 1 ? H / sc : 1) || dm_fb_alloc(&big, W, H)) return 1;
  char tmpwav[1100]; tmpwav[0] = 0;
  static char fbwav[1100], dir[1100];
  FILE *vpipe = NULL;
  pid_t encoder = -1;
  int have_ffmpeg = 0;
  if (mp4) {
    have_ffmpeg = ffmpeg_available();
    if (!have_ffmpeg) {
      if (snprintf(dir, sizeof dir, "%s.frames", mp4) >= (int)sizeof dir || mkdir_p(dir)) {
        fprintf(stderr, "dm: cannot create %s.frames\n", mp4); return 1;
      }
      png = dir;
      if (!wav) { if (snprintf(fbwav, sizeof fbwav, "%s.wav", mp4) >= (int)sizeof fbwav) return 1; wav = fbwav; }
      fprintf(stderr, "dm: ffmpeg not found; writing %s/ and %s instead\n", dir, wav);
    } else if (!wav) { if (snprintf(tmpwav, sizeof tmpwav, "%s.tmp.wav", mp4) >= (int)sizeof tmpwav) return 1; wav = tmpwav; }
  }
  uint64_t total = (uint64_t)(seconds * DM_SR);
  if (wav) {
    dm_wav wv = {0};
    if (dm_wav_open(&wv, wav, DM_SR)) { fprintf(stderr, "dm: cannot write %s\n", wav); return 1; }
    float *buf = malloc(sizeof(float) * 2 * 4096);
    for (uint64_t done = 0; done < total;) { uint32_t n = total - done > 4096 ? 4096 : (uint32_t)(total - done); dm_player_render(&ac.p, buf, n); dm_wav_write(&wv, buf, n); done += n; }
    dm_wav_close(&wv); free(buf);
  }
  if (mp4 && have_ffmpeg) {
    signal(SIGPIPE, SIG_IGN);
    vpipe = start_ffmpeg(W, H, fps, wav, mp4, &encoder);
    if (!vpipe) { fprintf(stderr, "dm: cannot start ffmpeg\n"); return 1; }
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
    int write_error = fclose(vpipe);
    int status = 0;
    int waited = waitpid(encoder, &status, 0);
    if (tmpwav[0]) remove(tmpwav);
    if (render_error || write_error || waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      fprintf(stderr, "\ndm: ffmpeg failed or stopped before all frames were written\n"); return 1;
    }
    fprintf(stderr, "\rwrote %s: %d frames, %dx%d @ %d fps, %.1fs            \n", mp4, frame, W, H, fps, seconds);
  } else if (render_error) return 1;
  else if (mp4) {
    size_t len = strlen(png) + sizeof "/frame_%04d.png";
    char *pattern = malloc(len);
    if (!pattern) return 1;
    snprintf(pattern, len, "%s/frame_%%04d.png", png);
    fprintf(stderr, "to finish:\n  ffmpeg -framerate %d -i ", fps);
    print_shell_arg(stderr, pattern);
    fputs(" -i ", stderr); print_shell_arg(stderr, wav);
    fputs(" -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest ", stderr);
    print_shell_arg(stderr, mp4); fputc('\n', stderr);
    free(pattern);
  } else fprintf(stderr, "rendered %.2fs: %d frame(s)%s%s\n", seconds, frame, wav ? ", wav " : "", wav ? wav : "");
  dm_player_free(&ac.p); dm_scene_free(&scene); dm_fb_free(&fb); dm_fb_free(&big);
  return 0;
}

static int play_realtime(dm_module *mods, opts *o) {
  dm_module *m = &mods[0];
  int which = 0;
  static audio_ctx ac;
  pthread_mutex_init(&ac.mu, NULL);
  if (dm_player_init(&ac.p, m, DM_SR)) { fprintf(stderr, "dm: out of memory\n"); return 1; }
  if (o->solo >= 0) dm_player_solo(&ac.p, o->solo);
  dm_seq clk; dm_seq_init(&clk, m, DM_SR);
  dm_scene scene; dm_scene_init(&scene, m); scene.nthreads = o->threads;
  uint64_t start_sample = (uint64_t)(o->start * DM_SR);
  if (start_sample) { float tmp[2 * 1024]; uint64_t left = start_sample; while (left) { uint32_t k = left > 1024 ? 1024 : (uint32_t)left; dm_player_render(&ac.p, tmp, k); left -= k; } }
  dm_audio *au = o->mute ? NULL : dm_audio_open(DM_SR, audio_cb, &ac);
  if (!au && !o->mute) { fprintf(stderr, "dm: audio unavailable, running muted\n"); dm_sleep(1.0); }
  dm_term *term = dm_term_open();
  double t0 = dm_now(), last = t0, fps_est = 0, lastcheck = t0, srcm = mtime(o->file);
  char note[160] = "";
  double note_until = 0;
  int frames = 0;
  dm_fb fb = {0};
  for (;;) {
    double now = dm_now();
    uint64_t pos = au ? dm_audio_played(au) : (uint64_t)((now - t0) * DM_SR);
    pos += start_sample;
    if (pos < clk.sample) pos = clk.sample;
    dm_seq_seek(&clk, pos);
    int cols, rows;
    if (!o->W) { dm_term_size(term, &cols, &rows); rows = rows > 1 ? rows - 1 : 1; }
    else { cols = o->W; rows = (o->H + 1) / 2; }
    int sc = m->scale > 1 ? m->scale : 1;
    if (dm_fb_alloc(&fb, cols / sc > 1 ? cols / sc : 1, (rows * 2) / sc > 1 ? (rows * 2) / sc : 2)) break;
    dm_scene_render(&scene, &clk, ac.p.synth.cenv, &fb);
    char chs[DM_MAX_CHANNELS + 1];
    for (int c = 0; c < m->channels; c++) chs[c] = ac.p.synth.mute[c] ? '.' : (char)('1' + c);
    chs[m->channels] = 0;
    char status[320];
    snprintf(status, sizeof status, " %s  %dbpm  ord %02d  row %02d  beat %6.2f  %dx%d  %.0ffps  ch %s  %s%s[q]uit",
             m->title[0] ? m->title : o->file, clk.bpm, clk.order, clk.row, dm_seq_beat(&clk), fb.w, fb.h, fps_est, chs,
             now < note_until ? note : "", now < note_until ? "  " : "");
    dm_term_blit(term, &fb, cols, rows, status);
    int k = dm_term_key(term);
    if (k == 'q' || k == 27 || k == 3) break;
    if (k >= '1' && k <= '8') { int c = k - '1'; if (c < m->channels) ac.p.synth.mute[c] ^= 1; }
    if (k == '0') memset(ac.p.synth.mute, 0, sizeof ac.p.synth.mute);
    int reload = (k == 'r');
    if (o->watch && now - lastcheck > 0.5) {
      lastcheck = now;
      double mt = mtime(o->file);
      if (mt != srcm) { srcm = mt; reload = 1; }
    }
    if (reload && o->watch) {
      dm_module *nm = &mods[which ^ 1];
      if (load_any(o->file, nm) == 0) {
        static audio_ctx np; dm_player_init(&np.p, nm, DM_SR);
        memcpy(np.p.synth.mute, ac.p.synth.mute, sizeof np.p.synth.mute);
        pthread_mutex_lock(&ac.mu);
        dm_seq_seek(&np.p.seq, ac.p.seq.sample);
        dm_player old = ac.p; ac.p = np.p;
        pthread_mutex_unlock(&ac.mu);
        dm_player_free(&old);
        dm_module_free(m);
        which ^= 1; m = nm;
        dm_seq_init(&clk, m, DM_SR); dm_seq_seek(&clk, pos);
        int fr = scene.frame; dm_scene_free(&scene); dm_scene_init(&scene, m); scene.nthreads = o->threads; scene.frame = fr;
        snprintf(note, sizeof note, "reloaded"); note_until = now + 2;
      } else { snprintf(note, sizeof note, "%s", dm_error()); note_until = now + 8; }
    }
    frames++;
    double t1 = dm_now();
    if (t1 - last >= 1.0) { fps_est = frames / (t1 - last); frames = 0; last = t1; }
    dm_sleep(now + 1.0 / o->fps - dm_now());
  }
  dm_term_close(term);
  dm_audio_close(au);
  dm_player_free(&ac.p);
  dm_scene_free(&scene);
  dm_fb_free(&fb);
  dm_module_free(m);
  return 0;
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
