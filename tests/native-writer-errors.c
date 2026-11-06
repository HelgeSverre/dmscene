/* Native containers must surface buffered write/close failures. */
#include "dm.h"
#include "dm_mov.h"
#include <signal.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static int run(int mov) {
  char path[] = "/tmp/dmscene-native-write-XXXXXX";
  int fd = mkstemp(path); if (fd < 0) return 1; close(fd);
  pid_t pid = fork();
  if (!pid) {
    signal(SIGXFSZ, SIG_IGN);
    struct rlimit limit = {128, 128};
    if (setrlimit(RLIMIT_FSIZE, &limit)) _exit(2);
    uint8_t rgb[3 * 64] = {0}; float pcm[2 * 4096] = {0}; dm_fb fb = {8, 8, rgb};
    if (mov) {
      dm_mov *m = dm_mov_open(path, 8, 8, 30, DM_SR);
      int failed = !m;
      if (!failed && dm_mov_video(m, &fb)) failed = 1;
      if (!failed && dm_mov_audio(m, pcm, 4096)) failed = 1;
      if (m && dm_mov_close(m)) failed = 1;
      if (!failed) _exit(0);
    } else {
      dm_avi *a = dm_avi_open(path, 8, 8, 30, DM_SR);
      int failed = !a;
      if (!failed && dm_avi_video(a, &fb)) failed = 1;
      if (!failed && dm_avi_audio(a, pcm, 4096)) failed = 1;
      if (a && dm_avi_close(a)) failed = 1;
      if (!failed) _exit(0);
    }
    _exit(1);
  }
  int status; waitpid(pid, &status, 0); unlink(path);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 1) {
    fprintf(stderr, "FAIL: %s did not report close failure\n", mov ? "MOV" : "AVI");
    return 1;
  }
  return 0;
}

int main(void) {
  int bad = run(0) || run(1);
  if (!bad) puts("ok: native writer close failures propagate");
  return bad;
}
