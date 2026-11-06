/* Rebinding edited bytecode at the same module address must clear AOT dispatch. */
#include "dm.h"
#include <stdio.h>
extern int dm_aot_bind(const dm_module *m);
int main(int argc, char **argv) {
  static dm_module m;
  if (argc != 2 || dm_module_read(&m, argv[1]) || dm_aot_bind(&m) || !m.npixel) return 1;
  dm_prog *p = &m.pixel[0];
  m.code[p->off] = DM_OP_RET;
  if (!dm_aot_bind(&m)) { fprintf(stderr, "edited module still bound\n"); return 1; }
  float in[DM_IN_COUNT] = {0}, uni[DM_MAX_UNIFORMS] = {0}, state[DM_MAX_STATE] = {0};
  uint32_t rng = 1;
  dm_vm vm = {0}; vm.m = &m; vm.in = in; vm.uni = uni; vm.state = state; vm.rng = &rng;
  vm.out[0] = .123f;
  dm_vm_run(&vm, m.code + p->off, p->len);
  if (vm.out[0] != .123f) { fprintf(stderr, "failed bind retained native dispatch\n"); return 1; }
  dm_module_free(&m); puts("ok: failed native rebind clears specialized dispatch"); return 0;
}
