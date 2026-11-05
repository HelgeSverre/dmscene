/* dm_compile.c - .dmt text -> dm_module.
 *
 * Top level is line oriented:
 *   title Name
 *   song bpm=128 speed=6 rpb=4 channels=5 loop=1 volume=0.8
 *   order 0 1 2 1
 *   uniform zoom spin
 *   palette NAME {  pos r g b  ... }
 *   inst NAME { program }         pattern N rows=32 { rows }
 *   frame { program }  pixel { program }  master { program }
 *
 * Programs are expression statements, newline separated:
 *   x = expr      x += expr      $state = expr      stmt(args)
 * Expressions: + - * / % ^, comparisons, && || !, ?:, calls, pi tau e, r1 r2 r3.
 *
 * Pattern rows:  note inst vol fx | note inst vol fx | ...
 *   note: C-4 C#4 ... off/=== ; inst: decimal ; vol: hex 00-40 ; fx: 3 chars e.g. A0F
 *   any field may be '.' / '..' / '...' / '---' for "none".
 */
#include "dm.h"
#include <ctype.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ input names */
typedef struct { const char *name; int idx; int kinds; } inname;
#define K_FR (1 << DM_PK_FRAME)
#define K_PX (1 << DM_PK_PIXEL)
#define K_IN (1 << DM_PK_INST)
#define K_MA (1 << DM_PK_MASTER)
#define K_CH (1 << DM_PK_CHAN)
#define K_PO (1 << DM_PK_POST)
#define K_ALL (K_FR | K_PX | K_IN | K_MA | K_CH | K_PO)
static const inname g_inputs[] = {
  {"time", DM_IN_T, K_ALL}, {"t", DM_IN_T, K_FR | K_PX | K_MA | K_CH | K_PO}, {"t", DM_IN_VT, K_IN},
  {"order", DM_IN_ORDER, K_ALL}, {"pos", DM_IN_POS, K_ALL},
  {"mp0", DM_IN_MP0, K_ALL}, {"mp1", DM_IN_MP1, K_ALL}, {"mp2", DM_IN_MP2, K_ALL}, {"mp3", DM_IN_MP3, K_ALL},
  {"x", DM_IN_XIN, K_CH},
  {"beat", DM_IN_BEAT, K_ALL}, {"lbeat", DM_IN_LBEAT, K_ALL}, {"llen", DM_IN_LLEN, K_ALL},
  {"bar", DM_IN_BAR, K_ALL}, {"row", DM_IN_ROW, K_ALL}, {"tick", DM_IN_TICK, K_ALL}, {"bpm", DM_IN_BPM, K_ALL},
  {"sr", DM_IN_SR, K_ALL}, {"frame", DM_IN_FRAME, K_FR | K_PX | K_PO}, {"w", DM_IN_W, K_FR | K_PX | K_PO}, {"h", DM_IN_H, K_FR | K_PX | K_PO},
  {"aspect", DM_IN_ASPECT, K_FR | K_PX | K_PO},
  {"x", DM_IN_X, K_PX | K_PO}, {"y", DM_IN_Y, K_PX | K_PO}, {"u", DM_IN_U, K_PX | K_PO}, {"v", DM_IN_V, K_PX | K_PO},
  {"ph", DM_IN_PH, K_IN}, {"freq", DM_IN_FREQ, K_IN}, {"rel", DM_IN_REL, K_IN}, {"vel", DM_IN_VVEL, K_IN},
  {"note", DM_IN_VNOTE, K_IN}, {"p0", DM_IN_P0, K_IN}, {"p1", DM_IN_P1, K_IN}, {"p2", DM_IN_P2, K_IN}, {"p3", DM_IN_P3, K_IN},
  {"chan", DM_IN_CHAN, K_IN},
  {"l", DM_IN_L, K_MA}, {"r", DM_IN_R, K_MA},
  {NULL, 0, 0}
};

const char *dm_input_name(int idx) {
  static char tmp[16];
  for (const inname *n = g_inputs; n->name; n++) if (n->idx == idx && strcmp(n->name, "t")) return n->name;
  if (idx == DM_IN_T) return "time";
  if (idx >= DM_IN_TRIG0 && idx < DM_IN_TRIG0 + DM_MAX_CHANNELS) { snprintf(tmp, sizeof tmp, "trig(%d)", idx - DM_IN_TRIG0); return tmp; }
  if (idx >= DM_IN_VEL0 && idx < DM_IN_VEL0 + DM_MAX_CHANNELS) { snprintf(tmp, sizeof tmp, "vel(%d)", idx - DM_IN_VEL0); return tmp; }
  if (idx >= DM_IN_NOTE0 && idx < DM_IN_NOTE0 + DM_MAX_CHANNELS) { snprintf(tmp, sizeof tmp, "note(%d)", idx - DM_IN_NOTE0); return tmp; }
  if (idx >= DM_IN_CENV0 && idx < DM_IN_CENV0 + DM_MAX_CHANNELS) { snprintf(tmp, sizeof tmp, "cenv(%d)", idx - DM_IN_CENV0); return tmp; }
  snprintf(tmp, sizeof tmp, "in%d", idx);
  return tmp;
}

/* ------------------------------------------------------------ compiler state */
#define DM_MAX_FNS 32
#define DM_MAX_FNARGS 8
typedef struct { char name[DM_NAME_LEN]; char params[DM_MAX_FNARGS][DM_NAME_LEN]; int nparams; char *body; int line; } cfn;
typedef struct {
  dm_module *m;
  const char *srcname;
  char srcdir[1024];
  const char *src; size_t pos; int line;
  jmp_buf jb;
  char uni[DM_MAX_UNIFORMS][DM_NAME_LEN];
  cfn fns[DM_MAX_FNS]; int nfns;
  int ncalls;
} cc;

static void fail(cc *c, int line, const char *fmt, ...) {
  char msg[300];
  va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
  dm_set_error("%s:%d: %s", c->srcname, line, msg);
  longjmp(c->jb, 1);
}

static void emit8(cc *c, int line, unsigned b) {
  if (c->m->codelen >= DM_MAX_CODE) fail(c, line, "code segment full");
  c->m->code[c->m->codelen++] = (uint8_t)b;
}

/* ------------------------------------------------------------ expression lexer */
enum { T_EOF, T_NL, T_NUM, T_IDENT, T_STATE, T_OP, T_LP, T_RP, T_COMMA, T_Q, T_COLON, T_LB, T_RB };
typedef struct { int type; char text[DM_NAME_LEN * 2]; float num; int line; } tok;

typedef struct {
  cc *c;
  const char *s; size_t p; int line; int paren;
  int kind;
  char locals[DM_MAX_LOCALS][DM_NAME_LEN]; int nlocals;   /* names may carry a scope prefix */
  char states[DM_MAX_STATE][DM_NAME_LEN]; int nstate;
  int ndelay, nverb;
  tok cur; int peeked;
  int depth, maxdepth;
  char scope[24];      /* local-name prefix while inlining a fn body */
  int fndepth, maxlocals;
  char aliask[32][DM_NAME_LEN * 2], aliasv[32][DM_NAME_LEN]; int nalias;   /* fn params bound to resource names */
} ec;

