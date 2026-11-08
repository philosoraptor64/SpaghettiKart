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
#include "menu_items.h"
#include "port/Rollback.h"
#include <SDL2/SDL.h>
#if defined(__APPLE__)
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <mach-o/dyld.h>
#endif

extern OSContPad gControllerPads[4];

/* ------------------------------------------------------------------------ */
/* Settings                                                                  */

enum { NET_OFF, NET_HANDSHAKE, NET_RUNNING, NET_DONE };

#define RING 256          /* input ring (ticks); must exceed delay + 16 */
#define REDUNDANT 16      /* inputs repeated in every packet */
#define HASH_EVERY 30     /* frames between state-hash checkpoints */
#define HASH_SLOTS 16
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
static int sBot = 0;
static int sBotDrift = 0; /* MK64_NET_BOT_DRIFT=1: the bot also drifts */
static unsigned int sBotState = 1;
static int sBotSteer = 0, sBotTarget = 0;

typedef struct {
    u16 button;
    s8 x, y;
} NetPad;

static NetPad sInputs[2][RING];
static long sKnownTick[2][RING]; /* tick stored in the slot, -1 = empty */
static long sRemoteContig = -1;  /* highest remote tick with all earlier ticks known */

typedef struct {
    long tick;
    unsigned long long hash;
} Checkpoint;
static Checkpoint sLocalHashes[HASH_SLOTS];
static Checkpoint sRemoteHashes[HASH_SLOTS];
static unsigned long sSyncOk = 0, sSyncBad = 0, sStalls = 0;
static double sStallMs = 0;
static double sLastHelloMs = 0;
static int sPeerPaused = 0;
static int sFullView = 1; /* MK64_NET_VIEW=full (default) or split */
/* Full view: draw only the local screen (default). MK64_NET_VIEW=double draws
 * both split screens first and undoes the extra draw (the first version). */
static int sSingleView = 1;
static int sInjectStart = 0; /* press Start once after returning from the background */
static FILE* sDump = NULL; /* MK64_NET_DUMP: raw gPlayers at each checkpoint */

/* Timing: round-trip time, frame advantage and the adaptive input delay.
 * Each side schedules its own inputs `delay` frames ahead, so each side's delay
 * only protects its PEER from waiting. Both use max(own wish, peer's wish); the
 * wish comes from the recent round-trip times (up at once, down slowly). A side
 * can change its delay alone without breaking determinism: the inputs are
 * exchanged, and a tick is never scheduled twice (see ScheduleLocal). */
#define RTT_SAMPLES 90
static double sRtt[RTT_SAMPLES];
static int sRttCount = 0, sRttNext = 0;
static u32 sPeerStamp = 0, sLastEcho = 0;
static double sPeerStampAt = 0;
static int sHavePeerStamp = 0;
static long sPeerTick = -1;
static double sPeerTickAt = 0;
static double sFrameMs = 1000.0 / 30, sLastFrameAt = 0, sAdvAvg = 0;
static int sAdaptive = 1, sMinDelay = 2, sMaxDelay = 6;
static int sWantDelay = 2, sPeerWantDelay = 0, sCurDelay = 2;
static double sLowSince = 0;
static long sLastScheduled = -1; /* highest tick holding a local input */
static double sRttMaxWindow = 0;

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
#if defined(PLATFORM_IOS)
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

