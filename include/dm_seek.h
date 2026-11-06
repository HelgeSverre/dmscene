#ifndef DM_SEEK_H
#define DM_SEEK_H
#include "dm.h"

/* Cancellable background synthesis. Requests replace older requests; no audio
 * replay or audio-device operations run on the terminal's rendering thread. */
typedef struct dm_seek dm_seek;
dm_seek *dm_seek_open(const dm_module *m);
void dm_seek_request(dm_seek *s, uint64_t sample, const uint8_t *mute, int prepare_audio);
dm_player *dm_seek_take(dm_seek *s); /* caller owns returned player */
void dm_seek_env(dm_seek *s, float *env);
int dm_seek_failed(dm_seek *s);
void dm_seek_close(dm_seek *s);

typedef struct {
  double position, target, last_tick, last_event, density;
  int active, direction;
} dm_scrub;
void dm_scrub_add(dm_scrub *s, double delta, double now, double position);
double dm_scrub_tick(dm_scrub *s, double now);
#endif
