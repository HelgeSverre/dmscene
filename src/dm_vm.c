/* dm_vm.c - float stack machine + intrinsics + structured-control verifier. */
#include "dm.h"
#include <math.h>
#include <string.h>

const dm_opinfo dm_ops[DM_OP_COUNT] = {
#define X(id, name, n, k, imm) { name, n, k, imm },
  DM_OPLIST(X)
#undef X
};

uint32_t dm_rng_next(uint32_t *s) {
  uint32_t x = *s ? *s : 0x9e3779b9u;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  *s = x;
  return x;
}
float dm_rng_float(uint32_t *s) {
  return (float)(dm_rng_next(s) >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

int dm_op_stack_effect(int op) {
  const dm_opinfo *o = &dm_ops[op];
  if (o->kind == 'F') return 1 - o->nargs;
  if (o->kind == 'S') return -o->nargs;
  switch (op) {
    case DM_OP_PUSHK: case DM_OP_PUSHB: case DM_OP_LDL: case DM_OP_LDI:
    case DM_OP_LDU: case DM_OP_LDS: case DM_OP_DUP: return 1;
    case DM_OP_STL: case DM_OP_STU: case DM_OP_STS: case DM_OP_POP: case DM_OP_IFZ: return -1;
    case DM_OP_ADD: case DM_OP_SUB: case DM_OP_MUL: case DM_OP_DIV: case DM_OP_MOD:
    case DM_OP_LT: case DM_OP_LE: case DM_OP_GT: case DM_OP_GE: case DM_OP_EQ: case DM_OP_NE:
    case DM_OP_AND: case DM_OP_OR: return -1;
    case DM_OP_SEL: return -2;
    default: return 0;   /* NOP NEG NOT RET */
  }
}

/* ------------------------------------------------------------ intrinsics */
static inline float fractf(float x) { return x - floorf(x); }
static inline float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }

static float frac_escape(dm_vm *vm, float zx, float zy, float cx, float cy, int maxit, int kind) {
  float x = zx, y = zy, x2 = x * x, y2 = y * y;
  float trap = 1e9f, cross = 1e9f;
  int i;
  for (i = 0; i < maxit && x2 + y2 < 256.0f; i++) {
    if (kind == 2) { x = fabsf(x); y = fabsf(y); }           /* burning ship */
    y = 2.0f * x * y + cy;
    x = x2 - y2 + cx;
    x2 = x * x; y2 = y * y;
    float d = x2 + y2;
    if (d < trap) trap = d;
    float c = fabsf(x) < fabsf(y) ? fabsf(x) : fabsf(y);
    if (c < cross) cross = c;
  }
  vm->r[1] = sqrtf(trap);
  vm->r[2] = cross;
  vm->r[3] = atan2f(y, x);
  if (i >= maxit) return (float)maxit;
  float l = logf(x2 + y2) * 0.5f;             /* log|z| */
  if (l <= 0.0f) return (float)i;
  float nu = logf(l * 1.4426950409f) * 1.4426950409f;
  return (float)i + 1.0f - nu;
}

static float frac_kali(dm_vm *vm, float px, float py, float kx, float ky, int n) {
  float x = px, y = py, mind = 1e9f, acc = 0.0f;
  int i;
  for (i = 0; i < n; i++) {
    float d = x * x + y * y + 1e-9f;
    x = fabsf(x) / d - kx;
    y = fabsf(y) / d - ky;
    float l = sqrtf(x * x + y * y);
    if (l < mind) mind = l;
    acc += l;
  }
  vm->r[1] = n > 0 ? acc / (float)n : 0.0f;
  vm->r[2] = x;
  vm->r[3] = y;
  return mind;
}

static void hsv_to_rgb(float h, float s, float v, float *o) {
  h = fractf(h) * 6.0f;
  int i = (int)h; float f = h - (float)i;
  float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
  switch (i % 6) {
    case 0: o[0] = v; o[1] = t; o[2] = p; break;
    case 1: o[0] = q; o[1] = v; o[2] = p; break;
    case 2: o[0] = p; o[1] = v; o[2] = t; break;
    case 3: o[0] = p; o[1] = q; o[2] = v; break;
    case 4: o[0] = t; o[1] = p; o[2] = v; break;
    default: o[0] = v; o[1] = p; o[2] = q; break;
  }
}

static void pal_lookup(const dm_palette *p, float t, float *o) {
  float f = fractf(t) * 255.0f;
  int i = (int)f; float k = f - (float)i;
  int j = i + 1 > 255 ? 255 : i + 1;
  o[0] = p->lut[i][0] + (p->lut[j][0] - p->lut[i][0]) * k;
  o[1] = p->lut[i][1] + (p->lut[j][1] - p->lut[i][1]) * k;
  o[2] = p->lut[i][2] + (p->lut[j][2] - p->lut[i][2]) * k;
}

static inline float hash2(float x, float y) {
  float h = sinf(x * 127.1f + y * 311.7f) * 43758.5453f;
  return h - floorf(h);
}
static float vnoise(float x, float y) {
  float ix = floorf(x), iy = floorf(y), fx = x - ix, fy = y - iy;
  fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
  float a = hash2(ix, iy), b = hash2(ix + 1, iy), c = hash2(ix, iy + 1), d = hash2(ix + 1, iy + 1);
  return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}
static float fbm(float x, float y, int oct) {
  float v = 0, amp = 0.5f, norm = 0;
  for (int i = 0; i < oct; i++) {
    v += vnoise(x, y) * amp; norm += amp; amp *= 0.5f;
    float nx = x * 1.9f + y * 0.6f + 7.3f, ny = y * 1.9f - x * 0.6f + 3.1f;
    x = nx; y = ny;
  }
  return norm > 0 ? v / norm : 0;
}
static float voronoi(dm_vm *vm, float x, float y) {
  float ix = floorf(x), iy = floorf(y), d1 = 1e9f, d2 = 1e9f, id = 0;
  for (int j = -1; j <= 1; j++) for (int i = -1; i <= 1; i++) {
    float cx = ix + i, cy = iy + j;
    float px = cx + hash2(cx, cy), py = cy + hash2(cx + 17.0f, cy + 9.0f);
    float dx = px - x, dy = py - y, d = dx * dx + dy * dy;
    if (d < d1) { d2 = d1; d1 = d; id = hash2(cx + 3.0f, cy + 5.0f); }
    else if (d < d2) d2 = d;
  }
  vm->r[1] = id; vm->r[2] = sqrtf(d2);
  return sqrtf(d1);
}
static float frac_newton(dm_vm *vm, float zx, float zy, int maxit) {
  int i; float rx = 0, ry = 0;
  for (i = 0; i < maxit; i++) {
    float x2 = zx * zx - zy * zy, y2 = 2 * zx * zy;              /* z^2 */
    float fx = x2 * zx - y2 * zy - 1, fy = x2 * zy + y2 * zx;    /* z^3 - 1 */
    float dx = 3 * x2, dy = 3 * y2;                              /* 3 z^2 */
    float den = dx * dx + dy * dy; if (den < 1e-12f) break;
    float qx = (fx * dx + fy * dy) / den, qy = (fy * dx - fx * dy) / den;
    zx -= qx; zy -= qy;
    if (qx * qx + qy * qy < 1e-8f) break;
  }
  float ang = atan2f(zy, zx);
  int root = ang > 1.0472f ? 1 : (ang < -1.0472f ? 2 : 0);
  rx = root == 0 ? 1.0f : -0.5f; ry = root == 0 ? 0.0f : (root == 1 ? 0.8660254f : -0.8660254f);
  vm->r[1] = (float)i; vm->r[2] = sqrtf((zx - rx) * (zx - rx) + (zy - ry) * (zy - ry)); vm->r[3] = ang;
  return (float)root;
}
static float frac_kifs(dm_vm *vm, float x, float y, float scale, float ang, int n) {
  float cs = cosf(ang), sn = sinf(ang), trap = 1e9f;
  if (scale < 1.05f) scale = 1.05f;
  for (int i = 0; i < n; i++) {
    x = fabsf(x); y = fabsf(y);
    float rx = x * cs - y * sn, ry = x * sn + y * cs;
    x = rx * scale - (scale - 1.0f); y = ry * scale - (scale - 1.0f) * 0.5f;
    float l = sqrtf(x * x + y * y); if (l < trap) trap = l;
  }
  vm->r[1] = trap; vm->r[2] = x; vm->r[3] = y;
  return sqrtf(x * x + y * y) * powf(scale, -(float)n);
}
static void sample_rgb(const uint8_t *rgb, int w, int h, float x, float y, float *o) {
  if (!rgb || w < 1 || h < 1 || !isfinite(x) || !isfinite(y)) { o[0] = o[1] = o[2] = 0; return; }
  float fx = clampf(x * w - 0.5f, 0, (float)(w - 1));
  float fy = clampf(y * h - 0.5f, 0, (float)(h - 1));
  int ix = (int)fx, iy = (int)fy; float kx = fx - ix, ky = fy - iy;
  int ix1 = ix + 1 < w ? ix + 1 : ix, iy1 = iy + 1 < h ? iy + 1 : iy;
  const uint8_t *a = rgb + ((size_t)iy * w + ix) * 3, *b = rgb + ((size_t)iy * w + ix1) * 3;
  const uint8_t *c = rgb + ((size_t)iy1 * w + ix) * 3, *d = rgb + ((size_t)iy1 * w + ix1) * 3;
  for (int i = 0; i < 3; i++) {
    float top = a[i] + (b[i] - a[i]) * kx, bot = c[i] + (d[i] - c[i]) * kx;
    o[i] = (top + (bot - top) * ky) * (1.0f / 255.0f);
  }
}
static void sample_prev(const dm_vm *vm, float x, float y, float *o) {
  sample_rgb(vm->prev, vm->pw, vm->ph, x, y, o);
}

static float sample_read(const dm_module *m, int id, double pos_samples) {
  if (!m || id < 0 || id >= m->nsmp || !m->smp[id].pcm) return 0.0f;
  const dm_sample *sm = &m->smp[id];
  if (pos_samples < 0 || pos_samples >= (double)(sm->len - 1)) return 0.0f;
  uint32_t i = (uint32_t)pos_samples; float f = (float)(pos_samples - i);
  return (sm->pcm[i] + (sm->pcm[i + 1] - sm->pcm[i]) * f) * (1.0f / 127.0f);
}
static float venv_eval(const dm_venv *ve, float t, float rel) {
  if (ve->npts == 0) return 0.0f;
  float held = rel < 0.0f ? t : t - rel;
  int sus = ve->sustain < ve->npts ? ve->sustain : -1;
  float tt = held;
  if (sus >= 0) {
    if (rel < 0.0f) { if (tt > ve->t[sus]) tt = ve->t[sus]; }
    else { float ts = held < ve->t[sus] ? held : ve->t[sus]; tt = ts + rel; }
  }
  if (tt <= ve->t[0]) return ve->v[0];
  for (int i = 1; i < ve->npts; i++) {
    if (tt <= ve->t[i]) {
      float span = ve->t[i] - ve->t[i - 1];
      float k = span > 1e-6f ? (tt - ve->t[i - 1]) / span : 1.0f;
      return ve->v[i - 1] + (ve->v[i] - ve->v[i - 1]) * k;
    }
  }
  return ve->v[ve->npts - 1];
}
static inline float polyblep(float t, float dt) {
  if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
  if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
  return 0.0f;
}
static void sample_buf(const uint8_t *buf, int w, int h, int x, int y, float *o) {
  if (!buf) { o[0] = o[1] = o[2] = 0; return; }
  if (x < 0) x = 0; if (y < 0) y = 0; if (x >= w) x = w - 1; if (y >= h) y = h - 1;
  const uint8_t *p = buf + ((size_t)y * w + x) * 3;
  o[0] = p[0] * (1.0f / 255.0f); o[1] = p[1] * (1.0f / 255.0f); o[2] = p[2] * (1.0f / 255.0f);
}

/* ------------------------------------------------------------ run */
void dm_vm_run(dm_vm *vm, const uint8_t *code, uint32_t len) {
  float st[DM_MAX_STACK + 4];
  int sp = 0;
  const uint8_t *pc = code, *end = code + len;
  int loopc[DM_MAX_LOOP_DEPTH]; int lsp = 0;
  const float *in = vm->in;
  float a, b, c, d;
#define PUSH(v) (st[sp++] = (v))
#define POP()   (st[--sp])
#define TOP     (st[sp - 1])
#define IMM8()  (*pc++)
  while (pc < end) {
    switch (*pc++) {
      case DM_OP_NOP: break;
      case DM_OP_PUSHK: { float k; memcpy(&k, pc, 4); pc += 4; PUSH(k); break; }
      case DM_OP_PUSHB: PUSH((float)(int8_t)IMM8()); break;
      case DM_OP_LDL: PUSH(vm->locals[IMM8()]); break;
      case DM_OP_STL: vm->locals[IMM8()] = POP(); break;
      case DM_OP_LDI: PUSH(in[IMM8()]); break;
      case DM_OP_LDU: PUSH(vm->uni[IMM8()]); break;
      case DM_OP_STU: vm->uni[IMM8()] = POP(); break;
      case DM_OP_LDS: PUSH(vm->state[IMM8()]); break;
      case DM_OP_STS: vm->state[IMM8()] = POP(); break;
      case DM_OP_DUP: a = TOP; PUSH(a); break;
      case DM_OP_POP: sp--; break;
      case DM_OP_ADD: b = POP(); TOP += b; break;
      case DM_OP_SUB: b = POP(); TOP -= b; break;
      case DM_OP_MUL: b = POP(); TOP *= b; break;
      case DM_OP_DIV: b = POP(); TOP = b != 0.0f ? TOP / b : 0.0f; break;
      case DM_OP_MOD: b = POP(); TOP = b != 0.0f ? TOP - b * floorf(TOP / b) : 0.0f; break;
      case DM_OP_NEG: TOP = -TOP; break;
      case DM_OP_LT: b = POP(); TOP = TOP <  b ? 1.0f : 0.0f; break;
      case DM_OP_LE: b = POP(); TOP = TOP <= b ? 1.0f : 0.0f; break;
      case DM_OP_GT: b = POP(); TOP = TOP >  b ? 1.0f : 0.0f; break;
      case DM_OP_GE: b = POP(); TOP = TOP >= b ? 1.0f : 0.0f; break;
      case DM_OP_EQ: b = POP(); TOP = TOP == b ? 1.0f : 0.0f; break;
      case DM_OP_NE: b = POP(); TOP = TOP != b ? 1.0f : 0.0f; break;
      case DM_OP_AND: b = POP(); TOP = (TOP != 0.0f && b != 0.0f) ? 1.0f : 0.0f; break;
      case DM_OP_OR:  b = POP(); TOP = (TOP != 0.0f || b != 0.0f) ? 1.0f : 0.0f; break;
      case DM_OP_NOT: TOP = TOP == 0.0f ? 1.0f : 0.0f; break;
      case DM_OP_SEL: c = POP(); b = POP(); TOP = TOP != 0.0f ? b : c; break;
      case DM_OP_RET: return;
      case DM_OP_IFZ: { int skip = pc[0] | (pc[1] << 8); pc += 2; if (POP() == 0.0f) pc += skip; break; }
      case DM_OP_ELSE: { int skip = pc[0] | (pc[1] << 8); pc += 2 + skip; break; }
      case DM_OP_ENDIF: break;

      case DM_OP_SIN: TOP = sinf(TOP); break;
      case DM_OP_COS: TOP = cosf(TOP); break;
      case DM_OP_TAN: TOP = tanf(TOP); break;
      case DM_OP_ABS: TOP = fabsf(TOP); break;
      case DM_OP_FLOOR: TOP = floorf(TOP); break;
      case DM_OP_CEIL: TOP = ceilf(TOP); break;
      case DM_OP_FRACT: TOP = fractf(TOP); break;
      case DM_OP_SQRT: TOP = TOP > 0.0f ? sqrtf(TOP) : 0.0f; break;
      case DM_OP_EXP: TOP = expf(TOP < 80.0f ? TOP : 80.0f); break;
      case DM_OP_LOG: TOP = TOP > 0.0f ? logf(TOP) : -80.0f; break;
      case DM_OP_POW: b = POP();                  /* negative bases allowed for integer exponents */
        if (TOP > 0.0f) TOP = powf(TOP, b);
        else if (TOP == 0.0f) TOP = b > 0.0f ? 0.0f : (b == 0.0f ? 1.0f : 0.0f);
        else TOP = b == floorf(b) ? powf(TOP, b) : 0.0f;
        break;
      case DM_OP_MIN: b = POP(); if (b < TOP) TOP = b; break;
      case DM_OP_MAX: b = POP(); if (b > TOP) TOP = b; break;
      case DM_OP_CLAMP: c = POP(); b = POP(); TOP = clampf(TOP, b, c); break;
      case DM_OP_MIX: c = POP(); b = POP(); TOP = TOP + (b - TOP) * c; break;
      case DM_OP_STEP: b = POP(); TOP = b >= TOP ? 1.0f : 0.0f; break;        /* step(edge, x) */
      case DM_OP_SMOOTH: c = POP(); b = POP(); a = TOP;                      /* smooth(e0, e1, x) */
        d = b != a ? clampf((c - a) / (b - a), 0.0f, 1.0f) : (c >= b ? 1.0f : 0.0f);
        TOP = d * d * (3.0f - 2.0f * d); break;
      case DM_OP_SIGN: TOP = TOP > 0.0f ? 1.0f : (TOP < 0.0f ? -1.0f : 0.0f); break;
      case DM_OP_ATAN2: b = POP(); TOP = atan2f(TOP, b); break;
      case DM_OP_LEN: b = POP(); TOP = sqrtf(TOP * TOP + b * b); break;
      case DM_OP_TANH: TOP = tanhf(TOP); break;
      case DM_OP_HASH: TOP = fractf(sinf(TOP * 12.9898f) * 43758.5453f); break;
      case DM_OP_R1: PUSH(vm->r[1]); break;
      case DM_OP_R2: PUSH(vm->r[2]); break;
      case DM_OP_R3: PUSH(vm->r[3]); break;

      case DM_OP_OSC: TOP = sinf(TOP * DM_TAU); break;
      case DM_OP_SAW: TOP = 2.0f * fractf(TOP) - 1.0f; break;
      case DM_OP_TRI: TOP = 1.0f - 4.0f * fabsf(fractf(TOP) - 0.5f); break;
      case DM_OP_SQR: b = POP(); TOP = fractf(TOP) < b ? 1.0f : -1.0f; break;
      case DM_OP_NOISE: PUSH(dm_rng_float(vm->rng)); break;
      case DM_OP_ENV: {
        float r = POP(), s = POP(), dd = POP(), at = TOP;
        float t = in[DM_IN_VT], rel = in[DM_IN_REL];
        float held = rel < 0.0f ? t : t - rel;
        if (at < 1e-4f) at = 1e-4f; if (dd < 1e-4f) dd = 1e-4f; if (r < 1e-4f) r = 1e-4f;
        float lvl = held < at ? held / at : (held < at + dd ? 1.0f - (1.0f - s) * (held - at) / dd : s);
        if (rel >= 0.0f) { float k = 1.0f - rel / r; lvl *= k > 0.0f ? k : 0.0f; }
        TOP = lvl; break;
      }
      case DM_OP_LP: case DM_OP_HP: {
        int op = pc[-1]; uint8_t slot = IMM8();
        float cut = POP(), x = TOP;
        float k = 1.0f - expf(-DM_TAU * clampf(cut, 0.0f, in[DM_IN_SR] * 0.45f) / in[DM_IN_SR]);
        float *s = &vm->state[slot];
        *s += k * (x - *s);
        TOP = op == DM_OP_LP ? *s : x - *s;
        break;
      }
      case DM_OP_DLY: {
        uint8_t line = IMM8();
        float fb = POP(), tm = POP(), x = TOP;
        if (!vm->dly || !vm->dly[line]) { TOP = 0.0f; break; }
        uint32_t n = vm->dlylen;
        uint32_t dl = (uint32_t)clampf(tm * in[DM_IN_SR], 1.0f, (float)(n - 1));
        uint32_t pos = vm->dlypos[line];
        float rd = vm->dly[line][(pos + n - dl) % n];
        if (fb > 0.98f) fb = 0.98f; if (fb < -0.98f) fb = -0.98f;
        vm->dly[line][pos] = x + rd * fb;
        vm->dlypos[line] = (pos + 1) % n;
        TOP = rd; break;
      }
      case DM_OP_DIST: b = POP(); TOP = b > 1e-3f ? tanhf(TOP * b) / tanhf(b) : TOP; break;
      case DM_OP_SVF: {                       /* Chamberlin SVF, 2x oversampled: returns LP, r1 = HP, r2 = BP */
        uint8_t slot = IMM8();
        float res = POP(), cut = POP(), x = TOP;
        float f = sinf(DM_PI * clampf(cut, 10.0f, in[DM_IN_SR] * 0.24f) / in[DM_IN_SR]);
        float q = 1.0f - clampf(res, 0.0f, 0.98f);
        float *low = &vm->state[slot], *band = &vm->state[slot + 1], high = 0;
        for (int k = 0; k < 2; k++) { *low += f * *band; high = x - *low - q * *band; *band += f * high; }
        if (*low != *low || fabsf(*low) > 1e4f) { *low = *band = 0; high = 0; }
        vm->r[1] = high; vm->r[2] = *band; TOP = *low; break;
      }
      case DM_OP_CRUSH: b = POP(); if (b < 1) b = 1; TOP = floorf(TOP * b + 0.5f) / b; break;
      case DM_OP_USAW: {                      /* 7 detuned saws, own phases in state[slot..slot+6] */
        uint8_t slot = IMM8();
        float det = POP(), f = TOP, inv = 1.0f / in[DM_IN_SR], sum = 0.0f;
        float *phs = &vm->state[slot];
        for (int k = 0; k < DM_USAW_VOICES; k++) {
          float semis = det * ((float)k - 3.0f) / 3.0f;
          float ratio = 1.0f + semis * 0.0577623f + semis * semis * 0.00167f;   /* ~2^(semis/12) */
          phs[k] += f * ratio * inv; phs[k] -= floorf(phs[k]);
          sum += 2.0f * phs[k] - 1.0f;
        }
        TOP = sum * 0.38f; break;
      }
      case DM_OP_VERB: {                      /* freeverb-lite: 4 parallel combs -> 2 series allpasses, wet only */
        uint8_t id = IMM8();
        float damp = POP(), decay = POP(), x = TOP;
        if (!vm->verb || id >= vm->nverb || !vm->verb[id].buf) { TOP = 0.0f; break; }
        dm_verb *V = &vm->verb[id];
        static const uint32_t clen[4] = {1116, 1188, 1277, 1356}, alen[2] = {556, 441};
        uint32_t spread = 23u * id, base = 0; float acc = 0.0f;
        float fb = clampf(decay, 0.0f, 0.98f), dp = clampf(damp, 0.0f, 0.99f);
        for (int k = 0; k < 4; k++) {
          uint32_t n = clen[k] + spread; float *b = V->buf + base; uint32_t p = V->pos[k];
          float y = b[p];
          V->lp[k] = y * (1.0f - dp) + V->lp[k] * dp;
          b[p] = x + V->lp[k] * fb;
          V->pos[k] = p + 1 >= n ? 0 : p + 1;
          acc += y; base += n;
        }
        acc *= 0.25f;
        for (int k = 0; k < 2; k++) {
          uint32_t n = alen[k] + spread; float *b = V->buf + base; uint32_t p = V->pos[4 + k];
          float y = b[p];
          b[p] = acc + y * 0.5f;
          acc = y - acc * 0.5f;
          V->pos[4 + k] = p + 1 >= n ? 0 : p + 1;
          base += n;
        }
        if (acc != acc) acc = 0.0f;
        TOP = acc; break;
      }

      case DM_OP_MANDEL: c = POP(); b = POP(); a = TOP;
        TOP = frac_escape(vm, 0.0f, 0.0f, a, b, (int)clampf(c, 1.0f, 1024.0f), 0); break;
      case DM_OP_JULIA: { float it = POP(); d = POP(); c = POP(); b = POP(); a = TOP;
        TOP = frac_escape(vm, a, b, c, d, (int)clampf(it, 1.0f, 1024.0f), 1); break; }
      case DM_OP_SHIP: c = POP(); b = POP(); a = TOP;
        TOP = frac_escape(vm, 0.0f, 0.0f, a, b, (int)clampf(c, 1.0f, 1024.0f), 2); break;
      case DM_OP_KALI: { float it = POP(); d = POP(); c = POP(); b = POP(); a = TOP;
        TOP = frac_kali(vm, a, b, c, d, (int)clampf(it, 1.0f, 64.0f)); break; }
      case DM_OP_ROT: { float ang = POP(); b = POP(); a = TOP; float cs = cosf(ang), sn = sinf(ang);
        TOP = a * cs - b * sn; vm->r[1] = a * sn + b * cs; break; }
      case DM_OP_POLAR: b = POP(); a = TOP; TOP = sqrtf(a * a + b * b); vm->r[1] = atan2f(b, a); break;

      case DM_OP_FBM: c = POP(); b = POP(); a = TOP; TOP = fbm(a, b, (int)clampf(c, 1, 8)); break;
      case DM_OP_VOR: b = POP(); a = TOP; TOP = voronoi(vm, a, b); break;
      case DM_OP_NEWTON: c = POP(); b = POP(); a = TOP; TOP = frac_newton(vm, a, b, (int)clampf(c, 1, 256)); break;
      case DM_OP_KIFS: { float it = POP(); d = POP(); c = POP(); b = POP(); a = TOP; TOP = frac_kifs(vm, a, b, c, d, (int)clampf(it, 1, 32)); break; }
      case DM_OP_PREV: { float col[3]; b = POP(); a = TOP; sample_prev(vm, a, b, col); TOP = col[0]; vm->r[1] = col[1]; vm->r[2] = col[2]; break; }
      case DM_OP_TEX: { uint8_t id = IMM8(); float col[3]; b = POP(); a = TOP;
        const dm_texture *tx = vm->m && id < vm->m->ntex ? &vm->m->tex[id] : NULL;
        sample_rgb(tx ? tx->rgb : NULL, tx ? tx->w : 0, tx ? tx->h : 0, a, b, col);
        TOP = col[0]; vm->r[1] = col[1]; vm->r[2] = col[2]; break; }
      case DM_OP_CPREV: { float col[3]; c = POP(); b = POP(); a = POP(); sample_prev(vm, a, b, col);
        vm->out[0] += (col[0] - vm->out[0]) * c; vm->out[1] += (col[1] - vm->out[1]) * c; vm->out[2] += (col[2] - vm->out[2]) * c; break; }

      case DM_OP_LOOP: {                      /* slot count(u16) skip(u16): locals[slot] = 0; count 0 skips the body */
        uint8_t slot = IMM8(); int count = pc[0] | (pc[1] << 8); int skip = pc[2] | (pc[3] << 8); pc += 4;
        vm->locals[slot] = 0.0f;
        if (count <= 0) { pc += skip; break; }
        if (lsp < DM_MAX_LOOP_DEPTH) loopc[lsp++] = count;
        break;
      }
      case DM_OP_ENDL: {                      /* slot back(u16): ++locals[slot]; if < count jump back */
        uint8_t slot = IMM8(); int back = pc[0] | (pc[1] << 8); pc += 2;
        int count = lsp > 0 ? loopc[lsp - 1] : 0;
        float i = vm->locals[slot] + 1.0f; vm->locals[slot] = i;
        if ((int)i < count) pc -= back; else if (lsp > 0) lsp--;
        break;
      }
      case DM_OP_TAP: { float col[3]; b = POP(); a = TOP;
        sample_buf(vm->cur, vm->pw, vm->ph, (int)(in[DM_IN_X] * vm->pw + a), (int)(in[DM_IN_Y] * vm->ph + b), col);
        TOP = col[0]; vm->r[1] = col[1]; vm->r[2] = col[2]; break; }
      case DM_OP_CPREVZ: {                    /* pivot px,py (0..1), zoom, angle, k: sample prev around the pivot */
        float col[3]; float k = POP(), ang = POP(), zm = POP(), py = POP(), px = TOP; sp--;
        float asp = in[DM_IN_ASPECT];
        float dx = (in[DM_IN_X] - px) * asp, dy = in[DM_IN_Y] - py;
        float cs = cosf(ang), sn = sinf(ang);
        if (zm < 1e-3f) zm = 1e-3f;
        float rx = (dx * cs - dy * sn) / zm, ry = (dx * sn + dy * cs) / zm;
        sample_prev(vm, px + rx / asp, py + ry, col);
        vm->out[0] += (col[0] - vm->out[0]) * k; vm->out[1] += (col[1] - vm->out[1]) * k; vm->out[2] += (col[2] - vm->out[2]) * k;
        break; }
      case DM_OP_GLYPH: { float gy = POP(), gx = POP(); int code = (int)TOP;
        int cx = (int)floorf(gx * 6.0f), cy = (int)floorf(gy * 8.0f);
        if (code < 32 || code > 126 || cx < 0 || cx > 4 || cy < 0 || cy > 7 || gx < 0 || gy < 0) { TOP = 0.0f; break; }
        TOP = (dm_font5x7[code - 32][cx] >> cy) & 1 ? 1.0f : 0.0f; break; }
      case DM_OP_STR: { uint8_t id = IMM8(); int i = (int)floorf(TOP);
        const dm_text *tx = vm->m && id < vm->m->ntext ? &vm->m->text[id] : NULL;
        TOP = (tx && i >= 0 && i < tx->len) ? (float)(unsigned char)tx->s[i] : 32.0f; break; }
      case DM_OP_STRLEN: { uint8_t id = IMM8(); PUSH(vm->m && id < vm->m->ntext ? (float)vm->m->text[id].len : 0.0f); break; }
      case DM_OP_SMP: { uint8_t id = IMM8(); float sec = TOP;
        TOP = vm->m && id < vm->m->nsmp ? sample_read(vm->m, id, (double)sec * vm->m->smp[id].rate) : 0.0f; break; }
      case DM_OP_SMPLEN: { uint8_t id = IMM8(); PUSH(vm->m && id < vm->m->nsmp && vm->m->smp[id].rate ? (float)vm->m->smp[id].len / vm->m->smp[id].rate : 0.0f); break; }
      case DM_OP_WAVE: { uint8_t id = IMM8(); float ph = fractf(TOP);
        TOP = vm->m && id < vm->m->nsmp ? sample_read(vm->m, id, (double)ph * (vm->m->smp[id].len - 1)) : 0.0f; break; }
      case DM_OP_VENV: { uint8_t id = IMM8(); PUSH(vm->m && id < vm->m->nvenv ? venv_eval(&vm->m->venv[id], in[DM_IN_VT], in[DM_IN_REL]) : 0.0f); break; }
      case DM_OP_BSAW: { float hz = POP(); float t = fractf(TOP), dt = hz / in[DM_IN_SR]; if (dt > 0.5f) dt = 0.5f;
        TOP = 2.0f * t - 1.0f - polyblep(t, dt); break; }
      case DM_OP_BSQR: { float hz = POP(), pw = POP(); float t = fractf(TOP), dt = hz / in[DM_IN_SR]; if (dt > 0.5f) dt = 0.5f;
        pw = clampf(pw, 0.02f, 0.98f);
        float v = t < pw ? 1.0f : -1.0f;
        v += polyblep(t, dt); v -= polyblep(fractf(t - pw + 1.0f), dt);
        TOP = v; break; }
      case DM_OP_FOLD: { float k = POP(); float x = TOP * (k < 0.0f ? 0.0f : k); TOP = 4.0f * fabsf(fractf(x * 0.25f + 0.25f) - 0.5f) - 1.0f; break; }
      case DM_OP_EVT: { uint8_t id = IMM8(); PUSH(vm->evhold && id < DM_MAX_EVNAMES ? vm->evhold[id] : 0.0f); break; }
      case DM_OP_EVTL: { uint8_t id = IMM8(); PUSH(vm->evlerp && id < DM_MAX_EVNAMES ? vm->evlerp[id] : 0.0f); break; }
      case DM_OP_SCENE: { uint8_t id = IMM8(); PUSH((float)id); break; }
      case DM_OP_SELECT: { int i = (int)POP(); if (i < 0) i = 0; vm->scene = i; break; }

      case DM_OP_OUT1: vm->out[0] = POP(); break;
      case DM_OP_OUT2: vm->out[1] = POP(); vm->out[0] = POP(); break;
      case DM_OP_OUT3: vm->out[2] = POP(); vm->out[1] = POP(); vm->out[0] = POP(); break;
      case DM_OP_OUT4: vm->out[3] = POP(); vm->out[2] = POP(); vm->out[1] = POP(); vm->out[0] = POP(); break;
      case DM_OP_PAL: { uint8_t id = IMM8(); a = POP(); if (id < vm->npal) pal_lookup(&vm->pal[id], a, vm->out); break; }
      case DM_OP_PALMIX: { uint8_t id = IMM8(); float col[3]; b = POP(); a = POP();
        if (id < vm->npal) { pal_lookup(&vm->pal[id], a, col);
          vm->out[0] += (col[0] - vm->out[0]) * b; vm->out[1] += (col[1] - vm->out[1]) * b; vm->out[2] += (col[2] - vm->out[2]) * b; }
        break; }
      case DM_OP_HSV: c = POP(); b = POP(); a = POP(); hsv_to_rgb(a, b, c, vm->out); break;
      case DM_OP_CMUL: a = POP(); vm->out[0] *= a; vm->out[1] *= a; vm->out[2] *= a; break;
      case DM_OP_CADD: c = POP(); b = POP(); a = POP(); vm->out[0] += a; vm->out[1] += b; vm->out[2] += c; break;
      case DM_OP_CGET: { int i = (int)TOP; TOP = (i >= 0 && i < 4) ? vm->out[i] : 0.0f; break; }
      default: return;   /* unreachable after verification */
    }
  }
#undef PUSH
#undef POP
#undef TOP
#undef IMM8
}

/* ------------------------------------------------------------ verify */
static int op_pops(int op) {
  const dm_opinfo *o = &dm_ops[op];
  if (o->kind != 'I') return o->nargs;
  switch (op) {
    case DM_OP_STL: case DM_OP_STU: case DM_OP_STS: case DM_OP_POP:
    case DM_OP_NEG: case DM_OP_NOT: case DM_OP_DUP: case DM_OP_IFZ: return 1;
    case DM_OP_ADD: case DM_OP_SUB: case DM_OP_MUL: case DM_OP_DIV: case DM_OP_MOD:
    case DM_OP_LT: case DM_OP_LE: case DM_OP_GT: case DM_OP_GE: case DM_OP_EQ: case DM_OP_NE:
    case DM_OP_AND: case DM_OP_OR: return 2;
    case DM_OP_SEL: return 3;
    default: return 0;
  }
}

int dm_prog_verify(const dm_module *m, const dm_prog *p, const char *what) {
  if ((uint32_t)p->off + p->len > m->codelen) { dm_set_error("%s: code out of range", what); return -1; }
  if (p->nlocals > DM_MAX_LOCALS || p->nstate > DM_MAX_STATE || p->ndelay > DM_MAX_DELAY_LINES || p->nverb > DM_MAX_VERBS) {
    dm_set_error("%s: resource counts too large", what); return -1;
  }
  const uint8_t *pc = m->code + p->off, *end = pc + p->len;
  int depth = 0, maxd = 0;
  const uint8_t *lstart[DM_MAX_LOOP_DEPTH], *lend[DM_MAX_LOOP_DEPTH];
  int ldepth[DM_MAX_LOOP_DEPTH], lifsp[DM_MAX_LOOP_DEPTH], lsp = 0;
  struct { const uint8_t *target, *end; int depth, loop, has_else; } branch[32];
  int bsp = 0;
  while (pc < end) {
    int op = *pc++;
    if (op >= DM_OP_COUNT) { dm_set_error("%s: bad opcode %d", what, op); return -1; }
    const dm_opinfo *o = &dm_ops[op];
    if ((size_t)(end - pc) < o->imm) { dm_set_error("%s: truncated immediate", what); return -1; }
    uint32_t imm = o->imm == 1 ? pc[0] : 0;
    switch (op) {
      case DM_OP_LDL: case DM_OP_STL: if (imm >= p->nlocals) { dm_set_error("%s: local %u out of range", what, imm); return -1; } break;
      case DM_OP_USAW: if (imm + DM_USAW_VOICES > p->nstate) { dm_set_error("%s: state %u out of range", what, imm); return -1; } break;
      case DM_OP_VERB: if (imm >= p->nverb) { dm_set_error("%s: reverb %u out of range", what, imm); return -1; } break;
      case DM_OP_SVF: if (imm + 1 >= p->nstate) { dm_set_error("%s: state %u out of range", what, imm); return -1; } break;
      case DM_OP_LDS: case DM_OP_STS: case DM_OP_LP: case DM_OP_HP:
        if (imm >= p->nstate) { dm_set_error("%s: state %u out of range", what, imm); return -1; } break;
      case DM_OP_LDI: if (imm >= DM_IN_COUNT) { dm_set_error("%s: input %u out of range", what, imm); return -1; } break;
      case DM_OP_LDU: case DM_OP_STU: if (imm >= DM_MAX_UNIFORMS) { dm_set_error("%s: uniform %u out of range", what, imm); return -1; } break;
      case DM_OP_PAL: case DM_OP_PALMIX: if ((int)imm >= m->npal) { dm_set_error("%s: palette %u out of range", what, imm); return -1; } break;
      case DM_OP_DLY: if (imm >= p->ndelay) { dm_set_error("%s: delay line %u out of range", what, imm); return -1; } break;
      case DM_OP_STR: case DM_OP_STRLEN: if ((int)pc[0] >= m->ntext) { dm_set_error("%s: text %u out of range", what, pc[0]); return -1; } break;
      case DM_OP_SMP: case DM_OP_SMPLEN: case DM_OP_WAVE: if ((int)pc[0] >= m->nsmp) { dm_set_error("%s: sample %u out of range", what, pc[0]); return -1; } break;
      case DM_OP_TEX: if ((int)pc[0] >= m->ntex) { dm_set_error("%s: texture %u out of range", what, pc[0]); return -1; } break;
      case DM_OP_VENV: if ((int)pc[0] >= m->nvenv) { dm_set_error("%s: envelope %u out of range", what, pc[0]); return -1; } break;
      case DM_OP_EVT: case DM_OP_EVTL: if ((int)pc[0] >= m->nevname) { dm_set_error("%s: event %u out of range", what, pc[0]); return -1; } break;
      case DM_OP_SCENE: if ((int)pc[0] >= m->npixel) { dm_set_error("%s: scene %u out of range", what, pc[0]); return -1; } break;
      case DM_OP_LOOP: {
        if (pc[0] >= p->nlocals) { dm_set_error("%s: loop counter local out of range", what); return -1; }
        if (lsp >= DM_MAX_LOOP_DEPTH) { dm_set_error("%s: loops nested too deep", what); return -1; }
        int skip = pc[3] | (pc[4] << 8);
        if ((size_t)(end - (pc + 5)) < (size_t)skip) { dm_set_error("%s: loop skip out of range", what); return -1; }
        lstart[lsp] = pc + 5; lend[lsp] = pc + 5 + skip; ldepth[lsp] = depth; lifsp[lsp] = bsp; lsp++;
        break;
      }
      case DM_OP_ENDL: {
        if (lsp == 0) { dm_set_error("%s: endl without loop", what); return -1; }
        int back = pc[1] | (pc[2] << 8);
        lsp--;
        if (bsp != lifsp[lsp] || pc + 3 != lend[lsp]) { dm_set_error("%s: loop crosses a branch or skip offset mismatch", what); return -1; }
        if ((size_t)back > (size_t)(pc + 3 - (m->code + p->off)) || pc + 3 - back != lstart[lsp]) { dm_set_error("%s: loop back offset mismatch", what); return -1; }
        if (depth != ldepth[lsp]) { dm_set_error("%s: stack depth changes across loop body", what); return -1; }
        break;
      }
      case DM_OP_IFZ: {
        if (bsp >= 32) { dm_set_error("%s: if blocks nested too deep", what); return -1; }
        int skip = pc[0] | (pc[1] << 8);
        if ((size_t)(end - (pc + 2)) < (size_t)skip) { dm_set_error("%s: if target out of range", what); return -1; }
        branch[bsp].target = pc + 2 + skip;
        branch[bsp].end = NULL;
        branch[bsp].depth = depth - 1;
        branch[bsp].loop = lsp;
        branch[bsp].has_else = 0;
        bsp++;
        break;
      }
      case DM_OP_ELSE: {
        if (!bsp || branch[bsp - 1].has_else || branch[bsp - 1].loop != lsp || branch[bsp - 1].target != pc + 2) {
          dm_set_error("%s: malformed else target", what); return -1;
        }
        if (depth != branch[bsp - 1].depth) { dm_set_error("%s: stack depth differs at else", what); return -1; }
        int skip = pc[0] | (pc[1] << 8);
        if ((size_t)(end - (pc + 2)) < (size_t)skip) { dm_set_error("%s: else target out of range", what); return -1; }
        branch[bsp - 1].end = pc + 2 + skip;
        branch[bsp - 1].has_else = 1;
        break;
      }
      case DM_OP_ENDIF: {
        if (!bsp || branch[bsp - 1].loop != lsp ||
            (branch[bsp - 1].has_else ? branch[bsp - 1].end : branch[bsp - 1].target) != pc) {
          dm_set_error("%s: malformed endif target", what); return -1;
        }
        if (depth != branch[bsp - 1].depth) { dm_set_error("%s: stack depth differs at endif", what); return -1; }
        bsp--;
        break;
      }
      default: break;
    }
    pc += o->imm;
    int need = op_pops(op);
    if (depth < need) { dm_set_error("%s: stack underflow at %d", what, (int)(pc - m->code - p->off)); return -1; }
    depth += dm_op_stack_effect(op);
    if (depth > maxd) maxd = depth;
    if (maxd > DM_MAX_STACK) { dm_set_error("%s: expression too deep", what); return -1; }
  }
  if (lsp) { dm_set_error("%s: unterminated loop", what); return -1; }
  if (bsp) { dm_set_error("%s: unterminated if", what); return -1; }
  if (depth) { dm_set_error("%s: stack depth %d at program end", what, depth); return -1; }
  return 0;
}

/* ------------------------------------------------------------ disasm */
void dm_disasm(const dm_module *m, const dm_prog *p, FILE *f) {
  const uint8_t *base = m->code + p->off, *pc = base, *end = base + p->len;
  while (pc < end) {
    int op = *pc;
    if (op >= DM_OP_COUNT) { fprintf(f, "  %04x  ??? %d\n", (int)(pc - base), op); return; }
    const dm_opinfo *o = &dm_ops[op];
    fprintf(f, "  %04x  %-7s", (int)(pc - base), o->name);
    if (op == DM_OP_PUSHK) { float k; memcpy(&k, pc + 1, 4); fprintf(f, " %g", k); }
    else if (op == DM_OP_PUSHB) fprintf(f, " %d", (int8_t)pc[1]);
    else if (op == DM_OP_LDI) fprintf(f, " %s", dm_input_name(pc[1]));
    else if (op == DM_OP_LOOP) fprintf(f, " i=%u count=%u skip=%u", pc[1], pc[2] | (pc[3] << 8), pc[4] | (pc[5] << 8));
    else if (op == DM_OP_ENDL) fprintf(f, " i=%u back=%u", pc[1], pc[2] | (pc[3] << 8));
    else if (op == DM_OP_IFZ || op == DM_OP_ELSE) fprintf(f, " skip=%u", pc[1] | (pc[2] << 8));
    else if (o->imm == 1) fprintf(f, " %u", pc[1]);
    fputc('\n', f);
    pc += 1 + o->imm;
  }
}
