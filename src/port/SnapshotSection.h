/* Rollback snapshot section placement (same scheme as BattleShip's
 * port/rollback/snapshot_section.h).
 *
 * Force-included into the game's C files (see CMakeLists.txt): every mutable
 * global of the game, including function-static locals, lands in the
 * __MKSNAP segment, so port/Rollback.cpp can save and restore the C side of
 * the simulation with two memcpys instead of a hand-kept list.
 *
 * Files whose state must survive a rollback (netplay transport, replay
 * checker) or that are not simulation (audio) are compiled with
 * MK_SNAPSHOT_EXCLUDE and keep the default sections. Inside a snapshotted
 * file, MK_SNAPSHOT_OUTSIDE moves one big render-only buffer back out.
 *
 * Apple (Mach-O) only; elsewhere this is a no-op and rollback is off. */
#ifndef MK_SNAPSHOT_SECTION_H
#define MK_SNAPSHOT_SECTION_H

#if defined(__APPLE__) && defined(__clang__)
#define MK_SNAPSHOT_SUPPORTED 1
#define MK_SNAPSHOT_SEGMENT "__MKSNAP"
#define MK_SNAPSHOT_OUTSIDE __attribute__((section("__DATA,__bss")))
#if !defined(MK_SNAPSHOT_EXCLUDE) && !defined(__cplusplus)
#pragma clang section bss = "__MKSNAP,__bss" data = "__MKSNAP,__data"
#endif
#else
#define MK_SNAPSHOT_SUPPORTED 0
#define MK_SNAPSHOT_OUTSIDE
#endif

#endif
