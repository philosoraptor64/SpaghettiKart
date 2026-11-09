/* Re-entry: run the game several times in one process (see Reentry.cpp). */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* First thing in main(): copy the program's writable globals as they are. */
void Reentry_SavePristine(void);
/* After a full shutdown: put every global back to that copy. */
void Reentry_RestorePristine(void);
/* Saved sizes (copied / reset as zero pages) and the process's physical
 * footprint (KB) and thread count right now. */
void Reentry_Stats(unsigned long* copiedKb, unsigned long* zeroKb, unsigned long* footprintKb, unsigned int* threads);

#ifdef __cplusplus
}
#endif
