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
#include "engine/sky/Sky.h"
#include "port/SimHeap.h"

#include <libultraship.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <chrono>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
extern "C" const struct mach_header_64 __dso_handle; /* this image (the app, or the i64 module dylib) */
#include <mach-o/dyld.h>
#include <dlfcn.h>
#include "n64net_reloc.h"
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
#include "objects.h"
#include "render_player.h"
#include "menu_items.h"
#include "menus.h"
void rollback_sim_frame(void); /* main.c */
extern const size_t gPlayerVtxSize[8]; /* data/some_data.c */
extern struct ObjectInterpData2 prevObject2[]; /* math_util_2.c */
extern const size_t gPrevObject2Size;
}

#include <set>
#include <string>
namespace {

struct Region {
    const char* name;
    uint8_t* base;
    size_t size;
};

/* The menus' C++ state (course select): the World's cup and each cup's track cursor, and which track object is
 * loaded. Outside the C sections, so a rollback in the menus put them back by hand (and rebuilds the track object). */
struct MenuCups {
    size_t cupIndex = 0;
    long currentCup = -1;   /* index in World::Cups */
    std::vector<size_t> cursors;
    std::string track;      /* the World's track (ResourceName), "" = none */
    std::string selected;   /* TrackBrowser's last selected track (its "already selected" cache) */
};

struct Snapshot {
    std::vector<uint8_t> data, bss, pool, heap, roots;
    uintptr_t poolUsed = 0;
    MenuCups menu;
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
    sData.base = getsectiondata(&__dso_handle, "__MKSNAP", "__data", &size);
    sData.size = size;
    sBss.base = getsectiondata(&__dso_handle, "__MKSNAP", "__bss", &size);
    sBss.size = size;
    sSupported = sData.base && sBss.base;
    fprintf(stderr, "Rollback: snapshot sections data=%zu KB bss=%zu KB%s\n", sData.size / 1024, sBss.size / 1024,
            sSupported ? "" : " (missing: rollback off)");
#endif
}

uintptr_t PoolStart() {
    return sPoolEnd - 0x10000000;
}

/* The C++ simulation's roots outside the sim heap: the World's containers and
 * other race members (their headers; everything they own is in the sim heap),
 * and the sky's actor list. Not the whole World: it also holds render
 * matrices that drawing changes mid-frame. */
struct Root {
    void* p;
    size_t n;
};
std::vector<Root> Roots() {
    World* w = GetWorld();
    std::vector<Root> r = {
        { &w->Actors, sizeof(w->Actors) },
        { &w->Objects, sizeof(w->Objects) },
        { &w->Emitters, sizeof(w->Emitters) },
        { &w->StaticMeshActors, sizeof(w->StaticMeshActors) },
        { &w->Lakitus, sizeof(w->Lakitus) },
        { &w->Crossings, sizeof(w->Crossings) },
        { &w->Cameras, sizeof(w->Cameras) },
        { &w->mPlayerBombKart, sizeof(w->mPlayerBombKart) },
    };
    if (Sky::Instance) {
        r.push_back({ &Sky::Instance->GetSkyActors(), sizeof(std::vector<std::unique_ptr<SkyActor>>) });
    }
    return r;
}

extern "C" const char* TrackBrowser_GetSelectedTrack(void);          /* engine/TrackBrowser.cpp */
extern "C" void TrackBrowser_RestoreSelectedTrack(const char* name);
extern "C" void TrackBrowser_SetTrack(const char* name);

static void SaveMenuCups(MenuCups& m) {
    World* w = GetWorld();
    m.cupIndex = w->CupIndex;
    m.currentCup = -1;
    m.cursors.clear();
    for (size_t i = 0; i < w->Cups.size(); i++) {
        if (w->Cups[i] == w->GetCurrentCup()) m.currentCup = (long) i;
        m.cursors.push_back(w->Cups[i] ? w->Cups[i]->CursorPosition : 0);
    }
    m.track = w->GetTrack() ? w->GetTrack()->ResourceName : "";
    m.selected = TrackBrowser_GetSelectedTrack();
}

static void LoadMenuCups(const MenuCups& m) {
    World* w = GetWorld();
    w->CupIndex = m.cupIndex;
    if (m.currentCup >= 0 && (size_t) m.currentCup < w->Cups.size()) {
        w->SetCurrentCup(w->Cups[(size_t) m.currentCup]);   /* (resets its cursor: restored next) */
    }
    for (size_t i = 0; i < w->Cups.size() && i < m.cursors.size(); i++) {
        if (w->Cups[i]) w->Cups[i]->CursorPosition = m.cursors[i];
    }
    const std::string now = w->GetTrack() ? w->GetTrack()->ResourceName : "";
    if (!m.track.empty() && now != m.track) {
        TrackBrowser_SetTrack(m.track.c_str());   /* the track object as it was (course select swaps it) */
    }
    TrackBrowser_RestoreSelectedTrack(m.selected.c_str());
}

void Save(Snapshot& s) {
    int scope = SimHeap_Suspend(); /* the snapshot buffers themselves: system heap */
    if (gGamestate != RACING) {
        SaveMenuCups(s.menu);
    }
    s.data.assign(sData.base, sData.base + sData.size);
    s.bss.assign(sBss.base, sBss.base + sBss.size);
    s.poolUsed = gNextFreeMemoryAddress - PoolStart();
    s.pool.assign((uint8_t*) PoolStart(), (uint8_t*) PoolStart() + s.poolUsed);
    uint8_t* hb = (uint8_t*) SimHeap_Base();
    if (hb) {
        s.heap.assign(hb, hb + SimHeap_Used());
    }
    s.roots.clear();
    for (const Root& r : Roots()) {
        s.roots.insert(s.roots.end(), (uint8_t*) r.p, (uint8_t*) r.p + r.n);
    }
    s.valid = true;
    SimHeap_Resume(scope);
}

void Load(const Snapshot& s) {
    memcpy(sData.base, s.data.data(), sData.size);
    memcpy(sBss.base, s.bss.data(), sBss.size);
    /* Memory handed out after the snapshot is simply forgotten: the cursor
     * (in __MKSNAP) goes back with the rest. */
    memcpy((void*) PoolStart(), s.pool.data(), s.pool.size());
    uint8_t* hb = (uint8_t*) SimHeap_Base();
    if (hb && !s.heap.empty()) {
        memcpy(hb, s.heap.data(), s.heap.size()); /* bookkeeping included: later blocks become free again */
    }
    size_t off = 0;
    for (const Root& r : Roots()) {
        if (off + r.n <= s.roots.size()) {
            memcpy(r.p, s.roots.data() + off, r.n);
        }
        off += r.n;
    }
    if (gGamestate != RACING && s.menu.currentCup >= 0) {   /* (gGamestate: just restored) */
        int scope = SimHeap_Suspend();
        LoadMenuCups(s.menu);
        SimHeap_Resume(scope);
    }
}

/* ---- SyncTest ---- */

struct Frame {
    Snapshot snap;          /* state at the start of the frame (before input) */
    OSContPad pads[4];      /* the frame's inputs after netplay/replay overrides */
    long tick = -1;
    int liveMenu = -1, liveFade = -1, liveDelay = -1, liveMenuFade = -1; /* MK64_SYNCTEST_TRACE: after the live frame */
};

int sSyncTestN = 0;
long sSyncTestFrom = 0; /* MK64_SYNCTEST_FROM: first frame to check */
std::vector<Frame> sRing;
long sTick = -1;   /* frame being run, -1 = not in a race */
FILE* sLog = nullptr;
unsigned long sChecks = 0, sMismatches = 0;
double sSaveMs = 0, sResimMs = 0;
int sReportBudget = 40;
std::set<std::pair<std::string, size_t>> sReported; /* (region, offset) already detailed */
size_t sReportedBefore = 0;


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
        /* detail the first mismatching frames, and after that every place
         * that hasn't mismatched before */
        const bool fresh = sReported.insert(std::make_pair(std::string(name), start)).second && sReported.size() < 2000;
        if (sReportBudget > 0 || fresh) {
            if (sReportBudget > 0) {
                sReportBudget--;
            }
            char what[256] = "";
            void* payload;
            size_t psize;
            if (strcmp(name, "heap") == 0 && SimHeap_BlockOf((void*) (base + start), &payload, &psize)) {
                /* name the object by its vtable, if it has one */
                Dl_info info;
                void* vptr = *(void**) payload;
                const char* cls = (dladdr(vptr, &info) && info.dli_sname) ? info.dli_sname : "?";
                snprintf(what, sizeof(what), " in block %p (+0x%lx of %zu bytes, vtable %s)", payload,
                         (unsigned long) ((base + start) - (uintptr_t) payload), psize, cls);
            }
            uintptr_t addr = base + start;
            if (addr >= (uintptr_t) gObjectList && addr < (uintptr_t) gObjectList + sizeof(Object) * OBJECT_LIST_SIZE) {
                size_t idx = (addr - (uintptr_t) gObjectList) / sizeof(Object);
                snprintf(what, sizeof(what), " gObjectList[%zu]+0x%zx (sizeof %zu)", idx,
                         (size_t) (addr - (uintptr_t) &gObjectList[idx]), sizeof(Object));
            }
            LogLine("  %s +0x%zx (addr 0x%lx) %zu bytes%s", name, start, (unsigned long) addr, i - start, what);
        }
    }
    if (a.size() != b.size()) {
        LogLine("  %s size %zu vs %zu", name, a.size(), b.size());
        bad++;
    }
    return bad;
}

