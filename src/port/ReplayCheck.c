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

/* Script state (independent of the game's RNG). */
static unsigned int sScriptState;
static int sSteer = 0, sSteerTarget = 0;

static long EnvInt(const char* name, long def) {
    const char* v = getenv(name);
    return (v && *v) ? strtol(v, NULL, 0) : def;
}

static void Init(void) {
    sInitDone = 1;
    const char* path = getenv("MK64_REPLAY_CHECK");
    if (path == NULL || *path == '\0') {
        return;
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
    fprintf(sLog, "# ReplayCheck seed=%u rng=0x%04x cup=%d course=%d cc=%d char=%d frames=%lu player_size=%zu\n",
            sInputSeed, sRngSeed, sCup, sCourse, sCC, sChar, sMaxFrames, sizeof(Player));
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
    gDemoMode = DEMO_MODE_INACTIVE;
    gModeSelection = GRAND_PRIX;
    gCCSelection = sCC;
    gPlayerCount = 1;
    gScreenModeSelection = SCREEN_MODE_1P;
    gCupSelection = (s8) sCup;
    gCourseIndexInCup = (s8) sCourse;
    gCharacterSelections[0] = (s8) sChar;
    gIsMirrorMode = 0;
    gMenuSelection = RACING_DUPLICATE; /* anything but START_MENU, so setup_race uses the cup */
    gRandomSeed16 = (u16) sRngSeed;    /* setup_race uses the RNG for spawn order */
    gGamestateNext = RACING;
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
    /* Real controllers never reach the game during a check. */
    memset(gControllerPads, 0, sizeof(OSContPad) * 4);
    if (gGamestate != RACING) {
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
    unsigned long long h = 1469598103934665603ull;
    h = Fnv1a(h, gPlayers, sizeof(Player) * NUM_PLAYERS);
    h = Fnv1a(h, &gRandomSeed16, sizeof(gRandomSeed16));
    fprintf(sLog, "%lu %016llx rng=%04x p0=%.4f,%.4f,%.4f lap=%d rank=%d\n", sRaceFrame, h, gRandomSeed16,
            gPlayers[0].pos[0], gPlayers[0].pos[1], gPlayers[0].pos[2], gPlayers[0].lapCount,
            gPlayers[0].currentRank);
    sRaceFrame++;
    if (sRaceFrame >= sMaxFrames) {
        fprintf(sLog, "# done\n");
        fclose(sLog);
        sLog = NULL;
        _exit(0);
    }
}
