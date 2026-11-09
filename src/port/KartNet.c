/* iOS build? PLATFORM_IOS comes from CMake for iOS builds, but macOS's
 * <mach-o/loader.h> (included below for the rollback probe) also defines a
 * PLATFORM_IOS constant, so decide before any include. */
#if defined(PLATFORM_IOS)
#define KARTNET_IOS 1
#endif
#include "KartNet.h"

#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <libultraship.h>
#include "defines.h"
#include "main.h"
#include "menus.h"
#include "buffers/random.h"
#include "code_800029B0.h"
#include "code_80057C60.h"
#include "racing/skybox_and_splitscreen.h"
#include "port/RaceStart.h"
#include "spawn_players.h"
#include "objects.h"
#include "camera.h"
#include "actor_types.h"
#include "racing/math_util.h"
#include "port/Game.h"
#include "menu_items.h"
#include "port/Rollback.h"
#include "port/SimHeap.h"
#include "n64net_input.h"
#include "n64net_rendezvous.h"
#include "kartnet_build_id.h"
#include <SDL2/SDL.h>
#ifdef KARTNET_VOICE
#include "n64net_voice.h"
#endif
void GameEngine_AudioKeepAlive(void); /* Engine.cpp */
#if defined(__APPLE__)
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
extern const struct mach_header_64 __dso_handle; /* this image (the app, or the i64 module dylib) */
#include <mach-o/dyld.h>
#endif

extern OSContPad gControllerPads[4];

/* ------------------------------------------------------------------------ */
/* Settings                                                                  */

enum { NET_OFF, NET_HANDSHAKE, NET_RUNNING, NET_DONE };

#define HASH_EVERY 30     /* frames between state-hash checkpoints */
#define WAIT_TIMEOUT_MS 20000   /* give up if the peer is silent this long (and not paused) */
#define PAUSED_TIMEOUT_MS 600000 /* ... or paused (app in the background) this long */
#define START_BOOT_FRAME 30      /* both sides wait here, so they start from identical states */

static int sInitDone = 0;
static int sState = NET_OFF;
static int sIsHost = 0;
static int sLocalPlayer = 0; /* 0 host, 1 guest */
static FILE* sLog = NULL;

static int sSock = -1;
static struct sockaddr_in sPeer;
static int sHavePeer = 0;

/* Match config (host decides; guest receives it). */
static unsigned int sSeed = 0;
static int sMode = GRAND_PRIX, sCup = 0, sCourse = 0, sCC = CC_150, sDelay = 2;
static int sChars[2] = { MARIO, LUIGI };

static unsigned long sBootFrames = 0;
static int sRaceRequested = 0;
static unsigned long sTick = 0;      /* netplay frame, 0 = the race-request frame */
static unsigned long sMaxTicks = 0;  /* 0 = unlimited */
static void LoadReplay(const char* path);
static int UiLogHook(const char* line);
static void SetWait(double seconds, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static int sUiSock = -1; /* Online screen: socket the room-code lookup found the peer on */
static struct sockaddr_in sUiPeer;
static int sBot = 0;
static int sBotDrift = 0; /* MK64_NET_BOT_DRIFT=1: the bot also drifts */
static long sBotQuitAt = 0; /* MK64_NET_BOT_QUIT=n: session bot quits each race n ticks in */
static int sBotQuitFinish = 0; /* MK64_NET_BOT_QUIT_FINISH=1: ... 5 s after another human finished */
static long sBotQuitRace = -2, sBotQuitTick = -1;
static int sBotExplore = 0;
static unsigned int sExploreRng = 1;
static int sExploreModePicks = 2; /* game modes picked so far (picks the next one); starts at battle */
static unsigned int sBotState = 1;
static int sBotSteer = 0, sBotTarget = 0;

typedef struct {
    u16 button;
    s8 x, y;
} NetPad;

static unsigned long sSyncOk = 0, sSyncBad = 0, sStalls = 0; /* sync results for the done line */
static double sStallMs = 0;
static double sLastHelloMs = 0;
static int sPeerPaused = 0;
static int sFullView = 1; /* MK64_NET_VIEW=full (default) or split */
/* Full view: draw only the local screen (default). MK64_NET_VIEW=double draws
 * both split screens first and undoes the extra draw (the first version). */
static int sSingleView = 1;
static int sInjectStart = 0; /* press Start once after returning from the background */


/* Inputs travel through n64net_input (input delay, prediction, misprediction
 * -> rollback, time sync, sync check); KartNet keeps the handshake. Rollback is
 * on by default (MK64_NET_ROLLBACK=0, or no snapshot support: lockstep).
 * Race setup isn't rollback-safe (setup objects are created in
 * the first frames), so the first MK64_NET_ROLLBACK_FROM ticks run in lockstep. */
#define RB_SLOTS 16
#define RB_HASHES 64
static int sRollback = 0;
static int sAdaptive = 1, sMaxDelay = 3; /* n64net auto delay: host DELAY .. sMaxDelay */
static uint32_t sRbWindow = 8;
static long sRbFrom = 60;
static n64net_input* sNet = NULL;
static unsigned long sSimTick = 0;
static unsigned long sSlotTick[RB_SLOTS];
static unsigned long long sTickHash[RB_HASHES];
static long sTickHashTick[RB_HASHES];
static long sLastRecorded = -1;
static unsigned long sRbCount = 0, sRbFrames = 0, sRbMaxDepth = 0, sHolds = 0;
static int sDesyncLogged = 0;

/* Session mode (MK64_NET_SESSION=1): both games run as one console from the
 * start-up wait on: the intro, the title screen, the game's own menus (host's
 * controller = player 1, guest's = player 2), races, results and the next
 * race, all from the exchanged inputs. Menus and race setup run in lockstep;
 * a race switches to rollback once it is MK64_NET_ROLLBACK_FROM ticks old.
 * Game-state changes (race -> menus, next race) run outside anything a
 * rollback can undo, so a pending one first waits until every input before it
 * is confirmed and any misprediction is corrected (GateTransition). */
static int sSession = 0;
/* People in the session, one per device (player index = device). Only 2 is
 * connected up today (the handshake, records and the local view are 2-player);
 * the menus and the input exchange already follow this number. */
static int sSessionPlayers = 2;
static int sRbActive = 0;      /* rollback window open right now */
static long sRaceStartTick = -1; /* tick the current race (RACING) began, -1 = not racing */
static int sMenuDelayCeiling = 6; /* MK64_NET_MENU_MAX_DELAY: auto-delay ceiling in lockstep parts */
static int sLastCeiling = -1;

/* Desync forensics. Every race writes "<log>.rec": the race config, every
 * confirmed tick's inputs for both players ("i tick b0 x0 y0 b1 x1 y1") and
 * every checkpoint hash ("h tick hash"). MK64_NET_PLAYBACK=<file.rec> re-runs
 * that race offline from those inputs and compares its own checkpoint hashes:
 * the side whose hashes the playback matches simulated correctly (add
 * MK64_SYNCTEST=8 to find state a rollback restores wrongly). On a desync
 * both sides also write the checkpoint's kart state ("<log>.desync.bin", the
 * hashed copy of gPlayers) and log a per-kart hash line to compare. */
#define DETAIL_RING 8
static FILE* sRec = NULL;
static char sRecBase[1024];
static Player sDetail[DETAIL_RING][NUM_PLAYERS];
static long sDetailTick[DETAIL_RING];
static u16 sDetailSeed[DETAIL_RING];
static s32 sDetailState[DETAIL_RING];
static int sPlayback = 0;
static int sStartShift = -1; /* Start presses take effect late in races (rollback); -1 = as sRollback */
static unsigned long sPlayLen = 0;
static u16* sPlayButtons = NULL;
static s8* sPlayStick = NULL;
static u32* sPlayHash = NULL; /* per checkpoint, 0 = none */
static unsigned long sPlayMatched = 0, sPlayChecked = 0;
static long sPlayFirstBad = -1;
int gKartNetHoldFrame = 0; /* time sync: this displayed frame runs no game logic */

/* Simulated bad network for tests (outgoing packets of this side):
 * MK64_NET_SIM_LATENCY / _JITTER (ms), _LOSS (%), _SPIKE (ms extra for
 * _SPIKE_LEN ms every _SPIKE_EVERY s). */
static int sSimLatency = 0, sSimJitter = 0, sSimLoss = 0, sSimSpike = 0, sSimSpikeEvery = 10, sSimSpikeLen = 1000;
static double sSimStart = 0;

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */

static double NowMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void Log(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "KartNet: %s\n", buf);
    if (sLog) {
        fprintf(sLog, "%s\n", buf);
        fflush(sLog);
    }
}

static long EnvInt(const char* name, long def) {
    const char* v = getenv(name);
    return (v && *v) ? strtol(v, NULL, 0) : def;
}

static const char* DocumentsPath(const char* path, char* out, size_t outSize) {
#if defined(KARTNET_IOS)
    const char* home = getenv("HOME");
    if (path && *path && path[0] != '/' && home) {
        snprintf(out, outSize, "%s/Documents/%s", home, path);
        return out;
    }
#endif
    (void) out;
    (void) outSize;
    return path;
}

#if defined(KARTNET_IOS)
/* iOS apps get no environment from a home-screen launch: read KEY=VALUE lines
 * from Documents/netplay.cfg (environment variables still win). */
static void LoadConfigFile(void) {
    char path[1024];
    DocumentsPath("netplay.cfg", path, sizeof(path));
    FILE* f = fopen(path, "r");
    if (!f) {
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* nl = strpbrk(line, "\r\n");
        if (nl) {
            *nl = '\0';
        }
        if (line[0] == '#' || line[0] == '\0') {
            continue;
        }
        char* eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        setenv(line, eq + 1, 0);
    }
    fclose(f);
}
#endif

static int ParseAddr(const char* s, struct sockaddr_in* out) {
    char host[256];
    const char* colon = strrchr(s, ':');
    if (!colon || (size_t) (colon - s) >= sizeof(host)) {
        return 0;
    }
    memcpy(host, s, colon - s);
    host[colon - s] = '\0';
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host[0] ? host : "0.0.0.0", colon + 1, &hints, &res) != 0 || !res) {
        return 0;
    }
    memcpy(out, res->ai_addr, sizeof(*out));
    freeaddrinfo(res);
    return 1;
}

static const char* AddrStr(const struct sockaddr_in* a) {
    static char buf[64];
    char ip[32];
    inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
    snprintf(buf, sizeof(buf), "%s:%d", ip, ntohs(a->sin_port));
    return buf;
}

