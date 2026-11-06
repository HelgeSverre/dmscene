/* File-size limits produce real write/flush errors without /dev/full. */
#include "dm.h"
#include <signal.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static int one(int which) {
  char path[] = "/tmp/dmscene-write-XXXXXX";
  int fd = mkstemp(path); if (fd < 0) return 1; close(fd);
  pid_t pid = fork();
  if (!pid) {
    signal(SIGXFSZ, SIG_IGN);
    rlim_t bytes = which == 4 ? 44 : which >= 5 ? 128 : 1;
    struct rlimit limit = {bytes, bytes}; if (setrlimit(RLIMIT_FSIZE, &limit)) _exit(2);
    if (which == 3) {
      execl("./dm", "dm", "render", "demos/pulse.dmt", "--wav", path, "--seconds", "0.01", (char *)NULL);
      _exit(2);
    }
    if (which >= 4) {
      dm_wav wav = {0}; float pcm[4096] = {0};
      if (dm_wav_open(&wav, path, DM_SR)) _exit(2);
      int rc = dm_wav_write(&wav, pcm, which == 4 ? 2 : which == 5 ? 2048 : UINT32_MAX);
      if (which > 4 && !rc) _exit(0);
      if (!dm_wav_close(&wav) || !dm_wav_close(&wav) || !dm_wav_write(&wav, pcm, 1)) _exit(0);
      _exit(1);
    }
    uint8_t rgb[3] = {0}; dm_fb fb = {1, 1, rgb};
    dm_module m = {0};
    int rc = which == 0 ? dm_png_write(&fb, path) : which == 1 ? dm_ppm_write(&fb, path) : dm_module_write(&m, path);
    _exit(rc ? 1 : 0);
  }
  int status; waitpid(pid, &status, 0); unlink(path);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 1) {
    fprintf(stderr, "FAIL: %s did not report a failed output\n", (const char *[]) {"PNG", "PPM", "DMS", "WAV CLI", "WAV flush", "WAV write retry", "WAV size limit"}[which]);
    return 1;
  }
  return 0;
}
int main(void) {
  int bad = 0; for (int i = 0; i < 7; i++) bad |= one(i);
  if (!bad) puts("ok: buffered output failures reach the caller and CLI");
  return bad;
}
