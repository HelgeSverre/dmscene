/* The showcase bass and hook are short phrases, including across breaks/loops. */
#include "dm.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  static dm_module m;
  FILE *f = fopen("demos/showcase.dmt", "rb");
  if (!f) return 1;
  if (fseek(f, 0, SEEK_END)) return 1;
  long len = ftell(f);
  if (len < 0 || fseek(f, 0, SEEK_SET)) return 1;
  char *src = malloc((size_t)len + 1);
  if (!src) return 1;
  if (fread(src, 1, (size_t)len, f) != (size_t)len) return 1;
  fclose(f);
  src[len] = 0;
  if (dm_compile(src, "demos/showcase.dmt", &m)) {
    fprintf(stderr, "%s\n", dm_error());
    return 1;
  }
  free(src);
  dm_seq q;
  dm_seq_init(&q, &m, DM_SR);
  double longest[DM_MAX_CHANNELS] = {0};
  /* Include a second pass: an empty intro must not inherit a held bass note. */
  while (q.loop_count < 2) {
    dm_seq_tick(&q);
    for (int c = 0; c < m.channels; c++) {
      double held = (double)(q.sample - q.ch[c].trig_sample) / DM_SR;
      if (q.ch[c].gate && held > longest[c]) longest[c] = held;
    }
    dm_seq_advance(&q, dm_seq_remaining(&q));
  }
  int failed = 0;
  const int channels[] = {3, 6};
  for (int i = 0; i < 2; i++) {
    int c = channels[i];
    if (!q.ch[c].trig_serial) {
      fprintf(stderr, "FAIL: showcase channel %d never played a note\n", c);
      failed = 1;
    } else if (longest[c] > 0.35) {
      fprintf(stderr, "FAIL: showcase channel %d held a note for %.3fs (maximum 0.35s)\n", c, longest[c]);
      failed = 1;
    }
  }
  dm_module_free(&m);
  if (!failed) puts("ok: showcase bass/hook gates release across breaks and song loops");
  return failed;
}