static tok lex(ec *e) {
  tok t; memset(&t, 0, sizeof t);
  const char *s = e->s;
  for (;;) {
    char ch = s[e->p];
    if (ch == ' ' || ch == '\t' || ch == '\r') { e->p++; continue; }
    if (ch == '#' || (ch == '/' && s[e->p + 1] == '/')) { while (s[e->p] && s[e->p] != '\n') e->p++; continue; }
    if (ch == ';') { e->p++; if (!e->paren) { t.type = T_NL; t.line = e->line; return t; } continue; }
    if (ch == '\n') { e->p++; e->line++; if (!e->paren) { t.type = T_NL; t.line = e->line - 1; return t; } continue; }
    break;
  }
  t.line = e->line;
  char ch = s[e->p];
  if (!ch) { t.type = T_EOF; return t; }
  if (isdigit((unsigned char)ch) || (ch == '.' && isdigit((unsigned char)s[e->p + 1]))) {
    char *endp; t.num = strtof(s + e->p, &endp);
    if (ch == '0' && (s[e->p + 1] == 'x' || s[e->p + 1] == 'X')) t.num = (float)strtol(s + e->p, &endp, 16);
    e->p = (size_t)(endp - s); t.type = T_NUM; return t;
  }
  if (isalpha((unsigned char)ch) || ch == '_' || ch == '$') {
    size_t i = 0;
    if (ch == '$') { t.type = T_STATE; e->p++; } else t.type = T_IDENT;
    while (isalnum((unsigned char)s[e->p]) || s[e->p] == '_') { if (i + 1 < sizeof t.text) t.text[i++] = s[e->p]; e->p++; }
    t.text[i] = 0;
    if (t.type == T_STATE && !i) fail(e->c, t.line, "expected state name after $");
    return t;
  }
  static const char *two[] = {"<=", ">=", "==", "!=", "&&", "||", "+=", "-=", "*=", "/=", NULL};
  for (int i = 0; two[i]; i++) if (s[e->p] == two[i][0] && s[e->p + 1] == two[i][1]) {
    strcpy(t.text, two[i]); e->p += 2; t.type = T_OP; return t;
  }
  e->p++;
  switch (ch) {
    case '(': t.type = T_LP; e->paren++; return t;
    case ')': t.type = T_RP; if (e->paren) e->paren--; return t;
    case ',': t.type = T_COMMA; return t;
    case '{': t.type = T_LB; return t;
    case '}': t.type = T_RB; return t;
    case '?': t.type = T_Q; return t;
    case ':': t.type = T_COLON; return t;
    case '+': case '-': case '*': case '/': case '%': case '^': case '<': case '>': case '!': case '=':
      t.text[0] = ch; t.type = T_OP; return t;
    default: fail(e->c, t.line, "unexpected character '%c'", ch);
  }
  return t;
}
static tok *peek(ec *e) { if (!e->peeked) { e->cur = lex(e); e->peeked = 1; } return &e->cur; }
static tok next(ec *e) { tok t = *peek(e); e->peeked = 0; return t; }
static void expect(ec *e, int type, const char *what) { tok t = next(e); if (t.type != type) fail(e->c, t.line, "expected %s", what); }

/* ------------------------------------------------------------ emission with depth tracking */
static void push_depth(ec *e, int d) { e->depth += d; if (e->depth > e->maxdepth) e->maxdepth = e->depth; if (e->depth > DM_MAX_STACK - 2) fail(e->c, e->line, "expression too deep"); }
static void op(ec *e, int opcode) { emit8(e->c, e->line, opcode); push_depth(e, dm_op_stack_effect(opcode)); }
static void op_imm(ec *e, int opcode, unsigned imm) { emit8(e->c, e->line, opcode); emit8(e->c, e->line, imm); push_depth(e, dm_op_stack_effect(opcode)); }
static void push_const(ec *e, float v) {
  if (v == floorf(v) && v >= -128.0f && v <= 127.0f) { op_imm(e, DM_OP_PUSHB, (unsigned)(int8_t)(int)v & 255); return; }
  emit8(e->c, e->line, DM_OP_PUSHK);
  uint8_t b[4]; memcpy(b, &v, 4);
  for (int i = 0; i < 4; i++) emit8(e->c, e->line, b[i]);
  push_depth(e, 1);
}

static void scoped(const ec *e, const char *name, char *out, size_t cap) {
  if (e->scope[0]) snprintf(out, cap, "%s%s", e->scope, name); else snprintf(out, cap, "%s", name);
}
static int find_name(char tab[][DM_NAME_LEN], int n, const char *name) {
  for (int i = 0; i < n; i++) if (!strcmp(tab[i], name)) return i;
  return -1;
}
static int find_input(ec *e, const char *name) {
  for (const inname *n = g_inputs; n->name; n++) if ((n->kinds & (1 << e->kind)) && !strcmp(n->name, name)) return n->idx;
  return -1;
}
static int find_uniform(cc *c, const char *name) { return find_name(c->uni, c->m->nuniform, name); }
static int find_text(cc *c, const char *name) { for (int i = 0; i < c->m->ntext; i++) if (!strcmp(c->m->text[i].name, name)) return i; return -1; }
static int find_sample(cc *c, const char *name) { for (int i = 0; i < c->m->nsmp; i++) if (!strcmp(c->m->smp[i].name, name)) return i; return -1; }
static int find_venv(cc *c, const char *name) { for (int i = 0; i < c->m->nvenv; i++) if (!strcmp(c->m->venv[i].name, name)) return i; return -1; }
static int find_evname(cc *c, const char *name) { for (int i = 0; i < c->m->nevname; i++) if (!strcmp(c->m->evname[i], name)) return i; return -1; }
static int find_pixel(cc *c, const char *name) { for (int i = 0; i < c->m->npixel; i++) if (!strcmp(c->m->pixelname[i], name)) return i; return -1; }
static int find_palette(cc *c, const char *name) {
  for (int i = 0; i < c->m->npal; i++) if (!strcmp(c->m->pal[i].name, name)) return i;
  return -1;
}
static int is_resource(cc *c, const char *name) {
  return find_palette(c, name) >= 0 || find_text(c, name) >= 0 || find_sample(c, name) >= 0 || find_venv(c, name) >= 0 ||
         find_evname(c, name) >= 0 || find_pixel(c, name) >= 0;
}
/* a fn parameter bound to a resource name resolves to that name inside the body */
static const char *resolve_name(ec *e, const char *name) {
  char sn[DM_NAME_LEN * 2]; scoped(e, name, sn, sizeof sn);
  for (int i = e->nalias - 1; i >= 0; i--) if (!strcmp(e->aliask[i], sn)) return e->aliasv[i];
  return name;
}
static int alloc_state(ec *e, const char *name, int line) {
  if (e->nstate >= DM_MAX_STATE) fail(e->c, line, "too many state slots (max %d)", DM_MAX_STATE);
  snprintf(e->states[e->nstate], DM_NAME_LEN, "%s", name);
  return e->nstate++;
}
static int find_op(const char *name) {
  for (int i = 0; i < DM_OP_COUNT; i++) if (dm_ops[i].kind != 'I' && !strcmp(dm_ops[i].name, name)) return i;
  return -1;
}

static void parse_expr(ec *e, int minbp);
static void parse_statement(ec *e);

static int parse_args(ec *e, int max) {
  expect(e, T_LP, "'('");
  int n = 0;
  if (peek(e)->type == T_RP) { next(e); return 0; }
  for (;;) {
    if (n >= max) fail(e->c, peek(e)->line, "too many arguments");
    parse_expr(e, 0); n++;
    tok t = next(e);
    if (t.type == T_RP) break;
    if (t.type != T_COMMA) fail(e->c, t.line, "expected ',' or ')'");
  }
  return n;
}

