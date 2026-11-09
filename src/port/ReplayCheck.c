#include "ReplayCheck.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libultraship.h>
#include "defines.h"
#include "main.h"
#include "menus.h"
#include "code_800029B0.h"
#include "buffers/random.h"
#include "port/Game.h"
#include "port/RaceStart.h"

extern OSContPad gControllerPads[4];

static int sInitDone = 0;
static int sActive = 0;
static FILE* sLog = NULL;
static unsigned long sMaxFrames = 2400;
static unsigned long sBootFrames = 0;
static unsigned long sRaceFrame = 0;
static int sRaceRequested = 0;
static unsigned int sInputSeed = 1;
static unsigned int sRngSeed = 0x1234;
static int sCup = 0, sCourse = 0, sCC = CC_150, sChar = MARIO;

/* Record (a human plays) / playback (inputs come from a recording). */
typedef struct {
    u16 button;
    s8 stick_x;
    s8 stick_y;
} RecordedPad;
static FILE* sRecord = NULL;
static RecordedPad* sPlayback = NULL;
static unsigned long sPlaybackCount = 0;
static long sFinishedAt = -1;

/* Script state (independent of the game's RNG). */
static unsigned int sScriptState;
static int sSteer = 0, sSteerTarget = 0;

static long EnvInt(const char* name, long def) {
    const char* v = getenv(name);
    return (v && *v) ? strtol(v, NULL, 0) : def;
}

/* iOS apps start in "/", so relative paths are taken from the app's Documents
 * folder there (where devicectl copies files in and out). */
void Port_RequestStop(void); /* Game.cpp */

static const char* ResolvePath(const char* path) {
#if defined(PLATFORM_IOS)
    static char buf[4][1024];
    static int next = 0;
    const char* home = getenv("HOME");
    if (path != NULL && *path != '\0' && path[0] != '/' && home != NULL) {
        char* out = buf[next++ & 3];
        snprintf(out, sizeof(buf[0]), "%s/Documents/%s", home, path);
        return out;
    }
#endif
    return path;
}

static void Init(void) {
    sInitDone = 1;
    const char* path = ResolvePath(getenv("MK64_REPLAY_CHECK"));
    if (path == NULL || *path == '\0') {
        return;
    }
    /* in a soak (MK64_SOAK, several runs in one process) each run writes
     * <path>.<run> so the runs can be compared */
    char soakPath[1100];
    const char* run = getenv("MK64_SOAK_RUN");
    if (run != NULL) {
        snprintf(soakPath, sizeof(soakPath), "%s.%s", path, run);
        path = soakPath;
    }
    sLog = fopen(path, "w");
    if (sLog == NULL) {
        fprintf(stderr, "ReplayCheck: cannot open %s\n", path);
        return;
    }
    sActive = 1;
    sMaxFrames = (unsigned long) EnvInt("MK64_REPLAY_FRAMES", 2400);
    sInputSeed = (unsigned int) EnvInt("MK64_REPLAY_SEED", 1);
    sRngSeed = (unsigned int) EnvInt("MK64_REPLAY_RNG", 0x1234) & 0xFFFF;
    sCup = (int) EnvInt("MK64_REPLAY_CUP", 0);
    sCourse = (int) EnvInt("MK64_REPLAY_COURSE", 0);
    sCC = (int) EnvInt("MK64_REPLAY_CC", CC_150);
    sChar = (int) EnvInt("MK64_REPLAY_CHAR", MARIO);
    sScriptState = sInputSeed * 2654435761u + 1;

    const char* rec = ResolvePath(getenv("MK64_REPLAY_RECORD"));
    const char* play = ResolvePath(getenv("MK64_REPLAY_PLAY"));
    if (rec && *rec) {
        sRecord = fopen(rec, "w");
        if (sRecord == NULL) {
            fprintf(stderr, "ReplayCheck: cannot write %s\n", rec);
        } else if (getenv("MK64_REPLAY_FRAMES") == NULL) {
            sMaxFrames = 9000; /* 5 minutes; normally ends shortly after the finish line */
        }
    } else if (play && *play) {
        FILE* in = fopen(play, "r");
        unsigned long cap = 1024;
        sPlayback = (RecordedPad*) malloc(sizeof(RecordedPad) * cap);
        unsigned long f;
        unsigned int b;
        int x, y;
        char line[128];
        while (in && fgets(line, sizeof(line), in)) {
            if (line[0] == '#') {
                continue;
            }
            if (sscanf(line, "%lu %x %d %d", &f, &b, &x, &y) != 4) {
                continue;
            }
            if (sPlaybackCount == cap) {
                cap *= 2;
                sPlayback = (RecordedPad*) realloc(sPlayback, sizeof(RecordedPad) * cap);
            }
            sPlayback[sPlaybackCount].button = (u16) b;
            sPlayback[sPlaybackCount].stick_x = (s8) x;
            sPlayback[sPlaybackCount].stick_y = (s8) y;
            sPlaybackCount++;
        }
        if (in) {
            fclose(in);
        } else {
            fprintf(stderr, "ReplayCheck: cannot read %s\n", play);
        }
        sMaxFrames = sPlaybackCount;
    }
    fprintf(sLog, "# ReplayCheck seed=%u rng=0x%04x cup=%d course=%d cc=%d char=%d frames=%lu player_size=%zu\n",
            sInputSeed, sRngSeed, sCup, sCourse, sCC, sChar, sMaxFrames, sizeof(Player));
    fprintf(sLog, "# input: %s\n", sRecord ? "recording a human" : (sPlayback ? "playback" : "script"));
}

