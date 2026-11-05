/* dm_seq.c - XM-flavoured pattern sequencer.  Pure state machine: no audio,
 * so the same code runs in the audio thread (driving the synth) and in the
 * video thread (as a clock seeked to the sample position being heard). */
#include "dm.h"
#include <math.h>
#include <string.h>

float dm_note_freq(float note) {            /* note 49 = C-4 = midi 60 */
  return 440.0f * powf(2.0f, (note + 11.0f - 69.0f) / 12.0f);
}

static const dm_pattern *cur_pat(const dm_seq *s) {
  return &s->m->pat[s->m->order[s->order]];
}

static void set_tempo(dm_seq *s) {
  s->samples_per_tick = (uint32_t)(s->sr * 2.5 / (double)s->bpm);
}

void dm_seq_init(dm_seq *s, const dm_module *m, int sr) {
  memset(s, 0, sizeof *s);
  s->m = m; s->sr = sr;
  s->bpm = m->bpm ? m->bpm : 125;
  s->speed = m->speed ? m->speed : 6;
  s->next_order = s->next_row = -1;
  s->global_vol = m->volume;
  set_tempo(s);
  s->tick_pos = s->samples_per_tick;      /* first advance fires a tick */
  for (int c = 0; c < DM_MAX_CHANNELS; c++) {
    dm_chan *ch = &s->ch[c];
    ch->vol = 1.0f; ch->pan = m->pan[c]; ch->vel = 1.0f;
    ch->pitch = ch->target = ch->pitch_now = 49.0f;
    ch->trig_sample = 0; ch->gate = 0;
  }
  int rows = 0;
  for (int i = m->loop; i < m->norder; i++) rows += m->pat[m->order[i]].rows;
  s->llen = m->rpb ? (double)rows / m->rpb : rows / 4.0;
}

uint32_t dm_seq_remaining(const dm_seq *s) {
  return s->tick_pos >= s->samples_per_tick ? 0 : s->samples_per_tick - s->tick_pos;
}

void dm_seq_advance(dm_seq *s, uint32_t n) {
  s->tick_pos += n;
  s->sample += n;
}

static void enter_position(dm_seq *s, int order, int row) {
  const dm_module *m = s->m;
  if (order >= m->norder) { order = m->loop < m->norder ? m->loop : 0; s->loop_count++; }
  if (order != s->order) { s->loop_row = 0; s->loop_left = 0; }
  s->order = order;
  const dm_pattern *p = &m->pat[m->order[order]];
  s->row = row < p->rows ? row : 0;
  if (s->order == m->loop && s->row == 0) s->lbeat = 0.0;
}

static void next_row(dm_seq *s) {
  if (s->next_order >= 0 || s->next_row >= 0) {
    int o = s->next_order >= 0 ? s->next_order : s->order + 1;
    int r = s->next_row >= 0 ? s->next_row : 0;
    s->next_order = s->next_row = -1;
    enter_position(s, o, r);
    return;
  }
  int r = s->row + 1;
  if (r >= cur_pat(s)->rows) enter_position(s, s->order + 1, 0);
  else s->row = r;
}

static void trigger(dm_seq *s, dm_chan *ch, float note) {
  ch->pitch = ch->target = ch->pitch_now = note;
  ch->gate = 1;
  ch->trig_serial++;
  ch->trig_sample = s->sample;
  ch->vib_pos = 0;
}

