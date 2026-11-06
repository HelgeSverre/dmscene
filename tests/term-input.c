/* Streaming terminal input: fragments and payload bytes must not become keys. */
#include "dm.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t restored;
static void old_handler(int sig) { (void)sig; restored = 1; }
static int feed(int fd, const char *s, size_t n) { return write(fd, s, n) == (ssize_t)n ? 0 : 1; }
static int sequence(dm_term *t, int fd, const char *s, int expected) {
  size_t n = strlen(s);
  for (size_t i = 0; i < n; i++) {
    if (feed(fd, s+i, 1) || dm_term_key(t) != (i+1 == n ? expected : -1)) return 1;
  }
  return 0;
}
#define CHECK(expr, label) do { if (!(expr)) { fprintf(stderr, "FAIL: %s\n", label); return 1; } } while(0)
int main(void) {
  int p[2], oldin = dup(0), oldout = dup(1); FILE *null = fopen("/dev/null", "w");
  CHECK(!pipe(p) && oldin >= 0 && oldout >= 0 && null, "pipe setup");
  CHECK(dup2(p[0], 0) >= 0 && dup2(fileno(null), 1) >= 0, "fd setup");
  struct sigaction sa = {0}, saved; sa.sa_handler = old_handler;
  sigaction(SIGINT, &sa, &saved);
  dm_term *t = dm_term_open(); CHECK(t, "open");
  CHECK(!sequence(t,p[1],"\033[A",DM_KEY_UP), "fragmented CSI arrow");
  CHECK(!sequence(t,p[1],"\033OD",DM_KEY_LEFT), "fragmented SS3 arrow");
  CHECK(!sequence(t,p[1],"\033[1;5C",DM_KEY_RIGHT), "modified arrow");
  CHECK(!sequence(t,p[1],"\033[<64;12;7M",DM_KEY_SCRUB_BACK), "vertical touchpad back");
  CHECK(!sequence(t,p[1],"\033[<65;12;7M",DM_KEY_SCRUB_FORWARD), "vertical touchpad forward");
  CHECK(!sequence(t,p[1],"\033[<66;12;7M",DM_KEY_SCRUB_BACK), "horizontal touchpad back");
  CHECK(!sequence(t,p[1],"\033[<67;12;7M",DM_KEY_SCRUB_FORWARD), "horizontal touchpad forward");
  CHECK(!sequence(t,p[1],"\033[<65;12;7m",-1), "mouse release ignored");
  CHECK(!sequence(t,p[1],"\033[M qq",-1), "legacy mouse q coordinates ignored");
  CHECK(!sequence(t,p[1],"\033[M`qq",DM_KEY_SCRUB_BACK), "legacy wheel");
  CHECK(!sequence(t,p[1],"\033[123q",-1), "unknown CSI q terminator ignored");
  CHECK(!sequence(t,p[1],"\033]0;qqq\007",-1), "OSC payload ignored");
  CHECK(!sequence(t,p[1],"\033Pqqq\033\\",-1), "DCS payload ignored");
  CHECK(!sequence(t,p[1],"\033[200~qqq\033[A\033[201~",-1), "bracketed paste ignored");
  char long_input[1024]; memset(long_input,'9',sizeof long_input);
  CHECK(!sequence(t,p[1],"\033[",-1), "start long CSI");
  CHECK(!feed(p[1],long_input,sizeof long_input) && dm_term_key(t)==-1, "long parameter bounded");
  CHECK(!sequence(t,p[1],"q",-1), "long CSI terminator swallowed");
  CHECK(!sequence(t,p[1],"\033[200~",-1), "start long paste");
  memset(long_input,'q',sizeof long_input);
  for(int i=0;i<10;i++) CHECK(!feed(p[1],long_input,sizeof long_input) && dm_term_key(t)==-1, "long pasted q swallowed");
  CHECK(!sequence(t,p[1],"\033[201~",-1), "end long paste");
  CHECK(!feed(p[1],"\033[A\033[Bq",7), "queued keys");
  CHECK(dm_term_key(t)==DM_KEY_UP && dm_term_key(t)==DM_KEY_DOWN && dm_term_key(t)=='q', "queued events preserved");
  CHECK(!feed(p[1],"rq",2) && dm_term_key(t)=='r', "buffered wait setup");
  double start = dm_now(); dm_term_wait(t, .5);
  CHECK(dm_now()-start < .2 && dm_term_key(t)=='q', "wait ignored buffered input");
  pid_t child = fork(); CHECK(child >= 0, "wake test fork");
  if (!child) { dm_sleep(.020); _exit(feed(p[1],"q",1)); }
  start = dm_now(); dm_term_wait(t, 1);
  CHECK(dm_now()-start < .3 && dm_term_key(t)=='q', "input failed to wake frame wait");
  int child_status;
  CHECK(waitpid(child,&child_status,0)==child && WIFEXITED(child_status) && !WEXITSTATUS(child_status), "wake test child");
  raise(SIGTSTP); CHECK(dm_term_key(t)==DM_KEY_SUSPEND, "suspend signal event");
  raise(SIGINT); CHECK(dm_term_key(t)=='q', "interrupt signal event");
  dm_term_close(t); raise(SIGINT); CHECK(restored, "signal handler restored");
  t=dm_term_open(); CHECK(t && dm_term_key(t)==-1, "reopen clears interrupt");
  close(p[1]); CHECK(dm_term_key(t)==-1, "closed input");
  start=dm_now(); dm_term_wait(t,.020);
  CHECK(dm_now()-start >= .010, "closed input made frame wait spin");
  dm_term_close(t);
  sigaction(SIGINT,&saved,NULL);
  fclose(null); close(p[0]); dup2(oldin,0); dup2(oldout,1); close(oldin); close(oldout);
  puts("ok: terminal arrows, touchpad scrubbing, paste, escape fragments and signals");
  return 0;
}
