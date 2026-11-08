/*
 * Rollback snapshots for SpaghettiKart (Mario Kart 64).
 *
 * A snapshot is:
 *   - the game's mutable C globals: the __MKSNAP data and bss sections
 *     (see SnapshotSection.h; audio, netplay and render buffers stay outside),
 *   - the used part of the game's memory pool (racing/memory.c, a bump
 *     allocator; its cursor gNextFreeMemoryAddress is itself in __MKSNAP),
 *   - (TODO) the C++ simulation objects (World actors, objects, emitters).
 *
 * MK64_SYNCTEST=N (netplay or replay races) checks it every frame: after
 * frame t it rewinds to the start of frame t-N+1, re-simulates those N frames
 * without drawing from the recorded inputs, and compares the result with the
 * state the normal frames produced, byte for byte. Mismatching bytes are
 * reported by section offset (scripts map them to symbols with nm).
 */
#include "Rollback.h"
#include "port/Game.h"
#include "engine/World.h"

#include <libultraship.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <chrono>

#if defined(__APPLE__)
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <mach-o/dyld.h>
#endif

extern "C" {
#include "main.h"
#include "racing/memory.h"
#include "code_800029B0.h"
#include "defines.h"
extern uintptr_t sPoolEnd;
extern OSContPad gControllerPads[4];
#include "port/KartNet.h"
#include "buffers.h"
void rollback_sim_frame(void); /* main.c */
}

namespace {

struct Region {
    const char* name;
    uint8_t* base;
    size_t size;
};

struct Snapshot {
    std::vector<uint8_t> data, bss, pool;
    uintptr_t poolUsed = 0;
    bool valid = false;
};

bool sReady = false, sSupported = false;
Region sData{ "data", nullptr, 0 }, sBss{ "bss", nullptr, 0 };

void FindSections() {
    if (sReady) {
        return;
    }
    sReady = true;
#if defined(__APPLE__)
    unsigned long size = 0;
    sData.base = getsectiondata(&_mh_execute_header, "__MKSNAP", "__data", &size);
    sData.size = size;
    sBss.base = getsectiondata(&_mh_execute_header, "__MKSNAP", "__bss", &size);
    sBss.size = size;
    sSupported = sData.base && sBss.base;
    fprintf(stderr, "Rollback: snapshot sections data=%zu KB bss=%zu KB%s\n", sData.size / 1024, sBss.size / 1024,
            sSupported ? "" : " (missing: rollback off)");
#endif
}

uintptr_t PoolStart() {
    return sPoolEnd - 0x10000000;
}

void Save(Snapshot& s) {
    s.data.assign(sData.base, sData.base + sData.size);
    s.bss.assign(sBss.base, sBss.base + sBss.size);
    s.poolUsed = gNextFreeMemoryAddress - PoolStart();
    s.pool.assign((uint8_t*) PoolStart(), (uint8_t*) PoolStart() + s.poolUsed);
    s.valid = true;
}

void Load(const Snapshot& s) {
    memcpy(sData.base, s.data.data(), sData.size);
    memcpy(sBss.base, s.bss.data(), sBss.size);
    /* Memory handed out after the snapshot is simply forgotten: the cursor
     * (in __MKSNAP) goes back with the rest. */
    memcpy((void*) PoolStart(), s.pool.data(), s.pool.size());
}

/* ---- SyncTest ---- */

struct Frame {
    Snapshot snap;          /* state at the start of the frame (before input) */
    OSContPad pads[4];      /* the frame's inputs after netplay/replay overrides */
    long tick = -1;
};

int sSyncTestN = 0;
std::vector<Frame> sRing;
long sTick = -1;   /* frame being run, -1 = not in a race */
FILE* sLog = nullptr;
unsigned long sChecks = 0, sMismatches = 0;
double sSaveMs = 0, sResimMs = 0;
int sReportBudget = 40;

double NowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

void LogLine(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void LogLine(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "Rollback: %s\n", buf);
    if (sLog) {
        fprintf(sLog, "%s\n", buf);
        fflush(sLog);
    }
}

/* Report mismatching byte ranges of one region (merged, at most a few). */
size_t Compare(const char* name, const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, uintptr_t base) {
    size_t n = std::min(a.size(), b.size()), bad = 0;
    size_t i = 0;
    while (i < n) {
        if (a[i] == b[i]) {
            i++;
            continue;
        }
        size_t start = i;
        while (i < n && (a[i] != b[i] || (i + 8 < n && memcmp(&a[i], &b[i], 8) != 0))) {
            i++;
        }
        bad += i - start;
        if (sReportBudget > 0) {
            sReportBudget--;
            LogLine("  %s +0x%zx (addr 0x%lx) %zu bytes", name, start, (unsigned long) (base + start), i - start);
        }
    }
    if (a.size() != b.size()) {
        LogLine("  %s size %zu vs %zu", name, a.size(), b.size());
        bad++;
    }
    return bad;
}

/* Drawing-only state that the normal frames write and re-simulation does
 * not (no drawing): blank it in both copies before comparing. */
void MaskDrawingState(Snapshot& s) {
    auto maskIn = [&](std::vector<uint8_t>& buf, uint8_t* base, const void* p, size_t n, bool players) {
        uintptr_t a = (uintptr_t) p, b = (uintptr_t) base;
        if (a < b || a + n > b + buf.size()) {
            return;
        }
        if (players) {
            KartNet_MaskDrawingFields(buf.data() + (a - b));
        } else {
            memset(buf.data() + (a - b), 0, n);
        }
    };
    for (auto* r : { &s.data, &s.bss }) {
        uint8_t* base = (r == &s.data) ? sData.base : sBss.base;
        maskIn(*r, base, gPlayers, sizeof(Player) * NUM_PLAYERS, true);
        maskIn(*r, base, gScreenContexts, sizeof(ScreenContext) * 4, false);
        maskIn(*r, base, &D_802BFB80, sizeof(D_802BFB80), false);
        maskIn(*r, base, gEncodedKartTexture, sizeof(gEncodedKartTexture[0]) * 2, false);
        maskIn(*r, base, gPlayerPalettesList, sizeof(gPlayerPalettesList), false);
    }
}

struct WorldCounts {
    size_t actors, objects, emitters, meshes;
};
WorldCounts CountWorld() {
    World* w = GetWorld();
    return { w->Actors.size(), w->Objects.size(), w->Emitters.size(), w->StaticMeshActors.size() };
}

} // namespace

