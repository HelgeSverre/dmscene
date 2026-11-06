/* Voice ownership must survive silent channels, note releases and slot reuse. */
#include "dm.h"
#include <stdio.h>

static int silent_channels_preserve_pcm(void) {
  const char *scores[] = {
    "song bpm=150 speed=6 rpb=4 channels=1\norder 0\n"
    "inst tone {\n out(bsqr(ph, 0.3, freq) * env(0.003, 0.05, 0.7, 0.1))\n}\n"
    "pattern 0 rows=16 {\n E-4 1 30\n}\n",
    "song bpm=150 speed=6 rpb=4 channels=3\norder 0\n"
    "inst tone {\n out(bsqr(ph, 0.3, freq) * env(0.003, 0.05, 0.7, 0.1))\n}\n"
    "pattern 0 rows=16 {\n . | E-4 1 30 | .\n}\n"
  };
  static float pcm[2][2 * 4410];
  for (int k = 0; k < 2; k++) {
    static dm_module m;
    static dm_player p;
    if (dm_compile(scores[k], "synth-voices.dmt", &m)) {
      fprintf(stderr, "%s\n", dm_error());
      return 1;
    }
    if (dm_player_init(&p, &m, DM_SR)) return 1;
    dm_player_render(&p, pcm[k], 4410);
    dm_player_free(&p);
    dm_module_free(&m);
  }
  for (int i = 0; i < 2 * 4410; i++) {
    if (pcm[0][i] != pcm[1][i]) {
      fprintf(stderr, "FAIL: silent channels changed PCM at sample %d\n", i);
      return 1;
    }
  }
  return 0;
}

static int probe(const char *label, const char *src, int channel,
                 uint32_t frames, uint32_t expected_age, int expected_tails) {
  static dm_module m;
  static dm_player p;
  if (dm_compile(src, "synth-voices.dmt", &m)) {
    fprintf(stderr, "%s\n", dm_error());
    return 1;
  }
  if (dm_player_init(&p, &m, DM_SR)) return 1;
  float audio[2 * 1024];
  for (uint32_t done = 0; done < frames;) {
    uint32_t n = frames - done < 1024 ? frames - done : 1024;
    dm_player_render(&p, audio, n);
    done += n;
  }
  int vi = p.synth.cur[channel], tails = 0;
  const dm_voice *v = vi >= 0 ? &p.synth.v[vi] : NULL;
  for (int i = 0; i < DM_MAX_VOICES; i++)
    if (p.synth.v[i].active && p.synth.v[i].chan == channel &&
        p.synth.v[i].rel >= 0) tails++;
  int failed = !v || !v->active || v->chan != channel ||
               v->t != expected_age || v->rel >= 0 || tails != expected_tails;
  if (failed) {
    fprintf(stderr, "FAIL: %s: age=%u expected=%u, owner=%d expected=%d, rel=%d, tails=%d expected=%d\n",
            label, v ? v->t : 0, expected_age, v ? v->chan : -1, channel,
            v ? v->rel : 0, tails, expected_tails);
  }
  dm_player_free(&p);
  dm_module_free(&m);
  return failed;
}

int main(void) {
  int failed = silent_channels_preserve_pcm();
  failed |= probe("held note with silent neighbours",
    "song bpm=150 speed=6 rpb=4 channels=3\norder 0\n"
    "inst tone {\n out(osc(ph) * env(0.003, 0.05, 0.7, 0.1))\n}\n"
    "pattern 0 rows=16 {\n . | E-4 1 30 | .\n}\n",
    1, 4410, 4410, 0);
  failed |= probe("held NNA note with silent neighbours",
    "song bpm=150 speed=6 rpb=4 channels=3\norder 0\n"
    "inst tone nna=continue {\n out(osc(ph) * env(0.003, 0.05, 0.7, 0.4))\n}\n"
    "pattern 0 rows=16 {\n . | E-4 1 30 | .\n}\n",
    1, 4410, 4410, 0);
  failed |= probe("NNA retrigger preserves exactly one old tail",
    "song bpm=150 speed=6 rpb=4 channels=3\norder 0\n"
    "inst tone nna=continue {\n out(osc(ph) * env(0.003, 0.05, 0.7, 0.4))\n}\n"
    "pattern 0 rows=16 {\n . | E-4 1 30 | .\n . | G-4 1 30 | .\n}\n",
    1, 8820, 4410, 1);
  failed |= probe("released channel cannot restart or release a reused slot",
    "song bpm=150 speed=6 rpb=4 channels=2\norder 0\n"
    "inst tone {\n out(osc(ph) * env(0.001, 0.01, 0.3, 0.01))\n}\n"
    "pattern 0 rows=16 {\n E-4 1 30 | .\n off | .\n . | G-4 1 30\n}\n",
    1, 15435, 6615, 0);
  if (!failed) puts("ok: synth voice ownership, silent-channel PCM, held notes, NNA tails and slot reuse");
  return failed;
}
