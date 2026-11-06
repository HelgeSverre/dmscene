/* dm_scene.c - frame program, selected pixel program, optional post pass; worker threads;
 * previous-frame buffer for feedback; ordered dither. */
#include "dm.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int dm_fb_alloc(dm_fb *fb, int w, int h) {
  if (!fb || w < 1 || h < 1 || (size_t)w > SIZE_MAX / 3 / (size_t)h) return -1;
  if (fb->rgb && fb->w == w && fb->h == h) return 0;
  uint8_t *rgb = calloc((size_t)w * h * 3, 1);
  if (!rgb) return -1;
  free(fb->rgb);
  fb->w = w; fb->h = h; fb->rgb = rgb;
  return 0;
}
void dm_fb_free(dm_fb *fb) { free(fb->rgb); fb->rgb = NULL; fb->w = fb->h = 0; }

void dm_fb_upscale(const dm_fb *src, dm_fb *dst) {
  int last_sy = -1;
  for (int y = 0; y < dst->h; y++) {
    int sy = (int)((size_t)y * src->h / dst->h);
    uint8_t *o = dst->rgb + (size_t)y * dst->w * 3;
    if (sy == last_sy) { memcpy(o, o - (size_t)dst->w * 3, (size_t)dst->w * 3); continue; }
    const uint8_t *row = src->rgb + (size_t)sy * src->w * 3;
    for (int x = 0; x < dst->w; x++) { const uint8_t *p = row + ((size_t)x * src->w / dst->w) * 3; o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o += 3; }
    last_sy = sy;
  }
}

void dm_scene_init(dm_scene *s, const dm_module *m) {
  memset(s, 0, sizeof *s);
  s->m = m; s->rng = 0xBEEFu;
}
void dm_scene_free(dm_scene *s) { free(s->prev); s->prev = NULL; free(s->cur); s->cur = NULL; }

static const float bayer4[4][4] = {
  { 0, 8, 2, 10}, {12, 4, 14, 6}, { 3, 11, 1, 9}, {15, 7, 13, 5}
};
static inline uint8_t to8(float v, float dth) {
  if (v != v) return 0;
  v = v * 255.0f + dth;
  return v <= 0.0f ? 0 : (v >= 255.0f ? 255 : (uint8_t)(v + 0.5f));
}

typedef struct {
  dm_scene *s; dm_fb *fb; int y0, step;
  const uint8_t *code; uint32_t len; int post;
  dm_vm vm; float in[DM_IN_COUNT], uni[DM_MAX_UNIFORMS]; uint32_t rng;
} job;

static void *run_rows(void *arg) {
  job *j = arg;
  dm_fb *fb = j->fb;
  dm_vm *vm = &j->vm;
  float *in = j->in;
  int dither = j->s->m->dither;
  float iw = 1.0f / (float)fb->w, ih = 1.0f / (float)fb->h, aspect = in[DM_IN_ASPECT];
  for (int y = j->y0; y < fb->h; y += j->step) {
    float fy = ((float)y + 0.5f) * ih;
    in[DM_IN_Y] = fy; in[DM_IN_V] = (0.5f - fy) * 2.0f;
    uint8_t *px = fb->rgb + (size_t)y * fb->w * 3;
    for (int x = 0; x < fb->w; x++) {
      float fx = ((float)x + 0.5f) * iw;
      in[DM_IN_X] = fx; in[DM_IN_U] = (fx - 0.5f) * 2.0f * aspect;
      if (j->post) { const uint8_t *c = vm->cur + ((size_t)y * fb->w + x) * 3; vm->out[0] = c[0] / 255.0f; vm->out[1] = c[1] / 255.0f; vm->out[2] = c[2] / 255.0f; }
      else { vm->out[0] = vm->out[1] = vm->out[2] = 0.0f; }
      vm->out[3] = 1.0f;
      dm_vm_run(vm, j->code, j->len);
      float d = dither ? (bayer4[y & 3][x & 3] - 7.5f) * 0.125f : 0.0f;
      px[0] = to8(vm->out[0], d); px[1] = to8(vm->out[1], d); px[2] = to8(vm->out[2], d);
      px += 3;
    }
  }
  return NULL;
}