static void parse_call(ec *e, const char *name, int line, int as_statement) {
  cc *c = e->c;
  if (!strcmp(name, "out")) {
    if (!as_statement) fail(c, line, "out() is a statement");
    int n = parse_args(e, 4);
    if (n < 1) fail(c, line, "out() needs 1..4 arguments");
    op(e, DM_OP_OUT1 + n - 1); return;
  }
  if (!strcmp(name, "trig") || !strcmp(name, "vel") || !strcmp(name, "note") || !strcmp(name, "cenv")) {
    expect(e, T_LP, "'('");
    tok t = next(e);
    if (t.type != T_NUM || t.num < 0 || t.num >= DM_MAX_CHANNELS || t.num != floorf(t.num)) fail(c, t.line, "%s(c): c must be a channel number 0..%d", name, DM_MAX_CHANNELS - 1);
    expect(e, T_RP, "')'");
    int base = name[0] == 't' ? DM_IN_TRIG0 : (name[0] == 'v' ? DM_IN_VEL0 : (name[0] == 'c' ? DM_IN_CENV0 : DM_IN_NOTE0));
    op_imm(e, DM_OP_LDI, base + (int)t.num);
    if (as_statement) op(e, DM_OP_POP);
    return;
  }
  if (!strcmp(name, "pal") || !strcmp(name, "mixpal")) {
    if (!as_statement) fail(c, line, "%s() is a statement (it writes the colour accumulator)", name);
    expect(e, T_LP, "'('");
    tok t = next(e);
    if (t.type != T_IDENT) fail(c, t.line, "%s(NAME, ...): first argument is a palette name", name);
    int id = find_palette(c, resolve_name(e, t.text));
    if (id < 0) fail(c, t.line, "unknown palette '%s'", t.text);
    int want = name[0] == 'p' ? 1 : 2;
    for (int i = 0; i < want; i++) { expect(e, T_COMMA, "','"); parse_expr(e, 0); }
    expect(e, T_RP, "')'");
    op_imm(e, name[0] == 'p' ? DM_OP_PAL : DM_OP_PALMIX, id);
    return;
  }
  for (int fi = 0; fi < c->nfns; fi++) {
    if (strcmp(c->fns[fi].name, name)) continue;
    cfn *fn = &c->fns[fi];
    if (e->fndepth >= 6) fail(c, line, "fn nesting too deep (recursion?)");
    char newscope[24]; snprintf(newscope, sizeof newscope, "%d~", c->ncalls++);
    int base_locals = e->nlocals;      /* fn locals are dead after the call: recycle the slots */
    /* evaluate args in the caller's scope, store into the callee's parameter locals */
    expect(e, T_LP, "'('");
    int base_alias = e->nalias;
    for (int i = 0; i < fn->nparams; i++) {
      if (i) expect(e, T_COMMA, "','");
      char nm[DM_NAME_LEN]; snprintf(nm, sizeof nm, "%s%s", newscope, fn->params[i]);
      /* a bare resource name (or an alias to one) binds the parameter as a name, not a value */
      tok *pk = peek(e);
      if (pk->type == T_IDENT) {
        tok save_tok = *pk; size_t save_p = e->p; int save_line = e->line, save_paren = e->paren;
        next(e);
        tok *after = peek(e);
        int is_name = (after->type == T_COMMA || after->type == T_RP);
        { char sn[DM_NAME_LEN]; scoped(e, save_tok.text, sn, sizeof sn);
          if (find_name(e->locals, e->nlocals, sn) >= 0 || find_input(e, save_tok.text) >= 0 || find_uniform(c, save_tok.text) >= 0) is_name = 0; }
        const char *rn = resolve_name(e, save_tok.text);
        if (is_name && is_resource(c, rn)) {
          if (e->nalias >= 32) fail(c, line, "too many name parameters");
          snprintf(e->aliask[e->nalias], sizeof e->aliask[0], "%s", nm);
          snprintf(e->aliasv[e->nalias], DM_NAME_LEN, "%s", rn);
          e->nalias++;
          continue;
        }
        /* not a name: rewind and parse as an expression */
        e->p = save_p; e->line = save_line; e->paren = save_paren; e->cur = save_tok; e->peeked = 1;
      }
      parse_expr(e, 0);
      if (e->nlocals >= DM_MAX_LOCALS) fail(c, line, "too many locals (fn inlining)");
      snprintf(e->locals[e->nlocals], DM_NAME_LEN, "%s", nm);
      op_imm(e, DM_OP_STL, e->nlocals++);
    }
    expect(e, T_RP, "')'");
    /* swap in the body */
    ec saved = *e;
    e->s = fn->body; e->p = 0; e->line = fn->line; e->paren = 0; e->peeked = 0;
    strcpy(e->scope, newscope); e->fndepth++;
    int returned = 0;
    for (;;) {
      tok *t = peek(e);
      if (t->type == T_EOF) break;
      if (t->type == T_NL) { next(e); continue; }
      if (t->type == T_IDENT && !strcmp(t->text, "return")) {
        next(e); parse_expr(e, 0); returned = 1;
        t = peek(e); if (t->type != T_NL && t->type != T_EOF) fail(c, t->line, "return must end the fn");
        while (peek(e)->type == T_NL) next(e);
        if (peek(e)->type != T_EOF) fail(c, peek(e)->line, "statements after return in fn '%s'", name);
        break;
      }
      parse_statement(e);
      t = peek(e);
      if (t->type != T_NL && t->type != T_EOF) fail(c, t->line, "unexpected token after statement");
    }
    if (!returned && !as_statement) fail(c, line, "fn '%s' has no return but is used in an expression", name);
    if (returned && as_statement) op(e, DM_OP_POP);
    /* restore the caller's lexer; keep locals/state/depth */
    e->s = saved.s; e->p = saved.p; e->line = saved.line; e->paren = saved.paren; e->cur = saved.cur; e->peeked = saved.peeked;
    strcpy(e->scope, saved.scope); e->fndepth = saved.fndepth;
    if (e->nlocals > e->maxlocals) e->maxlocals = e->nlocals;
    e->nlocals = base_locals;
    e->nalias = base_alias;
    return;
  }
  int opcode = find_op(name);
  if (opcode < 0) fail(c, line, "unknown function '%s'", name);
  const dm_opinfo *oi = &dm_ops[opcode];
  /* ops whose first argument is a resource name resolved at compile time */
  if (opcode == DM_OP_STR || opcode == DM_OP_STRLEN || opcode == DM_OP_SMP || opcode == DM_OP_SMPLEN || opcode == DM_OP_WAVE ||
      opcode == DM_OP_VENV || opcode == DM_OP_EVT || opcode == DM_OP_EVTL || opcode == DM_OP_SCENE) {
    if (opcode == DM_OP_VENV && e->kind != DM_PK_INST) fail(c, line, "venv() only exists inside inst programs");
    expect(e, T_LP, "'('");
    tok t = next(e);
    if (t.type != T_IDENT) fail(c, t.line, "%s(NAME, ...): first argument is a name", name);
    const char *rn = resolve_name(e, t.text);
    int id = -1; const char *what = "";
    switch (opcode) {
      case DM_OP_STR: case DM_OP_STRLEN: id = find_text(c, rn); what = "text"; break;
      case DM_OP_SMP: case DM_OP_SMPLEN: case DM_OP_WAVE: id = find_sample(c, rn); what = "sample"; break;
      case DM_OP_VENV: id = find_venv(c, rn); what = "venv"; break;
      case DM_OP_EVT: case DM_OP_EVTL: id = find_evname(c, rn); what = "timeline event"; break;
      default: id = find_pixel(c, rn); what = "pixel program"; break;
    }
    if (id < 0) fail(c, t.line, "unknown %s '%s'", what, t.text);
    for (int i = 0; i < oi->nargs; i++) { expect(e, T_COMMA, "','"); parse_expr(e, 0); }
    expect(e, T_RP, "')'");
    op_imm(e, opcode, id);
    if (as_statement) op(e, DM_OP_POP);
    return;
  }
  if (oi->kind == 'S' && !as_statement) fail(c, line, "%s() is a statement, not an expression", name);
  if (opcode == DM_OP_ENV && e->kind != DM_PK_INST) fail(c, line, "env() only exists inside inst programs");
  if ((opcode == DM_OP_DLY || opcode == DM_OP_VERB) && e->kind != DM_PK_MASTER && e->kind != DM_PK_CHAN) fail(c, line, "%s() only exists inside master and chan programs", name);
  if ((opcode == DM_OP_PREV || opcode == DM_OP_CPREV || opcode == DM_OP_CPREVZ) && e->kind != DM_PK_PIXEL && e->kind != DM_PK_POST) fail(c, line, "%s() only exists inside pixel and post programs", name);
  if (opcode == DM_OP_TAP && e->kind != DM_PK_POST) fail(c, line, "tap() only exists inside the post program");
  if (opcode == DM_OP_SELECT && e->kind != DM_PK_FRAME) fail(c, line, "select() only exists inside the frame program");
  int n = parse_args(e, oi->nargs);
  if (n != oi->nargs) fail(c, line, "%s() takes %d argument%s", name, oi->nargs, oi->nargs == 1 ? "" : "s");
  if (oi->imm) {
    unsigned imm = 0;
    if (opcode == DM_OP_LP || opcode == DM_OP_HP || opcode == DM_OP_SVF) {
      char nm[DM_NAME_LEN]; snprintf(nm, sizeof nm, "~%s%d", name, e->nstate); imm = alloc_state(e, nm, line);
      if (opcode == DM_OP_SVF) { snprintf(nm, sizeof nm, "~%s%d", name, e->nstate); alloc_state(e, nm, line); }
    }
    else if (opcode == DM_OP_DLY) { if (e->ndelay >= DM_MAX_DELAY_LINES) fail(c, line, "too many dly() lines (max %d)", DM_MAX_DELAY_LINES); imm = e->ndelay++; }
    else if (opcode == DM_OP_VERB) { if (e->nverb >= DM_MAX_VERBS) fail(c, line, "too many verb() instances (max %d)", DM_MAX_VERBS); imm = e->nverb++; }
    else if (opcode == DM_OP_USAW) {
      char nm[DM_NAME_LEN]; imm = e->nstate;
      for (int k = 0; k < DM_USAW_VOICES; k++) { snprintf(nm, sizeof nm, "~usaw%d", e->nstate); alloc_state(e, nm, line); }
    }
    op_imm(e, opcode, imm);
  } else op(e, opcode);
  if (as_statement && oi->kind == 'F') op(e, DM_OP_POP);
}