static unsigned long long Fnv1a(unsigned long long h, const void* data, size_t len) {
    const unsigned char* p = (const unsigned char*) data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

/* Little-endian packing (both ends are arm64/x86 today, but be explicit). */
static void Put32(u8* p, u32 v) {
    for (int i = 0; i < 4; i++) {
        p[i] = (v >> (8 * i)) & 0xFF;
    }
}
static u32 Get32(const u8* p) {
    return (u32) p[0] | ((u32) p[1] << 8) | ((u32) p[2] << 16) | ((u32) p[3] << 24);
}
/* ------------------------------------------------------------------------ */
/* Packets                                                                   */

enum { PKT_HELLO = 1, PKT_CONFIG = 2, PKT_START = 3, PKT_PAUSE = 5, PKT_BYE = 6 }; /* inputs: n64net_input packets */
static const u8 kMagic[4] = { 'M', 'K', 'N', '1' };

static void SendNow(const u8* buf, size_t len) {
    sendto(sSock, buf, len, 0, (const struct sockaddr*) &sPeer, sizeof(sPeer));
}

typedef struct {
    double at;
    size_t len;
    u8 data[512];
} SimPacket;
#define SIM_QUEUE 512
static SimPacket sSimQueue[SIM_QUEUE];
static int sSimQueued = 0;
static unsigned int sSimRng = 0x2545F491;

static unsigned int SimRand(void) { /* own generator: never the game's RNG */
    sSimRng ^= sSimRng << 13;
    sSimRng ^= sSimRng >> 17;
    sSimRng ^= sSimRng << 5;
    return sSimRng;
}

static void SendRaw(const u8* buf, size_t len) {
    if (!sHavePeer) {
        return;
    }
    if (sSimLatency || sSimJitter || sSimLoss || sSimSpike) {
        if (sSimLoss && (int) (SimRand() % 100) < sSimLoss) {
            return;
        }
        if (sSimQueued < SIM_QUEUE && len <= sizeof(sSimQueue[0].data)) {
            double now = NowMs();
            double extra = sSimLatency + (sSimJitter ? (double) (SimRand() % (unsigned) (sSimJitter + 1)) : 0);
            if (sSimSpike && sSimSpikeEvery > 0) {
                double t = fmod(now - sSimStart, sSimSpikeEvery * 1000.0);
                if (t < sSimSpikeLen) {
                    extra += sSimSpike;
                }
            }
            SimPacket* q = &sSimQueue[sSimQueued++];
            q->at = now + extra;
            q->len = len;
            memcpy(q->data, buf, len);
        }
        return;
    }
    SendNow(buf, len);
}

static void FlushSim(void) {
    if (sSimQueued == 0) {
        return;
    }
    double now = NowMs();
    int kept = 0;
    for (int i = 0; i < sSimQueued; i++) {
        if (sSimQueue[i].at <= now) {
            SendNow(sSimQueue[i].data, sSimQueue[i].len);
        } else {
            sSimQueue[kept++] = sSimQueue[i];
        }
    }
    sSimQueued = kept;
}

static size_t Header(u8* buf, u8 type) {
    memcpy(buf, kMagic, 4);
    buf[4] = type;
    return 5;
}

static void SendHello(void) {
    u8 buf[16];
    size_t n = Header(buf, PKT_HELLO);
    buf[n++] = (u8) sChars[1];
    SendRaw(buf, n);
}

static void SendConfig(void) {
    u8 buf[32];
    size_t n = Header(buf, PKT_CONFIG);
    Put32(buf + n, sSeed);
    n += 4;
    buf[n++] = (u8) sMode;
    buf[n++] = (u8) sCup;
    buf[n++] = (u8) sCourse;
    buf[n++] = (u8) sCC;
    buf[n++] = (u8) sChars[0];
    buf[n++] = (u8) sChars[1];
    buf[n++] = (u8) sDelay;
    SendRaw(buf, n);
}

static void SendStart(void) {
    u8 buf[8];
    size_t n = Header(buf, PKT_START);
    SendRaw(buf, n);
}

static void SendPause(int paused) {
    u8 buf[8];
    size_t n = Header(buf, PKT_PAUSE);
    buf[n++] = (u8) (paused ? 1 : 0);
    for (int i = 0; i < 3; i++) {
        SendRaw(buf, n); /* no acks: repeat to survive a lost packet */
    }
}

static void HandlePacket(const u8* buf, size_t len, const struct sockaddr_in* from) {
    if (len < 5 || memcmp(buf, kMagic, 4) != 0) {
        return;
    }
    u8 type = buf[4];
    const u8* p = buf + 5;
    len -= 5;
    switch (type) {
        case PKT_HELLO:
            if (sIsHost && len >= 1) {
                if (!sHavePeer) {
                    Log("guest connected from %s", AddrStr(from));
                }
                sPeer = *from;
                sHavePeer = 1;
                if (sState == NET_HANDSHAKE) {
                    sChars[1] = p[0] % 8;
                    if (sChars[1] == sChars[0]) {
                        sChars[1] = (sChars[0] + 1) % 8;
                    }
                }
                SendConfig();
            }
            break;
        case PKT_CONFIG:
            if (!sIsHost && len >= 11) {
                if (sState == NET_HANDSHAKE) {
                    sSeed = Get32(p);
                    sMode = p[4];
                    sCup = p[5];
                    sCourse = p[6];
                    sCC = p[7];
                    sChars[0] = p[8];
                    sChars[1] = p[9];
                    sDelay = p[10];
                    sState = NET_RUNNING;
                    Log("config from host: seed=0x%04x mode=%d cup=%d course=%d cc=%d chars=%d,%d delay=%d", sSeed,
                        sMode, sCup, sCourse, sCC, sChars[0], sChars[1], sDelay);
                }
                SendStart(); /* also re-acks a repeated config */
            }
            break;
        case PKT_START:
            if (sIsHost && sState == NET_HANDSHAKE) {
                sState = NET_RUNNING;
                Log("guest acknowledged the config; starting");
            }
            break;
        case PKT_BYE:
            if (sState == NET_HANDSHAKE || sState == NET_RUNNING) {
                Log("the other player left the session");
                SetWait(6, "The other player left");
                sState = NET_DONE;
            }
            break;
        case PKT_PAUSE:
            if (len >= 1 && sPeerPaused != (p[0] != 0)) {
                sPeerPaused = p[0] != 0;
                Log("peer %s", sPeerPaused ? "paused (app in the background); waiting" : "resumed");
            }
            break;
        default:
            break;
    }
}

/* Status overlay (port/ui/ImguiUI.cpp draws KartNet_WaitMessage over the
 * game): what the player is waiting for. Timed messages vanish by themselves. */
int Port_IsInBackground(void); /* Game.cpp */
void GameEngine_DrawStatusFrame(void); /* Engine.cpp */
static char sWaitMsg[200];
static double sWaitUntil = 0; /* 0 = until cleared */
static char sRoomCode[8];

static void SetWait(double seconds, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void SetWait(double seconds, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(sWaitMsg, sizeof(sWaitMsg), fmt, ap);
    va_end(ap);
    sWaitUntil = seconds > 0 ? NowMs() + seconds * 1000 : 0;
}

static void ClearWait(void) {
    sWaitMsg[0] = '\0';
}

const char* KartNet_WaitMessage(void) {
    if (sWaitMsg[0] && sWaitUntil > 0 && NowMs() > sWaitUntil) {
        sWaitMsg[0] = '\0';
    }
    return sWaitMsg;
}

/* Inside a blocking wait: draw a status-only frame now and then (that also
 * handles window events); just pump events while iOS has the app in the
 * background (no GPU work allowed then). */
static void ShowWaiting(void) {
    static double last = 0;
    double now = NowMs();
    if (Port_IsInBackground()) {
        SDL_PumpEvents();
        return;
    }
    if (now - last >= 33) {
        last = now;
        GameEngine_DrawStatusFrame();
    }
}

/* Voice chat (MK64_NET_VOICE=1 open mic, =ptt push-to-talk; MK64_NET_VOICE_TEST=1
 * a tone instead of the microphone). Voice packets share the race socket. */
static int sVoiceMode = 0, sVoiceTest = 0, sVoiceStarted = 0;
static void NetLogCb(const char* fmt, ...);
#ifdef KARTNET_VOICE
static void VoiceSend(const uint8_t* data, uint32_t size) {
    SendRaw(data, size);
}
#endif
static void VoiceUpdate(void) {
#ifdef KARTNET_VOICE
    if (sVoiceMode && !sVoiceStarted && sState == NET_RUNNING) {
        n64net_voice_config vc;
        memset(&vc, 0, sizeof(vc));
        vc.enabled = 1;
        vc.push_to_talk = sVoiceMode == 2;
        vc.test_tone = sVoiceTest;
        vc.log = NetLogCb;
        n64net_voice_start(&vc);
        sVoiceStarted = 1;
    }
    if (sVoiceStarted && sState == NET_DONE) {
        n64net_voice_stop();
        sVoiceStarted = 0;
    }
    if (sVoiceStarted) {
        n64net_voice_pump(VoiceSend);
        GameEngine_AudioKeepAlive();
    }
#endif
}

static void Poll(void) {
    u8 buf[1500];
    FlushSim();
    for (;;) {
        struct sockaddr_in from;
        socklen_t fromLen = sizeof(from);
        ssize_t n = recvfrom(sSock, buf, sizeof(buf), 0, (struct sockaddr*) &from, &fromLen);
        if (n <= 0) {
            break;
        }
        if (!sIsHost) {
            /* The guest only listens to its host. */
            if (from.sin_addr.s_addr != sPeer.sin_addr.s_addr || from.sin_port != sPeer.sin_port) {
                continue;
            }
        } else if (sHavePeer && sState != NET_HANDSHAKE &&
                   (from.sin_addr.s_addr != sPeer.sin_addr.s_addr || from.sin_port != sPeer.sin_port)) {
            continue;
        }
        if (sNet && n64net_input_handle_packet(sNet, buf, (uint32_t) n)) {
            continue;
        }
#ifdef KARTNET_VOICE
        if (sVoiceStarted && n64net_voice_handle_packet(buf, (int32_t) n)) {
            continue;
        }
#endif
        HandlePacket(buf, (size_t) n, &from);
    }
    VoiceUpdate();
}

/* ------------------------------------------------------------------------ */
/* Setup                                                                     */

static void NetLogCb(const char* fmt, ...);
static void PumpWhileWaiting(void) {
    ShowWaiting();
}

/* MK64_NET_PLAYBACK: load a race record (see sRec) and run it offline. */
static void LoadPlayback(const char* path) {
    FILE* in = fopen(path, "r");
    if (!in) {
        Log("playback: cannot open %s", path);
        return;
    }
    unsigned long cap = 16384;
    sPlayButtons = (u16*) calloc(cap * 2, sizeof(u16));
    sPlayStick = (s8*) calloc(cap * 4, 1);
    sPlayHash = (u32*) calloc(cap / HASH_EVERY + 1, sizeof(u32));
    char line[256];
    int haveRace = 0;
    while (fgets(line, sizeof(line), in)) {
        unsigned long t;
        unsigned int b0, b1, h, seed;
        int x0, y0, x1, y1, mode, cup, course, cc, c0, c1, delay;
        int rb;
        unsigned int win;
        if (sscanf(line, "# session: seed=0x%x delay=%d rollback=%d window=%u", &seed, &delay, &rb, &win) == 4) {
            sSeed = seed;
            sDelay = delay;
            sSession = 1;
            sStartShift = rb;
            sRbWindow = win;
            haveRace = 1;
        } else if (sscanf(line, "# rollback=%d window=%u", &rb, &win) == 2) {
            sStartShift = rb;
            sRbWindow = win;
        } else if (sscanf(line, "# race: seed=0x%x mode=%d cup=%d course=%d cc=%d chars=%d,%d delay=%d", &seed, &mode, &cup,
                   &course, &cc, &c0, &c1, &delay) == 8) {
            sSeed = seed;
            sMode = mode;
            sCup = cup;
            sCourse = course;
            sCC = cc;
            sChars[0] = c0;
            sChars[1] = c1;
            sDelay = delay;
            haveRace = 1;
        } else if (sscanf(line, "i %lu %x %d %d %x %d %d", &t, &b0, &x0, &y0, &b1, &x1, &y1) == 7) {
            while (t >= cap) {
                sPlayButtons = (u16*) realloc(sPlayButtons, cap * 4 * sizeof(u16));
                memset(sPlayButtons + cap * 2, 0, cap * 2 * sizeof(u16));
                sPlayStick = (s8*) realloc(sPlayStick, cap * 8);
                memset(sPlayStick + cap * 4, 0, cap * 4);
                sPlayHash = (u32*) realloc(sPlayHash, (cap * 2 / HASH_EVERY + 1) * sizeof(u32));
                memset(sPlayHash + cap / HASH_EVERY + 1, 0, (cap / HASH_EVERY) * sizeof(u32));
                cap *= 2;
            }
            sPlayButtons[t * 2] = (u16) b0;
            sPlayButtons[t * 2 + 1] = (u16) b1;
            sPlayStick[t * 4] = (s8) x0;
            sPlayStick[t * 4 + 1] = (s8) y0;
            sPlayStick[t * 4 + 2] = (s8) x1;
            sPlayStick[t * 4 + 3] = (s8) y1;
            if (t + 1 > sPlayLen) {
                sPlayLen = t + 1;
            }
        } else if (sscanf(line, "h %lu %x", &t, &h) == 2 && t % HASH_EVERY == 0 && t < cap) {
            sPlayHash[t / HASH_EVERY] = h;
        }
    }
    fclose(in);
    if (!haveRace || sPlayLen == 0) {
        Log("playback: %s has no race or no inputs", path);
        return;
    }
    sPlayback = 1;
    sIsHost = 1;
    sStartShift = sStartShift && sRbWindow > 0; /* as recorded */
    sLocalPlayer = (int) EnvInt("MK64_NET_PLAYBACK_VIEW", 0) ? 1 : 0;
    sRollback = 0;
    sState = NET_RUNNING;
    Log("playback of %s: %lu ticks, seed=0x%04x cup=%d course=%d chars=%d,%d", path, sPlayLen, sSeed, sCup, sCourse,
        sChars[0], sChars[1]);
}

static void Init(void) {
    sInitDone = 1;
#if defined(KARTNET_IOS)
    LoadConfigFile();
#endif
    const char* playback = getenv("MK64_NET_PLAYBACK");
    if (EnvInt("MK64_NET", 0) == 0 && !(playback && *playback)) {
        return;
    }
    char pathBuf[1024];
    const char* logPath = DocumentsPath(getenv("MK64_NET_LOG"), pathBuf, sizeof(pathBuf));
    if (logPath && *logPath) {
        sLog = fopen(logPath, "w");
        snprintf(sRecBase, sizeof(sRecBase), "%s", logPath);
        if (!(playback && *playback)) {
            char recPath[1100];
            snprintf(recPath, sizeof(recPath), "%s.rec", logPath);
            sRec = fopen(recPath, "w");
            snprintf(recPath, sizeof(recPath), "%s.desync.bin", logPath);
            remove(recPath); /* from an earlier race */
        }
    }

    sIsHost = EnvInt("MK64_NET_HOST", 0) != 0;
    sLocalPlayer = sIsHost ? 0 : 1;
    sDelay = (int) EnvInt("MK64_NET_DELAY", 2);
    if (sDelay < 0) {
        sDelay = 0;
    }
    if (sDelay > 30) {
        sDelay = 30;
    }
    const char* mode = getenv("MK64_NET_MODE");
    sMode = (mode && strcmp(mode, "vs") == 0) ? VERSUS : GRAND_PRIX;
    sCup = (int) EnvInt("MK64_NET_CUP", 0);
    sCourse = (int) EnvInt("MK64_NET_COURSE", 0);
    sCC = (int) EnvInt("MK64_NET_CC", CC_150);
    sChars[sLocalPlayer] = (int) EnvInt("MK64_NET_CHAR", sIsHost ? MARIO : LUIGI) % 8;
    sSeed = (unsigned int) EnvInt("MK64_NET_SEED", (long) (time(NULL) & 0xFFFF)) & 0xFFFF;
    sMaxTicks = (unsigned long) EnvInt("MK64_NET_FRAMES", 0);
    {
        const char* view = getenv("MK64_NET_VIEW");
        sFullView = !(view && strcmp(view, "split") == 0);
        sSingleView = !(view && strcmp(view, "double") == 0);
    }
    sRollback = EnvInt("MK64_NET_ROLLBACK", 1) != 0 && Rollback_Supported();
    sSession = EnvInt("MK64_NET_SESSION", 0) != 0;
    sMenuDelayCeiling = (int) EnvInt("MK64_NET_MENU_MAX_DELAY", 6);
    sRbWindow = (uint32_t) EnvInt("MK64_NET_ROLLBACK_WINDOW", 8);
    sRbFrom = EnvInt("MK64_NET_ROLLBACK_FROM", 60);
    if (sRollback && !SimHeap_Enable()) {
        sRollback = 0;
    }
    {
        const char* v = getenv("MK64_NET_VOICE");
        sVoiceMode = !v ? 0 : strcmp(v, "ptt") == 0 ? 2 : atoi(v) != 0;
        sVoiceTest = EnvInt("MK64_NET_VOICE_TEST", 0) != 0;
    }
    sAdaptive = EnvInt("MK64_NET_ADAPTIVE", 1) != 0;
    sMaxDelay = (int) EnvInt("MK64_NET_MAX_DELAY", sRollback ? 3 : 6); /* lockstep needs more */
    sSimLatency = (int) EnvInt("MK64_NET_SIM_LATENCY", 0);
    sSimJitter = (int) EnvInt("MK64_NET_SIM_JITTER", 0);
    sSimLoss = (int) EnvInt("MK64_NET_SIM_LOSS", 0);
    sSimSpike = (int) EnvInt("MK64_NET_SIM_SPIKE", 0);
    sSimSpikeEvery = (int) EnvInt("MK64_NET_SIM_SPIKE_EVERY", 10);
    sSimSpikeLen = (int) EnvInt("MK64_NET_SIM_SPIKE_LEN", 1000);
    sSimStart = NowMs();
    {
        char synctestPath[1024];
        Rollback_SyncTestInit(DocumentsPath(getenv("MK64_SYNCTEST_LOG"), synctestPath, sizeof(synctestPath)));
    }
    sBot = (int) EnvInt("MK64_NET_BOT", 0);
    sBotDrift = (int) EnvInt("MK64_NET_BOT_DRIFT", 0);
    sBotQuitAt = EnvInt("MK64_NET_BOT_QUIT", 0);
    sBotQuitFinish = EnvInt("MK64_NET_BOT_QUIT_FINISH", 0) != 0;
    sBotExplore = EnvInt("MK64_NET_BOT_EXPLORE", 0) != 0;
    sExploreRng = (unsigned int) EnvInt("MK64_NET_BOT_EXPLORE", 1) * 2654435761u + (sIsHost ? 1u : 7u);
    LoadReplay(getenv("MK64_NET_REPLAY"));
    sBotState = (unsigned int) sBot * 2654435761u + 1;
    if (playback && *playback) {
        LoadPlayback(playback);
        return;
    }


    if (sUiSock >= 0) {
        /* Online screen: the room-code lookup already found the peer on this socket */
        sSock = sUiSock;
        sUiSock = -1;
        fcntl(sSock, F_SETFL, fcntl(sSock, F_GETFL, 0) | O_NONBLOCK);
        sPeer = sUiPeer;
        sHavePeer = 1;
        sState = NET_HANDSHAKE;
        Log("%s; waiting for the %s", sIsHost ? "hosting" : "joining", sIsHost ? "guest" : "host");
        return;
    }
    sSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sSock < 0) {
        Log("socket() failed: %s", strerror(errno));
        return;
    }
    fcntl(sSock, F_SETFL, fcntl(sSock, F_GETFL, 0) | O_NONBLOCK);
    struct sockaddr_in bindAddr;
    const char* bind_s = getenv("MK64_NET_BIND");
    if (!bind_s || !*bind_s) {
        bind_s = sIsHost ? "0.0.0.0:7101" : "0.0.0.0:0";
    }
    if (!ParseAddr(bind_s, &bindAddr) || bind(sSock, (struct sockaddr*) &bindAddr, sizeof(bindAddr)) != 0) {
        Log("cannot bind %s: %s", bind_s, strerror(errno));
        close(sSock);
        sSock = -1;
        return;
    }
    const char* server = getenv("MK64_NET_SERVER");
    if (server && *server) {
        /* Room code: find the other player through the n64net rendezvous
         * server, on this same socket (the NAT mapping it opens is the one
         * the race uses). Blocks until the other player is there. */
        N64NetRendezvousConfig rc;
        N64NetRendezvousResult rr;
        memset(&rc, 0, sizeof(rc));
        memset(&rr, 0, sizeof(rr));
        rc.server = server;
        rc.game = "mk64-us+" KARTNET_BUILD_ID;
        rc.room = getenv("MK64_NET_ROOM");
        rc.password = getenv("MK64_NET_PASSWORD");
        rc.is_host = sIsHost;
        rc.force_relay = EnvInt("MK64_NET_RELAY", 0) != 0;
        rc.wait_secs = (int) EnvInt("MK64_NET_WAIT_SECS", 600);
        rc.log = NetLogCb;
        rc.yield = PumpWhileWaiting;
        if (sIsHost) {
            SetWait(0, "Opening a room...");
        } else {
            SetWait(0, "Joining room %s...", (rc.room && *rc.room) ? rc.room : "");
        }
        Log("%s room %s on %s (game %s)", sIsHost ? "hosting" : "joining", (rc.room && *rc.room) ? rc.room : "(random)",
            server, rc.game);
        if (n64net_rendezvous_connect(sSock, &rc, &rr) != 0) {
            Log("room code failed: %s", rr.error);
            if (!strstr(sWaitMsg, "version") && !strstr(sWaitMsg, "password")) {
                SetWait(8, "Could not connect to the other player\n%s", rr.error);
            }
            close(sSock);
            sSock = -1;
            sState = NET_DONE;
            return;
        }
        sPeer = rr.peer;
        sHavePeer = 1;
        snprintf(sRoomCode, sizeof(sRoomCode), "%s", rr.room);
        Log("room %s: connected to %s%s", rr.room, AddrStr(&rr.peer), rr.relayed ? " (relayed by the server)" : "");
    } else if (!sIsHost) {
        const char* peer = getenv("MK64_NET_PEER");
        if (!peer || !ParseAddr(peer, &sPeer)) {
            Log("guest needs MK64_NET_PEER=host:port");
            close(sSock);
            sSock = -1;
            return;
        }
        sHavePeer = 1;
    }
    sState = NET_HANDSHAKE;
    if (sLog) {
        fprintf(sLog, "# KartNet %s, bind %s%s%s\n", sIsHost ? "host (player 1)" : "guest (player 2)", bind_s,
                sIsHost ? "" : ", peer ", sIsHost ? "" : getenv("MK64_NET_PEER"));
    }
    Log("%s; waiting for the %s", sIsHost ? "hosting" : "joining", sIsHost ? "guest" : "host");
}


/* ------------------------------------------------------------------------ */
/* In-game Online screen (port/ui/OnlineWindow.cpp)                          */
/*
 * Host / Join start a session from inside the running game. The room-code
 * lookup (n64net_rendezvous_connect, which blocks until the other player is
 * there) runs on a background thread, so the game stays playable while the
 * host waits, and Cancel just drops the result. Once found, KartNet starts as
 * if from the MK64_NET_* settings (Init) on that socket, shakes hands, and
 * both games reset to the main menu with the host's seed (CM_SessionReset):
 * the session runs from there. Leaving, or losing the other player, ends it
 * and the game goes back to its own menus, offline.
 */
enum { UI_IDLE, UI_FINDING, UI_CONNECTING, UI_SESSION };
static volatile int sUiPhase = UI_IDLE;
static int sFromUi = 0;             /* the current session was started from the Online screen */
static char sUiStatus[200];
static char sUiRoomShown[8];
static int sUiBlocking = 0;          /* the Online screen is open: the game gets no input */

typedef struct {
    int sock;
    int host;
    char server[128], room[8], password[128];
    N64NetRendezvousResult res;
    volatile int done, ok, cancelled;
} UiLookup;
static UiLookup* sUiLookup = NULL;
static pthread_mutex_t sUiLock = PTHREAD_MUTEX_INITIALIZER;

static void UiSetStatus(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void UiSetStatus(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&sUiLock);
    vsnprintf(sUiStatus, sizeof(sUiStatus), fmt, ap);
    pthread_mutex_unlock(&sUiLock);
    va_end(ap);
}

/* Rendezvous log lines while the lookup thread runs: to the screen, not the overlay. */
static int UiLogHook(const char* line) {
    if (sUiPhase != UI_FINDING) {
        return 0;
    }
    char room[8];
    if (sscanf(line, "n64net: room code %7s - waiting", room) == 1) {
        pthread_mutex_lock(&sUiLock);
        snprintf(sUiRoomShown, sizeof(sUiRoomShown), "%s", room);
        pthread_mutex_unlock(&sUiLock);
        UiSetStatus("Room %s is open\nWaiting for your friend to join...", room);
    } else if (strstr(line, "different build")) {
        UiSetStatus("Your friend has a different version of the game\nBoth of you need the same build");
    } else if (strstr(line, "refused the password")) {
        UiSetStatus("The room server refused the password");
    }
    return 1;
}

static void UiLookupLog(const char* fmt, ...) {
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (sLog) {
        fprintf(sLog, "n64net: %s\n", buf);
    }
    fprintf(stderr, "KartNet: n64net: %s\n", buf);
    UiLogHook(buf);
}

static void* UiLookupThread(void* arg) {
    UiLookup* L = (UiLookup*) arg;
    N64NetRendezvousConfig rc;
    memset(&rc, 0, sizeof(rc));
    rc.server = L->server;
    rc.game = "mk64-us+" KARTNET_BUILD_ID;
    rc.room = L->room[0] ? L->room : NULL;
    rc.password = L->password[0] ? L->password : NULL;
    rc.is_host = L->host;
    rc.wait_secs = L->host ? 1800 : 30;
    rc.log = UiLookupLog;
    L->ok = n64net_rendezvous_connect(L->sock, &rc, &L->res) == 0;
    pthread_mutex_lock(&sUiLock);
    const int cancelled = L->cancelled;
    L->done = 1;
    pthread_mutex_unlock(&sUiLock);
    if (cancelled) {
        close(L->sock); /* nobody wants the result any more */
        free(L);
    }
    return NULL;
}

int KartNet_UiPhase(void) {
    return sUiPhase;
}

void KartNet_UiStatus(char* out, int outSize) {
    pthread_mutex_lock(&sUiLock);
    snprintf(out, (size_t) outSize, "%s", sUiStatus);
    pthread_mutex_unlock(&sUiLock);
}

void KartNet_UiSetBlocking(int blocking) {
    sUiBlocking = blocking;
}

void KartNet_UiSetVoice(int mode) {
    setenv("MK64_NET_VOICE", mode == 2 ? "ptt" : mode == 1 ? "1" : "0", 1);
}

/* host: 1 = open a room (room may be "" for a random code), 0 = join `room`. */
int KartNet_UiStart(int host, const char* server, const char* room, const char* password) {
    if (sUiPhase != UI_IDLE || sState != NET_OFF || sPlayback) {
        return 0;
    }
    if (!server || !*server) {
        UiSetStatus("Set the room server first (Online settings)");
        return 0;
    }
    if (!host && (!room || strlen(room) < 4)) {
        UiSetStatus("Enter your friend's room code");
        return 0;
    }
    struct sockaddr_in any;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0 || !ParseAddr("0.0.0.0:0", &any) || bind(sock, (struct sockaddr*) &any, sizeof(any)) != 0) {
        UiSetStatus("Could not open a network socket");
        if (sock >= 0) {
            close(sock);
        }
        return 0;
    }
    UiLookup* L = (UiLookup*) calloc(1, sizeof(UiLookup));
    L->sock = sock;
    L->host = host;
    snprintf(L->server, sizeof(L->server), "%s", server);
    snprintf(L->room, sizeof(L->room), "%s", room ? room : "");
    snprintf(L->password, sizeof(L->password), "%s", password ? password : "");
    pthread_mutex_lock(&sUiLock);
    sUiRoomShown[0] = '\0';
    pthread_mutex_unlock(&sUiLock);
    UiSetStatus(host ? "Opening a room..." : "Looking for room %s...", L->room);
    sUiLookup = L;
    sUiPhase = UI_FINDING;
    pthread_t t;
    if (pthread_create(&t, NULL, UiLookupThread, L) != 0) {
        close(sock);
        free(L);
        sUiLookup = NULL;
        sUiPhase = UI_IDLE;
        UiSetStatus("Could not start the connection");
        return 0;
    }
    pthread_detach(t);
    sFromUi = 1;
    setenv("MK64_NET_HOST", host ? "1" : "0", 1);
    return 1;
}

