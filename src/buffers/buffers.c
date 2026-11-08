#include <libultraship.h>
#include <macros.h>
#include <mk64.h>
#include "buffers.h"

/**
 * @brief look like to be a buffer of decoded textures
 */
/* Kart textures and palettes are drawing state, but they stay in place (the
 * render code indexes past their ends; moving them crashed): Rollback.cpp
 * masks them when comparing instead. */
ALIGNED8 union_D_802BFB80 D_802BFB80;
// [nothing][screen][player]
ALIGNED8 struct_D_802DFB80 gEncodedKartTexture[2][2][8];
#ifdef AVOID_UB
// [buffer][screen][player] Buffer might be two separate buffers or something?
ALIGNED8 struct_D_802F1F80 gPlayerPalettesList[2][4][8];
#else
ALIGNED8 u16 gPlayerPalettesList[2][4][0x100 * 8];
#endif

MK_SNAPSHOT_OUTSIDE ALIGNED8 u16 gZBuffer[SCREEN_WIDTH * SCREEN_HEIGHT];

#ifdef AVOID_UB
ALIGNED8 u16 gFramebuffers[3][SCREEN_WIDTH * SCREEN_HEIGHT];
#else
u16 gFramebuffer0[SCREEN_WIDTH * SCREEN_HEIGHT];
u16 gFramebuffer1[SCREEN_WIDTH * SCREEN_HEIGHT];
u16 gFramebuffer2[SCREEN_WIDTH * SCREEN_HEIGHT];
#endif
