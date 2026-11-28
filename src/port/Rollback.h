#ifndef MK_ROLLBACK_H
#define MK_ROLLBACK_H
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Snapshots of the race simulation and the SyncTest check (Rollback.cpp). */
extern int gRollbackResim; /* 1 while frames are re-simulated (no drawing, no sound) */
int Rollback_Supported(void);
void Rollback_PrepareWorld(void);
/* Snapshot slots (netplay rollback). */
int Rollback_InitSlots(int count);
void Rollback_SaveSlot(int slot);
void Rollback_LoadSlot(int slot);
/* Netplay: drawing-only state survives a rollback (keep before
 * n64net_rollback_frame, restore after one happened). */
void Rollback_ReleaseForReentry(void);
void Rollback_KeepDrawingState(void);
void Rollback_RestoreDrawingState(void);
void Rollback_SlotStats(double* avgSaveMs, double* avgLoadMs);
/* Resync (n64net_resync_*, getting back in step after a desync): the snapshot in a form another device can load
 * (pointers translated, n64net_reloc.h). Size 0 = not available. Save returns 0 if the state didn't fit (the guest
 * then refuses it); load returns 0 if refused. */
size_t Rollback_ResyncSize(void);
int Rollback_ResyncSave(unsigned char* out);
/* live resync: copy the state now (the hold tick); Rollback_ResyncSave then encodes the copy (any thread) */
void Rollback_ResyncCapture(void);
int Rollback_ResyncLoad(const unsigned char* in); /* before race setup, when the sim heap is on */
void Rollback_SyncTestInit(const char* logPath); /* MK64_SYNCTEST=N enables it */
int Rollback_SyncTestActive(void);
void Rollback_SyncTestBeginFrame(long tick);
void Rollback_SyncTestRecordInput(void);
void Rollback_SyncTestEndFrame(void);

#ifdef __cplusplus
}
#endif

#endif