void KartNet_UiRoom(char* out, int outSize) {
    pthread_mutex_lock(&sUiLock);
    snprintf(out, (size_t) outSize, "%s", sUiPhase == UI_SESSION ? sRoomCode : sUiRoomShown);
    pthread_mutex_unlock(&sUiLock);
}

static void ResetNetState(void);

/* Cancel a lookup, or leave a running session. */
void KartNet_UiStop(void) {
    if (sUiPhase == UI_FINDING) {
        pthread_mutex_lock(&sUiLock);
        sUiLookup->cancelled = 1;
        const int done = sUiLookup->done;
        pthread_mutex_unlock(&sUiLock);
        if (done) { /* finished in the meantime: the thread won't free it */
            close(sUiLookup->sock);
            free(sUiLookup);
        }
        sUiLookup = NULL;
        sUiPhase = UI_IDLE;
        sFromUi = 0;
        UiSetStatus("Cancelled");
        return;
    }
    if (sFromUi && (sState == NET_HANDSHAKE || sState == NET_RUNNING)) {
        u8 buf[8];
        size_t n = Header(buf, PKT_BYE);
        for (int i = 0; i < 3; i++) {
            SendNow(buf, n);
        }
        Log("left the session");
        SetWait(4, "You left the session");
        sState = NET_DONE;
    }
}

