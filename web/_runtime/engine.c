/* Browser adapter: the same compiler, synth, sequencer and scene VM as dm. */
#include "dm.h"
#include <emscripten/emscripten.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static dm_module module;
static dm_player player;
static dm_scene scene;
static dm_seq clock_seq;
static dm_fb fb;
static float pcm[1470 * 2];
static int player_ready;

EMSCRIPTEN_KEEPALIVE void web_close(void) {
  if (player_ready) dm_player_free(&player);
  player_ready = 0;
  dm_scene_free(&scene); dm_fb_free(&fb); dm_module_free(&module);
  memset(&player, 0, sizeof player);
}
EMSCRIPTEN_KEEPALIVE int web_open(const char *source) {
  web_close();
  if (dm_compile(source, "/demo.dmt", &module)) return -1;
  if (dm_player_init(&player, &module, DM_SR)) return -1;
  player_ready = 1;
  dm_scene_init(&scene, &module); scene.nthreads = 1;
  dm_seq_init(&clock_seq, &module, DM_SR);
  return 0;
}
EMSCRIPTEN_KEEPALIVE const char *web_error(void) { return dm_error(); }
EMSCRIPTEN_KEEPALIVE float *web_audio(int frames) {
  if (!player_ready || frames < 1 || frames > 1470) return NULL;
  dm_player_render(&player, pcm, (uint32_t)frames);
  return pcm;
}
EMSCRIPTEN_KEEPALIVE float *web_env(void) { return player.synth.cenv; }
EMSCRIPTEN_KEEPALIVE int web_order(void) { return player.seq.order; }
EMSCRIPTEN_KEEPALIVE int web_row(void) { return player.seq.row; }
EMSCRIPTEN_KEEPALIVE int web_tick(void) { return player.seq.tick; }
EMSCRIPTEN_KEEPALIVE unsigned char *web_frame(double seconds, int width, int height, const float *env, int reset) {
  if (!player_ready || !isfinite(seconds) || seconds < 0 || width < 1 || width > 320 || height < 2 || height > 200) return NULL;
  if (reset || seconds * DM_SR < clock_seq.sample) {
    dm_scene_free(&scene); dm_scene_init(&scene, &module); scene.nthreads = 1;
    dm_seq_init(&clock_seq, &module, DM_SR);
  }
  dm_seq_seek(&clock_seq, (uint64_t)(seconds * DM_SR));
  int scale = module.scale > 1 ? module.scale : 1;
  if (dm_fb_alloc(&fb, width / scale > 1 ? width / scale : 1, height / scale > 1 ? height / scale : 2)) return NULL;
  dm_scene_render(&scene, &clock_seq, env, &fb);
  return fb.rgb;
}
EMSCRIPTEN_KEEPALIVE int web_width(void) { return fb.w; }
EMSCRIPTEN_KEEPALIVE int web_height(void) { return fb.h; }
