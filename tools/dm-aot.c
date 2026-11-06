/* Experimental ahead-of-time pixel compiler. Reuses the interpreter's opcode
 * bodies verbatim, so math/texture semantics have one source of truth.
 * Run from the repository root: dm-aot module.dms > build/native.c */
#include "dm.h"
#include <stdlib.h>
#include <string.h>

static const char *ids[] = {
#define X(id, name, n, k, imm) #id,
  DM_OPLIST(X)
#undef X
};
static char *body[DM_OP_COUNT];
static void die(const char *msg) { fprintf(stderr, "dm-aot: %s\n", msg); exit(1); }
static void read_bodies(void) {
  FILE *f = fopen("src/dm_vm.c", "rb");
  if (!f) die("cannot read src/dm_vm.c");
  if (fseek(f, 0, SEEK_END)) die("cannot seek VM source");
  long n = ftell(f); if (n < 0) die("cannot size VM source"); rewind(f);
  char *text = malloc((size_t)n + 1);
  if (!text || fread(text, 1, n, f) != (size_t)n) die("cannot read VM source");
  text[n] = 0; fclose(f);
  char *p = strstr(text, "void dm_vm_run(");
  if (!p) die("VM function not found");
  char *limit = strstr(p, "\n      default:");
  if (!limit) die("VM switch not found");
  while ((p = strstr(p, "\n      case DM_OP_")) && p < limit) {
    p += 7;
    int aliases[DM_OP_COUNT], count = 0;
    do {
      char *colon = strchr(p, ':');
      if (!colon) die("malformed VM case");
      int op;
      for (op = 0; op < DM_OP_COUNT; op++)
        if ((size_t)(colon - (p + 11)) == strlen(ids[op]) && !strncmp(p + 11, ids[op], strlen(ids[op]))) break;
      if (op == DM_OP_COUNT) die("unknown VM case");
      aliases[count++] = op;
      p = colon + 1;
      while (*p == ' ' || *p == '\n') p++;
    } while (!strncmp(p, "case DM_OP_", 11));
    char *end = strstr(p, "\n      case DM_OP_");
    if (!end || end > limit) end = limit;
    char *s = malloc((size_t)(end - p) + 1);
    if (!s) die("out of memory");
    memcpy(s, p, (size_t)(end - p)); s[end - p] = 0;
    for (int i = 0; i < count; i++) body[aliases[i]] = s;
    p = end;
  }
  for (int i = 0; i < DM_OP_COUNT; i++) if (!body[i]) die(ids[i]);
  free(text);
}
static unsigned u16(const uint8_t *p) { return p[0] | ((unsigned)p[1] << 8); }
static void emit(const dm_module *m, const dm_prog *p) {
  unsigned counts[DM_MAX_LOOP_DEPTH], depth = 0;
  printf("static void native_%u(dm_vm *vm) {\nfloat st[DM_MAX_STACK+4]; int sp=0;\n"
         "const float *in=vm->in; float a,b,c,d;\n", p->off);
  for (uint32_t off = p->off; off < p->off + p->len;) {
    int op = m->code[off]; const uint8_t *imm = m->code + off + 1;
    printf("L%u:;\n", off);
    switch (op) {
      case DM_OP_RET: puts("return;"); break;
      case DM_OP_IFZ: printf("if (POP()==0.0f) goto L%u;\n", off + 3 + u16(imm)); break;
      case DM_OP_ELSE: printf("goto L%u;\n", off + 3 + u16(imm)); break;
      case DM_OP_ENDIF: break;
      case DM_OP_LOOP:
        counts[depth++] = u16(imm + 1);
        printf("vm->locals[%u]=0.0f;\n", imm[0]);
        if (!u16(imm + 1)) printf("goto L%u;\n", off + 6 + u16(imm + 3));
        break;
      case DM_OP_ENDL:
        printf("vm->locals[%u]+=1.0f; if ((int)vm->locals[%u]<%u) goto L%u;\n",
               imm[0], imm[0], counts[--depth], off + 4 - u16(imm + 1)); break;
      default:
        printf("{ const uint8_t *pc=aot_code+%u; do { %s } while(0); }\n", off + 1, body[op]);
    }
    off += 1 + dm_ops[op].imm;
  }
  printf("L%u:; return;\n}\n", p->off + p->len);
}
int main(int argc, char **argv) {
  if (argc != 2) die("usage: dm-aot MODULE.dms");
  static dm_module m;
  if (dm_module_read(&m, argv[1])) die(dm_error());
  read_bodies();
  puts("/* Generated from verified bytecode. Do not edit. */\n"
       "#define dm_vm_run dm_vm_interpret\n#include \"src/dm_vm.c\"\n#undef dm_vm_run\n"
       "#define PUSH(v) (st[sp++] = (v))\n#define POP() (st[--sp])\n"
       "#define TOP (st[sp-1])\n#define IMM8() (*pc++)");
  printf("static const uint8_t aot_code[%u]={", m.codelen);
  for (uint32_t i = 0; i < m.codelen; i++) printf("%s%u", i ? "," : "", m.code[i]);
  puts("};");
  if (m.has_frame) emit(&m, &m.frame);
  for (int i = 0; i < m.npixel; i++) emit(&m, &m.pixel[i]);
  if (m.has_post) emit(&m, &m.post);
  puts("static const dm_module *bound_module;\n"
       "int dm_aot_bind(const dm_module *m) {\n"
       "if(m->codelen!=sizeof aot_code || memcmp(m->code,aot_code,sizeof aot_code)) return -1;\n"
       "bound_module=m; return 0; }\n"
       "void dm_vm_run(dm_vm *vm,const uint8_t *code,uint32_t len) {\n"
       "if(vm->m==bound_module && bound_module) switch(code-bound_module->code) {");
  const dm_prog *progs[DM_MAX_PIXEL + 2]; int n = 0;
  if (m.has_frame) progs[n++] = &m.frame;
  for (int i = 0; i < m.npixel; i++) progs[n++] = &m.pixel[i];
  if (m.has_post) progs[n++] = &m.post;
  for (int i = 0; i < n; i++) printf("case %u: if(len==%u) { native_%u(vm); return; } break;\n", progs[i]->off, progs[i]->len, progs[i]->off);
  puts("}\ndm_vm_interpret(vm,code,len);\n}");
  dm_module_free(&m);
  return ferror(stdout) ? 1 : 0;
}