static void parse_primary(ec *e) {
  tok t = next(e);
  e->line = t.line;
  switch (t.type) {
    case T_NUM: push_const(e, t.num); return;
    case T_LP: parse_expr(e, 0); expect(e, T_RP, "')'"); return;
    case T_STATE: {
      int s = find_name(e->states, e->nstate, t.text);
      if (s < 0) s = alloc_state(e, t.text, t.line);
      op_imm(e, DM_OP_LDS, s); return;
    }
    case T_OP:
      if (!strcmp(t.text, "-")) { parse_expr(e, 9); op(e, DM_OP_NEG); return; }
      if (!strcmp(t.text, "!")) { parse_expr(e, 9); op(e, DM_OP_NOT); return; }
      if (!strcmp(t.text, "+")) { parse_expr(e, 9); return; }
      fail(e->c, t.line, "unexpected operator '%s'", t.text);
      return;
    case T_IDENT: {
      if (peek(e)->type == T_LP) { parse_call(e, t.text, t.line, 0); return; }
      if (!strcmp(t.text, "pi")) { push_const(e, DM_PI); return; }
      if (!strcmp(t.text, "tau")) { push_const(e, DM_TAU); return; }
      if (!strcmp(t.text, "r1")) { op(e, DM_OP_R1); return; }
      if (!strcmp(t.text, "r2")) { op(e, DM_OP_R2); return; }
      if (!strcmp(t.text, "r3")) { op(e, DM_OP_R3); return; }
      int i;
      { char sn[DM_NAME_LEN]; scoped(e, t.text, sn, sizeof sn);      /* fn params/locals shadow inputs */
        if ((i = find_name(e->locals, e->nlocals, sn)) >= 0) { op_imm(e, DM_OP_LDL, i); return; } }
      if ((i = find_input(e, t.text)) >= 0) { op_imm(e, DM_OP_LDI, i); return; }
      if ((i = find_uniform(e->c, t.text)) >= 0) { op_imm(e, DM_OP_LDU, i); return; }
      fail(e->c, t.line, "unknown identifier '%s'", t.text);
      return;
    }
    default: fail(e->c, t.line, "expected an expression");
  }
}

typedef struct { const char *op; int lbp, rbp, opcode; } binop;
static const binop g_binops[] = {
  {"||", 2, 3, DM_OP_OR}, {"&&", 3, 4, DM_OP_AND},
  {"==", 4, 5, DM_OP_EQ}, {"!=", 4, 5, DM_OP_NE},
  {"<", 5, 6, DM_OP_LT}, {"<=", 5, 6, DM_OP_LE}, {">", 5, 6, DM_OP_GT}, {">=", 5, 6, DM_OP_GE},
  {"+", 6, 7, DM_OP_ADD}, {"-", 6, 7, DM_OP_SUB},
  {"*", 7, 8, DM_OP_MUL}, {"/", 7, 8, DM_OP_DIV}, {"%", 7, 8, DM_OP_MOD},
  {"^", 11, 10, DM_OP_POW},
  {NULL, 0, 0, 0}
};

static void parse_expr(ec *e, int minbp) {
  parse_primary(e);
  for (;;) {
    tok *t = peek(e);
    if (t->type == T_Q) {
      if (1 < minbp) return;
      next(e);
      parse_expr(e, 0);
      expect(e, T_COLON, "':'");
      parse_expr(e, 1);
      op(e, DM_OP_SEL);
      continue;
    }
    if (t->type != T_OP) return;
    const binop *b = NULL;
    for (const binop *k = g_binops; k->op; k++) if (!strcmp(k->op, t->text)) { b = k; break; }
    if (!b || b->lbp < minbp) return;
    next(e);
    parse_expr(e, b->rbp);
    op(e, b->opcode);
  }
}

static void parse_block(ec *e, int line);

static void parse_repeat(ec *e, int line) {
  /* repeat NAME COUNT { ... } : counted loop, NAME is a local holding 0..COUNT-1 */
  tok nm = next(e);
  if (nm.type != T_IDENT) fail(e->c, nm.line, "repeat NAME COUNT {");
  tok cnt = next(e);
  if (cnt.type != T_NUM || cnt.num < 0 || cnt.num > 65535 || cnt.num != floorf(cnt.num)) fail(e->c, cnt.line, "repeat count must be a constant 0..65535");
  if (peek(e)->type != T_LB) fail(e->c, cnt.line, "expected '{' after repeat count");
  next(e);
  char sn[DM_NAME_LEN]; scoped(e, nm.text, sn, sizeof sn);
  int slot = find_name(e->locals, e->nlocals, sn);
  if (slot < 0) {
    if (e->nlocals >= DM_MAX_LOCALS) fail(e->c, line, "too many locals");
    snprintf(e->locals[e->nlocals], DM_NAME_LEN, "%s", sn);
    slot = e->nlocals++;
  }
  dm_module *m = e->c->m;
  emit8(e->c, line, DM_OP_LOOP); emit8(e->c, line, slot);
  int count = (int)cnt.num;
  emit8(e->c, line, count & 255); emit8(e->c, line, count >> 8);
  uint32_t skip_at = m->codelen; emit8(e->c, line, 0); emit8(e->c, line, 0);
  uint32_t body = m->codelen;
  parse_block(e, line);
  uint32_t back = m->codelen + 4 - body;
  if (back > 65535) fail(e->c, line, "loop body too large");
  emit8(e->c, line, DM_OP_ENDL); emit8(e->c, line, slot); emit8(e->c, line, back & 255); emit8(e->c, line, back >> 8);
  uint32_t skip = m->codelen - (skip_at + 2);
  m->code[skip_at] = skip & 255; m->code[skip_at + 1] = (skip >> 8) & 255;
}

/* statements until the matching '}' */
static void parse_block(ec *e, int line) {
  for (;;) {
    tok *t = peek(e);
    if (t->type == T_EOF) fail(e->c, line, "unterminated block (missing '}')");
    if (t->type == T_NL) { next(e); continue; }
    if (t->type == T_RB) { next(e); return; }
    parse_statement(e);
    if (e->depth != 0) fail(e->c, e->line, "internal: stack imbalance %d", e->depth);
    t = peek(e);
    if (t->type != T_NL && t->type != T_EOF && t->type != T_RB) fail(e->c, t->line, "unexpected token after statement");
  }
}

