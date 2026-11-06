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
  struct sigaction handlers[5]; unsigned installed;
  unsigned char input[256]; size_t input_pos, input_n;
  int state, invalid, private, param_i, digits, x10_left, x10_button, paste_match, string_bel;
  unsigned params[4], sequence_len;
};

static volatile sig_atomic_t g_quit, g_suspend;
static const int handled_signals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGTSTP};
static void on_sig(int s) { if (s == SIGTSTP) g_suspend = 1; else g_quit = 1; }
enum { INPUT_TEXT, INPUT_ESC, INPUT_CSI, INPUT_SS3, INPUT_X10, INPUT_STRING,
       INPUT_STRING_ESC, INPUT_PASTE, INPUT_ESC_INTERMEDIATE };

static void wr(const char *s, size_t n) {
  while (n) { ssize_t k = write(1, s, n); if (k <= 0) { if (k < 0 && (errno == EINTR || errno == EAGAIN)) continue; return; } s += k; n -= (size_t)k; }
}

dm_term *dm_term_open(void) {
  dm_term *t = calloc(1, sizeof *t);
  if (!t) return NULL;
  if (tcgetattr(0, &t->saved) == 0) {
    struct termios r = t->saved;
    r.c_lflag &= ~(ICANON | ECHO); r.c_lflag |= ISIG;
    r.c_iflag &= ~IXON;
    r.c_cc[VMIN] = 0; r.c_cc[VTIME] = 0;
    t->raw = tcsetattr(0, TCSANOW, &r) == 0;
  }
  struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_sig;   /* no SA_RESTART: interrupt sleeps/reads */
  g_quit = g_suspend = 0;
  for (unsigned i = 0; i < sizeof handled_signals / sizeof *handled_signals; i++)
    if (sigaction(handled_signals[i], &sa, &t->handlers[i]) == 0) t->installed |= 1u << i;
  const char *init = "\x1b[?1049h\x1b[?25l\x1b[?1000h\x1b[?1006h\x1b[?2004h\x1b[2J";
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
  if (!t || !fb || !fb->rgb || cols <= 0 || rows <= 0) return;
  int cells_h = rows;
  size_t status_n = status ? strlen(status) : 0;
  if (status_n > (size_t)cols) {
    status_n = (size_t)cols;
    /* Do not split a UTF-8 code point at the right edge. */
    while (status_n && ((unsigned char)status[status_n] & 0xc0) == 0x80) status_n--;
  }
  if ((size_t)cols > (SIZE_MAX - 6) / 48) return;
  size_t row_bytes = (size_t)cols * 48 + 6;
  if ((size_t)rows > (SIZE_MAX - 1024 - status_n) / row_bytes) return;
  size_t need = row_bytes * (size_t)rows + 1024 + status_n;
  if (t->cap < need) {
    char *buf = realloc(t->buf, need);
    if (!buf) { g_quit = 1; return; }
    t->buf = buf; t->cap = need;
  }
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
  if (status) {
    memcpy(p, "\x1b[0m\x1b[K", 7); p += 7;
    memcpy(p, status, status_n); p += status_n;
  }
  wr(t->buf, (size_t)(p - t->buf));
}

/* Input is a streaming state machine: even arbitrarily long or fragmented
 * escape strings never expose their payload as ordinary command keys. */
