/* dm.h - dmscene: a tiny composable demo engine.
 *
 *   text (.dmt)  --dm compile-->  byte-packed module (.dms)  --dm play-->  pixels + PCM
 *
 * One stack VM runs six kinds of program that all live in one code segment:
 *   frame   : once per video frame, writes uniforms (camera, colour, envelopes)
 *   pixel   : once per pixel, reads uniforms, calls fractal intrinsics, emits colour
 *   inst    : once per sample per voice, turns (phase, time, velocity, params) into audio
 *   chan    : once per sample on a channel's summed voices (insert effects)
 *   master  : once per sample on the stereo mix (delay lines live here)
 *   post    : once per pixel after the selected pixel program (framebuffer effects)
 * An XM-style tracker (patterns, order list, effects) drives the voices and exposes
 * its clock (beat, row, per-channel trigger times) to the visual programs.
 *
 * Pure C99. No third-party libraries.  Platform code (terminal, CoreAudio) is
 * confined to dm_term.c / dm_audio.c.
 */
#ifndef DM_H
#define DM_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "dm_ops.h"

#define DM_VERSION          4
#define DM_SR               44100
#define DM_MAX_CHANNELS     8
#define DM_MAX_INST         32
#define DM_MAX_PATTERNS     64
#define DM_MAX_ORDER        128
#define DM_MAX_PAL          8
#define DM_MAX_PALKEYS      16
#define DM_MAX_UNIFORMS     64
#define DM_MAX_LOCALS       200
#define DM_MAX_STATE        32
#define DM_MAX_STACK        64
#define DM_MAX_CODE         65536
#define DM_MAX_DELAY_LINES  4
#define DM_MAX_VOICES       32
#define DM_MAX_PIXEL        8
#define DM_MAX_TEXTS        16
#define DM_MAX_TEXT_LEN     1024
#define DM_MAX_SAMPLES      16
#define DM_MAX_TEXTURES     8
#define DM_MAX_TEX_DIM      1024
#define DM_MAX_TEX_BYTES    (24u * 1024u * 1024u)
#define DM_MAX_VENVS        16
#define DM_MAX_VENV_PTS     12
#define DM_MAX_EVNAMES      32
#define DM_MAX_EVENTS       512
#define DM_MAX_LOOP_DEPTH   4
#define DM_MAX_VERBS        4
#define DM_VERB_BUF         8192
#define DM_USAW_VOICES      7
#define DM_DELAY_SECONDS    2
#define DM_NAME_LEN         16
#define DM_NOTE_OFF         97
#define DM_PI               3.14159265358979323846f
#define DM_TAU              6.28318530717958647692f

/* ---------------------------------------------------------------- inputs */
/* Read-only values a program can see.  The compiler maps names per program
 * kind (see dm_compile.c); the VM just indexes this array. */
enum {
  DM_IN_T,        /* song seconds                                  */
  DM_IN_BEAT,     /* beats since song start (continuous)           */
  DM_IN_LBEAT,    /* beats since the loop point was last entered   */
  DM_IN_LLEN,     /* loop length in beats                          */
  DM_IN_BAR,      /* beat / 4                                      */
  DM_IN_ROW,      /* current pattern row                           */
  DM_IN_TICK,     /* current tick within row                       */
  DM_IN_BPM,
  DM_IN_SR,
  DM_IN_FRAME,    /* frame counter (video only)                    */
  DM_IN_W, DM_IN_H, DM_IN_ASPECT,
  DM_IN_TRIG0,    /* seconds since last note-on, per channel        */
  DM_IN_VEL0  = DM_IN_TRIG0 + DM_MAX_CHANNELS,   /* velocity of last note-on */
  DM_IN_NOTE0 = DM_IN_VEL0  + DM_MAX_CHANNELS,   /* midi note of last note-on */
  DM_IN_X = DM_IN_NOTE0 + DM_MAX_CHANNELS,       /* pixel 0..1 */
  DM_IN_Y,
  DM_IN_U, DM_IN_V,                              /* centred, aspect corrected, y up */
  DM_IN_PH,       /* voice phase 0..1 (engine accumulates, follows slides) */
  DM_IN_FREQ,     /* voice frequency Hz                            */
  DM_IN_VT,       /* seconds since note-on                         */
  DM_IN_REL,      /* seconds since note-off, -1 while held         */
  DM_IN_VVEL,     /* note-on velocity 0..1                         */
  DM_IN_VNOTE,    /* midi note (float, follows slides)             */
  DM_IN_P0, DM_IN_P1, DM_IN_P2, DM_IN_P3,        /* instrument params (Sxx..Vxx) */
  DM_IN_CHAN,
  DM_IN_L, DM_IN_R,                              /* master input */
  DM_IN_CENV0,                                   /* per-channel output level follower (audio-reactive) */
  DM_IN_MP0 = DM_IN_CENV0 + DM_MAX_CHANNELS,     /* master params set by Wxx..Zxx */
  DM_IN_MP1, DM_IN_MP2, DM_IN_MP3,
  DM_IN_XIN,                                     /* chan fx input sample */
  DM_IN_ORDER, DM_IN_POS,                        /* order index, song position in rows (float) */
  DM_IN_COUNT
};