extern "C" {

int gRollbackResim = 0;

int Rollback_Supported(void) {
    FindSections();
    return sSupported;
}

void Rollback_SyncTestInit(const char* logPath) {
    sSyncTestN = (int) strtol(getenv("MK64_SYNCTEST") ? getenv("MK64_SYNCTEST") : "0", nullptr, 0);
    if (sSyncTestN <= 0 || !Rollback_Supported()) {
        sSyncTestN = 0;
        return;
    }
    sRing.resize((size_t) sSyncTestN + 1);
    if (logPath && *logPath) {
        sLog = fopen(logPath, "w");
    }
    LogLine("synctest: rewind %d frames every frame", sSyncTestN);
#if defined(__APPLE__)
    LogLine("# sections data 0x%lx bss 0x%lx slide 0x%lx", (unsigned long) sData.base, (unsigned long) sBss.base,
            (unsigned long) _dyld_get_image_vmaddr_slide(0));
#endif
}

int Rollback_SyncTestActive(void) {
    return sSyncTestN > 0;
}

/* Start of frame `tick` (before the inputs are applied). */
void Rollback_SyncTestBeginFrame(long tick) {
    if (sSyncTestN <= 0) {
        return;
    }
    sTick = tick;
    double t0 = NowMs();
    Frame& f = sRing[(size_t) tick % sRing.size()];
    Save(f.snap);
    f.tick = tick;
    sSaveMs += NowMs() - t0;
}

/* The frame's final inputs (after netplay/replay overrides). */
void Rollback_SyncTestRecordInput(void) {
    if (sSyncTestN <= 0 || sTick < 0) {
        return;
    }
    memcpy(sRing[(size_t) sTick % sRing.size()].pads, gControllerPads, sizeof(gControllerPads));
}

/* End of frame `tick` (logic and drawing done). */
void Rollback_SyncTestEndFrame(void) {
    if (sSyncTestN <= 0 || sTick < sSyncTestN) {
        return;
    }
    long from = sTick - sSyncTestN + 1;
    Frame& first = sRing[(size_t) from % sRing.size()];
    if (first.tick != from || gGamestate != RACING || gGamestateNext != RACING) {
        return;
    }
    static Snapshot real, resim;
    Save(real);
    WorldCounts realCounts = CountWorld();
    double t0 = NowMs();
    Load(first.snap);
    OSContPad saved[4];
    memcpy(saved, gControllerPads, sizeof(saved));
    gRollbackResim = 1;
    for (long t = from; t <= sTick; t++) {
        memcpy(gControllerPads, sRing[(size_t) t % sRing.size()].pads, sizeof(gControllerPads));
        rollback_sim_frame();
    }
    gRollbackResim = 0;
    memcpy(gControllerPads, saved, sizeof(saved));
    sResimMs += NowMs() - t0;
    Save(resim);
    WorldCounts resimCounts = CountWorld();
    sChecks++;
    MaskDrawingState(real);
    MaskDrawingState(resim);
    if (memcmp(&realCounts, &resimCounts, sizeof(realCounts)) != 0 && sReportBudget > 0) {
        sReportBudget--;
        LogLine("  C++ world after frame %ld: actors %zu/%zu objects %zu/%zu emitters %zu/%zu meshes %zu/%zu (real/resim)",
                sTick, realCounts.actors, resimCounts.actors, realCounts.objects, resimCounts.objects,
                realCounts.emitters, resimCounts.emitters, realCounts.meshes, resimCounts.meshes);
    }
    size_t bad = Compare("data", real.data, resim.data, (uintptr_t) sData.base) +
                 Compare("bss", real.bss, resim.bss, (uintptr_t) sBss.base) +
                 Compare("pool", real.pool, resim.pool, PoolStart());
    if (bad) {
        sMismatches++;
        if (sMismatches <= 20 || sMismatches % 100 == 0) {
            LogLine("MISMATCH after frame %ld (resim from %ld): %zu bytes", sTick, from, bad);
        }
    }
    /* Continue from the state the normal frames produced. */
    Load(real);
    if (sChecks % 300 == 0) {
        LogLine("synctest frame %ld: %lu checks, %lu mismatches; save %.2f ms, resim of %d frames %.2f ms (avg)",
                sTick, sChecks, sMismatches, sSaveMs / (double) (sChecks + sSyncTestN), sSyncTestN,
                sResimMs / (double) sChecks);
    }
}

} // extern "C"
