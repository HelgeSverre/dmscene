/* dm_term.c - truecolor terminal blitter (two pixels per cell via U+2580) + timing. */
#include "dm.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

struct dm_term {
  struct termios saved;
  int raw;
  char *buf; size_t cap;
};

static volatile sig_atomic_t g_quit;
static void on_sig(int s) { (void)s; g_quit = 1; }

static void wr(const char *s, size_t n) {
  while (n) { ssize_t k = write(1, s, n); if (k < 0) { if (errno == EINTR || errno == EAGAIN) continue; return; } s += k; n -= (size_t)k; }
}

dm_term *dm_term_open(void) {
  dm_term *t = calloc(1, sizeof *t);
  if (tcgetattr(0, &t->saved) == 0) {
    struct termios r = t->saved;
    r.c_lflag &= ~(ICANON | ECHO);
    r.c_cc[VMIN] = 0; r.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &r);
    t->raw = 1;
  }
  struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_sig;   /* no SA_RESTART: interrupt sleeps/reads */
  sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL); sigaction(SIGHUP, &sa, NULL);
  const char *init = "\x1b[?1049h\x1b[?25l\x1b[2J";
  wr(init, strlen(init));
  return t;
}

void dm_term_size(dm_term *t, int *cols, int *rows) {
  (void)t;
  struct winsize ws;
  if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) { *cols = ws.ws_col; *rows = ws.ws_row; }
  else { *cols = 80; *rows = 24; }
}

void dm_term_blit(dm_term *t, const dm_fb *fb, int cols, int rows, const char *status) {
  int cells_h = rows;
  size_t need = (size_t)cols * cells_h * 48 + 1024 + (status ? strlen(status) : 0);
  if (t->cap < need) { t->cap = need; t->buf = realloc(t->buf, need); }
  char *p = t->buf;
  memcpy(p, "\x1b[H", 3); p += 3;
  int pf = -1, pb = -1;
  int ph = cells_h * 2;
  for (int cy = 0; cy < cells_h; cy++) {
    const uint8_t *trow = fb->rgb + (size_t)((cy * 2) * fb->h / ph) * fb->w * 3;
    const uint8_t *brow = fb->rgb + (size_t)((cy * 2 + 1) * fb->h / ph) * fb->w * 3;
    for (int x = 0; x < cols; x++) {
      const uint8_t *top = trow + (size_t)(x * fb->w / cols) * 3;
      const uint8_t *bot = brow + (size_t)(x * fb->w / cols) * 3;
      int f = (top[0] << 16) | (top[1] << 8) | top[2];
      int b = (bot[0] << 16) | (bot[1] << 8) | bot[2];
      if (f != pf && b != pb) { p += sprintf(p, "\x1b[38;2;%d;%d;%d;48;2;%d;%d;%dm", top[0], top[1], top[2], bot[0], bot[1], bot[2]); pf = f; pb = b; }
      else if (f != pf) { p += sprintf(p, "\x1b[38;2;%d;%d;%dm", top[0], top[1], top[2]); pf = f; }
      else if (b != pb) { p += sprintf(p, "\x1b[48;2;%d;%d;%dm", bot[0], bot[1], bot[2]); pb = b; }
      memcpy(p, "\xe2\x96\x80", 3); p += 3;
    }
    memcpy(p, "\x1b[0m\r\n", 6); p += 6;
    pf = pb = -1;
  }
  if (status) { p += sprintf(p, "\x1b[0m\x1b[K%s", status); }
  wr(t->buf, (size_t)(p - t->buf));
}

int dm_term_key(dm_term *t) {
  (void)t;
  if (g_quit) return 'q';
  struct pollfd pf = { 0, POLLIN, 0 };
  if (poll(&pf, 1, 0) <= 0 || !(pf.revents & POLLIN)) return -1;
  unsigned char ch;
  if (read(0, &ch, 1) == 1) return ch;
  return -1;
}

void dm_term_close(dm_term *t) {
  if (!t) return;
  const char *fin = "\x1b[0m\x1b[?25h\x1b[?1049l";
  wr(fin, strlen(fin));
  if (t->raw) tcsetattr(0, TCSANOW, &t->saved);
  free(t->buf); free(t);
}

double dm_now(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}
void dm_sleep(double sec) {
  if (sec <= 0) return;
  struct timespec ts; ts.tv_sec = (time_t)sec; ts.tv_nsec = (long)((sec - (double)ts.tv_sec) * 1e9);
  nanosleep(&ts, NULL);
}