enum { DM_PK_FRAME, DM_PK_PIXEL, DM_PK_INST, DM_PK_MASTER, DM_PK_CHAN, DM_PK_POST, DM_PK_COUNT };

/* ---------------------------------------------------------------- module */
typedef struct { uint8_t note, inst, vol, fx, fxp; } dm_cell;   /* 0 = empty, vol stored +1 */
typedef struct { uint16_t rows; uint8_t channels; dm_cell *cells; } dm_pattern;
typedef struct { uint16_t off, len; uint8_t nlocals, nstate, ndelay, nverb; } dm_prog;
typedef struct { float *buf; uint32_t pos[6]; float lp[4]; } dm_verb;   /* 4 combs + 2 allpasses */
enum { DM_INST_NNA = 1 };                                   /* new note lets the old voice release */
typedef struct { char name[DM_NAME_LEN]; dm_prog prog; uint8_t flags; } dm_inst;
typedef struct { char name[DM_NAME_LEN]; char *s; uint16_t len; } dm_text;
typedef struct { char name[DM_NAME_LEN]; int8_t *pcm; uint32_t len; uint16_t rate; } dm_sample;
typedef struct { char name[DM_NAME_LEN]; uint8_t *rgb; uint16_t w, h; } dm_texture;
typedef struct { char name[DM_NAME_LEN]; uint8_t npts, sustain; float t[DM_MAX_VENV_PTS], v[DM_MAX_VENV_PTS]; } dm_venv;
typedef struct { uint8_t order, row, id; float value; } dm_event;
typedef struct {
  char name[DM_NAME_LEN];
  uint8_t nkeys;
  float keys[DM_MAX_PALKEYS][4];   /* pos r g b */
  float lut[256][3];
} dm_palette;

typedef struct dm_module {
  char title[32];
  uint8_t bpm, speed, rpb, channels, loop, norder;
  float volume;
  uint8_t order[DM_MAX_ORDER];
  dm_pattern pat[DM_MAX_PATTERNS];
  dm_inst inst[DM_MAX_INST]; int ninst;      /* id 1..ninst -> inst[id-1] */
  dm_palette pal[DM_MAX_PAL]; int npal;
  dm_prog frame, master, post;
  int has_frame, has_master, has_post;
  dm_prog pixel[DM_MAX_PIXEL]; char pixelname[DM_MAX_PIXEL][DM_NAME_LEN]; int npixel;
  dm_prog chanfx[DM_MAX_CHANNELS]; uint8_t has_chanfx;        /* bit mask */
  dm_text text[DM_MAX_TEXTS]; int ntext;
  dm_sample smp[DM_MAX_SAMPLES]; int nsmp;
  dm_texture tex[DM_MAX_TEXTURES]; int ntex;
  dm_venv venv[DM_MAX_VENVS]; int nvenv;
  char evname[DM_MAX_EVNAMES][DM_NAME_LEN]; int nevname;
  dm_event ev[DM_MAX_EVENTS]; int nev;
  uint16_t cumrows[DM_MAX_ORDER + 1];                          /* song position of each order, in rows */
  uint8_t scale, dither;
  float pan[DM_MAX_CHANNELS];
  int nuniform;
  uint8_t code[DM_MAX_CODE]; uint32_t codelen;
} dm_module;

const char *dm_error(void);
void dm_set_error(const char *fmt, ...);
void dm_module_free(dm_module *m);
int  dm_module_write(const dm_module *m, const char *path);
int  dm_module_read(dm_module *m, const char *path);
int  dm_module_load(dm_module *m, const uint8_t *data, size_t len);
void dm_palette_build(dm_palette *p);
int  dm_module_verify(dm_module *m);

/* ---------------------------------------------------------------- compiler */
int  dm_compile(const char *src, const char *srcname, dm_module *out);
extern const uint8_t dm_font5x7[95][5];   /* ASCII 32..126, column bitmaps, bit 0 = top row */
void dm_disasm(const dm_module *m, const dm_prog *p, FILE *f);
const char *dm_input_name(int idx);   /* debugging aid */