/* MK64_NET_UITEST=host|join (tests): press Host / Join on the Online screen at
 * frame MK64_NET_UITEST_AT, with MK64_NET_UITEST_SERVER/_ROOM/_PASSWORD.
 * MK64_NET_UITEST_ROUNDS=n: after a session ends, wait MK64_NET_UITEST_AT
 * frames again and start the next one; MK64_NET_UITEST_LEAVE=t: press Leave
 * t ticks into each session (the host, say). */
static void UiTestHook(void) {
    static unsigned long frames = 0;
    static int rounds = 0;
    const char* what = getenv("MK64_NET_UITEST");
    if (!what || !*what) {
        return;
    }
    const long leaveAt = EnvInt("MK64_NET_UITEST_LEAVE", 0);
    if (leaveAt > 0 && sState == NET_RUNNING && sRaceRequested && (long) sTick == leaveAt) {
        fprintf(stderr, "KartNet: UI test: leave at tick %lu\n", sTick);
        KartNet_UiStop();
        return;
    }
    if (sUiPhase != UI_IDLE || sState != NET_OFF) {
        frames = 0;
        return;
    }
    if (rounds >= EnvInt("MK64_NET_UITEST_ROUNDS", 1)) {
        return;
    }
    if (++frames < (unsigned long) EnvInt("MK64_NET_UITEST_AT", 100)) {
        return;
    }
    rounds++;
    const char* server = getenv("MK64_NET_UITEST_SERVER");
    const char* room = getenv("MK64_NET_UITEST_ROOM");
    const char* pw = getenv("MK64_NET_UITEST_PASSWORD");
    fprintf(stderr, "KartNet: UI test: %s at frame %lu\n", what, frames);
    KartNet_UiStart(strcmp(what, "host") == 0, server, room ? room : "", pw ? pw : "");
}

/* Every frame (BeginFrame): pick up a finished lookup, end a finished session. */
static void UiUpdate(void) {
    UiTestHook();
    int lookupDone = 0;
    if (sUiPhase == UI_FINDING && sUiLookup) {
        pthread_mutex_lock(&sUiLock);
        lookupDone = sUiLookup->done;
        pthread_mutex_unlock(&sUiLock);
    }
    if (lookupDone) {
        UiLookup* L = sUiLookup;
        sUiLookup = NULL;
        if (!L->ok) {
            if (!strstr(sUiStatus, "version") && !strstr(sUiStatus, "password")) {
                UiSetStatus("Could not connect: %s", L->res.error[0] ? L->res.error : "no answer");
            }
            close(L->sock);
            free(L);
            sUiPhase = UI_IDLE;
            sFromUi = 0;
            return;
        }
        sUiSock = L->sock;
        sUiPeer = L->res.peer;
        snprintf(sRoomCode, sizeof(sRoomCode), "%s", L->res.room);
        free(L);
        /* the session's settings (Init reads them like the test scripts' ones) */
        setenv("MK64_NET", "1", 1);
        setenv("MK64_NET_SESSION", "1", 1);
#if defined(KARTNET_IOS)
        setenv("MK64_NET_LOG", "kartnet.log", 1); /* Documents/kartnet.log (+ .rec) */
#endif
        sUiPhase = UI_CONNECTING;
        UiSetStatus("Connecting to your friend...");
        ResetNetState();
        Init();
        if (sState == NET_HANDSHAKE) {
            Log("room %s: connected to %s (from the Online screen)", sRoomCode, AddrStr(&sPeer));
        } else {
            UiSetStatus("Could not start the session");
            sUiPhase = UI_IDLE;
            sFromUi = 0;
        }
        return;
    }
    if (sFromUi && sState == NET_DONE) {
        /* session over: back to the game's own menus, offline */
        char reason[200];
        snprintf(reason, sizeof(reason), "%s", sWaitMsg[0] ? sWaitMsg : "The session ended");
        for (char* c = reason; *c; c++) {
            if (*c == '\n') {
                *c = ' ';
            }
        }
#ifdef KARTNET_VOICE
        if (sVoiceStarted) {
            n64net_voice_stop();
            sVoiceStarted = 0;
        }
#endif
        Log("session over: %s", reason);
        ResetNetState();
        sUiPhase = UI_IDLE;
        sFromUi = 0;
        UiSetStatus("%s", reason);
        CM_RequestReset();
    }
}

/* Back to "no netplay": what a new session must not inherit. */
static void ResetNetState(void) {
    if (sNet) {
        n64net_input_destroy(sNet);
        sNet = NULL;
    }
    if (sSock >= 0) {
        close(sSock);
        sSock = -1;
    }
    if (sLog) {
        fclose(sLog);
        sLog = NULL;
    }
    if (sRec) {
        fclose(sRec);
        sRec = NULL;
    }
    sState = NET_OFF;
    sHavePeer = 0;
    sRaceRequested = 0;
    sTick = 0;
    sMaxTicks = 0;
    sSyncOk = sSyncBad = sStalls = 0;
    sStallMs = 0;
    sLastHelloMs = 0;
    sPeerPaused = 0;
    sInjectStart = 0;
    sSimTick = 0;
    sLastRecorded = -1;
    sRbCount = sRbFrames = sRbMaxDepth = sHolds = 0;
    sDesyncLogged = 0;
    gKartNetHoldFrame = 0;
    sSession = 0;
    sRbActive = 0;
    sRaceStartTick = -1;
    sLastCeiling = -1;
    for (int i = 0; i < DETAIL_RING; i++) {
        sDetailTick[i] = -1;
    }
    sWaitMsg[0] = '\0';
    sWaitUntil = 0;
}

void KartNet_SetPaused(int paused) {
    if (sState == NET_HANDSHAKE || sState == NET_RUNNING) {
        Log("local app %s", paused ? "paused (background)" : "resumed");
        SendPause(paused);
        if (sNet) {
            n64net_input_set_away(sNet, paused);
        }
        if (!paused && sState == NET_RUNNING && sRaceRequested) {
            /* Come back to the game's own pause menu (in sync on both sides,
             * since the press travels as a normal input) instead of straight
             * into the race: the player resumes with Start when ready. */
            sInjectStart = 1;
        }
    }
}