static void run_pass(dm_scene *s, dm_fb *fb, const dm_prog *p, int post, float *state) {
  int nt = s->nthreads;
  if (nt <= 0) { const char *env = getenv("DM_THREADS"); nt = env ? atoi(env) : (int)sysconf(_SC_NPROCESSORS_ONLN); }
  if (nt < 1) nt = 1; if (nt > 64) nt = 64; if (nt > fb->h) nt = fb->h;
  job jobs[64]; pthread_t th[64]; int started[64] = {0};
  for (int i = 0; i < nt; i++) {
    job *j = &jobs[i];
    j->s = s; j->fb = fb; j->y0 = i; j->step = nt; j->post = post;
    j->code = s->m->code + p->off; j->len = p->len;
    memcpy(j->in, s->in, sizeof j->in);
    memcpy(j->uni, s->uni, sizeof j->uni);
    float *worker_state = s->worker_state[post][i];
    if (s->frame == 0) memset(worker_state, 0, sizeof s->worker_state[post][i]);
    j->rng = s->rng ^ (uint32_t)(i * 0x9E3779B9u) ^ (uint32_t)(post * 0x51ED27u);
    j->vm = s->vm; j->vm.in = j->in; j->vm.state = i == 0 ? state : worker_state; j->vm.rng = &j->rng;
    j->vm.uni = j->uni;
    j->vm.prev = s->prev; j->vm.pw = s->pw; j->vm.ph = s->ph; j->vm.cur = s->cur;
  }
  for (int i = 1; i < nt; i++) {
    if (pthread_create(&th[i], NULL, run_rows, &jobs[i]) == 0) started[i] = 1;
    else run_rows(&jobs[i]);
  }
  run_rows(&jobs[0]);
  for (int i = 1; i < nt; i++) if (started[i]) pthread_join(th[i], NULL);
}

void dm_scene_render(dm_scene *s, const dm_seq *clk, const float *cenv, dm_fb *fb) {
  const dm_module *m = s->m;
  float *in = s->in;
  dm_vm *vm = &s->vm;
  dm_seq_fill_inputs(clk, in);
  for (int c = 0; c < DM_MAX_CHANNELS; c++) in[DM_IN_CENV0 + c] = cenv ? cenv[c] : 0.0f;
  in[DM_IN_FRAME] = (float)s->frame;
  in[DM_IN_W] = (float)fb->w; in[DM_IN_H] = (float)fb->h;
  in[DM_IN_ASPECT] = fb->h ? (float)fb->w / (float)fb->h : 1.0f;
  dm_events_eval(m, dm_seq_pos(clk), s->evhold, s->evlerp);
  memset(vm, 0, sizeof *vm);
  vm->in = in; vm->uni = s->uni; vm->pal = m->pal; vm->npal = m->npal; vm->rng = &s->rng;
  vm->m = m; vm->evhold = s->evhold; vm->evlerp = s->evlerp; vm->scene = s->scene;

  if (m->has_frame) {
    vm->state = s->fstate;
    dm_vm_run(vm, m->code + m->frame.off, m->frame.len);
    if (vm->scene >= 0 && vm->scene < m->npixel) s->scene = vm->scene;
  }
  if (m->npixel) {
    size_t bytes = (size_t)fb->w * fb->h * 3;
    if (!s->prev || s->pw != fb->w || s->ph != fb->h) {
      free(s->prev); s->prev = calloc(bytes, 1); free(s->cur); s->cur = calloc(bytes, 1); s->pw = fb->w; s->ph = fb->h;
    }
    const dm_prog *p = &m->pixel[s->scene < m->npixel ? s->scene : 0];
    run_pass(s, fb, p, 0, s->pstate);
    if (m->has_post) {
      memcpy(s->cur, fb->rgb, bytes);
      run_pass(s, fb, &m->post, 1, s->ostate);
    }
    memcpy(s->prev, fb->rgb, bytes);
  }
  s->frame++;
}
