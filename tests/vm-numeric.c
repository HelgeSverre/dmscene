#include "dm.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  const char *expressions[] = {
    "pal(p, pow(10,1000))", "smp(s, sin(pow(10,1000)))",
    "wave(s, pow(10,1000))", "glyph(pow(10,1000),0,0)",
    "str(t, pow(10,1000))", "cget(pow(10,1000))",
    "mandel(0,0,sin(pow(10,1000)))", "hsv(pow(10,1000),1,1)",
    "tap(pow(10,1000),sin(pow(10,1000)))", "select(pow(10,1000))"
  };
  for (unsigned i = 0; i < sizeof expressions / sizeof *expressions; i++) {
    dm_module m = {0}; char src[2048];
    snprintf(src, sizeof src,
      "song channels=1\npattern 0 rows=1 {\n .\n}\n"
      "palette p {\n 0 0 0 0\n 1 1 1 1\n}\n"
      "sample s \"assets/wt.wav\"\ntext t \"abc\"\npixel {\n rgb(%s,0,0)\n}\n", expressions[i]);
    if (i == 0 || i == 7 || i == 9) {
      char *p = strstr(src, " rgb("); snprintf(p, (size_t)(src + sizeof src - p), " %s\n}\n", expressions[i]);
    }
    if (i >= 8) {
      char *kind = strstr(src, "pixel {");
      /* Keep the same source length while switching the program context. */
      memcpy(kind, i == 8 ? "post  " : "frame ", 6);
    }
    if (dm_compile(src, "tests/numeric.dmt", &m)) { fprintf(stderr, "%s\n", dm_error()); return 2; }
    float inputs[DM_IN_COUNT] = {0}; inputs[DM_IN_SR] = DM_SR;
    dm_vm vm = {0}; vm.m = &m; vm.pal = m.pal; vm.npal = m.npal; vm.in = inputs;
    uint8_t pixel[3] = {10, 20, 30}; vm.cur = pixel; vm.pw = vm.ph = 1;
    const dm_prog *p = i == 8 ? &m.post : i == 9 ? &m.frame : &m.pixel[0];
    dm_vm_run(&vm, m.code + p->off, p->len);
    if (!isfinite(vm.out[0])) { fprintf(stderr, "FAIL: nonfinite lookup result: %s\n", expressions[i]); return 1; }
    dm_module_free(&m);
  }
  puts("ok: nonfinite arithmetic cannot become invalid lookup indices");
  return 0;
}