int KartNet_IsActive(void) {
    if (!sInitDone) {
        Init();
    }
    return sState != NET_OFF;
}

/* ------------------------------------------------------------------------ */
/* Frame hooks                                                               */

static void GateTransition(void);
void apply_gamestate_change(void); /* main.c */
/* Session mode, at the top of a frame, before main.c applies a pending game
 * state change: make it safe (GateTransition), then note races starting and
 * ending. 2-player races use the left/right split: each device draws only its
 * own player full screen (KartNet_RenderLocalView), which is built on it. */
/* Rollback re-simulates race logic without drawing. That is exact while
 * racing (SyncTest: drawing changes no logic state then), but the pause menu,
 * the quit fade, the results screens and game-state changes run logic in the
 * drawing code or outside the re-simulated part. So rollback runs only while
 * none of them is active; going into one first waits for confirmed inputs
 * (GateTransition). A pause can't come from a misprediction: Start presses
 * take effect ROLLBACK_WINDOW + 1 ticks late (ApplyNetPads). */
static int RaceRollbackSafe(void) {
    return gGamestate == RACING && gGamestateNext == gGamestate && !gIsGamePaused && !gIsInQuitToMenuTransition &&
           (gRaceState == RACE_STAGING || gRaceState == RACE_IN_PROGRESS ||
            /* someone has finished, the others still race (SyncTest-checked) */
            gRaceState == RACE_CALCULATE_RANKS);
}

static void SessionBeforeGamestate(void) {
    if (sRbActive && !RaceRollbackSafe()) {
        GateTransition();
        if (sState != NET_RUNNING) {
            return;
        }
    }
    if (gGamestateNext == gGamestate || !sSession) {
        return;
    }
    Log("tick %lu: game state %d -> %d (menu %d, players %d, mode %d)", sTick, gGamestate, gGamestateNext,
        gMenuSelection, gPlayerCount, gModeSelection);
    if (gGamestateNext == RACING) {
        if (gScreenModeSelection == SCREEN_MODE_2P_SPLITSCREEN_HORIZONTAL) {
            gScreenModeSelection = SCREEN_MODE_2P_SPLITSCREEN_VERTICAL;
        }
        sRaceStartTick = (long) sTick;
        Log("race starts at tick %lu (players %d, mode %d, cc %d, cup cursor %d, track '%s', chars %d %d)", sTick,
            gPlayerCount, gModeSelection, gCCSelection, (int) GetCupCursorPosition(), CM_GetTrackName(),
            gCharacterSelections[0], gCharacterSelections[1]);
    } else if (sRaceStartTick >= 0) {
        Log("race over at tick %lu (to game state %d)", sTick, gGamestateNext);
        sRaceStartTick = -1;
    }
}

void KartNet_BeginFrame(void) {
    if (!sInitDone) {
        Init();
    }
    UiUpdate();
    if (sState == NET_OFF || sState == NET_DONE) {
        VoiceUpdate(); /* closes the microphone after a disconnect */
        return;
    }
    Poll();
    sBootFrames++;
    /* Both sides boot identically (no input) to START_BOOT_FRAME and wait there
     * for the handshake, so the race starts from the same state on both: a side
     * that kept running into the attract demo while waiting would carry
     * leftover state into the race and desync. */
    const double handshakeStart = NowMs();
    while (sState == NET_HANDSHAKE) {
        double now = NowMs();
        if (sFromUi && now - handshakeStart > 30000) {
            Log("no answer from the other player; giving up");
            SetWait(6, "Your friend's game didn't answer");
            sState = NET_DONE;
            break;
        }
        if (now - sLastHelloMs > 100) {
            sLastHelloMs = now;
            if (sIsHost) {
                if (sHavePeer) {
                    SendConfig();
                }
            } else {
                SendHello();
            }
        }
        if (sIsHost && !sHavePeer) {
            if (sRoomCode[0] == '\0') {
                SetWait(0, "Waiting for the other player...");
            }
        } else {
            SetWait(0, "Connecting to the other player...");
        }
        if (sBootFrames < START_BOOT_FRAME) {
            return; /* keep booting meanwhile */
        }
        ShowWaiting();
        usleep(5000);
        Poll();
    }
    if (sState == NET_RUNNING && !sRaceRequested) {
        ClearWait();
    }
    if (sState != NET_RUNNING) {
        return;
    }
    if (!sRaceRequested && sBootFrames >= START_BOOT_FRAME && sSession) {
        sRaceRequested = 1; /* inputs flow from here on; the game boots on into its menus */
        if (sLog) {
            fprintf(sLog, "# session: seed=0x%04x delay=%d\n", sSeed, sDelay);
        }
        if (sRec) {
            fprintf(sRec, "# session: seed=0x%04x delay=%d rollback=%d window=%u\n", sSeed, sDelay, sRollback, sRbWindow);
            fprintf(sRec, "# %s, build %s\n", sIsHost ? "host (player 1)" : "guest (player 2)", KARTNET_BUILD_ID);
        }
        Log("session started (boot frame %lu): menus in lockstep, races with %s", sBootFrames,
            sRollback ? "rollback" : "lockstep");
        if (sFromUi) {
            /* started from the Online screen: the two games were in different
             * places; both go to the main menu from the same state now */
            CM_SessionReset((u16) sSeed);
            sUiPhase = UI_SESSION;
            UiSetStatus("Connected: room %s", sRoomCode);
        }
    }
    if (sSession && sRaceRequested) {
        /* The menus load the debug-mode setting from each device's own
         * settings, and it changes the game (e.g. Z starts a race): off. */
        gEnableDebugMode = 0;
    }
    if (sRaceRequested && (sNet || sPlayback)) {
        SessionBeforeGamestate(); /* (playback: only notes races starting and ending) */
    }
    if (!sRaceRequested && sBootFrames >= START_BOOT_FRAME) {
        sRaceRequested = 1;
        RaceStartParams p;
        p.mode = sMode;
        p.playerCount = 2;
        p.cup = sCup;
        p.course = sCourse;
        p.cc = sCC;
        p.chars[0] = sChars[0];
        p.chars[1] = sChars[1];
        p.rng = sSeed;
        RaceStart_Request(&p);
        if (sLog) {
            fprintf(sLog, "# race: seed=0x%04x mode=%d cup=%d course=%d cc=%d chars=%d,%d delay=%d\n", sSeed, sMode,
                    sCup, sCourse, sCC, sChars[0], sChars[1], sDelay);
        }
        if (sRec) {
            fprintf(sRec, "# race: seed=0x%04x mode=%d cup=%d course=%d cc=%d chars=%d,%d delay=%d\n", sSeed, sMode,
                    sCup, sCourse, sCC, sChars[0], sChars[1], sDelay);
            fprintf(sRec, "# rollback=%d window=%u\n", sRollback, sRbWindow);
            fprintf(sRec, "# %s, build %s\n", sIsHost ? "host (player 1)" : "guest (player 2)", KARTNET_BUILD_ID);
        }
        Log("race requested (boot frame %lu)", sBootFrames);
    }
    if (sRaceRequested && gGamestate == RACING && gGamestateNext == RACING) {
        Rollback_SyncTestBeginFrame((long) sTick);
    }
}

/* MK64_NET_REPLAY=path: drive the local player from a ReplayCheck recording
 * (lines "frame buttons(hex) x y"), e.g. a human race, for realistic tests. */
static NetPad* sReplay = NULL;
static unsigned long sReplayLen = 0;
static void LoadReplay(const char* path) {
    FILE* in = path ? fopen(path, "r") : NULL;
    if (!in) {
        return;
    }
    unsigned long cap = 8192, f;
    unsigned int b;
    int x, y;
    char line[128];
    sReplay = (NetPad*) calloc(cap, sizeof(NetPad));
    while (fgets(line, sizeof(line), in)) {
        if (sscanf(line, "%lu %x %d %d", &f, &b, &x, &y) != 4) {
            continue;
        }
        while (f >= cap) {
            sReplay = (NetPad*) realloc(sReplay, cap * 2 * sizeof(NetPad));
            memset(sReplay + cap, 0, cap * sizeof(NetPad));
            cap *= 2;
        }
        sReplay[f].button = (u16) b;
        sReplay[f].x = (s8) x;
        sReplay[f].y = (s8) y;
        if (f + 1 > sReplayLen) {
            sReplayLen = f + 1;
        }
    }
    fclose(in);
    Log("# local input: replay %s (%lu frames)", path, sReplayLen);
}

/* MK64_NET_BOT_EXPLORE=seed (session tests): wander through the menus with
 * random presses (other player counts, modes incl. time trials and battle,
 * cc, characters, cups, tracks), drive in races, and now and then pause and
 * pick a random pause option (retry, course/driver change, quit). Stays out of
 * the options and data menus (erase-save). */
static long sExplorePauseAt = -1;
static unsigned int ExploreRand(void) {
    unsigned int x = sExploreRng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    sExploreRng = x;
    return x;
}
static NetPad DriveInput(unsigned long f);
static NetPad ExploreInput(unsigned long f) {
    NetPad pad = { 0, 0, 0 };
    if (gGamestate != RACING) {
        sExplorePauseAt = -1;
        if (gMenuSelection == OPTIONS_MENU || gMenuSelection == DATA_MENU) {
            if (f % 15 == 0) {
                pad.button = B_BUTTON;
            }
            return pad;
        }
        if (sIsHost && gMenuSelection == MAIN_MENU && gMainMenuSelection == MAIN_MENU_MODE_SELECT && gPlayerCount >= 1) {
            /* the host steers the game mode in rotation (for 2 players:
             * grand prix, versus, battle), so every mode gets played */
            if (f % 12 == 0) {
                const int col = gGameModeMenuColumn[gPlayerCount - 1];
                const int target = sExploreModePicks % 3;
                pad.button = col < target ? D_JPAD : col > target ? U_JPAD : A_BUTTON;
                if (pad.button == A_BUTTON) {
                    sExploreModePicks++; /* next time, the next mode */
                }
            }
            return pad;
        }
        if (f % 12 == 0) {
            unsigned int r = ExploreRand() % 100;
            pad.button = r < 35 ? A_BUTTON
                         : r < 52 ? R_JPAD
                         : r < 69 ? L_JPAD
                         : r < 84 ? D_JPAD
                         : r < 97 ? U_JPAD
                                  : B_BUTTON;
        }
        return pad;
    }
    if (sRaceStartTick < 0) {
        return pad;
    }
    long r = (long) sTick - sRaceStartTick;
    if (sExplorePauseAt < 0) {
        /* the host pauses every race, the guest in about a third */
        sExplorePauseAt = (sIsHost || ExploreRand() % 3 == 0) ? 200 + (long) (ExploreRand() % 900) : 1L << 30;
    }
    if (r >= sExplorePauseAt) {
        long k = r - sExplorePauseAt;
        /* Start takes effect ROLLBACK_WINDOW + 1 ticks late; once the pause
         * menu is gone again (Continue), drive on and pause again later */
        if (k > 60 && !gIsGamePaused) {
            sExplorePauseAt = r + 150 + (long) (ExploreRand() % 600);
            return DriveInput(f);
        }
        if (k == 0) {
            pad.button = START_BUTTON;
        } else if (k % 15 == 0) {
            unsigned int c = ExploreRand() % 100;
            pad.button = c < 35 ? D_JPAD : c < 50 ? U_JPAD : A_BUTTON;
        }
        return pad;
    }
    return DriveInput(f);
}