/* ---------------------------------------------------------------- VM */
typedef struct dm_vm {
  const float *in;
  float *uni;
  float *state;
  const dm_palette *pal; int npal;
  float **dly; uint32_t *dlypos; uint32_t dlylen;
  dm_verb *verb; int nverb;
  uint32_t *rng;
  const uint8_t *prev; int pw, ph;     /* previous frame (pixel programs), NULL elsewhere */
  const uint8_t *cur;                  /* current frame for tap() in post programs */
  const struct dm_module *m;           /* text, samples, textures, envelopes */
  const float *evhold, *evlerp;        /* timeline values by event name id */
  int scene;                           /* written by select() */
  float out[4];
  float r[4];               /* secondary results r1..r3 */
  float locals[DM_MAX_LOCALS];
} dm_vm;

void dm_vm_run(dm_vm *vm, const uint8_t *code, uint32_t len);
int  dm_prog_verify(const dm_module *m, const dm_prog *p, const char *what);
int  dm_op_stack_effect(int op);
uint32_t dm_rng_next(uint32_t *s);
float dm_rng_float(uint32_t *s);   /* -1..1 */

/* ---------------------------------------------------------------- sequencer */
typedef struct {
  float pitch, target, pitch_now;   /* note numbers 1..96 (C-4 = 49), float */
  float vol, pan, vel;
  float params[4];
  uint8_t inst, gate;
  uint8_t fx, fxp;
  uint8_t porta_speed, tone_speed, vib_speed, vib_depth, vol_slide, retrig;
  uint8_t delay_ticks, delay_note, delay_inst, delay_vol, cut_tick;
  int vib_pos;
  uint32_t trig_serial;
  uint64_t trig_sample;
} dm_chan;

typedef struct {
  const dm_module *m;
  int sr;
  int order, row, tick, speed, bpm;
  int next_order, next_row;
  uint32_t samples_per_tick, tick_pos;
  uint64_t sample;
  double beat, lbeat, llen;
  float global_vol;
  float mp[4];                       /* master params Wxx..Zxx */
  dm_chan ch[DM_MAX_CHANNELS];
  int started;
  uint32_t loop_count;
  int loop_row, loop_left;           /* E6x pattern loop */
} dm_seq;
double dm_seq_pos(const dm_seq *s);                   /* song position in rows (continuous) */
void   dm_events_eval(const dm_module *m, double pos, float *hold, float *lerp);

void   dm_seq_init(dm_seq *s, const dm_module *m, int sr);
void   dm_seq_tick(dm_seq *s);                        /* process the next tick */
void   dm_seq_advance(dm_seq *s, uint32_t n);         /* consume n samples (n <= remaining in tick) */
uint32_t dm_seq_remaining(const dm_seq *s);           /* samples left in the current tick */
void   dm_seq_seek(dm_seq *s, uint64_t sample);       /* advance clock-only to absolute sample */
double dm_seq_beat(const dm_seq *s);                  /* interpolated */
double dm_seq_lbeat(const dm_seq *s);
void   dm_seq_fill_inputs(const dm_seq *s, float *in);
float  dm_note_freq(float note);

/* ---------------------------------------------------------------- synth */
typedef struct {
  uint32_t serial;
  int active;
  uint8_t inst, chan;
  double ph;
  float freq, vel, amp_l, amp_r, note;
  uint32_t t; int32_t rel;
  float params[4];
  float state[DM_MAX_STATE];
  uint32_t rng;
  int quiet;
} dm_voice;

typedef struct {
  float state[DM_MAX_STATE];
  float *dly[DM_MAX_DELAY_LINES];
  uint32_t dlypos[DM_MAX_DELAY_LINES];
  dm_verb verb[DM_MAX_VERBS];
} dm_fxunit;

typedef struct {
  const dm_module *m;
  int sr;
  dm_voice v[DM_MAX_VOICES];
  int cur[DM_MAX_CHANNELS];          /* voice currently owned by each channel, -1 none */
  uint32_t trig_serial[DM_MAX_CHANNELS]; /* last note-on handled, independent of voice slot reuse */
  float in[DM_IN_COUNT];
  dm_vm vm;
  dm_fxunit master, chan[DM_MAX_CHANNELS];
  uint32_t dlylen;
  uint32_t rng;
  float cenv[DM_MAX_CHANNELS];       /* level followers */
  uint8_t mute[DM_MAX_CHANNELS];
  float evhold[DM_MAX_EVNAMES], evlerp[DM_MAX_EVNAMES];
  float *mix;                        /* per-channel scratch, 2 * block */
  uint32_t mixcap;
} dm_synth;

