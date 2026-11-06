/* Public terminal API against a pseudo-terminal and an independent RGB decoder. */
#define _XOPEN_SOURCE 600
#include "dm.h"
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x, why) do { if (!(x)) { fprintf(stderr, "FAIL: %s\n", why); exit(1); } } while (0)
static int value(const char *s, const char *key) {
  const char *p = strstr(s, key); return p ? atoi(p + strlen(key)) : -1;
}
static unsigned digit(char c) {
  const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const char *p = strchr(alphabet, c); CHECK(c && p, "invalid base64"); return (unsigned)(p - alphabet);
}
static void frames(void) {
  int saved = dup(1); FILE *out = tmpfile(); CHECK(saved >= 0 && out, "capture setup");
  CHECK(dup2(fileno(out), 1) >= 0, "capture stdout");
  dm_term *t = dm_term_open(); CHECK(t, "terminal allocation");
  dm_term_graphics(t, DM_TERM_AUTO); CHECK(!dm_term_is_kitty(t), "redirected auto output must use text");
  dm_term_graphics(t, DM_TERM_KITTY); CHECK(dm_term_is_kitty(t), "forced Kitty mode");
  uint8_t rgb[37 * 29 * 3], decoded[sizeof rgb];
  for (size_t i = 0; i < sizeof rgb; i++) rgb[i] = (uint8_t)(i * 37 + i / 19);
  dm_fb fb = {0}; fb.w = 37; fb.h = 29; fb.rgb = rgb;
  dm_term_blit(t, &fb, 13, 7, "first frame");
  dm_term_blit(t, &fb, 13, 7, "second frame");
  dm_term_close(t);
  long size = ftell(out); CHECK(size > 0, "capture size");
  char *data = calloc((size_t)size + 1, 1); CHECK(data, "capture allocation");
  rewind(out); CHECK(fread(data, 1, (size_t)size, out) == (size_t)size, "capture read");
  dup2(saved, 1); close(saved); fclose(out);
  CHECK(strstr(data, "\033[8;1H") && strstr(data, "\033[?2026l"), "status position and synchronized output end");
  CHECK(!strstr(data, "\xe2\x96\x80"), "Kitty output contains half-block text");
  int frame = 0, chunks = 0, deletes = 0, ids[2] = {0}; size_t used = 0;
  char *cursor = data;
  while ((cursor = strstr(cursor, "\033_G"))) {
    char *header = cursor + 3, *payload = strchr(header, ';'), *end = strstr(header, "\033\\");
    CHECK(payload && end && payload < end, "framed graphics command");
    *payload++ = 0; cursor = end + 2;
    if (strstr(header, "a=d")) {
      CHECK(frame > 0 && strstr(header, "d=I"), "deletion before a complete frame");
      int id = value(header, "i="); CHECK(id == ids[0] || (frame == 2 && id == ids[1]), "deletion of an unowned image");
      deletes++; continue;
    }
    if (strstr(header, "a=T")) {
      CHECK(frame < 2 && used == 0, "overlapping frame transmission");
      CHECK(value(header, "f=") == 24 && value(header, "s=") == 37 && value(header, "v=") == 29, "RGB image dimensions");
      CHECK(value(header, "c=") == 13 && value(header, "r=") == 7 && value(header, "C=") == 1 && value(header, "q=") == 2, "placement parameters");
      ids[frame] = value(header, "i="); CHECK(ids[frame] > 0 && (!frame || ids[0] != ids[1]), "double buffering image IDs");
    }
    size_t n = (size_t)(end - payload); CHECK(n <= 4096 && n % 4 == 0, "base64 chunk boundary");
    CHECK(used + n / 4 * 3 <= sizeof decoded, "decoded frame overflow");
    for (size_t i = 0; i < n; i += 4) {
      unsigned v = digit(payload[i]) << 18 | digit(payload[i+1]) << 12 | digit(payload[i+2]) << 6 | digit(payload[i+3]);
      decoded[used++] = (uint8_t)(v >> 16); decoded[used++] = (uint8_t)(v >> 8); decoded[used++] = (uint8_t)v;
    }
    chunks++;
    if (value(header, "m=") == 0) {
      CHECK(used == sizeof rgb && !memcmp(rgb, decoded, sizeof rgb), "RGB round trip differs");
      frame++; used = 0;
    } else CHECK(n == 4096, "non-final chunk must contain 4096 base64 bytes");
  }
  CHECK(frame == 2 && chunks == 4 && deletes == 3, "complete frames and bounded cleanup"); free(data);
}
static void write_all(int fd, const char *text) {
  size_t n = strlen(text);
  while (n) { ssize_t k = write(fd, text, n); CHECK(k > 0, "PTY write"); text += k; n -= (size_t)k; }
}
static void probe(int late) {
  int master = posix_openpt(O_RDWR | O_NOCTTY);
  CHECK(master >= 0 && !grantpt(master) && !unlockpt(master), "PTY setup");
  char *name = ptsname(master); CHECK(name, "PTY slave name");
  int slave = open(name, O_RDWR | O_NOCTTY); CHECK(slave >= 0, "PTY slave");
  pid_t child = fork(); CHECK(child >= 0, "PTY fork");
  if (!child) {
    close(master); dup2(slave, 0); dup2(slave, 1); close(slave);
    dm_term *t = dm_term_open(); CHECK(t, "PTY terminal open");
    double start = dm_now(); dm_term_graphics(t, DM_TERM_AUTO);
    CHECK(dm_now() - start < .05 && !dm_term_is_kitty(t), "probe must not block or assume support");
    int marker = 0, quit = 0;
    while (dm_now() < start + 3 && !quit) {
      int k = dm_term_key(t);
      if (k == 'r') { CHECK(!dm_term_is_kitty(t), "unrelated or malformed reply enabled images"); marker++; }
      if (k == 'q') quit = 1;
      dm_term_wait(t, .001);
    }
    CHECK(marker == 1 && quit && dm_term_is_kitty(t) == !late, "probe response or ordinary keys lost");
    dm_term_close(t); _exit(0);
  }
  close(slave);
  char data[1024] = {0}; size_t used = 0; double deadline = dm_now() + 2;
  while (!strstr(data, ";AAAA\033\\") && dm_now() < deadline) {
    struct pollfd fd = {master, POLLIN, 0};
    if (poll(&fd, 1, 50) <= 0) continue;
    ssize_t n = read(master, data + used, sizeof data - used - 1);
    CHECK(n > 0, "read query"); used += (size_t)n; data[used] = 0;
  }
  CHECK(strstr(data, ";AAAA\033\\"), "missing Kitty query");
  int id = value(data, "i="); CHECK(id > 0, "query ID");
  char reply[256];
  snprintf(reply, sizeof reply, "\033_Gi=%d;OKq\033\\\033]Gi=%d;OK\007r", id, id);
  write_all(master, reply);
  if (late) dm_sleep(1.1);
  snprintf(reply, sizeof reply, "\033_Gi=%d;OK\033\\q", id);
  for (size_t i = 0; reply[i]; i++) { CHECK(write(master, reply + i, 1) == 1, "fragmented reply"); dm_sleep(.001); }
  int status = 0, done = 0; deadline = dm_now() + 4;
  while (dm_now() < deadline) {
    if (waitpid(child, &status, WNOHANG) == child) { done = 1; break; }
    dm_sleep(.005);
  }
  if (!done) { kill(child, SIGKILL); waitpid(child, &status, 0); }
  close(master);
  CHECK(done && WIFEXITED(status) && !WEXITSTATUS(status), "PTY capability detection");
}
int main(void) {
  frames(); probe(0); probe(1);
  puts("ok: Kitty detection, fallback, fragmented replies, RGB chunks and image cleanup");
  return 0;
}
