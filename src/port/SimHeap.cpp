/*
 * Sim heap: a fixed-address heap for the C++ simulation objects (World
 * actors, objects, emitters, the vectors and strings inside them), so a
 * rollback snapshot can save and restore all of them with one copy.
 *
 * While the game thread runs race logic inside SimHeap_Enter()/SimHeap_Leave()
 * (race setup and the logic half of race_logic_loop, see main.c), every C++
 * allocation (global operator new) comes from this heap. operator delete
 * routes by address. The allocator keeps all its bookkeeping inside the heap
 * (header at the start), so the used part [base, cursor) is the complete
 * state: restoring it also restores which blocks are free.
 *
 * Off unless SimHeap_Enable() was called (rollback or SyncTest), so normal
 * play keeps the system allocator.
 *
 * Hazards and how they are handled:
 *  - Memory allocated outside the heap and freed inside a scope (e.g. a
 *    container that grew before the race) is NOT freed: a rollback could bring
 *    back a pointer to it. Counted as "outside frees deferred" (leaked).
 *  - Over-aligned allocations (> 16 bytes) fall back to the system allocator.
 *  - Long-lived allocations made in a scope by non-simulation code (engine
 *    caches) would be rolled back with the heap: wrap such calls in
 *    SimHeap_Suspend()/SimHeap_Resume().
 */
#include "SimHeap.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <sys/mman.h>

