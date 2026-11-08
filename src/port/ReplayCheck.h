#ifndef REPLAY_CHECK_H
#define REPLAY_CHECK_H

/*
 * Determinism test harness (netplay groundwork).
 *
 * Inactive unless MK64_REPLAY_CHECK=<log path> is set. When active it:
 *   - jumps straight into a race (no menus) with a fixed RNG seed,
 *   - replaces all controller input with a seeded script for player 1,
 *   - logs one hash of all karts + the RNG seed per race frame,
 *   - exits after MK64_REPLAY_FRAMES race frames (default 2400).
 *
 * Two runs with the same settings must produce identical logs.
 *
 * Optional: MK64_REPLAY_SEED (input script, default 1), MK64_REPLAY_RNG
 * (gRandomSeed16 at race start, default 0x1234), MK64_REPLAY_CUP and
 * MK64_REPLAY_COURSE (default 0/0 = Luigi Raceway), MK64_REPLAY_CC (0-3,
 * default 2 = 150cc), MK64_REPLAY_CHAR (default 0 = Mario).
 */

#ifdef __cplusplus
extern "C" {
#endif

void ReplayCheck_BeginFrame(void);
void ReplayCheck_OverrideInput(void);
void ReplayCheck_EndFrame(void);

#ifdef __cplusplus
}
#endif

#endif
