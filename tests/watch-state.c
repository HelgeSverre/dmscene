#define main dm_tool_main
#include "../tools/dm.c"
#undef main
#include <fcntl.h>

int main(void) {
  char path[] = "/tmp/dmscene-watch-XXXXXX";
  int fd = mkstemp(path); if (fd < 0) return 2;
  struct timespec times[2] = {{1700000000, 100}, {1700000000, 100}};
  if (futimens(fd, times)) return 2;
  file_stamp first = mtime(path);
  times[1].tv_nsec = 200;
  if (futimens(fd, times)) return 2;
  file_stamp second = mtime(path);
  close(fd);
  if (same_stamp(first, second)) { fprintf(stderr, "FAIL: watch misses saves within one second\n"); return 1; }
  char replacement[] = "/tmp/dmscene-replacement-XXXXXX";
  fd = mkstemp(replacement); if (fd < 0) return 2;
  times[1].tv_nsec = 100;
  if (futimens(fd, times) || rename(replacement, path)) return 2;
  close(fd);
  if (same_stamp(first, mtime(path))) { fprintf(stderr, "FAIL: watch misses atomic replacement\n"); return 1; }
  unlink(path);
  if (same_stamp(mtime(path), second)) return 1;

  dm_module m = {0};
  if (load_any("demos/pulse.dmt", &m)) return 2;
  dm_cell *old_cells = m.pat[0].cells;
  char old_title[sizeof m.title]; memcpy(old_title, m.title, sizeof old_title);
  /* Binary parse fails after resource allocations. Retry must preserve the
   * active module and release the incomplete candidate each time. */
  char binary[256]; snprintf(binary, sizeof binary, "%s.dms", path);
  if (dm_module_write(&m, binary)) return 2;
  struct stat st; if (stat(binary, &st) || truncate(binary, st.st_size - 1)) return 2;
  for (int i = 0; i < 20; i++) {
    if (!load_any(binary, &m) || m.pat[0].cells != old_cells || memcmp(m.title, old_title, sizeof old_title)) {
      fprintf(stderr, "FAIL: failed reload changed the active module\n"); return 1;
    }
  }
  if (dm_module_write(&m, binary) || load_any(binary, &m) || dm_module_verify(&m)) return 2;
  dm_module_free(&m); unlink(binary);
  puts("ok: watch rapid saves, atomic replacement and failed-load recovery");
  return 0;
}
