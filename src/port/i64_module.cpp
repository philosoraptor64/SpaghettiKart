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
 * ROM import (n64_game_import_rom): Torch (linked for GameExtractor anyway)
 * turns the player's ROM into <data>/mk64.o2r with the recipe in the
 * module's resources (config.yml, yamls/, meta/); spaghetti.o2r, the port's
 * own assets, ships there too and is copied next to it.
 *
 * Not yet: the session blob, title art, pause. */
#if defined(SPAGHETTIKART_I64_MODULE)

#define N64_GAME_BUILDING_MODULE 1
#include <chrono>
#include "i64/n64_game.h"

#include "Reentry.h"
#include "SimHeap.h"
#include <atomic>
#include "Rollback.h"
#include "kartnet_build_id.h"
#include "mk64_asset_recipe.h"

#include "Companion.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

extern "C" {
void Port_RunBegin(void);
int Port_RunFrame(void);
void Port_RunEnd(int fullShutdown);
void Port_RequestStop(void);
void LUS_SetExternalWindow(void* sdlWindow, void** rendererCache);
void LUS_SetExternalPacing(bool hostPaced);
void GameEngine_SetOnePicturePerHostFrame(bool on);   // Engine.cpp
int GameEngine_HostFrameWithoutLogic(void);
/* Game audio goes to the host (Engine.cpp AudioOutPlay). */
extern void (*gPortHostAudioWrite)(const int16_t* stereo, uint32_t frames, uint32_t rate);
extern uint32_t (*gPortHostAudioBuffered)(uint32_t rate);
/* Online: the host's game channel (KartNet.c). */
extern int (*gKartNetHostSend)(const void* data, uint32_t size);
extern int (*gKartNetHostRecv)(void* buf, uint32_t cap);
extern int (*gKartNetHostPeerLeft)(void);
int KartNet_NetDone(void);
const char* KartNet_EndReason(void);
void KartNet_Shutdown(void);
void KartNet_HostPause(int paused, int initiator);
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
    60, /* host frames/s: the 30 Hz logic plus one interpolated picture between ticks (Engine.cpp) */
    0,
};

/* Survive the reset of the globals between runs (Reentry.cpp). */
#define KEEP __attribute__((section("__DATA,__mkkeep")))
KEEP const n64_host* sHost = nullptr;
extern "C" bool gLusHostPaced; /* libultraship gfx_sdl2.cpp */
/* libultraship gfx_metal.cpp: one picture of the screen for i64's home screen */
extern "C" void (*gLusFrameCaptureCallback)(const uint8_t* rgba, uint32_t w, uint32_t h);
extern "C" std::atomic<int> gLusFrameCaptureRequest;
static unsigned sRunFrames = 0;
static constexpr unsigned kPreviewFirst = 1800, kPreviewEvery = 3600; /* 30 s in, then each minute (60 host frames/s) */

/* when to offer a picture: kPreviewFirst, then every kPreviewEvery frames; tests: I64_TEST_PREVIEW_AT="400,700" */
static bool PreviewDue(unsigned frame) {
    static const char* at = getenv("I64_TEST_PREVIEW_AT");
    if (at) {
        for (const char* p = at; p && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : nullptr) {
            if ((unsigned) atoi(p) == frame) return true;
        }
        return false;
    }
    return frame >= kPreviewFirst && (frame - kPreviewFirst) % kPreviewEvery == 0;
}
KEEP bool sRunning = false;
KEEP void* sRenderer = nullptr; /* SDL renderer on the host's window, reused across runs */
KEEP void* sRendererWindow = nullptr;
KEEP bool sOnline = false;
KEEP bool sPeerLeft = false;
KEEP char sNetLog[1100] = {};

const n64_alloc_hooks kAllocHooks = { SimHeap_HookAlloc, SimHeap_HookAllocAligned, SimHeap_HookFree };