static void process_row(dm_seq *s) {
  const dm_module *m = s->m;
  const dm_pattern *p = cur_pat(s);
  int nch = p->channels < m->channels ? p->channels : m->channels;
  for (int c = 0; c < m->channels; c++) {
    dm_chan *ch = &s->ch[c];
    dm_cell cell = {0, 0, 0, 0, 0};
    if (c < nch) cell = p->cells[s->row * p->channels + c];
    ch->fx = cell.fx; ch->fxp = cell.fxp;
    int trig = 0;
    if (cell.inst) { ch->inst = cell.inst; if (!cell.vol) ch->vol = 1.0f; }
    ch->cut_tick = 0; ch->delay_ticks = 0;
    if (cell.fx == 0x0E && (cell.fxp >> 4) == 0xD && (cell.fxp & 15) && cell.note && cell.note != DM_NOTE_OFF) {
      ch->delay_ticks = cell.fxp & 15; ch->delay_note = cell.note; ch->delay_vol = cell.vol;     /* EDx: trigger later */
    } else if (cell.note == DM_NOTE_OFF) ch->gate = 0;
    else if (cell.note) {
      if (cell.fx == 3 || cell.fx == 5) ch->target = (float)cell.note;
      else { trigger(s, ch, (float)cell.note); trig = 1; }
    }
    if (cell.vol) ch->vol = (float)(cell.vol - 1) / 64.0f;
    if (trig) ch->vel = cell.vol ? ch->vol : 1.0f;
    uint8_t x = cell.fxp >> 4, y = cell.fxp & 15;
    switch (cell.fx) {
      case 1: if (cell.fxp) ch->porta_speed = cell.fxp; break;
      case 2: if (cell.fxp) ch->porta_speed = cell.fxp; break;
      case 3: if (cell.fxp) ch->tone_speed = cell.fxp; break;
      case 4: if (x) ch->vib_speed = x; if (y) ch->vib_depth = y; break;
      case 8: ch->pan = cell.fxp / 255.0f; break;
      case 0xA: if (cell.fxp) ch->vol_slide = cell.fxp; break;
      case 0xB: s->next_order = cell.fxp; break;
      case 0xC: ch->vol = (cell.fxp > 64 ? 64 : cell.fxp) / 64.0f; break;
      case 0xD: s->next_row = (cell.fxp >> 4) * 10 + (cell.fxp & 15); break;
      case 0xF: if (cell.fxp == 0) break;
                if (cell.fxp < 0x20) s->speed = cell.fxp; else { s->bpm = cell.fxp; set_tempo(s); }
                break;
      case 0x0E:                                                                       /* Exy extended */
        switch (x) {
          case 1: ch->pitch += y / 16.0f; ch->pitch_now = ch->pitch; break;             /* fine porta up */
          case 2: ch->pitch -= y / 16.0f; ch->pitch_now = ch->pitch; break;             /* fine porta down */
          case 6: if (y == 0) s->loop_row = s->row;                                     /* pattern loop */
                  else { if (s->loop_left == 0) s->loop_left = y + 1; s->loop_left--;
                         if (s->loop_left > 0) { s->next_order = s->order; s->next_row = s->loop_row; } }
                  break;
          case 9: ch->fx = 0x1B; ch->fxp = y; ch->retrig = y; break;                    /* retrigger */
          case 0xC: ch->cut_tick = y + 1; break;                                         /* note cut at tick y */
          default: break;
        }
        break;
      case 0x10: s->global_vol = (cell.fxp > 64 ? 64 : cell.fxp) / 64.0f; break;      /* Gxx */
      case 0x20: case 0x21: case 0x22: case 0x23: s->mp[cell.fx - 0x20] = cell.fxp / 255.0f; break;   /* Wxx..Zxx master params */
      case 0x1B: ch->retrig = y; break;                                                /* Rxy */
      case 0x1C: ch->params[0] = cell.fxp / 255.0f; break;                             /* Sxx */
      case 0x1D: ch->params[1] = cell.fxp / 255.0f; break;                             /* Txx */
      case 0x1E: ch->params[2] = cell.fxp / 255.0f; break;                             /* Uxx */
      case 0x1F: ch->params[3] = cell.fxp / 255.0f; break;                             /* Vxx */
      default: break;
    }
  }
}

static void per_tick(dm_seq *s) {
  for (int c = 0; c < s->m->channels; c++) {
    dm_chan *ch = &s->ch[c];
    if (ch->delay_ticks && s->tick == ch->delay_ticks) {
      trigger(s, ch, (float)ch->delay_note);
      if (ch->delay_vol) ch->vol = (float)(ch->delay_vol - 1) / 64.0f;
      ch->vel = ch->delay_vol ? ch->vol : 1.0f;
      ch->delay_ticks = 0;
    }
    if (ch->cut_tick && s->tick == ch->cut_tick - 1) ch->vol = 0.0f;
    float p = ch->pitch;
    uint8_t x = ch->fxp >> 4, y = ch->fxp & 15;
    switch (ch->fx) {
      case 0: if (ch->fxp) { int k = s->tick % 3; p += k == 1 ? x : (k == 2 ? y : 0); } break;
      case 1: if (s->tick) ch->pitch += ch->porta_speed / 16.0f; p = ch->pitch; break;
      case 2: if (s->tick) ch->pitch -= ch->porta_speed / 16.0f; p = ch->pitch; break;
      case 3: case 5:
        if (s->tick) {
          float st = ch->tone_speed / 16.0f;
          if (ch->pitch < ch->target) { ch->pitch += st; if (ch->pitch > ch->target) ch->pitch = ch->target; }
          else if (ch->pitch > ch->target) { ch->pitch -= st; if (ch->pitch < ch->target) ch->pitch = ch->target; }
        }
        p = ch->pitch;
        if (ch->fx == 5 && s->tick) { float d = (x - y) / 64.0f; ch->vol += d; }
        break;
      case 4: case 6:
        ch->vib_pos += ch->vib_speed;
        p += sinf(ch->vib_pos * (DM_TAU / 64.0f)) * ch->vib_depth / 8.0f;
        if (ch->fx == 6 && s->tick) { float d = (x - y) / 64.0f; ch->vol += d; }
        break;
      case 0xA: if (s->tick) { float d = ((ch->vol_slide >> 4) - (ch->vol_slide & 15)) / 64.0f; ch->vol += d; } break;
      case 0x1B: if (ch->retrig && s->tick && s->tick % ch->retrig == 0) trigger(s, ch, ch->pitch); break;
      default: break;
    }
    if (ch->vol < 0.0f) ch->vol = 0.0f; if (ch->vol > 1.0f) ch->vol = 1.0f;
    ch->pitch_now = p;
  }
}