static void parse_statement(ec *e) {
  tok t = next(e);
  e->line = t.line;
  if (t.type == T_IDENT && !strcmp(t.text, "repeat")) { parse_repeat(e, t.line); return; }
  if (t.type == T_IDENT && peek(e)->type == T_LP) { parse_call(e, t.text, t.line, 1); return; }
  if (t.type != T_IDENT && t.type != T_STATE) fail(e->c, t.line, "expected a statement");
  tok a = next(e);
  if (a.type != T_OP || !(strcmp(a.text, "=") == 0 || strcmp(a.text, "+=") == 0 || strcmp(a.text, "-=") == 0 || strcmp(a.text, "*=") == 0 || strcmp(a.text, "/=") == 0))
    fail(e->c, a.line, "expected '=' after '%s'", t.text);
  int store_op, slot;
  if (t.type == T_STATE) {
    slot = find_name(e->states, e->nstate, t.text);
    if (slot < 0) slot = alloc_state(e, t.text, t.line);
    store_op = DM_OP_STS;
  } else {
    { char sn[DM_NAME_LEN]; scoped(e, t.text, sn, sizeof sn);
      if (find_name(e->locals, e->nlocals, sn) < 0 && find_input(e, t.text) >= 0) fail(e->c, t.line, "'%s' is a read-only input", t.text); }
    if (!strcmp(t.text, "pi") || !strcmp(t.text, "tau") || (t.text[0] == 'r' && t.text[2] == 0 && t.text[1] >= '1' && t.text[1] <= '3'))
      fail(e->c, t.line, "'%s' is a built-in constant", t.text);
    slot = find_uniform(e->c, t.text);
    if (slot >= 0) store_op = DM_OP_STU;
    else {
      char sn[DM_NAME_LEN]; scoped(e, t.text, sn, sizeof sn);
      slot = find_name(e->locals, e->nlocals, sn);
      if (slot < 0) {
        if (a.text[1] == '=') fail(e->c, t.line, "'%s' used before assignment", t.text);
        if (e->nlocals >= DM_MAX_LOCALS) fail(e->c, t.line, "too many locals");
        snprintf(e->locals[e->nlocals], DM_NAME_LEN, "%s", sn);
        slot = e->nlocals++;
      }
      store_op = DM_OP_STL;
    }
  }
  if (a.text[1] == '=') op_imm(e, store_op == DM_OP_STS ? DM_OP_LDS : store_op == DM_OP_STU ? DM_OP_LDU : DM_OP_LDL, slot);
  parse_expr(e, 0);
  if (a.text[1] == '=') op(e, a.text[0] == '+' ? DM_OP_ADD : a.text[0] == '-' ? DM_OP_SUB : a.text[0] == '*' ? DM_OP_MUL : DM_OP_DIV);
  op_imm(e, store_op, slot);
}

static void compile_program(cc *c, const char *body, int first_line, int kind, dm_prog *out, const char *what) {
  ec e; memset(&e, 0, sizeof e);
  e.c = c; e.s = body; e.line = first_line; e.kind = kind;
  out->off = (uint16_t)c->m->codelen;
  for (;;) {
    tok *t = peek(&e);
    if (t->type == T_EOF) break;
    if (t->type == T_NL) { next(&e); continue; }
    parse_statement(&e);
    if (e.depth != 0) fail(c, e.line, "internal: stack imbalance %d", e.depth);
    t = peek(&e);
    if (t->type != T_NL && t->type != T_EOF) fail(c, t->line, "unexpected token after statement");
  }
  uint32_t len = c->m->codelen - out->off;
  if (len > 65535) fail(c, first_line, "%s: program too large", what);
  out->len = (uint16_t)len;
  if (e.nlocals > e.maxlocals) e.maxlocals = e.nlocals;
  out->nlocals = (uint8_t)e.maxlocals; out->nstate = (uint8_t)e.nstate; out->ndelay = (uint8_t)e.ndelay; out->nverb = (uint8_t)e.nverb;
  if (dm_prog_verify(c->m, out, what)) longjmp(c->jb, 1);
}

/* ------------------------------------------------------------ top level (line based) */
static int get_line(cc *c, char *buf, size_t cap) {
  if (!c->src[c->pos]) return 0;
  size_t i = 0;
  while (c->src[c->pos] && c->src[c->pos] != '\n') { if (i + 1 < cap) buf[i++] = c->src[c->pos]; c->pos++; }
  if (c->src[c->pos] == '\n') c->pos++;
  buf[i] = 0;
  c->line++;
  return 1;
}
static void strip_comment(char *s) {
  /* '#' opens a comment only at line start or after whitespace, so D#4 survives */
  for (char *p = s; *p; p++) {
    if ((*p == '#' && (p == s || isspace((unsigned char)p[-1]))) || (*p == '/' && p[1] == '/')) { *p = 0; return; }
  }
}
static int split(char *s, char **argv, int max) {
  int n = 0;
  char *p = s;
  while (*p && n < max) {
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) break;
    argv[n++] = p;
    while (*p && !isspace((unsigned char)*p)) p++;
    if (*p) *p++ = 0;
  }
  return n;
}
static int kv(const char *arg, const char *key, const char **val) {
  size_t k = strlen(key);
  if (strncmp(arg, key, k) == 0 && arg[k] == '=') { *val = arg + k + 1; return 1; }
  return 0;
}
static void set_name(cc *c, int line, char *dst, const char *src) {
  if (strlen(src) >= DM_NAME_LEN) fail(c, line, "name '%s' too long (max %d)", src, DM_NAME_LEN - 1);
  for (const char *p = src; *p; p++) if (!isalnum((unsigned char)*p) && *p != '_') fail(c, line, "bad name '%s'", src);
  strcpy(dst, src);
}

/* Collect a { ... } block body.  The opening brace is on the header line; the
 * body ends at a line that is exactly '}'.  Returns malloc'd text. */
static char *read_block(cc *c, int *first_line) {
  size_t cap = 1024, n = 0;
  char *body = malloc(cap), line[1024];
  *first_line = c->line + 1;
  int start = c->line, depth = 0;
  for (;;) {
    if (!get_line(c, line, sizeof line)) fail(c, start, "unterminated block (missing '}')");
    char tmp[1024]; strcpy(tmp, line); strip_comment(tmp);
    char *argv[64]; int argc = split(tmp, argv, 64);
    if (argc == 1 && !strcmp(argv[0], "}")) { if (depth == 0) break; depth--; }
    else if (argc >= 1 && !strcmp(argv[argc - 1], "{")) depth++;
    size_t l = strlen(line);
    if (n + l + 2 > cap) { cap = (cap + l) * 2; body = realloc(body, cap); }
    memcpy(body + n, line, l); n += l; body[n++] = '\n';
  }
  body[n] = 0;
  return body;
}

