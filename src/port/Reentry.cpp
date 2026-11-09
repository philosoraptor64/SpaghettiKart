/*
 * Re-entry: start and stop the game several times in one process (for i64,
 * the launcher, where a game module is started and stopped while the app
 * keeps running). The approach is BattleShip's (port/reentry.cpp): at the top
 * of main(), before the game runs, copy the program's own writable data (C
 * and C++ globals: __DATA,__data/__bss/__common and __MKSNAP, which holds the
 * decomp's globals); after a full shutdown, put it back, so the next start
 * sees every global as a fresh process would.
 *
 * Mario Kart's __DATA,__bss is ~275 MB (framebuffers, the game's memory
 * pool), almost all of it zero at start. So the copy is kept per page: a page
 * that was all zero is reset by mapping fresh zero pages over it (no copy, no
 * touching, and its memory goes back to the system); only pages with start-up
 * data are copied. Partial pages at a section's ends are copied byte for byte
 * so neighbouring sections are never touched.
 *
 * State that must survive (this file's own bookkeeping) lives in
 * __DATA,__mkkeep, which is not restored. Libraries linked dynamically (SDL,
 * spdlog from Homebrew on the Mac) keep their state: the shutdown must leave
 * them clean (SDL windows destroyed, spdlog loggers dropped).
 */
#include "Reentry.h"
#include "SimHeap.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(__APPLE__)
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <mach/mach.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <cstdio>
#endif

#define MK_KEEP __attribute__((section("__DATA,__mkkeep")))

namespace {

struct Piece {        /* a stretch of one section */
    uint8_t* base;
    size_t size;
    uint8_t* copy;    /* the pristine bytes, or nullptr = all zero (whole pages) */
};

constexpr int kMaxPieces = 1 << 16;
MK_KEEP Piece* sPieces = nullptr;
MK_KEEP int sPieceCount = 0;
MK_KEEP size_t sCopiedBytes = 0, sZeroBytes = 0;
MK_KEEP bool sSaved = false;

void AddPiece(uint8_t* base, size_t size, bool zero) {
    if (size == 0 || sPieceCount >= kMaxPieces) {
        return;
    }
    Piece& p = sPieces[sPieceCount++];
    p.base = base;
    p.size = size;
    p.copy = nullptr;
    if (zero) {
        sZeroBytes += size;
        return;
    }
    p.copy = static_cast<uint8_t*>(std::malloc(size));
    std::memcpy(p.copy, base, size);
    sCopiedBytes += size;
}

bool AllZero(const uint8_t* p, size_t n) {
    const uint64_t* w = reinterpret_cast<const uint64_t*>(p);
    for (size_t i = 0; i < n / 8; i++) {
        if (w[i] != 0) {
            return false;
        }
    }
    return true;
}

void SaveSection(const char* segment, const char* section) {
#if defined(__APPLE__)
    unsigned long size = 0;
    uint8_t* base = getsectiondata(&_mh_execute_header, segment, section, &size);
    if (base == nullptr || size == 0) {
        return;
    }
    const uintptr_t page = (uintptr_t) getpagesize();
    uintptr_t start = (uintptr_t) base, end = start + size;
    uintptr_t first = (start + page - 1) & ~(page - 1); /* first whole page */
    uintptr_t last = end & ~(page - 1);                 /* end of the last whole page */
    if (first >= last) {                                /* no whole page: copy it all */
        AddPiece(base, size, false);
        return;
    }
    AddPiece(base, first - start, false); /* head */
    /* whole pages: runs of zero pages and runs of data pages */
    uintptr_t runStart = first;
    bool runZero = AllZero((const uint8_t*) first, page);
    for (uintptr_t p = first + page; p < last; p += page) {
        const bool z = AllZero((const uint8_t*) p, page);
        if (z != runZero) {
            AddPiece((uint8_t*) runStart, p - runStart, runZero);
            runStart = p;
            runZero = z;
        }
    }
    AddPiece((uint8_t*) runStart, last - runStart, runZero);
    AddPiece((uint8_t*) last, end - last, false); /* tail */
#else
    (void) segment;
    (void) section;
#endif
}

} // namespace

extern "C" void Reentry_SavePristine(void) {
    if (sSaved) {
        return;
    }
    sSaved = true;
    sPieces = static_cast<Piece*>(std::calloc(kMaxPieces, sizeof(Piece)));
    SaveSection("__DATA", "__data");
    SaveSection("__DATA", "__bss");
    SaveSection("__DATA", "__common");
    SaveSection("__MKSNAP", "__data");
    SaveSection("__MKSNAP", "__bss");
    SimHeap_FreezePreMainBlocks(); /* the C++ heap blocks those globals own */
}

extern "C" void Reentry_RestorePristine(void) {
#if defined(__APPLE__)
    for (int i = 0; i < sPieceCount; i++) {
        const Piece& p = sPieces[i];
        if (p.copy) {
            std::memcpy(p.base, p.copy, p.size);
        } else {
            /* fresh zero pages in place of whatever is there */
            void* r = mmap(p.base, p.size, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
            if (r == MAP_FAILED) {
                std::memset(p.base, 0, p.size);
            }
        }
    }
    SimHeap_RestorePreMainBlocks();
#endif
}

extern "C" void Reentry_Stats(unsigned long* copiedKb, unsigned long* zeroKb, unsigned long* footprintKb,
                              unsigned int* threads) {
    *copiedKb = (unsigned long) (sCopiedBytes / 1024);
    *zeroKb = (unsigned long) (sZeroBytes / 1024);
    *footprintKb = 0;
    *threads = 0;
#if defined(__APPLE__)
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        *footprintKb = (unsigned long) (info.phys_footprint / 1024);
    }
    thread_act_array_t list;
    mach_msg_type_number_t n = 0;
    if (task_threads(mach_task_self(), &list, &n) == KERN_SUCCESS) {
        *threads = n;
        const bool names = getenv("MK64_SOAK_THREADS") != nullptr;
        for (mach_msg_type_number_t i = 0; i < n; i++) {
            if (names) {
                char name[64] = "";
                pthread_t pt = pthread_from_mach_thread_np(list[i]);
                if (pt) {
                    pthread_getname_np(pt, name, sizeof(name));
                }
                thread_identifier_info_data_t idInfo;
                mach_msg_type_number_t c = THREAD_IDENTIFIER_INFO_COUNT;
                unsigned long long dq = 0;
                if (thread_info(list[i], THREAD_IDENTIFIER_INFO, (thread_info_t) &idInfo, &c) == KERN_SUCCESS) {
                    dq = idInfo.dispatch_qaddr;
                }
                fprintf(stderr, "  thread %u: '%s'%s\n", i, name, dq ? " (dispatch)" : "");
            }
            mach_port_deallocate(mach_task_self(), list[i]);
        }
        vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(list), n * sizeof(thread_act_t));
    }
#endif
}