/* the host's session events (also polled from KartNet's blocking waits) */
int PollPeerLeft(void) {
    if (sHost != nullptr && sHost->net_poll_event != nullptr) {
        for (int e; (e = sHost->net_poll_event()) != 0;) {
            if (e == N64_NET_PEER_LEFT) {
                sPeerLeft = true;
            }
        }
    }
    return sPeerLeft ? 1 : 0;
}

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
    namespace fs = std::filesystem;
    /* Before Torch runs: everything so far is static initialisation, the
     * pristine state (else Torch's buffers join the "before main" blocks
     * Reentry keeps forever, and its globals stay changed after a run). */
    Reentry_SavePristine();
    if (host == nullptr || rom_path == nullptr || host->data_dir == nullptr || host->bundle_dir == nullptr) {
        std::snprintf(err, err_size, "missing folders");
        return 1;
    }
    if (sRunning) {
        std::snprintf(err, err_size, "the game is running");
        return 1;
    }
    if (progress != nullptr) {
        progress(0.0f, user);
    }
    std::error_code ec;
    const fs::path data(host->data_dir);
    const fs::path recipe(host->bundle_dir);
    fs::create_directories(data, ec);
    if (!fs::exists(recipe / "config.yml", ec) || !fs::exists(recipe / "yamls", ec)) {
        std::snprintf(err, err_size, "the game's extraction recipe is missing from the module");
        return 1;
    }

    std::vector<uint8_t> rom;
    {
        std::ifstream in(rom_path, std::ios::binary);
        rom.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (rom.size() < (8u << 20) || rom.size() > (64u << 20) || (rom.size() % 4) != 0) {
        std::snprintf(err, err_size, "that file isn't an N64 ROM");
        return 1;
    }
    /* Torch wants big-endian (.z64): 80 37 12 40 */
    if (!(rom[0] == 0x80 && rom[1] == 0x37 && rom[2] == 0x12 && rom[3] == 0x40)) {
        if (rom[0] == 0x37 && rom[1] == 0x80) { /* .v64: 16-bit byteswapped */
            for (size_t i = 0; i + 1 < rom.size(); i += 2) {
                std::swap(rom[i], rom[i + 1]);
            }
        } else if (rom[0] == 0x40 && rom[1] == 0x12) { /* .n64: 32-bit little-endian */
            for (size_t i = 0; i + 3 < rom.size(); i += 4) {
                std::swap(rom[i], rom[i + 3]);
                std::swap(rom[i + 1], rom[i + 2]);
            }
        } else {
            std::snprintf(err, err_size, "that file isn't an N64 ROM");
            return 1;
        }
    }

    /* Torch's diagnostics (e.g. an unknown SHA-1) go to <data>/torch.log */
    auto previousLogger = spdlog::default_logger();
    try {
        spdlog::drop("torch");
        auto log = spdlog::basic_logger_mt("torch", (data / "torch.log").string(), true);
        log->flush_on(spdlog::level::info);
        spdlog::set_default_logger(log);
    } catch (const std::exception&) {
    }

    const fs::path o2r = data / "mk64.o2r";
    const fs::path temp = data / "mk64.o2r.importing";
    fs::remove(temp, ec);
    std::string outPath;
    bool ok = false;
    std::string why;
    try {
        /* Torch writes <dest>/mk64.o2r (config.yml's output.binary): use a
         * scratch folder so a failed run never leaves a partial archive */
        fs::create_directories(temp, ec);
        auto* c = new Companion(std::move(rom), ArchiveType::O2R, false, recipe.string(), temp.string());
        Companion::Instance = c;
        c->SetAdditionalFiles({ "meta/mods.toml" });
        c->Init(ExportType::Binary);
        outPath = c->GetOutputPath();
        Companion::Instance = nullptr;
        delete c;
        ok = !outPath.empty() && fs::exists(outPath, ec);
        if (outPath.empty()) {
            why = "this isn't Mario Kart 64 (USA)";
        } else if (!ok) {
            why = "not enough space to import the ROM";
        }
    } catch (const std::exception& e) {
        Companion::Instance = nullptr;
        why = std::string("extracting the game data failed: ") + e.what();
    }
    spdlog::drop("torch");
    spdlog::set_default_logger(previousLogger);
    if (ok) {
        fs::rename(outPath, o2r, ec);
        if (ec) {
            fs::remove(o2r, ec);
            fs::rename(outPath, o2r, ec);
        }
        ok = !ec;
        if (!ok) {
            why = "couldn't store the game data";
        }
    }
    fs::remove_all(temp, ec);
    if (!ok) {
        std::snprintf(err, err_size, "%s", why.c_str());
        return 1;
    }
    /* the recipe stamp n64_game_start checks */
    {
        std::ofstream stamp(o2r.string() + ".recipe", std::ios::trunc);
        stamp << MK64_ASSET_RECIPE << "\n";
    }
    if (progress != nullptr) {
        progress(1.0f, user);
    }
    return 0;
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
    /* Game data: mk64.o2r is imported from the player's ROM (above);
     * spaghetti.o2r comes with the module. A mk64.o2r without a stamp was
     * copied in by hand from a Mac build: accept it. */
    if (host->data_dir != nullptr && host->data_dir[0] != '\0') {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path data(host->data_dir);
        const fs::path o2r = data / "mk64.o2r";
        if (!fs::exists(o2r, ec)) {
            std::snprintf(err, err_size, "import your Mario Kart 64 ROM first");
            return 1;
        }
        std::ifstream stamp(o2r.string() + ".recipe");
        std::string have;
        if (stamp && std::getline(stamp, have) && have != MK64_ASSET_RECIPE) {
            std::snprintf(err, err_size, "the game data is from another version: import the ROM again");
            return 1;
        }
        if (host->bundle_dir != nullptr && host->bundle_dir[0] != '\0') {
            const fs::path mine = fs::path(host->bundle_dir) / "spaghetti.o2r";
            const fs::path used = data / "spaghetti.o2r";
            if (fs::exists(mine, ec) && fs::path(host->bundle_dir) != data &&
                (!fs::exists(used, ec) || fs::file_size(mine, ec) != fs::file_size(used, ec) ||
                 fs::last_write_time(mine, ec) > fs::last_write_time(used, ec))) {
                fs::copy_file(mine, used, fs::copy_options::overwrite_existing, ec);
            }
        }
        if (!fs::exists(data / "spaghetti.o2r", ec)) {
            std::snprintf(err, err_size, "spaghetti.o2r is missing from the module");
            return 1;
        }
    }
    /* First start: everything the module has done so far is static
     * initialisation, i.e. the pristine state to come back to after a run. */
    Reentry_SavePristine();

    sHost = host;
    gLusHostPaced = (launch->flags & N64_LAUNCH_HOST_PACED) != 0; /* libultraship: i64 paces the frames */
    sRunFrames = 0;
    gLusFrameCaptureCallback = host->offer_title_art;
    SimHeap_HostArenaReserve = host->arena_reserve;   /* (NULL in an older i64) */
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
        gKartNetHostPeerLeft = PollPeerLeft;
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
    GameEngine_SetOnePicturePerHostFrame((launch->flags & N64_LAUNCH_HOST_PACED) != 0);
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
        if (PollPeerLeft() || KartNet_NetDone() || KartNet_EndReason()[0]) {
            return N64_GAME_FRAME_EXIT; /* the other player left, a desync...: back to i64 (exit message) */
        }
    }
    // speed check: host frames vs logic ticks (MK's logic is 30 Hz), every 5 s in i64.log
    static uint64_t sStatsAt = 0;
    static unsigned sHostFrames = 0, sLogicTicks = 0;
    const uint64_t nowMs = (uint64_t) std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch()).count();
    sHostFrames++;
    ++sRunFrames;
    if (PreviewDue(sRunFrames) && sHost && sHost->offer_title_art) {
        gLusFrameCaptureRequest = 1; /* a picture for i64's home screen (libultraship gfx_metal.cpp) */
    }
    if (nowMs >= sStatsAt) {
        if (sStatsAt && sHost && sHost->log) {
            sHost->log("mk64: %u host frames, %u logic ticks in the last 5 s", sHostFrames, sLogicTicks);
        }
        sHostFrames = sLogicTicks = 0;
        sStatsAt = nowMs + 5000;
    }
    // the host frame between two logic ticks: the interpolated picture
    if (GameEngine_HostFrameWithoutLogic()) {
        return N64_GAME_FRAME_CONTINUE;
    }
    sLogicTicks++;
    return Port_RunFrame() ? N64_GAME_FRAME_CONTINUE : N64_GAME_FRAME_EXIT;
}