static int parse_note(const char *s) {
  if (!strcmp(s, "off") || !strcmp(s, "OFF") || !strcmp(s, "===") || !strcmp(s, "^^^")) return DM_NOTE_OFF;
  static const int base[7] = {9, 11, 0, 2, 4, 5, 7};   /* A B C D E F G */
  char n = (char)toupper((unsigned char)s[0]);
  if (n < 'A' || n > 'G' || strlen(s) != 3) return -1;
  int semi = base[n - 'A'];
  if (s[1] == '#') semi++; else if (s[1] == 'b') semi--; else if (s[1] != '-') return -1;
  if (!isdigit((unsigned char)s[2])) return -1;
  int oct = s[2] - '0';
  int note = oct * 12 + semi + 1;
  if (note < 1 || note > 96) return -1;
  return note;
}
static int is_blank_field(const char *s) {
  for (; *s; s++) if (*s != '.' && *s != '-') return 0;
  return 1;
}
static int hexval(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  ch = (char)toupper((unsigned char)ch);
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

static void parse_pattern(cc *c, int id, int rows, int header_line) {
  dm_module *m = c->m;
  if (id < 0 || id >= DM_MAX_PATTERNS) fail(c, header_line, "pattern id out of range");
  if (m->pat[id].cells) fail(c, header_line, "pattern %d defined twice", id);
  int chn = m->channels ? m->channels : DM_MAX_CHANNELS;
  dm_cell *cells = calloc((size_t)rows * chn, sizeof(dm_cell));
  char line[1024];
  int row = 0, start = c->line;
  for (;;) {
    if (!get_line(c, line, sizeof line)) fail(c, start, "unterminated pattern");
    strip_comment(line);
    char *trim = line; while (isspace((unsigned char)*trim)) trim++;
    if (!*trim) continue;
    if (!strcmp(trim, "}")) break;
    if (row >= rows) fail(c, c->line, "pattern %d has more than %d rows", id, rows);
    /* optional "NN:" row label */
    char *p = trim; while (isxdigit((unsigned char)*p)) p++;
    if (*p == ':' && p > trim) trim = p + 1;
    int ch = 0;
    char *save = trim;
    while (save && ch < chn) {
      char *bar = strchr(save, '|');
      if (bar) *bar = 0;
      char *f[8]; int nf = split(save, f, 8);
      dm_cell *cell = &cells[row * chn + ch];
      if (nf > 4) fail(c, c->line, "channel %d: too many fields (note inst vol fx)", ch);
      if (nf >= 1 && !is_blank_field(f[0])) {
        int n = parse_note(f[0]);
        if (n < 0) fail(c, c->line, "bad note '%s'", f[0]);
        cell->note = (uint8_t)n;
      }
      if (nf >= 2 && !is_blank_field(f[1])) {
        char *endp; long v = strtol(f[1], &endp, 10);
        if (*endp || v < 1 || v > DM_MAX_INST) fail(c, c->line, "bad instrument '%s'", f[1]);
        cell->inst = (uint8_t)v;
      }
      if (nf >= 3 && !is_blank_field(f[2])) {
        char *endp; long v = strtol(f[2], &endp, 16);
        if (*endp || v < 0 || v > 0x40) fail(c, c->line, "bad volume '%s' (hex 00..40)", f[2]);
        cell->vol = (uint8_t)(v + 1);
      }
      if (nf >= 4 && !is_blank_field(f[3])) {
        if (strlen(f[3]) != 3) fail(c, c->line, "bad effect '%s' (3 chars, e.g. A0F)", f[3]);
        char t = (char)toupper((unsigned char)f[3][0]);
        int fx = isdigit((unsigned char)t) ? t - '0' : (t >= 'A' && t <= 'Z' ? t - 'A' + 10 : -1);
        int hi = hexval(f[3][1]), lo = hexval(f[3][2]);
        if (fx < 0 || hi < 0 || lo < 0) fail(c, c->line, "bad effect '%s'", f[3]);
        cell->fx = (uint8_t)fx; cell->fxp = (uint8_t)(hi * 16 + lo);
      }
      ch++;
      save = bar ? bar + 1 : NULL;
    }
    row++;
  }
  m->pat[id].rows = (uint16_t)rows; m->pat[id].channels = (uint8_t)chn; m->pat[id].cells = cells;
}

static void parse_palette(cc *c, const char *name, int header_line) {
  dm_module *m = c->m;
  if (m->npal >= DM_MAX_PAL) fail(c, header_line, "too many palettes");
  if (find_palette(c, name) >= 0) fail(c, header_line, "palette '%s' defined twice", name);
  dm_palette *p = &m->pal[m->npal];
  set_name(c, header_line, p->name, name);
  char line[512]; int start = c->line;
  for (;;) {
    if (!get_line(c, line, sizeof line)) fail(c, start, "unterminated palette");
    strip_comment(line);
    char *f[8]; int nf = split(line, f, 8);
    if (!nf) continue;
    if (nf == 1 && !strcmp(f[0], "}")) break;
    if (nf != 4) fail(c, c->line, "palette entry: pos r g b");
    if (p->nkeys >= DM_MAX_PALKEYS) fail(c, c->line, "too many palette keys");
    for (int i = 0; i < 4; i++) p->keys[p->nkeys][i] = strtof(f[i], NULL);
    if (p->nkeys && p->keys[p->nkeys][0] < p->keys[p->nkeys - 1][0]) fail(c, c->line, "palette keys must be sorted by position");
    p->nkeys++;
  }
  m->npal++;
}

/* ------------------------------------------------------------ wav loader -> int8 mono */
static int load_wav(cc *c, int line, const char *path, int rate, dm_sample *out) {
  char full[2048];
  if (path[0] == '/' || !c->srcdir[0]) snprintf(full, sizeof full, "%s", path); else snprintf(full, sizeof full, "%s/%s", c->srcdir, path);
  FILE *f = fopen(full, "rb");
  if (!f) fail(c, line, "cannot open sample '%s'", full);
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t *d = malloc((size_t)n + 1); size_t got = fread(d, 1, (size_t)n, f); fclose(f);
  if (got < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) { free(d); fail(c, line, "'%s' is not a WAV file", path); }
  size_t p = 12; int ch = 0, bits = 0, srate = 0; const uint8_t *data = NULL; size_t dlen = 0;
  while (p + 8 <= got) {
    uint32_t len = d[p + 4] | (d[p + 5] << 8) | (d[p + 6] << 16) | ((uint32_t)d[p + 7] << 24);
    if (!memcmp(d + p, "fmt ", 4) && len >= 16) {
      int fmt = d[p + 8] | (d[p + 9] << 8); ch = d[p + 10] | (d[p + 11] << 8);
      srate = d[p + 12] | (d[p + 13] << 8) | (d[p + 14] << 16) | (d[p + 15] << 24); bits = d[p + 22] | (d[p + 23] << 8);
      if (fmt != 1 || (bits != 8 && bits != 16)) { free(d); fail(c, line, "'%s': only 8/16-bit PCM WAV is supported", path); }
    } else if (!memcmp(d + p, "data", 4)) { data = d + p + 8; dlen = len; if (p + 8 + dlen > got) dlen = got - p - 8; }
    p += 8 + len + (len & 1);
  }
  if (!data || !ch || !srate) { free(d); fail(c, line, "'%s': missing fmt/data chunk", path); }
  size_t frames = dlen / (ch * bits / 8);
  if (rate <= 0) rate = srate;
  if (rate > 65535) rate = 65535;
  size_t outn = (size_t)((double)frames * rate / srate);
  if (outn < 2) outn = 2;
  int8_t *pcm = malloc(outn);
  for (size_t i = 0; i < outn; i++) {
    double sp = (double)i * srate / rate; size_t si = (size_t)sp; if (si >= frames - 1) si = frames > 1 ? frames - 2 : 0;
    float a = 0, b = 0;
    for (int k = 0; k < ch; k++) {
      size_t ia = (si * ch + k), ib = ((si + 1 < frames ? si + 1 : si) * ch + k);
      if (bits == 16) { a += (int16_t)(data[ia * 2] | (data[ia * 2 + 1] << 8)) / 32768.0f; b += (int16_t)(data[ib * 2] | (data[ib * 2 + 1] << 8)) / 32768.0f; }
      else { a += (data[ia] - 128) / 128.0f; b += (data[ib] - 128) / 128.0f; }
    }
    a /= ch; b /= ch;
    float v = a + (b - a) * (float)(sp - si);
    int q = (int)(v * 127.0f + (v >= 0 ? 0.5f : -0.5f)); if (q > 127) q = 127; if (q < -127) q = -127;
    pcm[i] = (int8_t)q;
  }
  free(d);
  out->pcm = pcm; out->len = (uint32_t)outn; out->rate = (uint16_t)rate;
  return 0;
}

static char *quoted(cc *c, int line, const char *raw) {
  const char *q = strchr(raw, '"');
  if (!q) fail(c, line, "expected a \"quoted string\"");
  const char *e = strrchr(raw, '"');
  if (e == q) fail(c, line, "unterminated string");
  size_t n = (size_t)(e - q - 1);
  char *s = malloc(n + 1); memcpy(s, q + 1, n); s[n] = 0;
  return s;
}

int dm_compile(const char *src, const char *srcname, dm_module *m) {
  cc c; memset(&c, 0, sizeof c);
  c.m = m; c.srcname = srcname; c.src = src;
  { const char *sl = strrchr(srcname, '/'); if (sl) { size_t n = (size_t)(sl - srcname); if (n >= sizeof c.srcdir) n = sizeof c.srcdir - 1; memcpy(c.srcdir, srcname, n); c.srcdir[n] = 0; } }
  memset(m, 0, sizeof *m);
  m->bpm = 125; m->speed = 6; m->rpb = 4; m->channels = 4; m->volume = 0.8f; m->scale = 1;
  for (int i = 0; i < DM_MAX_CHANNELS; i++) m->pan[i] = 0.5f;
  if (setjmp(c.jb)) { dm_module_free(m); return -1; }

  char line[1024];
  /* pre-scan: pixel program names and timeline event names may be referenced before they are defined */
  {
    cc pre = c; int in_tl = 0;
    while (get_line(&pre, line, sizeof line)) {
      strip_comment(line);
      char *argv[64]; int argc = split(line, argv, 64);
      if (!argc) continue;
      if (in_tl) { if (!strcmp(argv[0], "}")) in_tl = 0; else for (int i = 1; i < argc; i++) { char *eq = strchr(argv[i], '='); if (eq) { *eq = 0; if (find_evname(&c, argv[i]) < 0 && m->nevname < DM_MAX_EVNAMES) set_name(&c, pre.line, m->evname[m->nevname++], argv[i]); } } continue; }
      if (!strcmp(argv[0], "timeline") && argc == 2 && !strcmp(argv[1], "{")) in_tl = 1;
      else if (!strcmp(argv[0], "pixel") && argc >= 2 && !strcmp(argv[argc - 1], "{")) {
        const char *nm = argc == 3 ? argv[1] : "main";
        if (find_pixel(&c, nm) < 0 && m->npixel < DM_MAX_PIXEL) set_name(&c, pre.line, m->pixelname[m->npixel++], nm);
      }
    }
  }
  int npixel_names = m->npixel;
  while (get_line(&c, line, sizeof line)) {
    char raw[1024]; strcpy(raw, line);
    strip_comment(line);
    char *argv[64]; int argc = split(line, argv, 64);
    if (!argc) continue;
    const char *k = argv[0], *v;
    int ln = c.line;
    if (!strcmp(k, "title")) {
      char *p = strstr(raw, "title") + 5; while (isspace((unsigned char)*p)) p++;
      char *end = p + strlen(p); while (end > p && (isspace((unsigned char)end[-1]) || end[-1] == '#')) end--;
      size_t n = (size_t)(end - p); if (n >= sizeof m->title) n = sizeof m->title - 1;
      memcpy(m->title, p, n); m->title[n] = 0;
    } else if (!strcmp(k, "song")) {
      for (int i = 1; i < argc; i++) {
        if (kv(argv[i], "bpm", &v)) m->bpm = (uint8_t)atoi(v);
        else if (kv(argv[i], "speed", &v)) m->speed = (uint8_t)atoi(v);
        else if (kv(argv[i], "rpb", &v)) m->rpb = (uint8_t)atoi(v);
        else if (kv(argv[i], "channels", &v)) m->channels = (uint8_t)atoi(v);
        else if (kv(argv[i], "loop", &v)) m->loop = (uint8_t)atoi(v);
        else if (kv(argv[i], "volume", &v)) m->volume = strtof(v, NULL);
        else if (kv(argv[i], "scale", &v)) m->scale = (uint8_t)atoi(v);
        else if (kv(argv[i], "dither", &v)) m->dither = (uint8_t)atoi(v);
        else fail(&c, ln, "unknown song option '%s'", argv[i]);
      }
      if (m->bpm < 32) fail(&c, ln, "bpm must be >= 32 (and <= 255)");
      if (m->channels < 1 || m->channels > DM_MAX_CHANNELS) fail(&c, ln, "channels must be 1..%d", DM_MAX_CHANNELS);
      if (m->speed < 1 || m->speed > 31) fail(&c, ln, "speed must be 1..31");
    } else if (!strcmp(k, "order")) {
      for (int i = 1; i < argc; i++) {
        if (m->norder >= DM_MAX_ORDER) fail(&c, ln, "order list too long");
        m->order[m->norder++] = (uint8_t)atoi(argv[i]);
      }
    } else if (!strcmp(k, "uniform")) {
      for (int i = 1; i < argc; i++) {
        if (m->nuniform >= DM_MAX_UNIFORMS) fail(&c, ln, "too many uniforms");
        if (find_uniform(&c, argv[i]) >= 0) fail(&c, ln, "uniform '%s' declared twice", argv[i]);
        set_name(&c, ln, c.uni[m->nuniform++], argv[i]);
      }
    } else if (!strcmp(k, "palette")) {
      if (argc != 3 || strcmp(argv[2], "{")) fail(&c, ln, "usage: palette NAME {");
      parse_palette(&c, argv[1], ln);
    } else if (!strcmp(k, "pan")) {
      for (int i = 1; i < argc && i <= DM_MAX_CHANNELS; i++) m->pan[i - 1] = strtof(argv[i], NULL);
    } else if (!strcmp(k, "text")) {
      if (argc < 3) fail(&c, ln, "usage: text NAME \"string\"");
      if (m->ntext >= DM_MAX_TEXTS) fail(&c, ln, "too many texts");
      dm_text *t = &m->text[m->ntext];
      set_name(&c, ln, t->name, argv[1]);
      t->s = quoted(&c, ln, raw); size_t n = strlen(t->s); if (n > DM_MAX_TEXT_LEN) fail(&c, ln, "text too long"); t->len = (uint16_t)n;
      m->ntext++;
    } else if (!strcmp(k, "sample")) {
      if (argc < 3) fail(&c, ln, "usage: sample NAME \"file.wav\" [rate=N]");
      if (m->nsmp >= DM_MAX_SAMPLES) fail(&c, ln, "too many samples");
      dm_sample *sm = &m->smp[m->nsmp];
      set_name(&c, ln, sm->name, argv[1]);
      int rate = 0;
      for (int i = 2; i < argc; i++) if (kv(argv[i], "rate", &v)) rate = atoi(v);
      char *path = quoted(&c, ln, raw);
      load_wav(&c, ln, path, rate, sm);
      free(path);
      m->nsmp++;
    } else if (!strcmp(k, "venv")) {
      if (argc < 3 || strcmp(argv[argc - 1], "{")) fail(&c, ln, "usage: venv NAME [sustain=K] {");
      if (m->nvenv >= DM_MAX_VENVS) fail(&c, ln, "too many envelopes");
      dm_venv *ve = &m->venv[m->nvenv];
      set_name(&c, ln, ve->name, argv[1]);
      ve->sustain = 255;
      for (int i = 2; i < argc - 1; i++) { if (kv(argv[i], "sustain", &v)) ve->sustain = (uint8_t)atoi(v); else fail(&c, ln, "unknown venv option '%s'", argv[i]); }
      int start = c.line;
      for (;;) {
        if (!get_line(&c, line, sizeof line)) fail(&c, start, "unterminated venv");
        strip_comment(line);
        char *f[4]; int nf = split(line, f, 4);
        if (!nf) continue;
        if (nf == 1 && !strcmp(f[0], "}")) break;
        if (nf != 2) fail(&c, c.line, "venv point: time level");
        if (ve->npts >= DM_MAX_VENV_PTS) fail(&c, c.line, "too many envelope points");
        ve->t[ve->npts] = strtof(f[0], NULL); ve->v[ve->npts] = strtof(f[1], NULL);
        if (ve->npts && ve->t[ve->npts] < ve->t[ve->npts - 1]) fail(&c, c.line, "venv points must be in time order");
        ve->npts++;
      }
      if (!ve->npts) fail(&c, ln, "empty venv");
      m->nvenv++;
    } else if (!strcmp(k, "timeline")) {
      if (argc != 2 || strcmp(argv[1], "{")) fail(&c, ln, "usage: timeline {");
      int start = c.line;
      for (;;) {
        if (!get_line(&c, line, sizeof line)) fail(&c, start, "unterminated timeline");
        strip_comment(line);
        char *f[64]; int nf = split(line, f, 64);
        if (!nf) continue;
        if (nf == 1 && !strcmp(f[0], "}")) break;
        char *colon = strchr(f[0], ':');
        if (!colon) fail(&c, c.line, "timeline entry: ORDER:ROW name=value ...");
        *colon = 0;
        int ord = atoi(f[0]), row = atoi(colon + 1);
        if (ord < 0 || ord >= DM_MAX_ORDER || row < 0 || row > 255) fail(&c, c.line, "timeline position out of range");
        for (int i = 1; i < nf; i++) {
          char *eq = strchr(f[i], '=');
          if (!eq) fail(&c, c.line, "timeline entry: name=value");
          *eq = 0;
          int id = find_evname(&c, f[i]);
          if (id < 0) fail(&c, c.line, "internal: event name '%s' not pre-registered", f[i]);
          if (m->nev >= DM_MAX_EVENTS) fail(&c, c.line, "too many timeline events");
          dm_event *ev = &m->ev[m->nev++];
          ev->order = (uint8_t)ord; ev->row = (uint8_t)row; ev->id = (uint8_t)id; ev->value = strtof(eq + 1, NULL);
        }
      }
      /* keep sorted by position (stable insertion sort; lists are short) */
      for (int i = 1; i < m->nev; i++) { dm_event x = m->ev[i]; int j = i - 1;
        while (j >= 0 && (m->ev[j].order > x.order || (m->ev[j].order == x.order && m->ev[j].row > x.row))) { m->ev[j + 1] = m->ev[j]; j--; }
        m->ev[j + 1] = x; }
    } else if (!strcmp(k, "chan")) {
      if (argc != 3 || strcmp(argv[2], "{")) fail(&c, ln, "usage: chan N {");
      int ch = atoi(argv[1]);
      if (ch < 0 || ch >= m->channels) fail(&c, ln, "chan %d: song has %d channels", ch, m->channels);
      if ((m->has_chanfx >> ch) & 1) fail(&c, ln, "chan %d defined twice", ch);
      int first; char *body = read_block(&c, &first);
      char nm[16]; snprintf(nm, sizeof nm, "chan %d", ch);
      compile_program(&c, body, first, DM_PK_CHAN, &m->chanfx[ch], nm);
      free(body);
      m->has_chanfx |= (uint8_t)(1 << ch);
    } else if (!strcmp(k, "pixel")) {
      if (argc < 2 || argc > 3 || strcmp(argv[argc - 1], "{")) fail(&c, ln, "usage: pixel [NAME] {");
      const char *nm = argc == 3 ? argv[1] : "main";
      int idx = -1;
      for (int i = 0; i < npixel_names; i++) if (!strcmp(m->pixelname[i], nm)) idx = i;
      if (idx < 0) fail(&c, ln, "internal: pixel name '%s' not pre-registered", nm);
      if (m->pixel[idx].len) fail(&c, ln, "pixel '%s' defined twice", nm);
      int first; char *body = read_block(&c, &first);
      m->npixel = npixel_names;                    /* all names visible to scene() */
      compile_program(&c, body, first, DM_PK_PIXEL, &m->pixel[idx], nm);
      free(body);
    } else if (!strcmp(k, "post")) {
      if (argc != 2 || strcmp(argv[1], "{")) fail(&c, ln, "usage: post {");
      if (m->has_post) fail(&c, ln, "post defined twice");
      int first; char *body = read_block(&c, &first);
      compile_program(&c, body, first, DM_PK_POST, &m->post, "post");
      free(body);
      m->has_post = 1;
    } else if (!strcmp(k, "inst")) {
      if (argc < 3 || strcmp(argv[argc - 1], "{")) fail(&c, ln, "usage: inst NAME [nna=continue] {");
      if (m->ninst >= DM_MAX_INST) fail(&c, ln, "too many instruments");
      dm_inst *in = &m->inst[m->ninst];
      set_name(&c, ln, in->name, argv[1]);
      for (int i = 2; i < argc - 1; i++) {
        if (kv(argv[i], "nna", &v)) { if (!strcmp(v, "continue")) in->flags |= DM_INST_NNA; else if (strcmp(v, "cut")) fail(&c, ln, "nna must be cut or continue"); }
        else fail(&c, ln, "unknown inst option '%s'", argv[i]);
      }
      int first; char *body = read_block(&c, &first);
      compile_program(&c, body, first, DM_PK_INST, &in->prog, in->name);
      free(body);
      m->ninst++;
    } else if (!strcmp(k, "pattern")) {
      if (argc < 3 || strcmp(argv[argc - 1], "{")) fail(&c, ln, "usage: pattern ID rows=N {");
      int rows = 16;
      for (int i = 2; i < argc - 1; i++) { if (kv(argv[i], "rows", &v)) rows = atoi(v); else fail(&c, ln, "unknown pattern option '%s'", argv[i]); }
      if (rows < 1 || rows > 256) fail(&c, ln, "rows must be 1..256");
      parse_pattern(&c, atoi(argv[1]), rows, ln);
    } else if (!strcmp(k, "fn")) {
      /* fn name(a, b) {   -- re-split the raw line on the parens */
      char hdr[1024]; strcpy(hdr, raw); strip_comment(hdr);
      char *lp = strchr(hdr, '('), *rp = lp ? strchr(lp, ')') : NULL;
      if (!lp || !rp || strcmp(argv[argc - 1], "{")) fail(&c, ln, "usage: fn NAME(a, b) {");
      if (c.nfns >= DM_MAX_FNS) fail(&c, ln, "too many fns");
      cfn *fn = &c.fns[c.nfns];
      *lp = 0; char *nm = hdr + 2; while (isspace((unsigned char)*nm)) nm++;
      char *ne = nm + strlen(nm); while (ne > nm && isspace((unsigned char)ne[-1])) *--ne = 0;
      set_name(&c, ln, fn->name, nm);
      if (find_op(fn->name) >= 0) fail(&c, ln, "fn '%s' shadows a built-in", fn->name);
      *rp = 0;
      char *pv[DM_MAX_FNARGS + 1]; int np = 0;
      for (char *tk = strtok(lp + 1, ", \t"); tk; tk = strtok(NULL, ", \t")) { if (np > DM_MAX_FNARGS) fail(&c, ln, "too many fn params"); pv[np++] = tk; }
      if (np > DM_MAX_FNARGS) fail(&c, ln, "too many fn params");
      for (int i = 0; i < np; i++) set_name(&c, ln, fn->params[i], pv[i]);
      fn->nparams = np;
      int first; fn->body = read_block(&c, &first); fn->line = first;
      c.nfns++;
    } else if (!strcmp(k, "frame") || !strcmp(k, "master")) {
      if (argc != 2 || strcmp(argv[1], "{")) fail(&c, ln, "usage: %s {", k);
      int kind = k[0] == 'f' ? DM_PK_FRAME : DM_PK_MASTER;
      dm_prog *dst = kind == DM_PK_FRAME ? &m->frame : &m->master;
      int *flag = kind == DM_PK_FRAME ? &m->has_frame : &m->has_master;
      m->npixel = npixel_names;
      if (*flag) fail(&c, ln, "%s defined twice", k);
      int first; char *body = read_block(&c, &first);
      compile_program(&c, body, first, kind, dst, k);
      free(body);
      *flag = 1;
    } else fail(&c, ln, "unknown directive '%s'", k);
  }
  if (!m->norder) {
    for (int i = 0; i < DM_MAX_PATTERNS && m->norder < DM_MAX_ORDER; i++) if (m->pat[i].cells) m->order[m->norder++] = (uint8_t)i;
  }
  if (!m->norder) fail(&c, c.line, "no patterns");
  for (int i = 0; i < npixel_names; i++) if (!m->pixel[i].len && !m->pixel[i].off) { /* declared by pre-scan but never compiled: impossible */ }
  m->npixel = npixel_names;
  for (int i = 0; i < c.nfns; i++) free(c.fns[i].body);
  if (dm_module_verify(m)) { dm_module_free(m); return -1; }
  return 0;
}
