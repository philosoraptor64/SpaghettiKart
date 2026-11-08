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

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <sys/mman.h>

namespace {

constexpr size_t kReserve = (size_t) 1 << 30; /* 1 GB of address space; only touched pages cost memory */
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
    return p;
}

} // namespace

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

/* libultraship's resource manager suspends the scope while it runs. */
extern int (*LUS_AllocScopeSuspend)(void);
extern void (*LUS_AllocScopeResume)(int);

int SimHeap_Enable(void) {
    if (sEnabled) {
        return 1;
    }
    void* p = mmap(nullptr, kReserve, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
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
    fprintf(stderr, "SimHeap: enabled at %p\n", p);
    return 1;
}

int SimHeap_Enabled(void) {
    return sEnabled;
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
