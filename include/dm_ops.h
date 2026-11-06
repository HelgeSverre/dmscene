/* dm_ops.h - the single source of truth for the dmscene VM instruction set.
 *
 *   X(ID, "name", nargs, kind, immbytes)
 *
 * kind:  'I' internal      (emitted by the compiler, not callable by name)
 *        'F' function      (callable in expressions, pushes exactly one value)
 *        'S' statement     (callable as a statement, pushes nothing)
 * immbytes: number of immediate operand bytes following the opcode.
 *
 * Stack effect of F/S ops is (1 - nargs) / (-nargs); internal ops are listed
 * in dm_vm.c (dm_op_stack_effect).  Structured if/else and counted loops are
 * verified at their joins before bytecode is executed.
 */
#ifndef DM_OPS_H
#define DM_OPS_H

#define DM_OPLIST(X) \
  X(NOP,    "nop",    0, 'I', 0) \
  X(PUSHK,  "pushk",  0, 'I', 4) \
  X(PUSHB,  "pushb",  0, 'I', 1) \
  X(LDL,    "ldl",    0, 'I', 1) \
  X(STL,    "stl",    0, 'I', 1) \
  X(LDI,    "ldi",    0, 'I', 1) \
  X(LDU,    "ldu",    0, 'I', 1) \
  X(STU,    "stu",    0, 'I', 1) \
  X(LDS,    "lds",    0, 'I', 1) \
  X(STS,    "sts",    0, 'I', 1) \
  X(DUP,    "dup",    0, 'I', 0) \
  X(POP,    "pop",    0, 'I', 0) \
  X(ADD,    "add",    0, 'I', 0) \
  X(SUB,    "sub",    0, 'I', 0) \
  X(MUL,    "mul",    0, 'I', 0) \
  X(DIV,    "div",    0, 'I', 0) \
  X(MOD,    "mod",    0, 'I', 0) \
  X(NEG,    "neg",    0, 'I', 0) \
  X(LT,     "lt",     0, 'I', 0) \
  X(LE,     "le",     0, 'I', 0) \
  X(GT,     "gt",     0, 'I', 0) \
  X(GE,     "ge",     0, 'I', 0) \
  X(EQ,     "eq",     0, 'I', 0) \
  X(NE,     "ne",     0, 'I', 0) \
  X(AND,    "and",    0, 'I', 0) \
  X(OR,     "or",     0, 'I', 0) \
  X(NOT,    "not",    0, 'I', 0) \
  X(SEL,    "sel",    0, 'I', 0) \
  X(RET,    "ret",    0, 'I', 0) \
  /* ---- math ---- */ \
  X(SIN,    "sin",    1, 'F', 0) \
  X(COS,    "cos",    1, 'F', 0) \
  X(TAN,    "tan",    1, 'F', 0) \
  X(ABS,    "abs",    1, 'F', 0) \
  X(FLOOR,  "floor",  1, 'F', 0) \
  X(CEIL,   "ceil",   1, 'F', 0) \
  X(FRACT,  "fract",  1, 'F', 0) \
  X(SQRT,   "sqrt",   1, 'F', 0) \
  X(EXP,    "exp",    1, 'F', 0) \
  X(LOG,    "log",    1, 'F', 0) \
  X(POW,    "pow",    2, 'F', 0) \
  X(MIN,    "min",    2, 'F', 0) \
  X(MAX,    "max",    2, 'F', 0) \
  X(CLAMP,  "clamp",  3, 'F', 0) \
  X(MIX,    "mix",    3, 'F', 0) \
  X(STEP,   "step",   2, 'F', 0) \
  X(SMOOTH, "smooth", 3, 'F', 0) \
  X(SIGN,   "sign",   1, 'F', 0) \
  X(ATAN2,  "atan2",  2, 'F', 0) \
  X(LEN,    "len",    2, 'F', 0) \
  X(TANH,   "tanh",   1, 'F', 0) \
  X(HASH,   "hash",   1, 'F', 0) \
  X(R1,     "r1",     0, 'F', 0) \
  X(R2,     "r2",     0, 'F', 0) \
  X(R3,     "r3",     0, 'F', 0) \
  /* ---- audio ---- */ \
  X(OSC,    "osc",    1, 'F', 0) \
  X(SAW,    "saw",    1, 'F', 0) \
  X(TRI,    "tri",    1, 'F', 0) \
  X(SQR,    "sqr",    2, 'F', 0) \
  X(NOISE,  "noise",  0, 'F', 0) \
  X(ENV,    "env",    4, 'F', 0) \
  X(LP,     "lp",     2, 'F', 1) \
  X(HP,     "hp",     2, 'F', 1) \
  X(DLY,    "dly",    3, 'F', 1) \
  X(DIST,   "dist",   2, 'F', 0) \
  X(SVF,    "svf",    3, 'F', 1) \
  X(CRUSH,  "crush",  2, 'F', 0) \
  X(USAW,   "usaw",   2, 'F', 1) \
  X(VERB,   "verb",   3, 'F', 1) \
  /* ---- fractal / geometry ---- */ \
  X(MANDEL, "mandel", 3, 'F', 0) \
  X(JULIA,  "julia",  5, 'F', 0) \
  X(SHIP,   "ship",   3, 'F', 0) \
  X(KALI,   "kali",   5, 'F', 0) \
  X(ROT,    "rot",    3, 'F', 0) \
  X(POLAR,  "polar",  2, 'F', 0) \
  X(FBM,    "fbm",    3, 'F', 0) \
  X(VOR,    "vor",    2, 'F', 0) \
  X(NEWTON, "newton", 3, 'F', 0) \
  X(KIFS,   "kifs",   5, 'F', 0) \
  X(PREV,   "prev",   2, 'F', 0) \
  /* ---- output / colour accumulator ---- */ \
  X(OUT1,   "out1",   1, 'S', 0) \
  X(OUT2,   "out2",   2, 'S', 0) \
  X(OUT3,   "rgb",    3, 'S', 0) \
  X(OUT4,   "out4",   4, 'S', 0) \
  X(PAL,    "pal",    1, 'S', 1) \
  X(PALMIX, "mixpal", 2, 'S', 1) \
  X(HSV,    "hsv",    3, 'S', 0) \
  X(CMUL,   "cmul",   1, 'S', 0) \
  X(CADD,   "cadd",   3, 'S', 0) \
  X(CGET,   "cget",   1, 'F', 0) \
  X(CPREV,  "cprev",  3, 'S', 0) \
  /* ---- v3: loops, text, samples, envelopes, events, scenes, post ---- */ \
  X(LOOP,   "loop",   0, 'I', 5) \
  X(ENDL,   "endl",   0, 'I', 3) \
  X(TAP,    "tap",    2, 'F', 0) \
  X(CPREVZ, "cprevz", 5, 'S', 0) \
  X(GLYPH,  "glyph",  3, 'F', 0) \
  X(STR,    "str",    1, 'F', 1) \
  X(STRLEN, "strlen", 0, 'F', 1) \
  X(SMP,    "smp",    1, 'F', 1) \
  X(SMPLEN, "smplen", 0, 'F', 1) \
  X(WAVE,   "wave",   1, 'F', 1) \
  X(VENV,   "venv",   0, 'F', 1) \
  X(BSAW,   "bsaw",   2, 'F', 0) \
  X(BSQR,   "bsqr",   3, 'F', 0) \
  X(FOLD,   "fold",   2, 'F', 0) \
  X(EVT,    "evt",    0, 'F', 1) \
  X(EVTL,   "evtl",   0, 'F', 1) \
  X(SCENE,  "scene",  0, 'F', 1) \
  X(SELECT, "select", 1, 'S', 0) \
  /* ---- v4: structured branches and embedded RGB textures ---- */ \
  X(IFZ,    "ifz",    0, 'I', 2) \
  X(ELSE,   "else",   0, 'I', 2) \
  X(ENDIF,  "endif",  0, 'I', 0) \
  X(TEX,    "tex",    2, 'F', 1)

enum {
#define X(id, name, n, k, imm) DM_OP_##id,
  DM_OPLIST(X)
#undef X
  DM_OP_COUNT
};

typedef struct {
  const char *name;
  unsigned char nargs;
  char kind;
  unsigned char imm;
} dm_opinfo;

extern const dm_opinfo dm_ops[DM_OP_COUNT];

#endif