int  dm_synth_init(dm_synth *s, const dm_module *m, int sr);
void dm_synth_free(dm_synth *s);
void dm_synth_tick(dm_synth *s, const dm_seq *q);
void dm_synth_render(dm_synth *s, const dm_seq *q, float *lr, uint32_t n);

/* ---------------------------------------------------------------- player (seq + synth) */
typedef struct {
  const dm_module *m;
  dm_seq seq;
  dm_synth synth;
  int sr;
} dm_player;
void dm_player_solo(dm_player *p, int chan);          /* -1 = none */

int  dm_player_init(dm_player *p, const dm_module *m, int sr);
void dm_player_free(dm_player *p);
void dm_player_render(dm_player *p, float *lr, uint32_t frames);

/* ---------------------------------------------------------------- scene */
typedef struct { int w, h; uint8_t *rgb; } dm_fb;
int  dm_fb_alloc(dm_fb *fb, int w, int h);
void dm_fb_free(dm_fb *fb);

typedef struct {
  const dm_module *m;
  dm_vm vm;
  float in[DM_IN_COUNT];
  float uni[DM_MAX_UNIFORMS];
  float fstate[DM_MAX_STATE], pstate[DM_MAX_STATE], ostate[DM_MAX_STATE];
  float worker_state[2][64][DM_MAX_STATE]; /* pixel and post state for workers 1..63 */
  float evhold[DM_MAX_EVNAMES], evlerp[DM_MAX_EVNAMES];
  uint32_t rng;
  int frame;
  int nthreads;                        /* 0 = auto */
  int scene;                           /* current pixel program */
  uint8_t *prev; int pw, ph;
  uint8_t *cur;                        /* scratch for the post pass */
} dm_scene;
void dm_scene_free(dm_scene *s);
void dm_fb_upscale(const dm_fb *src, dm_fb *dst);     /* nearest neighbour */

void dm_scene_init(dm_scene *s, const dm_module *m);
void dm_scene_render(dm_scene *s, const dm_seq *clk, const float *cenv, dm_fb *fb);

/* ---------------------------------------------------------------- io */
int dm_png_write(const dm_fb *fb, const char *path);
int dm_ppm_write(const dm_fb *fb, const char *path);

typedef struct { FILE *f; uint32_t frames; int sr, failed; } dm_wav;
int  dm_wav_open(dm_wav *w, const char *path, int sr);
int  dm_wav_write(dm_wav *w, const float *lr, uint32_t frames);
int  dm_wav_close(dm_wav *w);

/* Experimental dependency-free AVI writer: raw BGR video plus PCM audio.
 * Files are widely playable but large; AVI's classic 4 GiB limit applies. */
typedef struct dm_avi dm_avi;
dm_avi *dm_avi_open(const char *path, int w, int h, int fps, int sr);
int     dm_avi_video(dm_avi *a, const dm_fb *fb);
int     dm_avi_audio(dm_avi *a, const float *lr, uint32_t frames);
int     dm_avi_close(dm_avi *a);

/* ---------------------------------------------------------------- platform: audio */
typedef void (*dm_audio_cb)(void *user, float *lr, uint32_t frames);
typedef struct dm_audio dm_audio;
dm_audio *dm_audio_open(int sr, dm_audio_cb cb, void *user);
uint64_t  dm_audio_played(dm_audio *a);     /* frames heard so far (best estimate) */
int       dm_audio_failed(dm_audio *a);     /* pipe worker stopped after output failure */
void      dm_audio_close(dm_audio *a);
const char *dm_audio_backend(dm_audio *a);

/* ---------------------------------------------------------------- platform: terminal */
typedef struct dm_term dm_term;
enum { DM_KEY_UP = 0x101, DM_KEY_DOWN, DM_KEY_LEFT, DM_KEY_RIGHT, DM_KEY_SUSPEND, DM_KEY_SCRUB_BACK, DM_KEY_SCRUB_FORWARD };

dm_term *dm_term_open(void);
void     dm_term_size(dm_term *t, int *cols, int *rows);
void     dm_term_blit(dm_term *t, const dm_fb *fb, int cols, int rows, const char *status);
int      dm_term_key(dm_term *t);           /* -1 if none */
void     dm_term_wait(dm_term *t, double seconds); /* wait for input or frame deadline */
void     dm_term_close(dm_term *t);
double   dm_now(void);                      /* monotonic seconds */
void     dm_sleep(double sec);

#endif