void dm_seq_tick(dm_seq *s) {
  int rpb = s->m->rpb ? s->m->rpb : 4;
  if (!s->started) s->started = 1;
  else {
    double db = 1.0 / (double)(s->speed * rpb);
    s->beat += db; s->lbeat += db;
    s->tick++;
    if (s->tick >= s->speed) { s->tick = 0; next_row(s); }
  }
  if (s->tick == 0) process_row(s);
  per_tick(s);
  s->tick_pos = 0;
}

void dm_seq_seek(dm_seq *s, uint64_t target) {
  while (s->sample < target) {
    if (s->tick_pos >= s->samples_per_tick) dm_seq_tick(s);
    uint64_t left = target - s->sample;
    uint32_t k = dm_seq_remaining(s);
    if ((uint64_t)k > left) k = (uint32_t)left;
    dm_seq_advance(s, k);
  }
}

static double frac_tick(const dm_seq *s) {
  int rpb = s->m->rpb ? s->m->rpb : 4;
  double f = s->samples_per_tick ? (double)s->tick_pos / s->samples_per_tick : 0.0;
  if (f > 1.0) f = 1.0;
  return f / (double)(s->speed * rpb);
}
double dm_seq_beat(const dm_seq *s)  { return s->beat + frac_tick(s); }
double dm_seq_pos(const dm_seq *s) {
  double f = s->samples_per_tick ? (double)s->tick_pos / s->samples_per_tick : 0.0;
  if (f > 1.0) f = 1.0;
  return (double)s->m->cumrows[s->order] + s->row + (s->tick + f) / (double)s->speed;
}

void dm_events_eval(const dm_module *m, double pos, float *hold, float *lerp) {
  for (int i = 0; i < DM_MAX_EVNAMES; i++) { hold[i] = 0.0f; lerp[i] = 0.0f; }
  for (int id = 0; id < m->nevname; id++) {
    const dm_event *last = NULL, *next = NULL;
    for (int i = 0; i < m->nev; i++) {
      const dm_event *e = &m->ev[i];
      if (e->id != id) continue;
      double ep = (double)m->cumrows[e->order] + e->row;
      if (ep <= pos) last = e; else { next = e; break; }
    }
    if (!last) { if (next) { hold[id] = lerp[id] = next->value; } continue; }
    hold[id] = last->value;
    if (!next) { lerp[id] = last->value; continue; }
    double lp = (double)m->cumrows[last->order] + last->row, np = (double)m->cumrows[next->order] + next->row;
    double k = np > lp ? (pos - lp) / (np - lp) : 1.0;
    lerp[id] = (float)(last->value + (next->value - last->value) * k);
  }
}
double dm_seq_lbeat(const dm_seq *s) { return s->lbeat + frac_tick(s); }

void dm_seq_fill_inputs(const dm_seq *s, float *in) {
  in[DM_IN_T] = (float)((double)s->sample / s->sr);
  in[DM_IN_BEAT] = (float)dm_seq_beat(s);
  in[DM_IN_LBEAT] = (float)dm_seq_lbeat(s);
  in[DM_IN_LLEN] = (float)s->llen;
  in[DM_IN_BAR] = in[DM_IN_BEAT] * 0.25f;
  in[DM_IN_ROW] = (float)s->row;
  in[DM_IN_TICK] = (float)s->tick;
  in[DM_IN_BPM] = (float)s->bpm;
  in[DM_IN_SR] = (float)s->sr;
  in[DM_IN_ORDER] = (float)s->order;
  in[DM_IN_POS] = (float)dm_seq_pos(s);
  for (int i = 0; i < 4; i++) in[DM_IN_MP0 + i] = s->mp[i];
  for (int c = 0; c < DM_MAX_CHANNELS; c++) {
    const dm_chan *ch = &s->ch[c];
    in[DM_IN_TRIG0 + c] = ch->trig_serial ? (float)((double)(s->sample - ch->trig_sample) / s->sr) : 1000.0f;
    in[DM_IN_VEL0 + c] = ch->trig_serial ? ch->vel : 0.0f;
    in[DM_IN_NOTE0 + c] = ch->pitch_now + 11.0f;
  }
}
