/* Exercise the actual play command through pipes: the terminal parser/blitter
 * support non-TTY descriptors too. No audio device or GUI is required. */
#include "dm.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int child_in, child_out;
static pid_t child;
static char pending[65536];
static size_t used;
static unsigned frames;
static double last_frame, max_gap, seconds, beat;

static void cleanup(void) {
  if (child > 0) { kill(child, SIGKILL); waitpid(child, NULL, 0); }
}
#define CHECK(x, msg) do { if (!(x)) { fprintf(stderr, "FAIL: %s\n", msg); exit(1); } } while (0)
static void send_keys(const char *keys) {
  CHECK(write(child_in, keys, strlen(keys)) == (ssize_t)strlen(keys), "send keys");
}
static void read_for(double duration) {
  double end = dm_now() + duration;
  while (dm_now() < end) {
    struct pollfd p = {child_out, POLLIN, 0};
    if (poll(&p, 1, (int)((end - dm_now()) * 1000) + 1) <= 0) continue;
    char buf[32768]; ssize_t n = read(child_out, buf, sizeof buf);
    CHECK(n > 0, "play exited unexpectedly");
    for (ssize_t i = 0; i < n; i++) {
      if (used == sizeof pending - 1) { memmove(pending, pending + used - 128, 128); used = 128; }
      pending[used++] = buf[i]; pending[used] = 0;
      if (used >= 3 && !memcmp(pending + used - 3, "\033[H", 3)) {
        double now = dm_now();
        if (last_frame && now - last_frame > max_gap) max_gap = now - last_frame;
        last_frame = now; frames++; used = 0;
      }
      if (used >= 2 && !memcmp(pending + used - 2, "ch", 2)) {
        char *label = strstr(pending, "x  ");
        if (label) sscanf(label + 3, "%lfs  beat %lf", &seconds, &beat);
      }
    }
  }
}

int main(int argc, char **argv) {
  int in[2], out[2]; CHECK(!pipe(in) && !pipe(out), "pipes");
  child = fork(); CHECK(child >= 0, "fork");
  if (!child) {
    dup2(in[0], 0); dup2(out[1], 1); dup2(out[1], 2);
    close(in[0]); close(in[1]); close(out[0]); close(out[1]);
    const char *bin = argc > 1 ? argv[1] : "./dm";
    int low_fps = argc > 2 && !strcmp(argv[2], "low-fps");
    if (argc > 2 && !low_fps)
      execl(bin, bin, "play", "demos/showcase.dmt", "--start", "30", "--size", "120x40", "--fps", "60", (char *)NULL);
    execl(bin, bin, "play", "demos/showcase.dmt", "--mute", "--start", "30", "--size", "120x40", "--fps", low_fps ? "2" : "60", (char *)NULL);
    _exit(127);
  }
  atexit(cleanup); close(in[0]); close(out[1]); child_in = in[1]; child_out = out[0];
  read_for(.5); send_keys(" "); read_for(.5);
  unsigned start_frames = frames; max_gap = 0; last_frame = dm_now();
  for (int i = 0; i < 20; i++) { send_keys("\033[<65;10;10M"); read_for(.025); }
  read_for(.4);
  CHECK(frames - start_frames >= 20 && max_gap < .250, "scrubbing blocks the rendering loop");
  double scrub_gap = max_gap;
  double forward = seconds, forward_beat = beat;
  for (int i = 0; i < 20; i++) { send_keys("\033[<64;10;10M"); read_for(.025); }
  read_for(.4);
  CHECK(seconds < forward - .2 && beat < forward_beat, "reverse scrub did not rewind time and beat");
  double held = seconds;
  read_for(.15); CHECK(seconds == held, "paused scrub did not stay paused");
  send_keys(" "); read_for(.8);
  CHECK(seconds > held + .1, "audio/visual clock did not resume after scrub");
  send_keys("\033[A"); read_for(.4);
  CHECK(seconds > held + .4, "speed change stalled playback");
  send_keys("q");
  int status = 0;
  double deadline = dm_now() + 3;
  while (waitpid(child, &status, WNOHANG) == 0) {
    CHECK(dm_now() < deadline, "quit timed out");
    struct pollfd p = {child_out, POLLIN, 0};
    if (poll(&p, 1, 10) > 0) { char discard[32768]; (void)read(child_out, discard, sizeof discard); }
  }
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "quit after scrub");
  child = 0; close(child_in); close(child_out);
  printf("ok: live scrub response, reverse clock, pause (max scrub frame gap %.1fms)\n", scrub_gap * 1000);
  return 0;
}