static NetPad BotInput(unsigned long f) {
    NetPad pad = { 0, 0, 0 };
    if (sSession && sBotExplore) {
        return ExploreInput(f);
    }
    if (sSession && gGamestate != RACING) {
        /* Menus: press A now and then (on through every screen); the host
         * first picks 2 players on the main menu. */
        if (sIsHost && gMenuSelection == MAIN_MENU && gPlayerCount == 1) {
            if (f % 30 == 15) {
                pad.button = R_JPAD;
            }
        } else if (f % 30 == 0) {
            pad.button = A_BUTTON;
        }
        return pad;
    }
    if (sSession && sIsHost && sRaceStartTick >= 0 && sBotQuitFinish) {
        /* session test with a person: quit 150 ticks (5 s) after another
         * human player finished (the bot itself never finishes) */
        if (sBotQuitRace != sRaceStartTick) {
            sBotQuitRace = sRaceStartTick;
            sBotQuitTick = -1;
        }
        if (sBotQuitTick < 0) {
            for (int p = 0; p < sSessionPlayers; p++) {
                if (p != sLocalPlayer && gPlayers[p].lapCount >= 3) {
                    sBotQuitTick = (long) sTick + 150;
                    Log("tick %lu: player %d finished; the bot quits in 5 s", sTick, p + 1);
                }
            }
        }
    }
    if (sSession && sIsHost && sRaceStartTick >= 0 && (sBotQuitAt > 0 || sBotQuitTick >= 0)) {
        /* session test: leave the race through the pause menu (Start, then
         * down and A now and then), back to the menus for the next race */
        long r = (long) sTick - sRaceStartTick;
        const long at = sBotQuitTick >= 0 ? sBotQuitTick - sRaceStartTick : sBotQuitAt;
        if (r >= at && r < at + 200) {
            long k = r - at;
            pad.button = (k == 0) ? START_BUTTON : (k % 40 == 20) ? D_JPAD : (k % 40 == 0) ? A_BUTTON : 0;
            return pad;
        }
    }
    return DriveInput(f);
}

static NetPad DriveInput(unsigned long f) {
    NetPad pad = { 0, 0, 0 };
    if (f % 20 == 0) {
        unsigned int x = sBotState;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        sBotState = x;
        sBotTarget = (int) (x % 161) - 80;
    }
    sBotSteer += (sBotTarget - sBotSteer) / 4;
    pad.button = A_BUTTON;
    if (f % 150 == 75) {
        pad.button |= Z_TRIG;
    }
    if (sBotDrift && f % 240 >= 120 && f % 240 < 200) {
        /* hop and hold R through a hard turn: drift (sparks) */
        pad.button |= R_TRIG;
        int dir = (f / 240) % 2 ? 1 : -1;
        pad.x = (s8) (dir * (((f / 8) % 2) ? 80 : 20)); /* wiggle: mini-turbo sparks */
        return pad;
    }
    pad.x = (s8) sBotSteer;
    return pad;
}

static unsigned long long StateHash(void);

static void NetSendCb(void* user, const uint8_t* data, uint32_t size) {
    (void) user;
    SendRaw(data, size);
}

static void NetLogCb(const char* fmt, ...) {
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Log("n64net: %s", buf);
    char room[8];
    if (UiLogHook(buf)) {
        return;
    }
    if (sscanf(buf, "n64net: room code %7s - waiting", room) == 1) {
        snprintf(sRoomCode, sizeof(sRoomCode), "%s", room);
        SetWait(0, "Room %s\nWaiting for the other player to join...", sRoomCode);
    } else if (strstr(buf, "different build")) {
        SetWait(10, "The other player has a different version\nBoth players need the same build");
    } else if (strstr(buf, "refused the password")) {
        SetWait(10, "The room server refused the password");
    }
}

/* The exchanged (or, in playback, recorded) input of player p for tick. */
static void GetPad(int p, long tick, n64net_pad* out) {
    if (sPlayback) {
        n64net_pad none = { 0, 0, 0 };
        *out = none;
        if (tick >= 0 && (unsigned long) tick < sPlayLen) {
            out->buttons = sPlayButtons[tick * 2 + p];
            out->stick_x = sPlayStick[tick * 4 + p * 2];
            out->stick_y = sPlayStick[tick * 4 + p * 2 + 1];
        }
        return;
    }
    n64net_input_get(sNet, p, (uint32_t) tick, out);
}

static void ApplyNetPads(unsigned long tick) {
    memset(gControllerPads, 0, sizeof(OSContPad) * 4);
    /* In races Start (pause) takes effect ROLLBACK_WINDOW + 1 ticks late: by
     * then every device has it confirmed, so no pause is ever predicted. */
    const long shift = (long) sRbWindow + 1;
    const long raceStart = sSession ? sRaceStartTick : 0;
    const int shiftStart = (sStartShift < 0 ? sRollback : sStartShift) && gGamestate == RACING && raceStart >= 0;
    for (int p = 0; p < sSessionPlayers; p++) {
        n64net_pad pad;
        GetPad(p, (long) tick, &pad);
        if (shiftStart) {
            pad.buttons &= (uint16_t) ~START_BUTTON;
            if ((long) tick - shift >= raceStart) {
                n64net_pad old;
                GetPad(p, (long) tick - shift, &old);
                pad.buttons |= old.buttons & START_BUTTON;
            }
        }
        gControllerPads[p].button = pad.buttons;
        gControllerPads[p].stick_x = pad.stick_x;
        gControllerPads[p].stick_y = pad.stick_y;
    }
}

static Player sHashCopy[NUM_PLAYERS];
static void RecordTickHash(unsigned long tick) {
    sTickHash[tick % RB_HASHES] = StateHash();
    sTickHashTick[tick % RB_HASHES] = (long) tick;
    if (tick % HASH_EVERY == 0) {
        /* StateHash left the masked kart copy in sHashCopy */
        const int d = (int) ((tick / HASH_EVERY) % DETAIL_RING);
        memcpy(sDetail[d], sHashCopy, sizeof(sHashCopy));
        sDetailTick[d] = (long) tick;
        sDetailSeed[d] = gRandomSeed16;
        sDetailState[d] = gGamestate;
    }
}

/* A desync at checkpoint `tick`: log a hash per kart and write the karts. */
static void WriteDesyncDetail(unsigned long tick) {
    const int d = (int) ((tick / HASH_EVERY) % DETAIL_RING);
    if (sDetailTick[d] != (long) tick) {
        Log("desync detail: checkpoint %lu no longer kept", tick);
        return;
    }
    char line[400];
    int n = snprintf(line, sizeof(line), "desync detail tick %lu: rng %04x state %d karts", tick, sDetailSeed[d],
                     (int) sDetailState[d]);
    for (int i = 0; i < NUM_PLAYERS && n < (int) sizeof(line); i++) {
        unsigned long long h = Fnv1a(1469598103934665603ull, &sDetail[d][i], sizeof(Player));
        n += snprintf(line + n, sizeof(line) - n, " %08x", (unsigned) (h ^ (h >> 32)));
    }
    Log("%s", line);
    if (sRecBase[0]) {
        char path[1100];
        snprintf(path, sizeof(path), "%s.desync.bin", sRecBase);
        FILE* f = fopen(path, "wb");
        if (f) {
            fwrite(sDetail[d], sizeof(Player), NUM_PLAYERS, f);
            fclose(f);
        }
    }
}

static void RbSave(void* user, uint32_t slot) {
    (void) user;
    Rollback_SaveSlot((int) slot);
    sSlotTick[slot % RB_SLOTS] = sSimTick;
}

static void RbLoad(void* user, uint32_t slot) {
    (void) user;
    Rollback_LoadSlot((int) slot);
    sSimTick = sSlotTick[slot % RB_SLOTS];
}

/* One game tick from the ledger's inputs, without drawing or sound. */
static void RbAdvance(void* user) {
    (void) user;
    ApplyNetPads(sSimTick);
    gRollbackResim = 1;
    rollback_sim_frame();
    gRollbackResim = 0;
    RecordTickHash(sSimTick);
    sSimTick++;
}

/* Hashes of states whose inputs are all confirmed go to the peer. */
static void RecordConfirmedHashes(void) {
    uint32_t c = n64net_input_confirmed_tick(sNet);
    if (c == UINT32_MAX) {
        return;
    }
    for (long t = sLastRecorded + 1; t <= (long) c && t <= (long) sTick; t++) {
        const int slot = (int) (t % RB_HASHES);
        if (sRec) {
            n64net_pad a, b;
            n64net_input_get(sNet, 0, (uint32_t) t, &a);
            n64net_input_get(sNet, 1, (uint32_t) t, &b);
            fprintf(sRec, "i %ld %04x %d %d %04x %d %d\n", t, a.buttons, a.stick_x, a.stick_y, b.buttons, b.stick_x,
                    b.stick_y);
        }
        if (t % HASH_EVERY == 0 && sTickHashTick[slot] == t) {
            unsigned long long h = sTickHash[slot];
            n64net_sync_record(sNet, (uint32_t) t, 0, (uint32_t) (h ^ (h >> 32)));
            if (sRec) {
                fprintf(sRec, "h %ld %08x\n", t, (unsigned) (h ^ (h >> 32)));
                if (t % (HASH_EVERY * 10) == 0) {
                    fflush(sRec); /* the app may be killed: keep the file current */
                }
            }
        }
        sLastRecorded = t;
    }
    uint32_t dt;
    uint8_t kind;
    if (!sDesyncLogged && n64net_sync_desynced(sNet, &dt, &kind)) {
        sDesyncLogged = 1;
        Log("DESYNC tick %u (%s)", dt, sRollback ? "rollback" : "lockstep");
        WriteDesyncDetail(dt);
        if (sRec) {
            fprintf(sRec, "# desync at tick %u\n", dt);
            fflush(sRec);
        }
    }
}

/* KartNet_OverrideInput through n64net_input: local input for this tick, wait
 * until the peer is close enough (lockstep: until its input for this tick is
 * here), roll back if a prediction was wrong, then apply this tick's inputs
 * (predicted for the peer if not here yet). */
