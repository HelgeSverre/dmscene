#include "dm_seek.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x, msg) do { if (!(x)) { fprintf(stderr, "FAIL: %s\n", msg); exit(1); } } while (0)

static dm_player *wait_result(dm_seek *s) {
  double deadline = dm_now() + 10;
  for (;;) {
    dm_player *p = dm_seek_take(s);
    if (p) return p;
    CHECK(!dm_seek_failed(s) && dm_now() < deadline, "seek did not finish");
    dm_sleep(.001);
  }
}

static void compare_seek(dm_seek *s, const dm_module *m, uint64_t pos, const uint8_t *mute) {
  dm_seek_request(s, pos, mute, 1);
  dm_player *got = wait_result(s);
  CHECK(got->seq.sample == pos, "stale seek result");
  dm_player ref;
  CHECK(!dm_player_init(&ref, m, DM_SR), "reference init");
  memcpy(ref.synth.mute, mute, DM_MAX_CHANNELS);
  float a[2048], b[2048];
  for (uint64_t at = 0; at < pos;) {
    uint32_t n = pos - at > 1024 ? 1024 : (uint32_t)(pos - at);
    dm_player_render(&ref, b, n); at += n;
  }
  /* Independent full replay vs. restored checkpoint, including evolving delay,
   * reverb, noise and voice state over several subsequent blocks. */
  for (int i = 0; i < 4; i++) {
    dm_player_render(got, a, 1024); dm_player_render(&ref, b, 1024);
    CHECK(!memcmp(a, b, sizeof a), "checkpoint seek changed PCM");
  }
  dm_player_free(&ref); dm_player_free(got); free(got);
}

int main(void) {
  static dm_module m;
  CHECK(!dm_module_read(&m, "build/modules/showcase.dms"), "read showcase");
  dm_seek *s = dm_seek_open(&m); CHECK(s, "open seek worker");
  uint8_t mute[DM_MAX_CHANNELS] = {0};
  compare_seek(s, &m, 3 * DM_SR + 173, mute);
  compare_seek(s, &m, DM_SR + 79, mute);
  compare_seek(s, &m, 2 * DM_SR + 391, mute);
  compare_seek(s, &m, 0, mute);
  /* Superseding a huge request must cancel work at the next source block. */
  dm_seek_request(s, (uint64_t)DM_SR * 100000, mute, 1);
  dm_sleep(.010);
  compare_seek(s, &m, DM_SR / 2, mute);
  mute[0] = mute[3] = 1;
  compare_seek(s, &m, 2 * DM_SR + 27, mute);
  dm_seek_close(s); dm_module_free(&m);

  dm_scrub slow = {0}, fast = {0};
  for (int i = 0; i < 20; i++) {
    dm_scrub_add(&slow, 1, 1 + i * .08, i ? slow.position : 10);
    dm_scrub_add(&fast, 1, 1 + i * .01, i ? fast.position : 10);
  }
  CHECK(fast.target - 10 > (slow.target - 10) * 1.2, "scroll speed does not affect displacement");
  double before = fast.position;
  dm_scrub_add(&fast, -1, 1.20, fast.position);
  before = fast.position;
  CHECK(dm_scrub_tick(&fast, 1.21) < before, "direction reversal retains forward motion");
  dm_scrub batch = {0}; dm_scrub_add(&batch, 200, 1, 10);
  CHECK(batch.target > 20, "large scroll burst was clipped");
  CHECK(dm_scrub_tick(&batch, 1.01) > 10 && batch.position < batch.target, "scroll jumps instead of interpolating");
  double target = batch.target;
  dm_scrub_tick(&batch, 2);
  CHECK(!batch.active && batch.position == target, "scroll fails to settle");
  dm_scrub_add(&batch, -10000, 3, 1); dm_scrub_tick(&batch, 4);
  CHECK(batch.position == 0 && batch.target == 0, "scroll before start");
  puts("ok: asynchronous seek, checkpoint PCM, cancellation, velocity, bursts and reversals");
  return 0;
}
