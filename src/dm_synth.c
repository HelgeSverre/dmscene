/* dm_synth.c - voice pool (NNA), per-channel fx programs, level followers, master program, player loop. */
#include "dm.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int fx_alloc(dm_fxunit *u, const dm_prog *p, uint32_t dlylen) {
  for (int i = 0; i < p->ndelay; i++) { u->dly[i] = calloc(dlylen, sizeof(float)); if (!u->dly[i]) return -1; }
  for (int i = 0; i < p->nverb; i++) { u->verb[i].buf = calloc(DM_VERB_BUF, sizeof(float)); if (!u->verb[i].buf) return -1; }
  return 0;
}
static void fx_free(dm_fxunit *u) {
  for (int i = 0; i < DM_MAX_DELAY_LINES; i++) { free(u->dly[i]); u->dly[i] = NULL; }
  for (int i = 0; i < DM_MAX_VERBS; i++) { free(u->verb[i].buf); u->verb[i].buf = NULL; }
}
static void fx_bind(dm_vm *vm, dm_fxunit *u, const dm_prog *p, uint32_t dlylen) {
  vm->state = u->state; vm->dly = u->dly; vm->dlypos = u->dlypos; vm->dlylen = dlylen; vm->verb = u->verb; vm->nverb = p->nverb;
}

int dm_synth_init(dm_synth *s, const dm_module *m, int sr) {
  memset(s, 0, sizeof *s);
  s->m = m; s->sr = sr; s->rng = 0xC0FFEEu;
  s->dlylen = (uint32_t)(sr * DM_DELAY_SECONDS);
  if (m->has_master && fx_alloc(&s->master, &m->master, s->dlylen)) return -1;
  for (int c = 0; c < DM_MAX_CHANNELS; c++) {
    s->cur[c] = -1;
    if (((m->has_chanfx >> c) & 1) && fx_alloc(&s->chan[c], &m->chanfx[c], s->dlylen)) return -1;
  }
  for (int i = 0; i < DM_MAX_VOICES; i++) s->v[i].rel = -1;
  return 0;
}

void dm_synth_free(dm_synth *s) {
  fx_free(&s->master);
  for (int c = 0; c < DM_MAX_CHANNELS; c++) fx_free(&s->chan[c]);
  free(s->mix); s->mix = NULL; s->mixcap = 0;
}

static int alloc_voice(dm_synth *s) {
  int best = 0; float bestscore = -1.0f;
  for (int i = 0; i < DM_MAX_VOICES; i++) {
    dm_voice *v = &s->v[i];
    if (!v->active) return i;
    float score = v->rel >= 0 ? 1e6f + (float)v->rel : (float)v->t;   /* prefer releasing, then oldest */
    if (score > bestscore) { bestscore = score; best = i; }
  }
  return best;
}

void dm_synth_tick(dm_synth *s, const dm_seq *q) {
  const dm_module *m = s->m;
  for (int c = 0; c < m->channels; c++) {
    const dm_chan *ch = &q->ch[c];
    int vi = s->cur[c];
    dm_voice *v = vi >= 0 ? &s->v[vi] : NULL;
    /* A finished/stolen slot may now belong to another channel. Never touch it
     * through this channel's old reference, even if trigger serials coincide. */
    if (v && (v->chan != c || v->serial != s->trig_serial[c])) {
      s->cur[c] = -1;
      v = NULL;
    }
    /* Allocate only for a real new note-on. Silent channels need no voice, and
     * losing a slot must not retrigger a note whose gate has already finished. */
    if (s->trig_serial[c] != ch->trig_serial) {
      if (v && v->active) {
        const dm_inst *oi = v->inst >= 1 && v->inst <= m->ninst ? &m->inst[v->inst - 1] : NULL;
        if (oi && (oi->flags & DM_INST_NNA)) { if (v->rel < 0) v->rel = 0; }   /* let it ring out */
        else v->active = 0;
      }
      vi = alloc_voice(s); v = &s->v[vi]; s->cur[c] = vi;
      memset(v, 0, sizeof *v);
      v->serial = ch->trig_serial;
      v->inst = ch->inst; v->chan = (uint8_t)c;
      v->rel = -1;
      v->vel = ch->vel;
      v->rng = 0x1234567u + (uint32_t)c * 7919u + ch->trig_serial * 104729u;
      v->active = v->inst >= 1 && v->inst <= m->ninst && ch->trig_serial > 0;
      s->trig_serial[c] = ch->trig_serial;
    }
    if (!v) continue;
    if (!ch->gate && v->rel < 0) v->rel = 0;
    v->note = ch->pitch_now + 11.0f;
    v->freq = dm_note_freq(ch->pitch_now);
    v->amp_l = ch->vol;                       /* channel volume (pan applied at mix time) */
    memcpy(v->params, ch->params, sizeof v->params);
  }
}

