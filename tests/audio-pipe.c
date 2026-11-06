/* Run on macOS too, forcing the actual POSIX pipe backend. Each child has a
 * deadline: a stuck writer/child must not make a test suite hang. */
#include "dm.h"
#include <signal.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void silence(void *user, float *lr, uint32_t n) {
  (void)user; memset(lr, 0, sizeof(float) * n * 2);
}
static int exercise(const char *cmd) {
  pid_t pid = fork();
  if (pid < 0) return 1;
  if (!pid) {
    alarm(3);
    setenv("DM_AUDIO_CMD", cmd, 1);
    dm_audio *a = dm_audio_open(DM_SR, silence, NULL);
    if (!a) _exit(2);
    for (int i = 0; i < 50; i++) { (void)dm_audio_played(a); dm_sleep(.002); }
    if (!strncmp(cmd, "exit", 4)) {
      double deadline = dm_now() + 1;
      while (!dm_audio_failed(a) && dm_now() < deadline) dm_sleep(.002);
      if (!dm_audio_failed(a)) _exit(3);
    }
    dm_audio_close(a);
    if (waitpid(-1, NULL, WNOHANG) != -1 || errno != ECHILD) _exit(4);
    _exit(0);
  }
  int status; waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status)) {
    fprintf(stderr, "FAIL: audio command '%s': exit=%d signal=%d\n", cmd,
      WIFEXITED(status) ? WEXITSTATUS(status) : -1, WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    return 1;
  }
  return 0;
}
int main(void) {
  int bad = exercise("exit 7");
  bad |= exercise("exec sleep 5");
  bad |= exercise("trap '' TERM; exec sleep 5");
  bad |= exercise("trap '' TERM; sleep 5 & wait");
  bad |= exercise("exec cat >/dev/null");
  setenv("DM_AUDIO_LATENCY", "nan", 1);
  bad |= exercise("exec cat >/dev/null");
  if (!bad) puts("ok: pipe audio closes after early exit, blocked input and normal playback");
  return bad;
}
