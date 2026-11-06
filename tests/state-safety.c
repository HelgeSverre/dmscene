/* Exercise compiler/verifier transitions through their public boundary. */
#include "dm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char prefix[] = "song channels=1\npattern 0 rows=1 {\n .\n}\npixel {\n";
static int rejected(const char *body) {
  dm_module m = {0}; char src[2048];
  snprintf(src, sizeof src, "%s%s\n}\n", prefix, body);
  int rc = dm_compile(src, "loop-test", &m);
  dm_module_free(&m);
  if (!rc) { fprintf(stderr, "FAIL: accepted unsafe loop: %s\n", body); return 1; }
  return 0;
}

int main(void) {
  int bad = 0;
  bad |= rejected("repeat i 2 {\n i = -1\n}");
  bad |= rejected("repeat i 2 {\n repeat i 1 {\n  rgb(0,0,0)\n }\n}");
  dm_module m = {0}; char src[2048];
  snprintf(src, sizeof src, "%srepeat i 2 {\n rgb(i,0,0)\n}\n}\n", prefix);
  if (dm_compile(src, "valid-loop", &m)) return 2;
  unsigned at = m.pixel[0].off;
  while (at < m.pixel[0].off + m.pixel[0].len && m.code[at] != DM_OP_ENDL)
    at += 1 + dm_ops[m.code[at]].imm;
  if (at >= m.codelen) return 2;
  m.code[at + 1] = 255;
  if (!dm_module_verify(&m)) { fprintf(stderr, "FAIL: accepted ENDL local 255\n"); bad = 1; }
  m.code[at + 1] = 1; m.pixel[0].nlocals = 2;
  if (!dm_module_verify(&m)) { fprintf(stderr, "FAIL: accepted mismatched ENDL local\n"); bad = 1; }
  dm_module_free(&m);
  snprintf(src, sizeof src, "%sa = 0\nrepeat i 2 {\n repeat j 3 {\n a += 1\n }\n}\nrepeat i 0 {\n a = 99\n}\nrepeat i 2 {\n a += 1\n}\nrgb(a,0,0)\n}\n", prefix);
  if (dm_compile(src, "valid-nesting", &m)) return 2;
  dm_vm vm = {0};
  dm_vm_run(&vm, m.code + m.pixel[0].off, m.pixel[0].len);
  if (vm.out[0] != 8) { fprintf(stderr, "FAIL: valid nested/reused/zero-count loop result\n"); bad = 1; }
  dm_module_free(&m);
  if (!bad) puts("ok: counted loops cannot escape their verifier state");
  return bad;
}
