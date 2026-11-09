/* Mario Kart as an i64 game module (src/port/i64/n64_game.h).
 *
 * Built only with -DSPAGHETTIKART_I64_MODULE=ON, which makes the game a shared
 * library that exports just the n64_game_* functions (CMakeLists.txt,
 * src/port/i64/exports.txt). i64 owns the app, SDL, the window, the audio
 * output and the network session; this file maps its calls onto the
 * stand-alone game's run (Game.cpp: Port_RunBegin / Port_RunFrame /
 * Port_RunEnd) and puts the module back to a clean slate after each run
 * (Reentry.cpp, SimHeap.cpp), so it can be started again in the same process.
 * Same layout as BattleShip's port/i64_module.cpp.
 *
 * Online runs use the host's game channel (host->net_send/net_recv, through
 * KartNet's SendNow/Poll): no socket, room server or voice of our own. The
 * run is a KartNet session (the whole game as one console, host = player 1)
 * seeded with launch->seed; it ends (frame() returns EXIT) when the other
 * player leaves or KartNet gives up on them.
 *
 * Not yet: the session blob, ROM import on the device, title art, pause. */
#if defined(SPAGHETTIKART_I64_MODULE)

#define N64_GAME_BUILDING_MODULE 1
#include "i64/n64_game.h"

#include "Reentry.h"
#include "SimHeap.h"
#include "Rollback.h"
#include "kartnet_build_id.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
void Port_RunBegin(void);
int Port_RunFrame(void);
void Port_RunEnd(int fullShutdown);
void Port_RequestStop(void);
void LUS_SetExternalWindow(void* sdlWindow, void** rendererCache);
void LUS_SetExternalPacing(bool hostPaced);
/* Game audio goes to the host (Engine.cpp AudioOutPlay). */
extern void (*gPortHostAudioWrite)(const int16_t* stereo, uint32_t frames, uint32_t rate);
extern uint32_t (*gPortHostAudioBuffered)(uint32_t rate);
/* Online: the host's game channel (KartNet.c). */
extern int (*gKartNetHostSend)(const void* data, uint32_t size);
extern int (*gKartNetHostRecv)(void* buf, uint32_t cap);
int KartNet_NetDone(void);
void KartNet_Shutdown(void);
/* SimHeap.cpp: the game's allocator, for the host's operator new */
void* SimHeap_HookAlloc(size_t n);
void* SimHeap_HookAllocAligned(size_t n, size_t alignment);
void SimHeap_HookFree(void* p);
}

namespace {

const n64_game_rom kRoms[] = {
    { "579c48e211ae952530ffc8738709f078d5dd215e", "USA" },
};

const n64_game_info kInfo = {
    N64_GAME_ABI_VERSION,
    "mk64-us",
    "Mario Kart 64",
    kRoms,
    1,
    KARTNET_BUILD_ID,
    2,
    30, /* gInterpolationFPS's default */
    0,
};

/* Survive the reset of the globals between runs (Reentry.cpp). */
#define KEEP __attribute__((section("__DATA,__mkkeep")))
KEEP const n64_host* sHost = nullptr;
KEEP bool sRunning = false;
KEEP void* sRenderer = nullptr; /* SDL renderer on the host's window, reused across runs */
KEEP void* sRendererWindow = nullptr;
KEEP bool sOnline = false;
KEEP bool sPeerLeft = false;
KEEP char sNetLog[1100] = {};

const n64_alloc_hooks kAllocHooks = { SimHeap_HookAlloc, SimHeap_HookAllocAligned, SimHeap_HookFree };

void SetEnv(const char* key, const std::string& value) {
    setenv(key, value.c_str(), 1);
}

} // namespace

extern "C" N64_GAME_EXPORT const n64_game_info* n64_game_get_info(void) {
    return &kInfo;
}

extern "C" N64_GAME_EXPORT int n64_game_import_rom(const n64_host* host, const char* rom_path,
                                                   void (*progress)(float, void*), void* user, char* err,
                                                   size_t err_size) {
    (void) host;
    (void) rom_path;
    (void) progress;
    (void) user;
    std::snprintf(err, err_size, "ROM import inside i64 isn't built yet");
    return 1;
}

extern "C" N64_GAME_EXPORT uint32_t n64_game_session_blob(const n64_host* host, void* out, uint32_t cap) {
    (void) host;
    (void) out;
    (void) cap;
    return 0;
}