static void NetFrame(const NetPad* local) {
    if (!sNet) {
        n64net_input_config c;
        memset(&c, 0, sizeof(c));
        c.num_players = sSessionPlayers;
        c.local_player = sLocalPlayer;
        c.session_id = (uint32_t) sSeed ^ 0x4D4B3634u;
        c.start_tick = 0;
        c.input_delay = (uint32_t) sDelay;
        c.rollback_window = 0; /* lockstep (rollback: until sRbFrom) */
        c.ticks_per_second = 30;
        c.send = NetSendCb;
        c.log = NetLogCb;
        sNet = n64net_input_create(&c);
        /* Automatic delay (n64net): the host's DELAY is the floor; with
         * rollback keep the ceiling low, since rollback hides the rest. */
        n64net_input_set_auto_delay(sNet, (uint32_t) sDelay, sAdaptive ? (uint32_t) sMaxDelay : 0);
        for (int i = 0; i < RB_HASHES; i++) {
            sTickHashTick[i] = -1;
        }
        if (sRollback) {
            Rollback_InitSlots(RB_SLOTS);
            Log("rollback on: window %u from tick %ld, delay %d..%d", sRbWindow, sRbFrom, sDelay, sAdaptive ? sMaxDelay : sDelay);
        } else {
            Log("lockstep via n64net_input, delay %d..%d", sDelay, sAdaptive ? sMaxDelay : sDelay);
        }
    }
    {
        /* Rollback only inside a race that is far enough in (its setup isn't
         * rollback-safe); one-race mode counts from the race request. */
        const long raceStart = sSession ? sRaceStartTick : 0;
        const int want = sRollback && raceStart >= 0 && (long) sTick - raceStart >= sRbFrom && RaceRollbackSafe();
        if (want && !sRbActive) {
            n64net_input_set_window(sNet, sRbWindow);
            sRbActive = 1;
            if (sSession) {
                Log("tick %lu: rollback on (race state %d)", sTick, gRaceState);
            }
        } else if (!want && sRbActive) {
            n64net_input_set_window(sNet, 0); /* (normally GateTransition already did) */
            sRbActive = 0;
        }
        /* lockstep parts of a session (menus, race setup): allow more input
         * delay instead of stalling; races keep the low rollback ceiling */
        const int ceiling = (sSession && !sRbActive) ? sMenuDelayCeiling : sMaxDelay;
        if (sSession && sAdaptive && ceiling != sLastCeiling) {
            n64net_input_set_auto_delay(sNet, (uint32_t) sDelay, (uint32_t) ceiling);
            sLastCeiling = ceiling;
        }
    }
    if (sRbActive && n64net_timesync_should_skip(sNet)) {
        gKartNetHoldFrame = 1; /* we're ahead: show this frame again, let the peer catch up */
        sHolds++;
        return;
    }
    n64net_pad lp = { local->button, local->x, local->y };
    n64net_input_set_local(sNet, (uint32_t) sTick, lp);

    if (!n64net_input_ready(sNet, (uint32_t) sTick)) {
        double start = NowMs(), lastPump = start;
        int shown = 0;
        sStalls++;
        while (!n64net_input_ready(sNet, (uint32_t) sTick)) {
            usleep(500);
            Poll();
            n64net_input_resend(sNet);
            double now = NowMs();
            double silentMs = n64net_input_peer_silence_usecs(sNet) / 1000.0;
            int away = n64net_input_peer_away(sNet) || sPeerPaused;
            if (now - start > 1000) {
                /* A long wait: say why (short ones would just flicker). */
                if (away) {
                    SetWait(0, "The other player switched to another app\nWaiting for them to come back...");
                } else {
                    SetWait(0, "Waiting for the other player...\nConnection is slow");
                }
                shown = 1;
                ShowWaiting();
            } else if (now - lastPump > 50) {
                lastPump = now;
                SDL_PumpEvents();
            }
            if (silentMs > (away ? PAUSED_TIMEOUT_MS : WAIT_TIMEOUT_MS) ||
                (silentMs == 0 && now - start > WAIT_TIMEOUT_MS)) {
                Log("peer silent at tick %lu; netplay stopped", sTick);
                SetWait(8, "Lost the connection to the other player");
                sState = NET_DONE;
                return;
            }
        }
        if (shown) {
            ClearWait();
        }
        sStallMs += NowMs() - start;
    }
    if (!sRbActive) {
        ApplyNetPads(sTick);
        return;
    }

    sSimTick = sTick;
    n64net_rollback_callbacks cb = { RbSave, RbLoad, RbAdvance, NULL, RB_SLOTS };
    Rollback_KeepDrawingState();
    int depth = n64net_rollback_frame(sNet, (uint32_t) sTick, &cb);
    if (depth > 0) {
        Rollback_RestoreDrawingState(); /* the next frame draws from what the last one drew */
    }
    if (depth > 0 && !RaceRollbackSafe()) {
        /* The re-simulation ran into a pause, the finish or a game state
         * change, which the peer (without this rollback) saw at the top of
         * this tick: confirm it the same way here, apply a pending state
         * change, and run this tick in lockstep. */
        SessionBeforeGamestate();
        if (sState != NET_RUNNING) {
            return;
        }
        if (!sRbActive) {
            if (gGamestateNext != gGamestate) {
                Log("state change at tick %lu applied after a rollback", sTick);
                apply_gamestate_change();
            }
            double start = NowMs();
            while (!n64net_input_ready(sNet, (uint32_t) sTick)) {
                usleep(500);
                Poll();
                n64net_input_resend(sNet);
                if (NowMs() - start > WAIT_TIMEOUT_MS) {
                    Log("peer silent at tick %lu; netplay stopped", sTick);
                    sState = NET_DONE;
                    return;
                }
            }
            ApplyNetPads(sTick);
            return;
        }
    }
    if (depth > 0) {
        sRbCount++;
        sRbFrames += (unsigned long) depth;
        if ((unsigned long) depth > sRbMaxDepth) {
            sRbMaxDepth = (unsigned long) depth;
        }
    } else if (depth < 0) {
        Log("rollback deeper than the snapshot ring at tick %lu", sTick);
    }
    ApplyNetPads(sTick);
}

/* A game-state change is pending at the top of tick sTick while rollback is
 * on: the tick that decided it may have run on a predicted input. Wait until
 * every input before sTick is confirmed, let n64net correct any misprediction
 * (which may remove the change), then go to lockstep for what follows. */
static void GateTransition(void) {
    const uint32_t T = (uint32_t) sTick;
    if (T == 0 || !sNet) {
        return;
    }
    double start = NowMs();
    for (;;) {
        uint32_t c = n64net_input_confirmed_tick(sNet);
        if (c != UINT32_MAX && c + 1 >= T) {
            break;
        }
        usleep(500);
        Poll();
        n64net_input_resend(sNet);
        if (NowMs() - start > WAIT_TIMEOUT_MS) {
            Log("peer silent at tick %lu (state change); netplay stopped", sTick);
            SetWait(8, "Lost the connection to the other player");
            sState = NET_DONE;
            return;
        }
        if (NowMs() - start > 1000) {
            SetWait(0, "Waiting for the other player...");
            ShowWaiting();
        }
    }
    ClearWait();
    sSimTick = sTick;
    n64net_rollback_callbacks cb = { RbSave, RbLoad, RbAdvance, NULL, RB_SLOTS };
    Rollback_KeepDrawingState();
    int depth = n64net_rollback_frame(sNet, T, &cb);
    if (depth > 0) {
        Rollback_RestoreDrawingState();
        sRbCount++;
        sRbFrames += (unsigned long) depth;
        Log("tick %u: pause/finish/state change re-checked after a %d-tick rollback: %s", T, depth,
            RaceRollbackSafe() ? "gone" : "still happens");
    }
    if (!RaceRollbackSafe()) {
        n64net_input_set_window(sNet, 0);
        sRbActive = 0;
        if (sSession) {
            Log("tick %u: rollback off (game state %d->%d, race state %d, paused %d, quitting %d)", T, gGamestate,
                gGamestateNext, gRaceState, gIsGamePaused, gIsInQuitToMenuTransition);
        }
    }
}

void KartNet_OverrideInput(void) {
    if (!sInitDone) {
        Init();
    }
    if (sUiBlocking && sState != NET_RUNNING) {
        memset(gControllerPads, 0, sizeof(OSContPad) * 4); /* the Online screen has the controller */
    }
    if (sState == NET_OFF) {
        return;
    }
    /* Real controllers only reach the game through the exchanged inputs. */
    OSContPad live = gControllerPads[0];
    memset(gControllerPads, 0, sizeof(OSContPad) * 4);
    if (sState != NET_RUNNING || !sRaceRequested) {
        return;
    }
    if (sPlayback) {
        ApplyNetPads(sTick);
        return;
    }

    NetPad local;
    if (sReplay) {
        NetPad none = { 0, 0, 0 };
        local = (sTick < sReplayLen) ? sReplay[sTick] : none;
    } else if (sBot) {
        local = BotInput(sTick);
    } else {
        local.button = live.button;
        local.x = live.stick_x;
        local.y = live.stick_y;
    }
    if (sInjectStart) {
        sInjectStart = 0;
        if (gGamestate == RACING && gIsGamePaused == 0) {
            /* A fresh Start press with L/R released opens the pause menu. */
            local.button = (u16) ((local.button & ~(L_TRIG | R_TRIG)) | START_BUTTON);
            Log("opening the pause menu after returning from the background");
        }
    }
    NetFrame(&local);
}

/* Hash the simulation part of gPlayers. Drawing a screen writes per-screen
 * fields (visibility, sprite angle and frame, drift-spark particles); each
 * device draws only its own screen, so those fields differ by design. Tested:
 * with one side drawing both screens and the other only its own, every other
 * field stayed identical over 3000 frames of drifting and items. */
static void MaskDrawingFields(Player* players);
static unsigned long long HashPlayers(unsigned long long h) {
    memcpy(sHashCopy, gPlayers, sizeof(sHashCopy));
    MaskDrawingFields(sHashCopy);
    return Fnv1a(h, sHashCopy, sizeof(sHashCopy));
}

/* Shells' "drawn on screen N" bits (actor flags 0xF, set by the drawing code
 * in actors.c actor_rendered) are read by race logic: a triple green shell
 * whose timer ran out is destroyed only if no screen shows it. In netplay
 * each device draws only its own screen, so the bits would differ between the
 * devices (and re-simulated frames draw nothing). Recompute them at the start
 * of every race frame from both players' cameras, exactly as the drawing code
 * tests them, so they are the same everywhere. */
void KartNet_UpdateShellVisibility(void) {
    if (!KartNet_IsActive()) {
        return;
    }
    size_t n = CM_GetActorSize();
    for (size_t i = 0; i < n; i++) {
        struct Actor* a = CM_GetActor(i);
        if (a->type != ACTOR_GREEN_SHELL && a->type != ACTOR_RED_SHELL && a->type != ACTOR_BLUE_SPINY_SHELL) {
            continue;
        }
        s16 flags = (s16) (a->flags & ~0xF);
        for (int s = 0; s < gPlayerCount && s < 4; s++) {
            Camera* c = &camera1[s];
            if (is_within_render_distance(c->pos, a->pos, c->rot[1], 0, c->fieldOfView, 490000.0f) >= 0.0f) {
                flags |= (s16) (1 << s);
            }
        }
        a->flags = flags;
    }
}

static unsigned long long StateHash(void) {
    unsigned long long h = 1469598103934665603ull;
    h = HashPlayers(h);
    h = Fnv1a(h, &gRandomSeed16, sizeof(gRandomSeed16));
    h = Fnv1a(h, &gGamestate, sizeof(gGamestate));
    if (sSession && gGamestate != RACING) {
        /* menus: what has been chosen so far */
        h = Fnv1a(h, &gMenuSelection, sizeof(gMenuSelection));
        h = Fnv1a(h, &gPlayerCount, sizeof(gPlayerCount));
        h = Fnv1a(h, &gModeSelection, sizeof(gModeSelection));
        h = Fnv1a(h, &gCCSelection, sizeof(gCCSelection));
        h = Fnv1a(h, &gScreenModeSelection, sizeof(gScreenModeSelection));
        h = Fnv1a(h, &gCupSelection, sizeof(gCupSelection));
        h = Fnv1a(h, &gCourseIndexInCup, sizeof(gCourseIndexInCup));
        h = Fnv1a(h, gCharacterSelections, sizeof(gCharacterSelections[0]) * 4);
        h = Fnv1a(h, &gMainMenuSelection, sizeof(gMainMenuSelection));
        h = Fnv1a(h, &gSubMenuSelection, sizeof(gSubMenuSelection));
    }
    return h;
}

/* Zero the Player fields that only drawing writes (a copy of gPlayers). */
void KartNet_MaskDrawingFields(void* players) {
    MaskDrawingFields((Player*) players);
}

static void MaskDrawingFields(Player* players) {
    for (int i = 0; i < NUM_PLAYERS; i++) {
        Player* p = &players[i];
        p->unk_002 = 0;
        memset(p->unk_048, 0, sizeof(p->unk_048));
        memset(p->unk_050, 0, sizeof(p->unk_050));
        memset(p->unk_0CC, 0, sizeof(p->unk_0CC));
        memset(p->unk_0D4, 0, sizeof(p->unk_0D4));
        memset(p->animFrameSelector, 0, sizeof(p->animFrameSelector));
        memset(p->animGroupSelector, 0, sizeof(p->animGroupSelector));
        memset(p->particlePool3, 0, sizeof(p->particlePool3));
    }
}

/* MK64_NET_PROBE=dir: at race ticks MK64_NET_PROBE_AT (default 300) and
 * +MK64_NET_PROBE_GAP (300) write the game's global memory (__data, __common,
 * __bss without the 256 MB memory pool) and the used part of the memory pool,
 * to find which state changes during a race (snapshot inventory). Mac only. */