#if defined(PLATFORM_IOS)
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
static void Put16(u8* p, u16 v) {
    p[0] = v & 0xFF;
    p[1] = v >> 8;
}
static void Put32(u8* p, u32 v) {
    for (int i = 0; i < 4; i++) {
        p[i] = (v >> (8 * i)) & 0xFF;
    }
}
static void Put64(u8* p, unsigned long long v) {
    for (int i = 0; i < 8; i++) {
        p[i] = (v >> (8 * i)) & 0xFF;
    }
}
static u16 Get16(const u8* p) {
    return (u16) (p[0] | (p[1] << 8));
}
static u32 Get32(const u8* p) {
    return (u32) p[0] | ((u32) p[1] << 8) | ((u32) p[2] << 16) | ((u32) p[3] << 24);
}
static unsigned long long Get64(const u8* p) {
    unsigned long long v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

/* ------------------------------------------------------------------------ */
/* Packets                                                                   */

enum { PKT_HELLO = 1, PKT_CONFIG = 2, PKT_START = 3, PKT_INPUT = 4, PKT_PAUSE = 5 };
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

static void SendInputs(long top) {
    u8 buf[512];
    size_t n = Header(buf, PKT_INPUT);
    long first = top - (REDUNDANT - 1);
    if (first < 0) {
        first = 0;
    }
    buf[n++] = (u8) sLocalPlayer;
    Put32(buf + n, (u32) first);
    n += 4;
    buf[n++] = (u8) (top - first + 1);
    for (long t = first; t <= top; t++) {
        const NetPad* p = &sInputs[sLocalPlayer][t % RING];
        Put16(buf + n, p->button);
        buf[n + 2] = (u8) p->x;
        buf[n + 3] = (u8) p->y;
        n += 4;
    }
    /* The latest few state-hash checkpoints ride along. */
    u8* countAt = buf + n++;
    u8 count = 0;
    for (int i = 0; i < HASH_SLOTS && count < 4; i++) {
        const Checkpoint* c = &sLocalHashes[i];
        if (c->tick >= 0 && c->tick + 4 * HASH_EVERY >= (long) sTick) {
            Put32(buf + n, (u32) c->tick);
            Put64(buf + n + 4, c->hash);
            n += 12;
            count++;
        }
    }
    *countAt = count;
    /* Timing: our tick, a time stamp, the peer's last stamp echoed back with
     * how long we held it (-> round-trip time), and our wished input delay. */
    double now = NowMs();
    Put32(buf + n, (u32) sTick);
    Put32(buf + n + 4, (u32) (long long) now);
    Put32(buf + n + 8, sHavePeerStamp ? sPeerStamp : 0);
    Put32(buf + n + 12, sHavePeerStamp ? (u32) (now - sPeerStampAt) : 0xFFFFFFFFu);
    buf[n + 16] = (u8) sWantDelay;
    n += 17;
    SendRaw(buf, n);
}

static void AddRtt(double ms) {
    sRtt[sRttNext] = ms;
    sRttNext = (sRttNext + 1) % RTT_SAMPLES;
    if (sRttCount < RTT_SAMPLES) {
        sRttCount++;
    }
    if (ms > sRttMaxWindow) {
        sRttMaxWindow = ms;
    }
}

static int CmpDouble(const void* a, const void* b) {
    double x = *(const double*) a, y = *(const double*) b;
    return (x > y) - (x < y);
}

/* Percentile (0..1) of the recent round-trip times; 0 if none yet. */
static double RttPercentile(double q) {
    if (sRttCount == 0) {
        return 0;
    }
    double tmp[RTT_SAMPLES];
    memcpy(tmp, sRtt, sizeof(double) * (size_t) sRttCount);
    qsort(tmp, (size_t) sRttCount, sizeof(double), CmpDouble);
    int i = (int) (q * (sRttCount - 1) + 0.5);
    return tmp[i];
}

static void CompareCheckpoint(long tick) {
    const Checkpoint* l = &sLocalHashes[(tick / HASH_EVERY) % HASH_SLOTS];
    const Checkpoint* r = &sRemoteHashes[(tick / HASH_EVERY) % HASH_SLOTS];
    if (l->tick != tick || r->tick != tick) {
        return;
    }
    if (l->hash == r->hash) {
        sSyncOk++;
        if (tick % (HASH_EVERY * 10) == 0) {
            Log("sync tick %ld OK %016llx (ok %lu, desync %lu, stalls %lu / %.0f ms)", tick, l->hash, sSyncOk,
                sSyncBad, sStalls, sStallMs);
            Log("# timing: rtt median %.0f p90 %.0f max %.0f ms, delay %d (wishes %d/%d), frame %.1f ms, ahead %.2f",
                RttPercentile(0.5), RttPercentile(0.9), sRttMaxWindow, sCurDelay, sWantDelay, sPeerWantDelay, sFrameMs,
                sAdvAvg);
            sRttMaxWindow = 0;
        }
    } else {
        sSyncBad++;
        Log("DESYNC tick %ld local %016llx remote %016llx", tick, l->hash, r->hash);
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
        case PKT_INPUT: {
            if (len < 6) {
                return;
            }
            if (sIsHost && sState == NET_HANDSHAKE) {
                sState = NET_RUNNING; /* the START was lost, but the guest is running */
                Log("guest is running (start ack lost); starting");
            }
            if (sPeerPaused) {
                sPeerPaused = 0;
                Log("peer resumed");
            }
            int player = p[0];
            if (player != 1 - sLocalPlayer) {
                return;
            }
            long first = (long) Get32(p + 1);
            int count = p[5];
            p += 6;
            len -= 6;
            if (len < (size_t) count * 4) {
                return;
            }
            for (int i = 0; i < count; i++) {
                long t = first + i;
                if (t > sRemoteContig + RING - 32) {
                    break; /* far ahead; impossible in lockstep, ignore */
                }
                NetPad* pad = &sInputs[player][t % RING];
                if (sKnownTick[player][t % RING] != t) {
                    pad->button = Get16(p);
                    pad->x = (s8) p[2];
                    pad->y = (s8) p[3];
                    sKnownTick[player][t % RING] = t;
                }
                p += 4;
            }
            len -= (size_t) count * 4;
            while (sKnownTick[player][(sRemoteContig + 1) % RING] == sRemoteContig + 1) {
                sRemoteContig++;
            }
            if (len >= 1) {
                int hashes = p[0];
                p++;
                len--;
                for (int i = 0; i < hashes && len >= 12; i++) {
                    long tick = (long) Get32(p);
                    Checkpoint* c = &sRemoteHashes[(tick / HASH_EVERY) % HASH_SLOTS];
                    if (c->tick != tick) {
                        c->tick = tick;
                        c->hash = Get64(p + 4);
                        CompareCheckpoint(tick);
                    }
                    p += 12;
                    len -= 12;
                }
            }
            if (len >= 17) {
                double now = NowMs();
                long peerTick = (long) Get32(p);
                u32 echo = Get32(p + 8);
                u32 held = Get32(p + 12);
                if (peerTick > sPeerTick) {
                    sPeerTick = peerTick;
                    sPeerTickAt = now;
                }
                sPeerStamp = Get32(p + 4);
                sPeerStampAt = now;
                sHavePeerStamp = 1;
                if (held != 0xFFFFFFFFu && echo != sLastEcho) {
                    sLastEcho = echo;
                    double rtt = (double) (u32) ((u32) (long long) now - echo) - (double) held;
                    if (rtt >= 0 && rtt < 5000) {
                        AddRtt(rtt);
                    }
                }
                sPeerWantDelay = p[16];
            }
            break;
        }
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
        HandlePacket(buf, (size_t) n, &from);
    }
}

/* ------------------------------------------------------------------------ */
/* Setup                                                                     */

static void Init(void) {
    sInitDone = 1;
#if defined(PLATFORM_IOS)
    LoadConfigFile();
#endif
    if (EnvInt("MK64_NET", 0) == 0) {
        return;
    }
    char pathBuf[1024];
    const char* logPath = DocumentsPath(getenv("MK64_NET_LOG"), pathBuf, sizeof(pathBuf));
    if (logPath && *logPath) {
        sLog = fopen(logPath, "w");
    }

    const char* dumpPath = DocumentsPath(getenv("MK64_NET_DUMP"), pathBuf, sizeof(pathBuf));
    if (dumpPath && *dumpPath) {
        sDump = fopen(dumpPath, "wb");
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
    sAdaptive = EnvInt("MK64_NET_ADAPTIVE", 1) != 0;
    sMaxDelay = (int) EnvInt("MK64_NET_MAX_DELAY", 6);
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
    LoadReplay(getenv("MK64_NET_REPLAY"));
    sBotState = (unsigned int) sBot * 2654435761u + 1;

    for (int pl = 0; pl < 2; pl++) {
        for (int i = 0; i < RING; i++) {
            sKnownTick[pl][i] = -1;
        }
    }
    for (int i = 0; i < HASH_SLOTS; i++) {
        sLocalHashes[i].tick = -1;
        sRemoteHashes[i].tick = -1;
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
    if (!sIsHost) {
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

void KartNet_SetPaused(int paused) {
    if (sState == NET_HANDSHAKE || sState == NET_RUNNING) {
        Log("local app %s", paused ? "paused (background)" : "resumed");
        SendPause(paused);
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

void KartNet_BeginFrame(void) {
    if (!sInitDone) {
        Init();
    }
    if (sState == NET_OFF || sState == NET_DONE) {
        return;
    }
    Poll();
    sBootFrames++;
    /* Both sides boot identically (no input) to START_BOOT_FRAME and wait there
     * for the handshake, so the race starts from the same state on both: a side
     * that kept running into the attract demo while waiting would carry
     * leftover state into the race and desync. */
    while (sState == NET_HANDSHAKE) {
        double now = NowMs();
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
        if (sBootFrames < START_BOOT_FRAME) {
            return; /* keep booting meanwhile */
        }
        SDL_PumpEvents(); /* keep the window (and iOS life cycle) responsive */
        usleep(5000);
        Poll();
    }
    if (sState != NET_RUNNING) {
        return;
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

static NetPad BotInput(unsigned long f) {
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

/* Once per frame: frame time, frame advantage and the wished input delay. */
static void UpdateTiming(void) {
    double now = NowMs();
    if (sLastFrameAt > 0) {
        double d = now - sLastFrameAt;
        if (d > 5 && d < 45) { /* ignore stalls */
            sFrameMs = sFrameMs * 0.95 + d * 0.05;
        }
    }
    sLastFrameAt = now;
    if (sPeerTick >= 0 && sRttCount > 0) {
        /* Frames we are ahead of the peer (n64net time sync will act on this). */
        double peerNow = sPeerTick + (now - sPeerTickAt + RttPercentile(0.5) / 2) / sFrameMs;
        sAdvAvg = sAdvAvg * 0.9 + ((double) sTick - peerNow) * 0.1;
    }
    if (!sAdaptive || sRttCount < 10) {
        return;
    }
    /* The peer's input for tick T leaves it at its tick T - delay and must arrive
     * before we reach T: cover the one-way time of most recent packets. The
     * measured round trip includes about a frame of polling (packets are read
     * once per frame on each side), which the +1 frame below already covers. */
    double oneWay = (RttPercentile(0.9) - sFrameMs) / 2;
    if (oneWay < 0) {
        oneWay = 0;
    }
    int want = (int) ceil((oneWay + 8) / sFrameMs) + 1;
    if (want < sMinDelay) {
        want = sMinDelay;
    }
    if (want > sMaxDelay) {
        want = sMaxDelay;
    }
    if (want > sWantDelay) {
        sWantDelay = want; /* worse network: protect at once */
        sLowSince = 0;
    } else if (want < sWantDelay) {
        if (sLowSince == 0) {
            sLowSince = now;
        } else if (now - sLowSince > 8000) {
            sWantDelay--; /* calmer for 8 s: one frame less */
            sLowSince = now;
        }
    } else {
        sLowSince = 0;
    }
}

static int CurrentDelay(void) {
    int d = sAdaptive ? (sWantDelay > sPeerWantDelay ? sWantDelay : sPeerWantDelay) : sDelay;
    if (d != sCurDelay) {
        Log("input delay %d -> %d frames at tick %lu (rtt median %.0f, p90 %.0f ms; wishes %d/%d)", sCurDelay, d, sTick,
            RttPercentile(0.5), RttPercentile(0.9), sWantDelay, sPeerWantDelay);
        sCurDelay = d;
    }
    return d;
}

/* Put this frame's local input at tick sTick + delay. Ticks are never
 * scheduled twice: after the delay grows, the gap is filled with this input;
 * after it shrinks, inputs are dropped until sTick + delay passes the last
 * scheduled tick. */
static void ScheduleLocal(const NetPad* local, int delay) {
    long target = (long) sTick + delay;
    for (long t = sLastScheduled + 1; t <= target; t++) {
        sInputs[sLocalPlayer][t % RING] = *local;
        sKnownTick[sLocalPlayer][t % RING] = t;
    }
    if (target > sLastScheduled) {
        sLastScheduled = target;
    }
}

void KartNet_OverrideInput(void) {
    if (!sInitDone) {
        Init();
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

    /* Schedule this frame's local input DELAY frames ahead. */
    if (sTick == 0) {
        sMinDelay = sDelay; /* the host's setting is the floor */
        sWantDelay = sCurDelay = sDelay;
        for (long t = 0; t < sDelay; t++) {
            NetPad zero = { 0, 0, 0 };
            sInputs[sLocalPlayer][t % RING] = zero;
            sKnownTick[sLocalPlayer][t % RING] = t;
        }
        sLastScheduled = sDelay - 1;
    }
    UpdateTiming();
    int delay = CurrentDelay();
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
    ScheduleLocal(&local, delay);
    long top = sLastScheduled;
    SendInputs(top);

    /* Lockstep: wait for the other player's input for this frame. */
    int remote = 1 - sLocalPlayer;
    if (sKnownTick[remote][sTick % RING] != (long) sTick) {
        double start = NowMs(), lastSend = start;
        sStalls++;
        double lastHeard = start, lastPump = start;
        unsigned long remoteBefore = (unsigned long) (sRemoteContig + 1);
        while (sKnownTick[remote][sTick % RING] != (long) sTick) {
            usleep(500);
            Poll();
            double now = NowMs();
            if (now - lastPump > 50) {
                lastPump = now;
                SDL_PumpEvents(); /* keep the window and the iOS life cycle responsive */
            }
            if (now - lastSend > 16) {
                lastSend = now;
                SendInputs(top);
            }
            if (sPeerPaused || (unsigned long) (sRemoteContig + 1) != remoteBefore) {
                lastHeard = now;
                remoteBefore = (unsigned long) (sRemoteContig + 1);
            }
            if (now - lastHeard > (sPeerPaused ? PAUSED_TIMEOUT_MS : WAIT_TIMEOUT_MS)) {
                Log("peer silent for %.0f s at tick %lu; netplay stopped", (now - lastHeard) / 1000, sTick);
                sState = NET_DONE;
                return;
            }
        }
        sStallMs += NowMs() - start;
    }

    for (int pl = 0; pl < 2; pl++) {
        const NetPad* p = &sInputs[pl][sTick % RING];
        gControllerPads[pl].button = p->button;
        gControllerPads[pl].stick_x = p->x;
        gControllerPads[pl].stick_y = p->y;
    }
}

/* Hash the simulation part of gPlayers. Drawing a screen writes per-screen
 * fields (visibility, sprite angle and frame, drift-spark particles); each
 * device draws only its own screen, so those fields differ by design. Tested:
 * with one side drawing both screens and the other only its own, every other
 * field stayed identical over 3000 frames of drifting and items. */
static Player sHashCopy[NUM_PLAYERS];
static void MaskDrawingFields(Player* players);
static unsigned long long HashPlayers(unsigned long long h) {
    memcpy(sHashCopy, gPlayers, sizeof(sHashCopy));
    MaskDrawingFields(sHashCopy);
    return Fnv1a(h, sHashCopy, sizeof(sHashCopy));
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
        u8* base = getsectiondata(&_mh_execute_header, "__DATA", sects[i], &size);
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
    if (sTick % HASH_EVERY == 0) {
        unsigned long long h = 1469598103934665603ull;
        h = HashPlayers(h);
        h = Fnv1a(h, &gRandomSeed16, sizeof(gRandomSeed16));
        h = Fnv1a(h, &gGamestate, sizeof(gGamestate));
        if (sDump) {
            u32 t = (u32) sTick;
            fwrite(&t, sizeof(t), 1, sDump);
            fwrite(gPlayers, sizeof(Player), NUM_PLAYERS, sDump);
            fflush(sDump);
        }
        Checkpoint* c = &sLocalHashes[(sTick / HASH_EVERY) % HASH_SLOTS];
        c->tick = (long) sTick;
        c->hash = h;
        CompareCheckpoint((long) sTick);
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
            SendInputs(sLastScheduled);
            usleep(16000);
        }
        Log("done after %lu frames: %lu checkpoints in sync, %lu desynced, %lu stalls (%.0f ms)", sTick, sSyncOk,
            sSyncBad, sStalls, sStallMs);
        if (sLog) {
            fprintf(sLog, "# done\n");
            fclose(sLog);
            sLog = NULL;
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
    return sState != NET_OFF && sRaceRequested && sFullView && gActiveScreenMode == SCREEN_MODE_2P_SPLITSCREEN_VERTICAL;
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
    if (sLocalPlayer == 0) {
        render_screens(view, RENDER_SCREEN_MODE_2P_VERTICAL_PLAYER_ONE, 1, 0);
    } else {
        render_screens(view, RENDER_SCREEN_MODE_2P_VERTICAL_PLAYER_TWO, 2, 1);
    }
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