namespace {

/* Address space for the simulation's C++ objects; only touched pages cost memory. A race uses a few MB (see
 * the "SimHeap: released" line); inside i64 the block comes from i64's game memory arena
 * (i64 docs/game-memory-arena.md), standalone from mmap. MK64_SIMHEAP_MB overrides. */
constexpr size_t kDefaultReserve = (size_t) 128 << 20;
size_t kReserve = kDefaultReserve;
bool sFromArena = false;
size_t sHighWater = 0;
constexpr int kClasses = 30;                  /* block sizes 16 << k */
constexpr uint32_t kMagic = 0x53494D48;       /* 'SIMH' */

struct Header {
    uint64_t magic;
    size_t cursor;            /* bytes used from base (header included) */
    void* freeList[kClasses]; /* free blocks per size class (payload pointers) */
    size_t liveBlocks, liveBytes;
};

struct BlockPrefix { /* 16 bytes before every payload */
    uint32_t magic;
    uint32_t cls;
    uint64_t pad;
};

uint8_t* sBase = nullptr;
std::mutex sLock; /* blocks may be freed on other threads (last shared_ptr owner) */
bool sEnabled = false;
thread_local int tScope = 0;
size_t sOutsideFreesDeferred = 0, sAlignedFallbacks = 0;

Header* H() {
    return reinterpret_cast<Header*>(sBase);
}

bool Owns(const void* p) {
    return sBase && (const uint8_t*) p >= sBase && (const uint8_t*) p < sBase + kReserve;
}

int ClassFor(size_t n) {
    size_t need = n + sizeof(BlockPrefix);
    int k = 0;
    while (((size_t) 16 << k) < need) {
        k++;
    }
    return k;
}

void* HeapAlloc(size_t n) {
    std::lock_guard<std::mutex> lock(sLock);
    int k = ClassFor(n);
    if (k >= kClasses) {
        return nullptr;
    }
    Header* h = H();
    uint8_t* payload;
    if (h->freeList[k]) {
        payload = (uint8_t*) h->freeList[k];
        h->freeList[k] = *(void**) payload;
    } else {
        size_t size = (size_t) 16 << k;
        if (h->cursor + size > kReserve) {
            fprintf(stderr, "SimHeap: full (%zu MB): raise kDefaultReserve or MK64_SIMHEAP_MB\n", kReserve >> 20);
            return nullptr;
        }
        uint8_t* block = sBase + h->cursor;
        h->cursor += size;
        BlockPrefix* pre = (BlockPrefix*) block;
        pre->magic = kMagic;
        pre->cls = (uint32_t) k;
        payload = block + sizeof(BlockPrefix);
    }
    h->liveBlocks++;
    h->liveBytes += (size_t) 16 << k;
    return payload;
}

void HeapFree(void* p) {
    std::lock_guard<std::mutex> lock(sLock);
    BlockPrefix* pre = (BlockPrefix*) ((uint8_t*) p - sizeof(BlockPrefix));
    if (pre->magic != kMagic) {
        fprintf(stderr, "SimHeap: bad free %p\n", p);
        abort();
    }
    Header* h = H();
    *(void**) p = h->freeList[pre->cls];
    h->freeList[pre->cls] = p;
    h->liveBlocks--;
    h->liveBytes -= (size_t) 16 << pre->cls;
}

/* Re-entry (Reentry.cpp): C++ objects built before main() (tables, maps,
 * strings in global objects) own heap blocks that the copy of the globals
 * points to. Record every block allocated before the pristine copy is taken;
 * at that moment copy their contents; from then on never free them, and put
 * their contents back on every restore. Then the pre-main heap comes back
 * exactly as it was, together with the globals. Kept in __DATA,__mkkeep (not
 * restored), with no allocation of its own while recording. */
#define MK_KEEP_SECTION __attribute__((section("__DATA,__mkkeep")))
struct PreBlock {
    void* p;
    size_t n;
    void* copy;
};
constexpr size_t kMaxPreBlocks = 1 << 16;
MK_KEEP_SECTION PreBlock sPre[kMaxPreBlocks];
MK_KEEP_SECTION size_t sPreCount = 0;
MK_KEEP_SECTION bool sPreFrozen = false; /* the pristine copy was taken */
MK_KEEP_SECTION bool sPreOverflow = false;

inline void PreRecord(void* p, size_t n) {
    if (sPreFrozen || !p) {
        return;
    }
    if (sPreCount < kMaxPreBlocks) {
        sPre[sPreCount++] = { p, n, nullptr };
    } else {
        sPreOverflow = true;
    }
}

/* true if p is a recorded pre-main block (after the freeze: never free it) */
inline bool PreForget(void* p) {
    if (!sPreFrozen) {
        for (size_t i = sPreCount; i-- > 0;) { /* freed before main: no longer pristine */
            if (sPre[i].p == p) {
                sPre[i] = sPre[--sPreCount];
                break;
            }
        }
        return false;
    }
    size_t lo = 0, hi = sPreCount; /* sorted at the freeze */
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if ((uintptr_t) sPre[mid].p < (uintptr_t) p) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < sPreCount && sPre[lo].p == p;
}

inline void* NewImpl(size_t n) {
    if (sEnabled && tScope > 0) {
        void* p = HeapAlloc(n ? n : 1);
        if (p) {
            return p;
        }
        fprintf(stderr, "SimHeap: out of space (%zu bytes)\n", n);
        abort();
    }
    void* p = malloc(n ? n : 1);
    if (!p) {
        throw std::bad_alloc();
    }
    PreRecord(p, n ? n : 1);
    return p;
}

inline void DeleteImpl(void* p) {
    if (!p) {
        return;
    }
    if (Owns(p)) {
        HeapFree(p);
        return;
    }
    if (sPreCount && PreForget(p)) {
        return; /* a pre-main block: kept for the next run (re-entry) */
    }
    if (sEnabled && tScope > 0) {
        sOutsideFreesDeferred++; /* see the header comment: leak, don't free */
        return;
    }
    free(p);
}

inline void* NewAligned(size_t n, std::align_val_t al) {
    if ((size_t) al <= 16) {
        return NewImpl(n);
    }
    if (sEnabled && tScope > 0) {
        sAlignedFallbacks++;
    }
    void* p = nullptr;
    if (posix_memalign(&p, (size_t) al, n ? n : 1) != 0) {
        throw std::bad_alloc();
    }
    PreRecord(p, n ? n : 1);
    return p;
}

} // namespace