extern uintptr_t sPoolEnd;
static void ProbeWrite(const char* dir, const char* name, const void* p, size_t n) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s-%s", dir, sIsHost ? "host" : "guest", name);
    FILE* f = fopen(path, "wb");
    if (f) {
        fwrite(p, 1, n, f);
        fclose(f);
    }
}
static void ProbeMaybeDump(void) {
#if defined(__APPLE__)
    static const char* dir = NULL;
    static long at = -1, gap = 300;
    if (at < 0) {
        dir = getenv("MK64_NET_PROBE");
        at = EnvInt("MK64_NET_PROBE_AT", 300);
        gap = EnvInt("MK64_NET_PROBE_GAP", 300);
    }
    if (!dir || ((long) sTick != at && (long) sTick != at + gap)) {
        return;
    }
    int which = (long) sTick == at ? 0 : 1;
    char name[64];
    const char* sects[] = { "__data", "__common", "__bss" };
    FILE* idx;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s-index%d.txt", dir, sIsHost ? "host" : "guest", which);
    idx = fopen(path, "w");
    uintptr_t poolStart = sPoolEnd - 0x10000000;
    for (int i = 0; i < 3; i++) {
        unsigned long size = 0;
        u8* base = getsectiondata(&__dso_handle, "__DATA", sects[i], &size);
        if (!base) {
            continue;
        }
        uintptr_t b = (uintptr_t) base, e = b + size;
        /* split around the memory pool */
        if (poolStart >= b && poolStart < e) {
            snprintf(name, sizeof(name), "%s_a_%d.bin", sects[i], which);
            ProbeWrite(dir, name, base, poolStart - b);
            fprintf(idx, "%s %lx %lx\n", name, (unsigned long) b, (unsigned long) (poolStart - b));
            snprintf(name, sizeof(name), "%s_b_%d.bin", sects[i], which);
            ProbeWrite(dir, name, (void*) sPoolEnd, e - sPoolEnd);
            fprintf(idx, "%s %lx %lx\n", name, (unsigned long) sPoolEnd, (unsigned long) (e - sPoolEnd));
        } else {
            snprintf(name, sizeof(name), "%s_%d.bin", sects[i], which);
            ProbeWrite(dir, name, base, size);
            fprintf(idx, "%s %lx %lx\n", name, (unsigned long) b, (unsigned long) size);
        }
    }
    size_t used = gNextFreeMemoryAddress - poolStart;
    snprintf(name, sizeof(name), "pool_%d.bin", which);
    ProbeWrite(dir, name, (void*) poolStart, used);
    fprintf(idx, "%s %lx %lx\n", name, (unsigned long) poolStart, (unsigned long) used);
    fprintf(idx, "slide %lx\n", (unsigned long) _dyld_get_image_vmaddr_slide(0));
    fclose(idx);
    Log("# probe %d written at tick %lu (pool used %zu bytes)", which, sTick, used);
#endif
}

void KartNet_EndFrame(void) {
    if (sState != NET_RUNNING || !sRaceRequested) {
        return;
    }
    if (gKartNetHoldFrame) {
        gKartNetHoldFrame = 0; /* no game tick ran this frame */
        return;
    }
    if (sPlayback) {
        RecordTickHash(sTick);
        if (sTick % HASH_EVERY == 0 && sPlayHash[sTick / HASH_EVERY] != 0) {
            unsigned long long h = sTickHash[sTick % RB_HASHES];
            const u32 mine = (u32) (h ^ (h >> 32)), rec = sPlayHash[sTick / HASH_EVERY];
            sPlayChecked++;
            if (mine == rec) {
                sPlayMatched++;
            } else if (sPlayFirstBad < 0) {
                sPlayFirstBad = (long) sTick;
                Log("playback differs from the recording at tick %lu (%08x here, %08x recorded)", sTick, mine, rec);
                WriteDesyncDetail(sTick);
            }
        }
        if (sTick + 1 >= sPlayLen) {
            Log("playback done after %lu ticks: %lu of %lu checkpoints match the recording%s", sTick + 1, sPlayMatched,
                sPlayChecked, sPlayFirstBad < 0 ? "" : " (see 'differs' above)");
            if (sLog) {
                fclose(sLog);
                sLog = NULL;
            }
            _exit(sPlayFirstBad < 0 ? 0 : 3);
        }
    }
    if (sNet) {
        RecordTickHash(sTick);
        RecordConfirmedHashes();
        if (sTick % (HASH_EVERY * 10) == 0 && sTick > 0) {
            n64net_input_stats st;
            n64net_input_get_stats(sNet, &st);
            double sv, ld;
            Rollback_SlotStats(&sv, &ld);
            Log("sync tick %lu %s (delay %u, matched %u, rollbacks %lu / %lu frames, max %lu, holds %lu, stalls %lu / %.0f ms, "
                "rtt %u/%u ms, save %.2f ms load %.2f ms)",
                sTick, sDesyncLogged ? "DESYNCED" : "OK", n64net_input_get_delay(sNet), n64net_sync_matched(sNet), sRbCount, sRbFrames, sRbMaxDepth,
                sHolds, sStalls, sStallMs, st.rtt_avg_usecs / 1000, st.rtt_max_usecs / 1000, sv, ld);
        }
    }
    if (getenv("MK64_NET_TRACE_LAKITU") && sTick < 400 && sTick % 5 == 0) {
        Object* a = &gObjectList[gIndexLakituList[0]];
        Object* b = &gObjectList[gIndexLakituList[1]];
        Log("# lakitu t%lu race%d go%d rs%d tmr%d z%.1f | p1 st%d tm%d tl%lx fl%x | p2 st%d tm%d tl%lx fl%x | spd %.2f %.2f",
            sTick, gRaceFrameCounter, D_8018D160, gRaceState, D_8015011E, gPlayers[0].pos[2], a->state, a->timer, (unsigned long) (uintptr_t) a->tlutList & 0xffff,
            a->status, b->state, b->timer, (unsigned long) (uintptr_t) b->tlutList & 0xffff, b->status,
            gPlayers[0].speed, gPlayers[1].speed);
    }
    ProbeMaybeDump();
    sTick++;
    if (sMaxTicks && sTick >= sMaxTicks) {
        /* Keep feeding the peer our last inputs briefly so it can finish too. */
        double end = NowMs() + 1500;
        while (NowMs() < end) {
            Poll();
            if (sNet) {
                n64net_input_resend(sNet);
            }
            usleep(16000);
        }
        if (sNet) {
            sSyncOk = n64net_sync_matched(sNet);
            sSyncBad = sDesyncLogged ? 1 : 0;
        }
        Log("done after %lu frames: %lu checkpoints in sync, %lu desynced, %lu stalls (%.0f ms)", sTick, sSyncOk,
            sSyncBad, sStalls, sStallMs);
        if (sLog) {
            fprintf(sLog, "# done\n");
            fclose(sLog);
            sLog = NULL;
        }
        if (sRec) {
            fclose(sRec);
            sRec = NULL;
        }
        _exit(sSyncBad ? 3 : 0);
    }
}

/* ------------------------------------------------------------------------ */
/* Rendering: one full-screen view of the local player                       */

static Player sRenderSnapshot[NUM_PLAYERS];

int KartNet_LocalPlayer(void) {
    return sLocalPlayer;
}

int KartNet_WantsLocalView(void) {
    return sState != NET_OFF && sRaceRequested && sFullView && gActiveScreenMode == SCREEN_MODE_2P_SPLITSCREEN_VERTICAL &&
           (!sSession || gGamestate == RACING);
}

int KartNet_InSession(void) {
    return sSession && sState != NET_OFF && sRaceRequested;
}

int KartNet_MinPlayers(void) {
    return KartNet_InSession() ? sSessionPlayers : 1; /* everyone in the session gets a kart */
}

int KartNet_MaxPlayers(void) {
    return KartNet_InSession() ? sSessionPlayers : 4;
}

static int sDrawingLocalView = 0;
int KartNet_ScreenIndex(int slot) {
    return (sDrawingLocalView && slot == 3) ? sLocalPlayer : slot;
}

int KartNet_TransitionsInLogic(void) {
    return KartNet_WantsLocalView();
}

int KartNet_SkipSplitRender(void) {
    return KartNet_WantsLocalView() && sSingleView;
}

void KartNet_RenderLocalView(void* splitStart) {
    /* The race is simulated, and drawn, as a normal 2-player split-screen race
     * on both sides: drawing updates per-screen state inside gPlayers
     * (visibility, animation frames, particles), which must stay identical.
     * So both split views were just drawn as usual; drop their display-list
     * commands (the GPU never draws them), then draw only the local player's
     * screen with the 1-player viewport and aspect ratio, and finally undo
     * everything that extra draw changed in gPlayers. */
    gDisplayListHead = (Gfx*) splitStart;
    memcpy(sRenderSnapshot, gPlayers, sizeof(sRenderSnapshot));
    s32 numScreens = gNumScreens;

    /* Draw through a copy of the local screen in the 4th screen slot, which is
     * unused in 2-player races: the display list keeps pointing at that slot's
     * viewport until the GPU reads it, and the real screens stay untouched. */
    ScreenContext* local = (sLocalPlayer == 0) ? gScreenOneCtx : gScreenTwoCtx;
    ScreenContext* view = &gScreenContexts[3];
    ScreenContext savedView = *view;
    f32 aspect = gScreenAspect;
    *view = *local;
    view->screenWidth = SCREEN_WIDTH;
    view->screenHeight = SCREEN_HEIGHT;
    view->screenStartX = SCREEN_WIDTH / 2;
    view->screenStartY = SCREEN_HEIGHT / 2;
    gScreenAspect = (f32) SCREEN_WIDTH / (f32) SCREEN_HEIGHT;
    /* Kart level-of-detail / shadow / particle distances: use the 1-player
     * values (spawn_players.c func_80039AE4); 2-player ones are shorter. */
    s16 lod[5] = { D_80165570, D_80165574, D_80165578, D_8016557A, D_80165580 };
    if (gModeSelection != BATTLE) {
        D_80165570 = 0x15E;
        D_80165574 = 0x1F4;
        D_80165578 = 0x4B0;
        D_8016557A = -0xA;
        D_80165580 = 0xFA;
    }
    /* Full-screen overlays (the lightning flash, menu_items.c func_8009E2F0)
     * take their box from the split screen's area: use the whole screen. */
    struct UnkStruct_8018E7E8 boxPos[2] = { D_8018E7E8[0], D_8018E7E8[1] };
    struct UnkStruct_8018E7E8 boxSize[2] = { D_8018E810[0], D_8018E810[1] };
    for (int i = 0; i < 2; i++) {
        D_8018E7E8[i] = D_8018E7E8[4];
        D_8018E810[i] = D_8018E810[4];
    }
    sDrawingLocalView = 1;
    if (sLocalPlayer == 0) {
        render_screens(view, RENDER_SCREEN_MODE_2P_VERTICAL_PLAYER_ONE, 1, 0);
    } else {
        render_screens(view, RENDER_SCREEN_MODE_2P_VERTICAL_PLAYER_TWO, 2, 1);
    }
    sDrawingLocalView = 0;
    gScreenAspect = aspect;
    for (int i = 0; i < 2; i++) {
        D_8018E7E8[i] = boxPos[i];
        D_8018E810[i] = boxSize[i];
    }
    D_80165570 = lod[0];
    D_80165574 = lod[1];
    D_80165578 = lod[2];
    D_8016557A = lod[3];
    D_80165580 = lod[4];
    {
        Vp fullViewport = view->viewport; /* read by the GPU later */
        *view = savedView;
        view->viewport = fullViewport;
    }

    gNumScreens = numScreens;
    if (!sSingleView) {
        memcpy(gPlayers, sRenderSnapshot, sizeof(sRenderSnapshot));
    }
}
