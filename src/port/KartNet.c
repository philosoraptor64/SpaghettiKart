#include "KartNet.h"

#include <errno.h>
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
#include "port/RaceStart.h"

extern OSContPad gControllerPads[4];

/* ------------------------------------------------------------------------ */
/* Settings                                                                  */

enum { NET_OFF, NET_HANDSHAKE, NET_RUNNING, NET_DONE };

#define RING 256          /* input ring (ticks); must exceed delay + 16 */
#define REDUNDANT 16      /* inputs repeated in every packet */
#define HASH_EVERY 30     /* frames between state-hash checkpoints */
#define HASH_SLOTS 16
#define WAIT_TIMEOUT_MS 10000

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
static int sBot = 0;
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

enum { PKT_HELLO = 1, PKT_CONFIG = 2, PKT_START = 3, PKT_INPUT = 4 };
static const u8 kMagic[4] = { 'M', 'K', 'N', '1' };

static void SendRaw(const u8* buf, size_t len) {
    if (sHavePeer) {
        sendto(sSock, buf, len, 0, (const struct sockaddr*) &sPeer, sizeof(sPeer));
    }
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
    SendRaw(buf, n);
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
            break;
        }
        default:
            break;
    }
}

static void Poll(void) {
    u8 buf[1500];
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
    sBot = (int) EnvInt("MK64_NET_BOT", 0);
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
    if (sState == NET_HANDSHAKE) {
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
        return;
    }
    /* Let boot finish (logo/menu setup) before jumping into the race. */
    if (!sRaceRequested && sBootFrames >= 30) {
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
    pad.x = (s8) sBotSteer;
    return pad;
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
    long top = (long) sTick + sDelay;
    if (sTick == 0) {
        for (long t = 0; t < sDelay; t++) {
            NetPad zero = { 0, 0, 0 };
            sInputs[sLocalPlayer][t % RING] = zero;
            sKnownTick[sLocalPlayer][t % RING] = t;
        }
    }
    NetPad local;
    if (sBot) {
        local = BotInput(sTick);
    } else {
        local.button = live.button;
        local.x = live.stick_x;
        local.y = live.stick_y;
    }
    sInputs[sLocalPlayer][top % RING] = local;
    sKnownTick[sLocalPlayer][top % RING] = top;
    SendInputs(top);

    /* Lockstep: wait for the other player's input for this frame. */
    int remote = 1 - sLocalPlayer;
    if (sKnownTick[remote][sTick % RING] != (long) sTick) {
        double start = NowMs(), lastSend = start;
        sStalls++;
        while (sKnownTick[remote][sTick % RING] != (long) sTick) {
            usleep(500);
            Poll();
            double now = NowMs();
            if (now - lastSend > 16) {
                lastSend = now;
                SendInputs(top);
            }
            if (now - start > WAIT_TIMEOUT_MS) {
                Log("peer silent for %d s at tick %lu; netplay stopped", WAIT_TIMEOUT_MS / 1000, sTick);
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

void KartNet_EndFrame(void) {
    if (sState != NET_RUNNING || !sRaceRequested) {
        return;
    }
    if (sTick % HASH_EVERY == 0) {
        unsigned long long h = 1469598103934665603ull;
        h = Fnv1a(h, gPlayers, sizeof(Player) * NUM_PLAYERS);
        h = Fnv1a(h, &gRandomSeed16, sizeof(gRandomSeed16));
        h = Fnv1a(h, &gGamestate, sizeof(gGamestate));
        Checkpoint* c = &sLocalHashes[(sTick / HASH_EVERY) % HASH_SLOTS];
        c->tick = (long) sTick;
        c->hash = h;
        CompareCheckpoint((long) sTick);
    }
    sTick++;
    if (sMaxTicks && sTick >= sMaxTicks) {
        /* Keep feeding the peer our last inputs briefly so it can finish too. */
        double end = NowMs() + 1500;
        while (NowMs() < end) {
            Poll();
            SendInputs((long) sTick - 1 + sDelay);
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