void dm_synth_render(dm_synth *s, const dm_seq *q, float *lr, uint32_t n) {
  const dm_module *m = s->m;
  float *in = s->in;
  dm_vm *vm = &s->vm;
  dm_seq_fill_inputs(q, in);
  for (int c = 0; c < DM_MAX_CHANNELS; c++) in[DM_IN_CENV0 + c] = s->cenv[c];
  dm_events_eval(m, dm_seq_pos(q), s->evhold, s->evlerp);
  memset(vm, 0, sizeof *vm);
  vm->in = in; vm->pal = m->pal; vm->npal = m->npal; vm->m = m; vm->evhold = s->evhold; vm->evlerp = s->evlerp;
  float inv_sr = 1.0f / (float)s->sr;
  memset(lr, 0, sizeof(float) * 2 * n);
  if (s->mixcap < n) { free(s->mix); s->mix = malloc(sizeof(float) * n); s->mixcap = n; }
  float *cb = s->mix;
  const float follow = expf(-1.0f / (0.06f * (float)s->sr));

  for (int c = 0; c < m->channels; c++) {
    memset(cb, 0, sizeof(float) * n);
    int any = 0;
    for (int vi = 0; vi < DM_MAX_VOICES; vi++) {
      dm_voice *v = &s->v[vi];
      if (!v->active || v->chan != c) continue;
      any = 1;
      const dm_prog *p = &m->inst[v->inst - 1].prog;
      const uint8_t *code = m->code + p->off;
      double inc = (double)v->freq * inv_sr;
      vm->state = v->state; vm->rng = &v->rng; vm->dly = NULL; vm->verb = NULL; vm->nverb = 0;
      in[DM_IN_FREQ] = v->freq; in[DM_IN_VVEL] = v->vel; in[DM_IN_VNOTE] = v->note; in[DM_IN_CHAN] = (float)c;
      in[DM_IN_P0] = v->params[0]; in[DM_IN_P1] = v->params[1]; in[DM_IN_P2] = v->params[2]; in[DM_IN_P3] = v->params[3];
      for (uint32_t i = 0; i < n; i++) {
        in[DM_IN_PH] = (float)v->ph;
        in[DM_IN_VT] = (float)v->t * inv_sr;
        in[DM_IN_REL] = v->rel < 0 ? -1.0f : (float)v->rel * inv_sr;
        vm->out[0] = 0.0f;
        dm_vm_run(vm, code, p->len);
        float o = vm->out[0];
        if (o != o) o = 0.0f;
        cb[i] += o * v->amp_l;
        v->ph += inc; if (v->ph >= 1.0) v->ph -= floor(v->ph);
        v->t++;
        if (v->rel >= 0) {
          v->rel++;
          if (fabsf(o) < 1e-5f) { if (++v->quiet > 2000) { v->active = 0; break; } } else v->quiet = 0;
          if (v->rel > 8 * s->sr) { v->active = 0; break; }
        }
      }
    }
    if ((m->has_chanfx >> c) & 1) {
      const dm_prog *p = &m->chanfx[c];
      const uint8_t *code = m->code + p->off;
      fx_bind(vm, &s->chan[c], p, s->dlylen); vm->rng = &s->rng;
      in[DM_IN_CHAN] = (float)c;
      for (uint32_t i = 0; i < n; i++) {
        in[DM_IN_XIN] = cb[i]; vm->out[0] = cb[i];
        dm_vm_run(vm, code, p->len);
        float o = vm->out[0]; cb[i] = o != o ? 0.0f : o;
      }
      any = 1;
    }
    float e = s->cenv[c];
    if (any) {
      float gl = q->global_vol * cosf(q->ch[c].pan * (DM_PI * 0.5f)), gr = q->global_vol * sinf(q->ch[c].pan * (DM_PI * 0.5f));
      int audible = !s->mute[c];
      for (uint32_t i = 0; i < n; i++) {
        float x = cb[i], ax = fabsf(x);
        e = ax > e ? ax : e * follow;
        if (audible) { lr[2 * i] += x * gl; lr[2 * i + 1] += x * gr; }
      }
    } else e *= powf(follow, (float)n);
    s->cenv[c] = e;
    in[DM_IN_CENV0 + c] = e;
  }

  if (m->has_master) {
    const dm_prog *p = &m->master;
    const uint8_t *code = m->code + p->off;
    fx_bind(vm, &s->master, p, s->dlylen); vm->rng = &s->rng;
    for (uint32_t i = 0; i < n; i++) {
      in[DM_IN_L] = lr[2 * i]; in[DM_IN_R] = lr[2 * i + 1];
      vm->out[0] = in[DM_IN_L]; vm->out[1] = in[DM_IN_R];
      dm_vm_run(vm, code, p->len);
      lr[2 * i] = vm->out[0]; lr[2 * i + 1] = vm->out[1];
    }
  }
  for (uint32_t i = 0; i < 2 * n; i++) {
    float x = lr[i];
    if (x != x) x = 0.0f;
    lr[i] = x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x);
  }
}

/* ------------------------------------------------------------ player */
int dm_player_init(dm_player *p, const dm_module *m, int sr) {
  p->m = m; p->sr = sr;
  dm_seq_init(&p->seq, m, sr);
  return dm_synth_init(&p->synth, m, sr);
}
void dm_player_free(dm_player *p) { dm_synth_free(&p->synth); }
void dm_player_solo(dm_player *p, int chan) {
  for (int c = 0; c < DM_MAX_CHANNELS; c++) p->synth.mute[c] = chan >= 0 && c != chan;
}

void dm_player_render(dm_player *p, float *lr, uint32_t frames) {
  while (frames > 0) {
    if (dm_seq_remaining(&p->seq) == 0) {
      dm_seq_tick(&p->seq);
      dm_synth_tick(&p->synth, &p->seq);
    }
    uint32_t k = dm_seq_remaining(&p->seq);
    if (k > frames) k = frames;
    dm_synth_render(&p->synth, &p->seq, lr, k);
    dm_seq_advance(&p->seq, k);
    lr += 2 * k; frames -= k;
  }
}