/* Called at the top of each game-thread iteration, before the gamestate switch. */
void ReplayCheck_BeginFrame(void) {
    if (!sInitDone) {
        Init();
    }
    if (!sActive || sRaceRequested) {
        return;
    }
    /* Let boot finish (logo/menu setup) before jumping into the race. */
    if (++sBootFrames < 30) {
        return;
    }
    sRaceRequested = 1;
    {
        RaceStartParams p;
        p.mode = GRAND_PRIX;
        p.playerCount = 1;
        p.cup = sCup;
        p.course = sCourse;
        p.cc = sCC;
        p.chars[0] = sChar;
        p.chars[1] = gCharacterSelections[1]; /* unused in 1P; left as it was */
        p.rng = sRngSeed;
        RaceStart_Request(&p);
    }
    fprintf(sLog, "# race requested at boot frame %lu\n", sBootFrames);
}

static unsigned int NextScript(void) {
    /* xorshift32 */
    unsigned int x = sScriptState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    sScriptState = x;
    return x;
}

/* Called right after osContGetReadData in read_controllers(). */
void ReplayCheck_OverrideInput(void) {
    if (!sInitDone) {
        Init();
    }
    if (!sActive) {
        return;
    }
    if (sRecord != NULL) {
        /* The human drives player 1 in the race; other ports and the menus are silenced. */
        OSContPad live = gControllerPads[0];
        memset(gControllerPads, 0, sizeof(OSContPad) * 4);
        if (gGamestate == RACING) {
            gControllerPads[0] = live;
            fprintf(sRecord, "%lu %04x %d %d\n", sRaceFrame, live.button, live.stick_x, live.stick_y);
            fflush(sRecord);
        }
        return;
    }
    /* Real controllers never reach the game during a check. */
    memset(gControllerPads, 0, sizeof(OSContPad) * 4);
    if (gGamestate != RACING) {
        return;
    }
    if (sPlayback != NULL) {
        if (sRaceFrame < sPlaybackCount) {
            gControllerPads[0].button = sPlayback[sRaceFrame].button;
            gControllerPads[0].stick_x = sPlayback[sRaceFrame].stick_x;
            gControllerPads[0].stick_y = sPlayback[sRaceFrame].stick_y;
        }
        return;
    }
    unsigned long f = sRaceFrame;
    if (f % 20 == 0) {
        sSteerTarget = (int) (NextScript() % 161) - 80;
    }
    /* Ease towards the target so the steering looks like a thumb, not noise. */
    sSteer += (sSteerTarget - sSteer) / 4;

    u16 buttons = A_BUTTON;
    unsigned int r = NextScript();
    if ((f % 90) < 12 && (r & 1)) {
        buttons |= R_TRIG; /* hop / drift */
    }
    if (f % 150 == 75) {
        buttons |= Z_TRIG; /* use item */
    }
    gControllerPads[0].button = buttons;
    gControllerPads[0].stick_x = (s8) sSteer;
    gControllerPads[0].stick_y = 0;
}

static unsigned long long Fnv1a(unsigned long long h, const void* data, size_t len) {
    const unsigned char* p = (const unsigned char*) data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

/* Called after game_state_handler(): one line per race frame. */
void ReplayCheck_EndFrame(void) {
    if (!sActive || gGamestate != RACING) {
        return;
    }
    if (sRaceFrame == 0) {
        /* Grid sanity check: track, and each driver's character and start position. */
        fprintf(sLog, "# track=%s cup=%u pos=%zu\n", GetCupName(), GetCupIndex(), GetCupCursorPosition());
        for (int i = 0; i < NUM_PLAYERS; i++) {
            fprintf(sLog, "# grid p%d char=%d pos=%.1f,%.1f type=%04x\n", i, gPlayers[i].characterId,
                    gPlayers[i].pos[0], gPlayers[i].pos[2], gPlayers[i].type);
        }
    }
    unsigned long long h = 1469598103934665603ull;
    h = Fnv1a(h, gPlayers, sizeof(Player) * NUM_PLAYERS);
    h = Fnv1a(h, &gRandomSeed16, sizeof(gRandomSeed16));
    fprintf(sLog, "%lu %016llx rng=%04x p0=%.4f,%.4f,%.4f lap=%d rank=%d\n", sRaceFrame, h, gRandomSeed16,
            gPlayers[0].pos[0], gPlayers[0].pos[1], gPlayers[0].pos[2], gPlayers[0].lapCount,
            gPlayers[0].currentRank);
    sRaceFrame++;
    if (sRaceFrame % 30 == 0) {
        fflush(sLog);
    }
    /* Recording: stop 5 s after player 1 completes the third lap. */
    if (sRecord != NULL && sFinishedAt < 0 && gPlayers[0].lapCount >= 3) {
        sFinishedAt = (long) sRaceFrame;
        fprintf(sLog, "# player 1 finished at race frame %lu\n", sRaceFrame);
    }
    if (sRecord != NULL && sFinishedAt >= 0 && (long) sRaceFrame >= sFinishedAt + 150) {
        sMaxFrames = sRaceFrame;
    }
    if (sRaceFrame >= sMaxFrames) {
        if (sRecord != NULL) {
            fclose(sRecord);
            sRecord = NULL;
        }
        fprintf(sLog, "# done\n");
        fclose(sLog);
        sLog = NULL;
        if (getenv("MK64_SOAK_RUN") != NULL) {
            Port_RequestStop(); /* end this run; the soak starts the next */
            sActive = 0;
            return;
        }
        _exit(0);
    }
}