extern "C" {
extern int32_t gGamestate;     /* main.c */
extern uint16_t gIsGamePaused; /* code_800029B0.c */
}
constexpr int32_t kGamestateRacing = 4; /* RACING, include/defines.h */

/* i64 keeps the game while its players are in i64's menu: 1 = this player
 * paused, 2 = the friend did, 0 = back into the game (KartNet_HostPause). */
extern "C" N64_GAME_EXPORT void n64_game_set_paused(int paused) {
    static int pausedBy = 0;
    if (!sRunning || !sOnline) {
        return;
    }
    if (paused != 0) {
        pausedBy = paused;
        KartNet_HostPause(1, paused == 1);
    } else {
        KartNet_HostPause(0, pausedBy == 1);
        pausedBy = 0;
    }
}

/* Ready to be frozen: not in a race that is still running (a race first
 * opens its pause menu, from the Start pressed by KartNet_HostPause). */
/* why frame() ended the game, for i64's menu ("" = the player quit) */
extern "C" N64_GAME_EXPORT void n64_game_exit_message(char* out, size_t cap) {
    std::snprintf(out, cap, "%s", sOnline ? KartNet_EndReason() : "");
}

extern "C" N64_GAME_EXPORT int n64_game_pause_ready(void) {
    if (!sRunning || !sOnline) {
        return 1;
    }
    return (gGamestate == kGamestateRacing && gIsGamePaused == 0) ? 0 : 1;
}

extern "C" void Editor_ReleaseForReentry(void);   // Game.cpp

extern "C" N64_GAME_EXPORT void n64_game_stop(void) {
    if (!sRunning) {
        return;
    }
    gLusFrameCaptureCallback = nullptr;
    gLusFrameCaptureRequest = 0;
    KartNet_Shutdown();
    Port_RunEnd(1);
    Rollback_ReleaseForReentry();
    Editor_ReleaseForReentry();
    gPortHostAudioWrite = nullptr;
    gPortHostAudioBuffered = nullptr;
    gKartNetHostSend = nullptr;
    gKartNetHostRecv = nullptr;
    gKartNetHostPeerLeft = nullptr;
    if (sOnline && sHost->set_alloc_hooks != nullptr) {
        sHost->set_alloc_hooks(nullptr);
    }
    sOnline = false;
    LUS_SetExternalWindow(nullptr, nullptr);
    LUS_SetExternalPacing(false);
    GameEngine_SetOnePicturePerHostFrame(false);
    SimHeap_ReleaseForReentry();
    Reentry_RestorePristine();
    sRunning = false;
    sHost = nullptr;
}

#endif /* SPAGHETTIKART_I64_MODULE */
