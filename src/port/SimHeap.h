#ifndef MK_SIM_HEAP_H
#define MK_SIM_HEAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed-address heap for the C++ simulation objects (SimHeap.cpp). */
int SimHeap_Enable(void);  /* reserve it; until then everything uses the system heap */
int SimHeap_Enabled(void);
void SimHeap_ReleaseForReentry(void); /* after a full shutdown (Reentry.cpp) */
void SimHeap_FreezePreMainBlocks(void);  /* with Reentry_SavePristine */
void SimHeap_RestorePreMainBlocks(void); /* with Reentry_RestorePristine */
void SimHeap_Enter(void);  /* game thread: C++ allocations go to the sim heap (nestable) */
void SimHeap_Leave(void);
int SimHeap_Suspend(void); /* temporarily back to the system heap (engine caches) */
void SimHeap_Resume(int saved);
int SimHeap_InScope(void);
void* SimHeap_Base(void);
int SimHeap_BlockOf(const void* p, void** payload, size_t* size); /* diagnostics */
size_t SimHeap_Used(void); /* bytes from the base that a snapshot must copy */
/* i64 module: n64_host.arena_reserve (set before start; NULL = mmap) */
extern uint64_t (*SimHeap_HostArenaReserve)(uint64_t size, uint64_t align);
void SimHeap_Stats(size_t* liveBlocks, size_t* liveBytes, size_t* used, size_t* deferredFrees, size_t* alignedFallbacks);

#ifdef __cplusplus
}
#endif

#endif