extern "C" N64_GAME_EXPORT int n64_game_start(const n64_host* host, const n64_launch* launch, char* err,
                                              size_t err_size) {
    if (sRunning) {
        std::snprintf(err, err_size, "already running");
        return 1;
    }
    if (host == nullptr || launch == nullptr || host->abi_version != N64_GAME_ABI_VERSION ||
        launch->abi_version != N64_GAME_ABI_VERSION) {
        std::snprintf(err, err_size, "i64 and this game module disagree on the interface version");
        return 1;
    }
    if (launch->mode == N64_LAUNCH_ONLINE &&
        (host->net_send == nullptr || host->net_recv == nullptr || launch->local_player > 1)) {
        std::snprintf(err, err_size, "online needs i64's game channel and 2 players");
        return 1;
    }
    /* First start: everything the module has done so far is static
     * initialisation, i.e. the pristine state to come back to after a run. */
    Reentry_SavePristine();

    sHost = host;
    if (host->data_dir != nullptr && host->data_dir[0] != '\0') {
        SetEnv("SHIP_HOME", host->data_dir); /* config, saves, logs, mk64.o2r, spaghetti.o2r */
    }
    /* netplay settings come from the environment: none from an earlier run */
    static const char* const kNetVars[] = { "MK64_NET", "MK64_NET_HOST", "MK64_NET_PEER", "MK64_NET_BIND",
                                            "MK64_NET_SERVER", "MK64_NET_ROOM", "MK64_NET_PASSWORD",
                                            "MK64_NET_SESSION", "MK64_NET_VOICE", "MK64_NET_SEED" };
    for (const char* v : kNetVars) {
        unsetenv(v);
    }
    unsetenv("MK64_NET_LOG");
    gKartNetHostSend = nullptr;
    gKartNetHostRecv = nullptr;
    sOnline = launch->mode == N64_LAUNCH_ONLINE;
    sPeerLeft = false;
    if (sOnline) {
        const bool isHost = launch->local_player == 0;
        SetEnv("MK64_NET", "1");
        SetEnv("MK64_NET_HOST", isHost ? "1" : "0");
        SetEnv("MK64_NET_SESSION", "1"); /* the whole game as one console */
        if (launch->seed != 0) {
            SetEnv("MK64_NET_SEED", std::to_string(launch->seed & 0xFFFF));
        }
        if (host->data_dir != nullptr && host->data_dir[0] != '\0') {
            /* sync log + per-race input/hash records (desync forensics) */
            std::snprintf(sNetLog, sizeof(sNetLog), "%s/kartnet-%s-run%u.log", host->data_dir, isHost ? "host" : "guest",
                          launch->run);
            SetEnv("MK64_NET_LOG", sNetLog);
        }
        gKartNetHostSend = host->net_send;
        gKartNetHostRecv = host->net_recv;
        /* rollback keeps race objects in SimHeap: libc++'s own allocations
         * must go the same way (n64_alloc_hooks) */
        if (host->set_alloc_hooks != nullptr) {
            host->set_alloc_hooks(&kAllocHooks);
        }
    }
    if (sRendererWindow != host->window) {
        sRenderer = nullptr; /* a different window: LUS creates a new renderer */
        sRendererWindow = host->window;
    }
    LUS_SetExternalWindow(host->window, &sRenderer);
    LUS_SetExternalPacing((launch->flags & N64_LAUNCH_HOST_PACED) != 0);
    gPortHostAudioWrite = host->audio_write;
    gPortHostAudioBuffered = host->audio_buffered;

    Port_RunBegin();
    sRunning = true;
    return 0;
}

extern "C" N64_GAME_EXPORT n64_frame_result n64_game_frame(void) {
    if (!sRunning) {
        return N64_GAME_FRAME_EXIT;
    }
    if (sOnline) {
        if (sHost->net_poll_event != nullptr) {
            for (int e; (e = sHost->net_poll_event()) != 0;) {
                if (e == N64_NET_PEER_LEFT) {
                    sPeerLeft = true;
                }
            }
        }
        if (sPeerLeft || KartNet_NetDone()) {
            return N64_GAME_FRAME_EXIT; /* the other player left: back to i64 */
        }
    }
    return Port_RunFrame() ? N64_GAME_FRAME_CONTINUE : N64_GAME_FRAME_EXIT;
}

extern "C" N64_GAME_EXPORT void n64_game_set_paused(int paused) {
    (void) paused; /* not yet: i64 simply stops calling frame() meanwhile */
}

extern "C" N64_GAME_EXPORT void n64_game_stop(void) {
    if (!sRunning) {
        return;
    }
    KartNet_Shutdown();
    Port_RunEnd(1);
    Rollback_ReleaseForReentry();
    gPortHostAudioWrite = nullptr;
    gPortHostAudioBuffered = nullptr;
    gKartNetHostSend = nullptr;
    gKartNetHostRecv = nullptr;
    if (sOnline && sHost->set_alloc_hooks != nullptr) {
        sHost->set_alloc_hooks(nullptr);
    }
    sOnline = false;
    LUS_SetExternalWindow(nullptr, nullptr);
    LUS_SetExternalPacing(false);
    SimHeap_ReleaseForReentry();
    Reentry_RestorePristine();
    sRunning = false;
    sHost = nullptr;
}

#endif /* SPAGHETTIKART_I64_MODULE */
