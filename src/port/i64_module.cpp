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
 * Not yet: online runs over host->net_*, the session blob, ROM import on the
 * device, title art, pause. */
#if defined(SPAGHETTIKART_I64_MODULE)

#define N64_GAME_BUILDING_MODULE 1
#include "i64/n64_game.h"

#include "Reentry.h"
#include "SimHeap.h"
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
    if (launch->mode == N64_LAUNCH_ONLINE) {
        std::snprintf(err, err_size, "Mario Kart online inside i64 isn't built yet");
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
    return Port_RunFrame() ? N64_GAME_FRAME_CONTINUE : N64_GAME_FRAME_EXIT;
}

extern "C" N64_GAME_EXPORT void n64_game_set_paused(int paused) {
    (void) paused; /* not yet: i64 simply stops calling frame() meanwhile */
}

extern "C" N64_GAME_EXPORT void n64_game_stop(void) {
    if (!sRunning) {
        return;
    }
    Port_RunEnd(1);
    gPortHostAudioWrite = nullptr;
    gPortHostAudioBuffered = nullptr;
    LUS_SetExternalWindow(nullptr, nullptr);
    LUS_SetExternalPacing(false);
    SimHeap_ReleaseForReentry();
    Reentry_RestorePristine();
    sRunning = false;
    sHost = nullptr;
}

#endif /* SPAGHETTIKART_I64_MODULE */
