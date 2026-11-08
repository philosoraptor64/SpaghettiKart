#ifndef MK_ROLLBACK_H
#define MK_ROLLBACK_H

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
void Rollback_SlotStats(double* avgSaveMs, double* avgLoadMs); /* before race setup, when the sim heap is on */
void Rollback_SyncTestInit(const char* logPath); /* MK64_SYNCTEST=N enables it */
int Rollback_SyncTestActive(void);
void Rollback_SyncTestBeginFrame(long tick);
void Rollback_SyncTestRecordInput(void);
void Rollback_SyncTestEndFrame(void);

#ifdef __cplusplus
}
#endif

#endif