/* ---- the same for a host app's global operator new/delete (i64 module:
 * n64_alloc_hooks), so libc++'s out-of-line code allocates like the game ---- */
extern "C" void* SimHeap_HookAlloc(size_t n) {
    try {
        return NewImpl(n);
    } catch (...) {
        return nullptr;
    }
}
extern "C" void* SimHeap_HookAllocAligned(size_t n, size_t alignment) {
    try {
        return NewAligned(n, (std::align_val_t) alignment);
    } catch (...) {
        return nullptr;
    }
}
extern "C" void SimHeap_HookFree(void* p) {
    DeleteImpl(p);
}

/* ---- global operator new/delete ---- */
void* operator new(size_t n) {
    return NewImpl(n);
}
void* operator new[](size_t n) {
    return NewImpl(n);
}
void* operator new(size_t n, const std::nothrow_t&) noexcept {
    try {
        return NewImpl(n);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](size_t n, const std::nothrow_t&) noexcept {
    try {
        return NewImpl(n);
    } catch (...) {
        return nullptr;
    }
}
void* operator new(size_t n, std::align_val_t al) {
    return NewAligned(n, al);
}
void* operator new[](size_t n, std::align_val_t al) {
    return NewAligned(n, al);
}
void operator delete(void* p) noexcept {
    DeleteImpl(p);
}
void operator delete[](void* p) noexcept {
    DeleteImpl(p);
}
void operator delete(void* p, size_t) noexcept {
    DeleteImpl(p);
}
void operator delete[](void* p, size_t) noexcept {
    DeleteImpl(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept {
    DeleteImpl(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept {
    DeleteImpl(p);
}
void operator delete(void* p, std::align_val_t) noexcept {
    DeleteImpl(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
    DeleteImpl(p);
}
void operator delete(void* p, size_t, std::align_val_t) noexcept {
    DeleteImpl(p);
}
void operator delete[](void* p, size_t, std::align_val_t) noexcept {
    DeleteImpl(p);
}

extern "C" {

uint64_t (*SimHeap_HostArenaReserve)(uint64_t size, uint64_t align) = nullptr;

/* libultraship's resource manager suspends the scope while it runs. */
extern int (*LUS_AllocScopeSuspend)(void);
extern void (*LUS_AllocScopeResume)(int);

int SimHeap_Enable(void) {
    if (sEnabled) {
        return 1;
    }
    kReserve = kDefaultReserve;
    if (const char* mb = getenv("MK64_SIMHEAP_MB")) {
        if (atoi(mb) > 0) {
            kReserve = (size_t) atoi(mb) << 20;
        }
    }
    void* p = MAP_FAILED;
    sFromArena = false;
    if (SimHeap_HostArenaReserve) {
        uint64_t a = SimHeap_HostArenaReserve(kReserve, 0);
        if (a && mprotect((void*) (uintptr_t) a, kReserve, PROT_READ | PROT_WRITE) == 0) {
            p = (void*) (uintptr_t) a;
            sFromArena = true;
        } else {
            fprintf(stderr, "SimHeap: i64's arena has no %zu MB left, using mmap\n", kReserve >> 20);
        }
    }
    if (p == MAP_FAILED) {
        p = mmap(nullptr, kReserve, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    }
    if (p == MAP_FAILED) {
        fprintf(stderr, "SimHeap: cannot reserve %zu MB\n", kReserve >> 20);
        return 0;
    }
    sBase = (uint8_t*) p;
    Header* h = H();
    memset(h, 0, sizeof(*h));
    h->magic = kMagic;
    h->cursor = (sizeof(Header) + 15) & ~(size_t) 15;
    LUS_AllocScopeSuspend = SimHeap_Suspend;
    LUS_AllocScopeResume = SimHeap_Resume;
    sEnabled = true;
    fprintf(stderr, "SimHeap: enabled at %p (%zu MB, %s)\n", p, kReserve >> 20, sFromArena ? "i64 arena" : "mmap");
    return 1;
}

int SimHeap_Enabled(void) {
    return sEnabled;
}

/* Re-entry (Reentry.cpp): freeze the pre-main blocks (copy their contents)
 * when the pristine copy of the globals is taken. */
void SimHeap_FreezePreMainBlocks(void) {
    if (sPreFrozen) {
        return;
    }
    std::sort(sPre, sPre + sPreCount, [](const PreBlock& a, const PreBlock& b) { return a.p < b.p; });
    sPreFrozen = true;
    for (size_t i = 0; i < sPreCount; i++) {
        sPre[i].copy = malloc(sPre[i].n);
        memcpy(sPre[i].copy, sPre[i].p, sPre[i].n);
    }
    fprintf(stderr, "SimHeap: %zu pre-main C++ blocks kept for re-entry%s\n", sPreCount,
            sPreOverflow ? " (TABLE FULL: some missed)" : "");
}

/* ...and put their contents back with the globals. */
void SimHeap_RestorePreMainBlocks(void) {
    for (size_t i = 0; i < sPreCount; i++) {
        memcpy(sPre[i].p, sPre[i].copy, sPre[i].n);
    }
}

/* Re-entry (Reentry.cpp): after the game shut down, give the reservation
 * back; the globals that point into it are reset next. Nothing may run in a
 * scope now. */
void SimHeap_ReleaseForReentry(void) {
    if (!sBase) {
        return;
    }
    fprintf(stderr, "SimHeap: released, %.1f MB used of %zu MB\n", H()->cursor / 1048576.0, kReserve >> 20);
    if (sFromArena) {
        /* i64 wipes its arena after the game stops; until then keep the range reserved (no hole to fill) */
        mmap(sBase, kReserve, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    } else {
        munmap(sBase, kReserve);
    }
    sBase = nullptr;
    sEnabled = false;
    LUS_AllocScopeSuspend = nullptr;
    LUS_AllocScopeResume = nullptr;
}

void SimHeap_Enter(void) {
    tScope++;
}
void SimHeap_Leave(void) {
    tScope--;
}
int SimHeap_Suspend(void) {
    int saved = tScope;
    tScope = 0;
    return saved;
}
void SimHeap_Resume(int saved) {
    tScope = saved;
}
int SimHeap_InScope(void) {
    return sEnabled && tScope > 0;
}

/* The block holding address p (walks the heap; diagnostics only). */
int SimHeap_BlockOf(const void* p, void** payload, size_t* size) {
    if (!Owns(p)) {
        return 0;
    }
    size_t off = (sizeof(Header) + 15) & ~(size_t) 15;
    while (off < H()->cursor) {
        BlockPrefix* pre = (BlockPrefix*) (sBase + off);
        size_t bs = (size_t) 16 << pre->cls;
        if ((const uint8_t*) p < sBase + off + bs) {
            *payload = sBase + off + sizeof(BlockPrefix);
            *size = bs - sizeof(BlockPrefix);
            return 1;
        }
        off += bs;
    }
    return 0;
}

void* SimHeap_Base(void) {
    return sBase;
}
size_t SimHeap_Used(void) {
    return sBase ? H()->cursor : 0;
}
void SimHeap_Stats(size_t* liveBlocks, size_t* liveBytes, size_t* used, size_t* deferredFrees, size_t* alignedFallbacks) {
    *liveBlocks = sBase ? H()->liveBlocks : 0;
    *liveBytes = sBase ? H()->liveBytes : 0;
    *used = SimHeap_Used();
    *deferredFrees = sOutsideFreesDeferred;
    *alignedFallbacks = sAlignedFallbacks;
}

} // extern "C"