/* More drawing-only globals: the kart sprite quads (render_player.c writes
 * their heights) and the "object jumped on screen, skip a frame" positions
 * (math_util_2.c mtxf_set_matrix_gObjectList). */
std::vector<std::pair<uint8_t*, size_t>> MoreDrawingState() {
    std::vector<std::pair<uint8_t*, size_t>> r;
    for (int i = 0; i < 8; i++) {
        r.push_back({ (uint8_t*) gPlayerVtx[i], gPlayerVtxSize[i] });
    }
    r.push_back({ (uint8_t*) prevObject2, gPrevObject2Size });
    return r;
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
    /* Camera view matrices: computed while drawing. */
    uint8_t* hb = (uint8_t*) SimHeap_Base();
    if (hb) {
        for (auto& cam : GetWorld()->Cameras) {
            for (Mtx* m : { cam->GetPerspMatrix(), cam->GetLookAtMatrix() }) {
                uintptr_t off = (uintptr_t) m - (uintptr_t) hb;
                if ((uintptr_t) m >= (uintptr_t) hb && off + sizeof(Mtx) <= s.heap.size()) {
                    memset(s.heap.data() + off, 0, sizeof(Mtx));
                }
            }
        }
    }
    /* Actors' "drawn on screen N" flag bits (0xF): written by drawing; the
     * shells' copy that race logic reads is recomputed each frame
     * (KartNet_UpdateShellVisibility). */
    if (hb) {
        size_t n = CM_GetActorSize();
        for (size_t i = 0; i < n; i++) {
            struct Actor* a = CM_GetActor(i);
            uintptr_t off = (uintptr_t) &a->flags - (uintptr_t) hb;
            if ((uintptr_t) &a->flags >= (uintptr_t) hb && off + sizeof(a->flags) <= s.heap.size()) {
                s.heap[off] &= 0xF0; /* little endian: low byte holds bits 0-7 */
            }
        }
    }
    for (auto* r : { &s.data, &s.bss }) {
        uint8_t* base = (r == &s.data) ? sData.base : sBss.base;
        maskIn(*r, base, gPlayers, sizeof(Player) * NUM_PLAYERS, true);
        maskIn(*r, base, gScreenContexts, sizeof(ScreenContext) * 4, false);
        maskIn(*r, base, &D_802BFB80, sizeof(D_802BFB80), false);
        maskIn(*r, base, gEncodedKartTexture, sizeof(gEncodedKartTexture[0]) * 2, false);
        maskIn(*r, base, gPlayerPalettesList, sizeof(gPlayerPalettesList), false);
        maskIn(*r, base, gPlayersToRenderScreenId, sizeof(s16) * 8, false);
        for (auto& d : MoreDrawingState()) {
            maskIn(*r, base, d.first, d.second, false);
        }
        /* Object visibility flags that per-camera drawing code sets and only
         * drawing code reads (code_80086E70.c func_8008A1D0/func_8008A364:
         * hot-air balloon, chain chomps, boos, hedgehogs, smoke particles);
         * race logic recomputes them from all cameras before reading them
         * (func_8008A6DC). */
        for (int i = 0; i < OBJECT_LIST_SIZE; i++) {
            uintptr_t a = (uintptr_t) &gObjectList[i].status, b = (uintptr_t) base;
            if (a >= b && a + sizeof(s32) <= b + r->size()) {
                s32 st;
                memcpy(&st, r->data() + (a - b), sizeof(st));
                st &= ~(0x00100000 | 0x00040000 | 0x00020000);
                memcpy(r->data() + (a - b), &st, sizeof(st));
            }
        }
        /* lightning flash progress (menu_items.c func_8009E2F0): started by the
         * logic, advanced only by drawing, read only by drawing */
        maskIn(*r, base, D_8018E838, sizeof(s8) * 4, false);
        maskIn(*r, base, D_8018E840, sizeof(s32) * 4, false);
        /* Lakitu faces the camera: OLakitu::Draw sets its orientation. */
        for (int i = 0; i < 4; i++) {
            s32 idx = gIndexLakituList[i];
            if (idx > 0 && idx < OBJECT_LIST_SIZE) {
                maskIn(*r, base, &gObjectList[idx].orientation, sizeof(gObjectList[idx].orientation), false);
            }
        }
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

/* Race setup is about to run in the sim heap: give the World's containers
 * fresh (empty) storage there, so nothing they own is on the system heap. */
void Rollback_PrepareWorld(void) {
    World* w = GetWorld();
    SimHeap_Enter();
    decltype(w->Actors)().swap(w->Actors);
    decltype(w->Objects)().swap(w->Objects);
    decltype(w->Emitters)().swap(w->Emitters);
    decltype(w->StaticMeshActors)().swap(w->StaticMeshActors);
    decltype(w->Lakitus)().swap(w->Lakitus);
    decltype(w->Crossings)().swap(w->Crossings);
    decltype(w->Cameras)().swap(w->Cameras);
    if (Sky::Instance) {
        std::vector<std::unique_ptr<SkyActor>>().swap(Sky::Instance->GetSkyActors());
    }
    SimHeap_Leave();
}

/* ---- Snapshot slots for the netplay rollback driver ---- */
static std::vector<Snapshot> sSlots;
static double sSlotSaveMs = 0, sSlotLoadMs = 0;
static unsigned long sSlotSaves = 0, sSlotLoads = 0;

} // extern "C"

namespace {

/* Drawing-only state (what SyncTest masks): a rollback must not bring back old
 * values of it. Rendering writes it in frame N and reads it again in N+1
 * (e.g. the kart textures and palettes prepared for the next frame), and the
 * re-simulated frames don't draw, so after a rollback the next frame would
 * draw from the snapshot's stale copy: wrong textures, objects drawn with
 * other matrices. Keep the live values across the rollback instead. */
struct DrawKeep {
    std::vector<std::pair<uint8_t*, size_t>> regions;
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> players, playerMask; /* gPlayers: only the drawing fields */
};
DrawKeep sDrawKeep;

std::vector<std::pair<uint8_t*, size_t>> DrawingRegions() {
    std::vector<std::pair<uint8_t*, size_t>> r = {
        { (uint8_t*) gScreenContexts, sizeof(ScreenContext) * 4 },
        { (uint8_t*) &D_802BFB80, sizeof(D_802BFB80) },
        { (uint8_t*) gEncodedKartTexture, sizeof(gEncodedKartTexture[0]) * 2 },
        { (uint8_t*) gPlayerPalettesList, sizeof(gPlayerPalettesList) },
        { (uint8_t*) gPlayersToRenderScreenId, sizeof(s16) * 8 },
    };
    for (auto& d : MoreDrawingState()) {
        r.push_back(d);
    }
    for (int i = 0; i < 4; i++) {
        s32 idx = gIndexLakituList[i];
        if (idx > 0 && idx < OBJECT_LIST_SIZE) {
            r.push_back({ (uint8_t*) &gObjectList[idx].orientation, sizeof(gObjectList[idx].orientation) });
        }
    }
    if (SimHeap_Base()) {
        for (auto& cam : GetWorld()->Cameras) {
            r.push_back({ (uint8_t*) cam->GetPerspMatrix(), sizeof(Mtx) });
            r.push_back({ (uint8_t*) cam->GetLookAtMatrix(), sizeof(Mtx) });
        }
    }
    return r;
}

} // namespace

extern "C" {

void Rollback_KeepDrawingState(void) {
    int scope = SimHeap_Suspend();
    sDrawKeep.regions = DrawingRegions();
    sDrawKeep.bytes.clear();
    for (auto& [p, n] : sDrawKeep.regions) {
        sDrawKeep.bytes.insert(sDrawKeep.bytes.end(), p, p + n);
    }
    const size_t pn = sizeof(Player) * NUM_PLAYERS;
    if (sDrawKeep.playerMask.size() != pn) {
        sDrawKeep.playerMask.assign(pn, 0xFF);
        KartNet_MaskDrawingFields(sDrawKeep.playerMask.data()); /* zero = a drawing byte */
    }
    sDrawKeep.players.assign((uint8_t*) gPlayers, (uint8_t*) gPlayers + pn);
    SimHeap_Resume(scope);
}

void Rollback_RestoreDrawingState(void) {
    size_t off = 0;
    for (auto& [p, n] : sDrawKeep.regions) {
        memcpy(p, sDrawKeep.bytes.data() + off, n);
        off += n;
    }
    uint8_t* live = (uint8_t*) gPlayers;
    for (size_t i = 0; i < sDrawKeep.players.size(); i++) {
        if (sDrawKeep.playerMask[i] == 0) {
            live[i] = sDrawKeep.players[i];
        }
    }
}

int Rollback_InitSlots(int count) {
    if (!Rollback_Supported()) {
        return 0;
    }
    sSlots.resize((size_t) count);
    return 1;
}

void Rollback_SaveSlot(int slot) {
    double t0 = NowMs();
    Save(sSlots[(size_t) slot % sSlots.size()]);
    sSlotSaveMs += NowMs() - t0;
    sSlotSaves++;
}

void Rollback_LoadSlot(int slot) {
    double t0 = NowMs();
    const Snapshot& s = sSlots[(size_t) slot % sSlots.size()];
    if (s.valid) {
        Load(s);
    }
    sSlotLoadMs += NowMs() - t0;
    sSlotLoads++;
}

void Rollback_SlotStats(double* avgSaveMs, double* avgLoadMs) {
    *avgSaveMs = sSlotSaves ? sSlotSaveMs / (double) sSlotSaves : 0;
    *avgLoadMs = sSlotLoads ? sSlotLoadMs / (double) sSlotLoads : 0;
}

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
    SimHeap_Enable();
    sSyncTestFrom = getenv("MK64_SYNCTEST_FROM") ? strtol(getenv("MK64_SYNCTEST_FROM"), nullptr, 0) : 0;
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
static bool sBegun = false; /* this frame was recorded (BeginFrame ran for it) */
void Rollback_SyncTestBeginFrame(long tick) {
    if (sSyncTestN <= 0) {
        return;
    }
    sTick = tick;
    sBegun = true;
    double t0 = NowMs();
    Frame& f = sRing[(size_t) tick % sRing.size()];
    Save(f.snap);
    f.tick = tick;
    sSaveMs += NowMs() - t0;
}

/* The frame's final inputs (after netplay/replay overrides). */
void Rollback_SyncTestRecordInput(void) {
    if (sSyncTestN <= 0 || sTick < 0 || !sBegun) {   /* (an unrecorded frame would overwrite the last recorded inputs) */
        return;
    }
    memcpy(sRing[(size_t) sTick % sRing.size()].pads, gControllerPads, sizeof(gControllerPads));
}

/* End of frame `tick` (logic and drawing done). */
void Rollback_SyncTestEndFrame(void) {
    /* Only a frame that was recorded: otherwise sTick still names the last recorded one, and the state after this
     * later frame was compared with a replay that stops earlier (false mismatches one frame after every switch into
     * a frame SyncTest doesn't record, e.g. each menu change). */
    const bool begun = sBegun;
    sBegun = false;
    if (!begun) {
        return;
    }
    if (sSyncTestN > 0 && sTick >= 0) {
        Frame& cur = sRing[(size_t) sTick % sRing.size()];
        cur.liveMenu = gMenuSelection;
        cur.liveFade = gFadeModeSelection;
        cur.liveDelay = gMenuDelayTimer;
        cur.liveMenuFade = gMenuFadeType;
    }
    if (sSyncTestN <= 0 || sTick < sSyncTestN || sTick < sSyncTestFrom) {
        return;
    }
    long from = sTick - sSyncTestN + 1;
    Frame& first = sRing[(size_t) from % sRing.size()];
    const bool menus = getenv("MK64_SYNCTEST_MENUS") && KartNet_MenuRollbackSafe();
    const bool menusOnly = getenv("MK64_SYNCTEST_MENUS") && strcmp(getenv("MK64_SYNCTEST_MENUS"), "only") == 0;
    if (first.tick != from || !((!menusOnly && gGamestate == RACING && gGamestateNext == RACING) || menus)) {
        return;
    }
    for (long t = from; t <= sTick; t++) { /* every frame in the window was a checkable one (no state change) */
        if (sRing[(size_t) t % sRing.size()].tick != t) {
            return;
        }
    }
    static Snapshot real, resim;
    auto dumpSky = [](const char* when) {
        if (!getenv("MK64_SYNCTEST_DEBUG") || !Sky::Instance) return;
        auto& v = Sky::Instance->GetSkyActors();
        LogLine("  sky %s: %zu actors data %p", when, v.size(), (void*) v.data());
        for (size_t i = 0; i < v.size() && i < 3; i++) {
            ScreenContext* sc = v[i]->mScreen;
            LogLine("    [%zu] %p screen %p camera %p", i, (void*) v[i].get(), (void*) sc, sc ? (void*) sc->camera : nullptr);
        }
        LogLine("    screens cam %p %p %p %p", (void*) gScreenContexts[0].camera, (void*) gScreenContexts[1].camera,
                (void*) gScreenContexts[2].camera, (void*) gScreenContexts[3].camera);
    };
    dumpSky("before");
    Save(real);
    WorldCounts realCounts = CountWorld();
    double t0 = NowMs();
    Load(first.snap);
    OSContPad saved[4];
    memcpy(saved, gControllerPads, sizeof(saved));
    gRollbackResim = 1;
    static char trace[2048];
    trace[0] = 0;
    for (long t = from; t <= sTick; t++) {
        memcpy(gControllerPads, sRing[(size_t) t % sRing.size()].pads, sizeof(gControllerPads));
        rollback_sim_frame();
        const Frame& lf = sRing[(size_t) t % sRing.size()];
        size_t n = strlen(trace);
        snprintf(trace + n, sizeof(trace) - n, " [%ld b%04x/%04x live m%d f%d d%d t%d | replay m%d f%d d%d t%d]", t,
                 (unsigned) lf.pads[0].button, (unsigned) lf.pads[1].button, lf.liveMenu, lf.liveFade, lf.liveDelay,
                 lf.liveMenuFade, gMenuSelection, gFadeModeSelection, gMenuDelayTimer, gMenuFadeType);
    }
    gRollbackResim = 0;
    memcpy(gControllerPads, saved, sizeof(saved));
    sResimMs += NowMs() - t0;
    Save(resim);
    WorldCounts resimCounts = CountWorld();
    if (getenv("MK64_SYNCTEST_TWICE")) {
        /* test: a second replay of the same frames; replay vs replay (MISMATCH2) separates "the live frame had an
         * input the replay lacks" (replays agree) from "a replay depends on state outside the snapshot" */
        static Snapshot resim2;
        Load(first.snap);
        gRollbackResim = 1;
        for (long t = from; t <= sTick; t++) {
            memcpy(gControllerPads, sRing[(size_t) t % sRing.size()].pads, sizeof(gControllerPads));
            rollback_sim_frame();
        }
        gRollbackResim = 0;
        memcpy(gControllerPads, saved, sizeof(saved));
        Save(resim2);
        MaskDrawingState(resim2);
        Snapshot r1 = resim;
        MaskDrawingState(r1);
        const size_t b2 = Compare("data", r1.data, resim2.data, (uintptr_t) sData.base) +
                          Compare("bss", r1.bss, resim2.bss, (uintptr_t) sBss.base) +
                          Compare("pool", r1.pool, resim2.pool, PoolStart());
        if (b2) LogLine("MISMATCH2 replay vs replay after frame %ld: %zu bytes (menu %d)", sTick, b2, gMenuSelection);
    }
    /* Continue from the state the normal frames produced (before masking the
     * copies for the comparison below). */
    Load(real);
    dumpSky("after restore");
    sChecks++;
    sReportBudget = (sMismatches < 60) ? 12 : 0; /* lines per mismatching frame */
    sReportedBefore = sReported.size();
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
                 Compare("pool", real.pool, resim.pool, PoolStart()) +
                 Compare("heap", real.heap, resim.heap, (uintptr_t) SimHeap_Base()) +
                 Compare("roots", real.roots, resim.roots, 0);
    if (bad) {
        sMismatches++;
        if (sMismatches <= 20 || sMismatches % 100 == 0 || sReported.size() != sReportedBefore) {
            LogLine("MISMATCH after frame %ld (resim from %ld): %zu bytes (game state %d, menu %d)", sTick, from, bad,
                    gGamestate, gMenuSelection);
            if (getenv("MK64_SYNCTEST_TRACE")) LogLine("  trace:%s", trace);
        }
    }
    dumpSky("after resim");
    static unsigned long perMenu[16];   /* checks per menu (gMenuSelection), in the summary below */
    perMenu[gMenuSelection & 15]++;
    static const long every = getenv("MK64_SYNCTEST_EVERY") ? std::max(1L, strtol(getenv("MK64_SYNCTEST_EVERY"), nullptr, 0)) : 300;
    if (sChecks % every == 0) {
        LogLine("synctest frame %ld: %lu checks, %lu mismatches; save %.2f ms, resim of %d frames %.2f ms (avg)",
                sTick, sChecks, sMismatches, sSaveMs / (double) (sChecks + sSyncTestN), sSyncTestN,
                sResimMs / (double) sChecks);
        if (getenv("MK64_SYNCTEST_MENUS")) {
            LogLine("  checks per menu: start %lu, main %lu, characters %lu, courses %lu, other %lu", perMenu[START_MENU],
                    perMenu[MAIN_MENU], perMenu[CHARACTER_SELECT_MENU], perMenu[COURSE_SELECT_MENU],
                    sChecks - perMenu[START_MENU] - perMenu[MAIN_MENU] - perMenu[CHARACTER_SELECT_MENU] -
                        perMenu[COURSE_SELECT_MENU]);
        }
    }
}

} // extern "C"

/* Re-entry (i64 module / soak): free the snapshot slots, the SyncTest ring and
 * the kept drawing state before the globals are reset (their containers would
 * be forgotten with their memory still allocated). */
/* ---- Resync: the same parts as a snapshot, in a form another device can load ---- */
#if defined(__APPLE__)
namespace {
/* room per part (the same on every device); Luigi Raceway uses 125 KB of pool and 66 KB of sim heap (the log
 * says how much each time) */
constexpr size_t kResyncPoolCap = (size_t) 8 << 20, kResyncHeapCap = (size_t) 8 << 20;
constexpr int kResyncCursors = 32;
struct MenuBlob {   /* MenuCups as plain bytes */
    uint64_t cupIndex;
    int64_t currentCup;
    uint32_t ncursors, valid;
    uint64_t cursors[kResyncCursors];
    char track[256], selected[256];
};
MenuBlob sMenuBlob;
n64net_reloc sReloc;
int sRelocReady = 0;
/* the root slots: always the same count (the sky's is empty without a sky) */
constexpr int kRootSlots = 9;
std::vector<uint8_t> sNoRoot(sizeof(std::vector<std::unique_ptr<SkyActor>>), 0);

int ResyncSpans(n64net_reloc_span* s) {
    FindSections();
    if (!sSupported) {
        return 0;
    }
    /* regions: the image, and the sim heap once it is on (the same reservation size on every device, wherever it
     * lies); set up each time (cheap) since the sim heap comes and goes with races */
    n64net_reloc_free(&sReloc);
    sRelocReady = n64net_reloc_init(&sReloc, &__dso_handle) ? 1 : -1;
    if (sRelocReady < 0) {
        return 0;
    }
    /* i64's game memory arena: at one fixed address on every device (pointers into it mean the same there); the
     * sim heap inside it comes after, so it is checked first */
    n64net_reloc_add(&sReloc, (const void*) (uintptr_t) 0x110000000ULL, (size_t) (576ULL << 20));
    if (SimHeap_Base()) {
        n64net_reloc_add(&sReloc, SimHeap_Base(), SimHeap_Capacity());
    }
    int n = 0;
    s[n++] = { sData.base, sData.size, sData.size };
    s[n++] = { sBss.base, sBss.size, sBss.size };
    s[n++] = { (void*) PoolStart(), (size_t) (gNextFreeMemoryAddress - PoolStart()), kResyncPoolCap };
    s[n++] = { SimHeap_Base() ? SimHeap_Base() : (void*) sNoRoot.data(), SimHeap_Used(), kResyncHeapCap };
    std::vector<Root> roots = SimHeap_Base() ? Roots() : std::vector<Root>();
    for (int i = 0; i < kRootSlots; i++) {
        if (i < (int) roots.size()) {
            s[n++] = { roots[(size_t) i].p, roots[(size_t) i].n, roots[(size_t) i].n };
        } else {
            s[n++] = { sNoRoot.data(), 0, sNoRoot.size() };
        }
    }
    s[n++] = { &sMenuBlob, sizeof(sMenuBlob), sizeof(sMenuBlob) };
    return n;
}
} // namespace

extern "C" size_t Rollback_ResyncSize(void) {
    n64net_reloc_span s[N64NET_RELOC_MAX_SPANS];
    const int n = ResyncSpans(s);
    return n ? n64net_reloc_state_size(s, n) : 0;
}

extern "C" int Rollback_ResyncSave(unsigned char* out) {
    n64net_reloc_span s[N64NET_RELOC_MAX_SPANS];
    n64net_reloc_stats st;
    double t0 = NowMs();
    int scope = SimHeap_Suspend();
    memset(&sMenuBlob, 0, sizeof(sMenuBlob));
    if (gGamestate != RACING) {
        MenuCups m;
        SaveMenuCups(m);
        sMenuBlob.valid = 1;
        sMenuBlob.cupIndex = m.cupIndex;
        sMenuBlob.currentCup = m.currentCup;
        sMenuBlob.ncursors = (uint32_t) std::min(m.cursors.size(), (size_t) kResyncCursors);
        for (uint32_t i = 0; i < sMenuBlob.ncursors; i++) sMenuBlob.cursors[i] = m.cursors[i];
        snprintf(sMenuBlob.track, sizeof(sMenuBlob.track), "%s", m.track.c_str());
        snprintf(sMenuBlob.selected, sizeof(sMenuBlob.selected), "%s", m.selected.c_str());
    }
    const int n = ResyncSpans(s);
    int ok = 0;
    if (n) {
        n64net_reloc_scan(&sReloc);
        ok = n64net_reloc_save(&sReloc, s, n, out, &st);
        LogLine("resync state: pool %zu KB, sim heap %zu KB, %llu words in the image or sim heap, %llu elsewhere "
                "in memory, %.1f ms%s", s[2].used / 1024, s[3].used / 1024, (unsigned long long) st.region_ptrs,
                (unsigned long long) st.foreign, NowMs() - t0, ok ? "" : " -- TOO BIG for its room: not usable");
        if (getenv("MK64_NET_RELOC_DUMP")) {   /* test: <dir>/mk-resync-<pid>.bin, to compare two devices */
            char path[1024];
            snprintf(path, sizeof(path), "%s/mk-resync-%d.bin", getenv("MK64_NET_RELOC_DUMP"), (int) getpid());
            if (FILE* f = fopen(path, "wb")) {
                fwrite(out, 1, n64net_reloc_state_size(s, n), f);
                fclose(f);
            }
        }
    }
    SimHeap_Resume(scope);
    return ok;
}

extern "C" int Rollback_ResyncLoad(const unsigned char* in) {
    n64net_reloc_span s[N64NET_RELOC_MAX_SPANS];
    n64net_reloc_stats st;
    const int n = ResyncSpans(s);
    if (!n) {
        return 0;
    }
    Rollback_KeepDrawingState();   /* this device's own drawing state stays (as after a rollback) */
    n64net_reloc_scan(&sReloc);
    const int ok = n64net_reloc_load(&sReloc, s, n, in, nullptr, &st);
    LogLine("resync load %s: %llu words kept, %llu translated, %llu own pointers, %llu matched, %llu guessed",
            ok ? "done" : "REFUSED", (unsigned long long) st.same, (unsigned long long) st.translated,
            (unsigned long long) st.own_pointer, (unsigned long long) st.matched, (unsigned long long) st.guessed);
    if (!ok) {
        return 0;
    }
    Rollback_RestoreDrawingState();
    if (gGamestate != RACING && sMenuBlob.valid && sMenuBlob.currentCup >= 0) {   /* (gGamestate: just loaded) */
        MenuCups m;
        m.cupIndex = (size_t) sMenuBlob.cupIndex;
        m.currentCup = (long) sMenuBlob.currentCup;
        for (uint32_t i = 0; i < sMenuBlob.ncursors && i < (uint32_t) kResyncCursors; i++) m.cursors.push_back(sMenuBlob.cursors[i]);
        sMenuBlob.track[sizeof(sMenuBlob.track) - 1] = sMenuBlob.selected[sizeof(sMenuBlob.selected) - 1] = 0;
        m.track = sMenuBlob.track;
        m.selected = sMenuBlob.selected;
        int scope = SimHeap_Suspend();
        LoadMenuCups(m);
        SimHeap_Resume(scope);
    }
    return 1;
}
#else
extern "C" size_t Rollback_ResyncSize(void) { return 0; }
extern "C" int Rollback_ResyncSave(unsigned char*) { return 0; }
extern "C" int Rollback_ResyncLoad(const unsigned char*) { return 0; }
#endif

extern "C" void Rollback_ReleaseForReentry(void) {
    std::vector<Snapshot>().swap(sSlots);
    std::vector<Frame>().swap(sRing);
    sDrawKeep = DrawKeep();
    if (sLog) {
        fclose(sLog);
        sLog = nullptr;
    }
}
