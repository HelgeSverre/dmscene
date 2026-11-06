/* Integration checks for structured branches and embedded PPM textures. */
#include "dm.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int near(float a, float b) { return fabsf(a - b) < 0.002f; }
static int render(const dm_module *m, int program, float x, float y, float r, float g, float b) {
  float in[DM_IN_COUNT] = {0}, uni[DM_MAX_UNIFORMS] = {0}, state[DM_MAX_STATE] = {0};
  uint32_t rng = 1;
  dm_vm vm = {0}; vm.in = in; vm.uni = uni; vm.state = state; vm.m = m; vm.rng = &rng;
  in[DM_IN_X] = x; in[DM_IN_Y] = y;
  dm_vm_run(&vm, m->code + m->pixel[program].off, m->pixel[program].len);
  return near(vm.out[0], r) && near(vm.out[1], g) && near(vm.out[2], b);
}

int main(void) {
  const char *ppm = "build/tests/branch-chart.ppm";
  const char *dms = "build/tests/branch-chart.dms";
  FILE *f = fopen(ppm, "wb");
  if (!f) return 1;
  fputs("P6\n# header comment\n2 2\n255\r\n", f);
  const unsigned char rgb[] = {35, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255};
  fwrite(rgb, 1, sizeof rgb, f); fclose(f);
  const char *src =
    "song channels=1\n"
    "texture chart \"branch-chart.ppm\"\n"
    "pixel {\n"
    "  if (x < 0.5) {\n"
    "    repeat i 2 {\n"
    "      if (i == 0) {\n"
    "        rgb(1, 0, 0)\n"
    "      } else {\n"
    "        rgb(0, 1, 0)\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "  else {\n"
    "    q = tex(chart, x, y)\n"
    "    rgb(q, r1, r2)\n"
    "  }\n"
    "}\n"
    "pixel raw {\n"
    "  q = tex(chart, x, y)\n"
    "  rgb(q, r1, r2)\n"
    "}\n"
    "pattern 0 rows=1 {\n"
    "  .\n"
    "}\n";
  dm_module m = {0}, loaded = {0};
  if (dm_compile(src, "build/tests/branch-chart.dmt", &m)) { fprintf(stderr, "%s\n", dm_error()); return 1; }
  if (!render(&m, 0, 0.25f, 0.25f, 0, 1, 0) ||
      !render(&m, 0, 0.75f, 0.25f, 0, 1, 0) ||
      !render(&m, 0, 0.5f, 0.5f, 290.0f / 1020.0f, 0.5f, 0.5f) ||
      !render(&m, 1, 0, 0, 35.0f / 255.0f, 0, 0)) {
    fprintf(stderr, "branch or texture sample wrong\n"); return 1;
  }
  if (dm_module_write(&m, dms) || dm_module_read(&loaded, dms) ||
      !render(&loaded, 0, 0.5f, 0.5f, 290.0f / 1020.0f, 0.5f, 0.5f)) {
    fprintf(stderr, "texture module round trip failed: %s\n", dm_error()); return 1;
  }
  dm_module_free(&loaded);
  const unsigned char badvar[] = {'D', 'M', 'S', DM_VERSION, 'Q', 0xff, 0xff, 0xff, 0xff, 0xff,
                                  0xff, 0xff, 0xff, 0xff, 0x7f};
  if (dm_module_load(&loaded, badvar, sizeof badvar) == 0) {
    fprintf(stderr, "overflowing module chunk length accepted\n"); return 1;
  }
  uint32_t off = m.pixel[0].off, ifpos = 0, elsepos = 0, texpos = 0;
  for (uint32_t i = off; i < off + m.pixel[0].len; i += 1 + dm_ops[m.code[i]].imm) {
    if (!ifpos && m.code[i] == DM_OP_IFZ) ifpos = i;
    if (!elsepos && m.code[i] == DM_OP_ELSE) elsepos = i;
    if (!texpos && m.code[i] == DM_OP_TEX) texpos = i;
  }
  if (!ifpos || !elsepos || !texpos) return 1;
  unsigned char skiplo = m.code[ifpos + 1], skiphi = m.code[ifpos + 2];
  m.code[ifpos + 1] = 0xff; m.code[ifpos + 2] = 0xff;
  if (dm_prog_verify(&m, &m.pixel[0], "broken") == 0) { fprintf(stderr, "bad branch accepted\n"); return 1; }
  m.code[ifpos + 1] = skiplo; m.code[ifpos + 2] = skiphi;
  m.code[ifpos + 1] = 0; m.code[ifpos + 2] = 0;
  if (dm_prog_verify(&m, &m.pixel[0], "broken") == 0) { fprintf(stderr, "in-range bad branch accepted\n"); return 1; }
  m.code[ifpos + 1] = skiplo; m.code[ifpos + 2] = skiphi;
  unsigned char elselo = m.code[elsepos + 1], elsehi = m.code[elsepos + 2];
  m.code[elsepos + 1] = 0; m.code[elsepos + 2] = 0;
  if (dm_prog_verify(&m, &m.pixel[0], "broken") == 0) { fprintf(stderr, "bad else accepted\n"); return 1; }
  m.code[elsepos + 1] = elselo; m.code[elsepos + 2] = elsehi;
  m.code[texpos + 1] = 255;
  if (dm_prog_verify(&m, &m.pixel[0], "broken") == 0) { fprintf(stderr, "bad texture id accepted\n"); return 1; }
  dm_module_free(&m);
  const char *bad = "song channels=1\npixel {\n  if (x > 0) {\n    hidden = 1\n  }\n  rgb(hidden, 0, 0)\n}\npattern 0 rows=1 {\n  .\n}\n";
  if (dm_compile(bad, "branch-scope.dmt", &m) == 0) { fprintf(stderr, "branch local escaped scope\n"); return 1; }
  f = fopen(ppm, "wb");
  if (!f) return 1;
  fputs("P6\n2 2\n255\n", f); fputc(35, f); fclose(f);
  if (dm_compile(src, "build/tests/branch-chart.dmt", &m) == 0 || !strstr(dm_error(), "truncated pixels")) {
    fprintf(stderr, "truncated PPM accepted: %s\n", dm_error()); return 1;
  }
  remove(ppm); remove(dms);
  puts("ok: branches, texture sampling, round trip, corrupt branch, local scope");
  return 0;
}