static void begin_sequence(dm_term *t, int state) {
  t->state = state; t->invalid = t->private = t->param_i = t->digits = 0;
  t->sequence_len = 0; memset(t->params, 0, sizeof t->params);
}
static int wheel_key(unsigned button) {
  button &= ~28u; /* Shift/Alt/Control modifiers. */
  if (button == 64 || button == 66) return DM_KEY_SCRUB_BACK;
  if (button == 65 || button == 67) return DM_KEY_SCRUB_FORWARD;
  return -1;
}
static int input_byte(dm_term *t, unsigned char ch) {
  switch (t->state) {
    case INPUT_TEXT:
      if (ch == 27) { t->state = INPUT_ESC; return -1; }
      if (ch == 3) return 'q';
      if (ch == 26) return DM_KEY_SUSPEND;
      return ch == 'q' || ch == 'r' || ch == ' ' || (ch >= '0' && ch <= '8') ? ch : -1;
    case INPUT_ESC:
      if (ch == '[') begin_sequence(t, INPUT_CSI);
      else if (ch == 'O') begin_sequence(t, INPUT_SS3);
      else if (ch == ']' || ch == 'P' || ch == 'X' || ch == '^' || ch == '_') {
        t->state = INPUT_STRING; t->string_bel = ch == ']';
      } else if (ch >= 0x20 && ch <= 0x2f) t->state = INPUT_ESC_INTERMEDIATE;
      else if (ch != 27) t->state = INPUT_TEXT;
      return -1;
    case INPUT_ESC_INTERMEDIATE:
      if (ch >= 0x30 && ch <= 0x7e) t->state = INPUT_TEXT;
      else if (ch == 27) t->state = INPUT_ESC;
      return -1;
    case INPUT_STRING:
      if (ch == 27) t->state = INPUT_STRING_ESC;
      else if (t->string_bel && ch == 7) t->state = INPUT_TEXT;
      return -1;
    case INPUT_STRING_ESC:
      if (ch == '\\' || (t->string_bel && ch == 7)) t->state = INPUT_TEXT;
      else if (ch != 27) t->state = INPUT_STRING;
      return -1;
    case INPUT_PASTE: {
      static const unsigned char end[] = "\033[201~";
      if (ch == end[t->paste_match]) t->paste_match++;
      else t->paste_match = ch == 27 ? 1 : 0;
      if (t->paste_match == (int)sizeof end - 1) { t->state = INPUT_TEXT; t->paste_match = 0; }
      return -1;
    }
    case INPUT_X10:
      if (t->x10_left == 3) t->x10_button = ch >= 32 ? ch - 32 : -1;
      if (--t->x10_left == 0) { t->state = INPUT_TEXT; return t->x10_button < 0 ? -1 : wheel_key((unsigned)t->x10_button); }
      return -1;
    case INPUT_CSI: case INPUT_SS3: {
      int csi = t->state == INPUT_CSI;
      if (ch == 27) { t->state = INPUT_ESC; return -1; }
      if (t->sequence_len < 65) t->sequence_len++;
      if (t->sequence_len > 64) t->invalid = 1;
      if (ch >= 0x40 && ch <= 0x7e) {
        t->state = INPUT_TEXT;
        if (t->invalid) return -1;
        if (csi && ch == 'M' && !t->digits && !t->private && !t->param_i) {
          t->state = INPUT_X10; t->x10_left = 3; return -1;
        }
        if (csi && ch == 'M' && t->private == '<' && t->param_i == 2) return wheel_key(t->params[0]);
        if (t->private) return -1;
        if (csi && ch == '~' && t->param_i == 0 && t->params[0] == 200) { t->state = INPUT_PASTE; t->paste_match = 0; return -1; }
        if (t->param_i > 1 || t->params[0] > 1 || (t->param_i == 1 && (t->params[1] < 1 || t->params[1] > 8))) return -1;
        return ch == 'A' ? DM_KEY_UP : ch == 'B' ? DM_KEY_DOWN : ch == 'C' ? DM_KEY_RIGHT : ch == 'D' ? DM_KEY_LEFT : -1;
      }
      if (ch >= '0' && ch <= '9') {
        t->digits = 1;
        if (t->params[t->param_i] > 100000) t->invalid = 1;
        else t->params[t->param_i] = t->params[t->param_i] * 10 + ch - '0';
      } else if (ch == ';') {
        if (t->param_i < 3) t->param_i++; else t->invalid = 1;
      } else if (ch == '<' && t->sequence_len == 1 && csi) t->private = '<';
      else t->invalid = 1;
      return -1;
    }
  }
  return -1;
}
int dm_term_key(dm_term *t) {
  if (!t) return -1;
  if (g_quit) return 'q';
  if (g_suspend) { g_suspend = 0; return DM_KEY_SUSPEND; }
  for (unsigned budget = 0; budget < 4096; budget++) {
    if (t->input_pos == t->input_n) {
      struct pollfd pf = {0, POLLIN, 0};
      if (poll(&pf, 1, 0) <= 0 || !(pf.revents & POLLIN)) return -1;
      ssize_t n = read(0, t->input, sizeof t->input);
      if (n <= 0) return -1;
      t->input_pos = 0; t->input_n = (size_t)n;
    }
    int key = input_byte(t, t->input[t->input_pos++]);
    if (key >= 0) return key;
  }
  return -1;
}

void dm_term_close(dm_term *t) {
  if (!t) return;
  const char *fin = "\x1b[0m\x1b[?2004l\x1b[?1006l\x1b[?1000l\x1b[?25h\x1b[?1049l";
  wr(fin, strlen(fin));
  if (t->raw) tcsetattr(0, TCSANOW, &t->saved);
  for (unsigned i = 0; i < sizeof handled_signals / sizeof *handled_signals; i++)
    if (t->installed & (1u << i)) sigaction(handled_signals[i], &t->handlers[i], NULL);
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
