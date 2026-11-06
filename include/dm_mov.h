#ifndef DM_MOV_H
#define DM_MOV_H

#include "dm.h"

/* Experimental QuickTime Animation writer: lossless RGB + PCM audio. */
typedef struct dm_mov dm_mov;
dm_mov *dm_mov_open(const char *path, int w, int h, int fps, int sr);
int dm_mov_video(dm_mov *m, const dm_fb *fb);
int dm_mov_audio(dm_mov *m, const float *lr, uint32_t frames);
int dm_mov_close(dm_mov *m);

#endif
