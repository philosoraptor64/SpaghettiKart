#include "RaceStart.h"

#include <libultraship.h>
#include "defines.h"
#include "main.h"
#include "menus.h"
#include "code_800029B0.h"
#include "buffers/random.h"
#include "port/Game.h"

void RaceStart_Request(const RaceStartParams* p) {
    gDemoMode = DEMO_MODE_INACTIVE;
    gModeSelection = p->mode;
    gCCSelection = p->cc;
    gPlayerCount = (s8) p->playerCount;
    gScreenModeSelection = (p->playerCount >= 2) ? SCREEN_MODE_2P_SPLITSCREEN_VERTICAL : SCREEN_MODE_1P;
    gCupSelection = (s8) p->cup;
    gCourseIndexInCup = (s8) p->course;
    /* The port picks the track (and whether this is race 1 of the cup, which makes
     * setup choose the CPU drivers and the grid order) from the World's current
     * cup, not from gCupSelection. Set it the way the cup menu would. */
    {
        void* cups[] = { GetMushroomCup(), GetFlowerCup(), GetStarCup(), GetSpecialCup() };
        int cup = (p->cup >= 0 && p->cup < 4) ? p->cup : 0;
        CM_SetCup(cups[cup]);
        CM_SetCupIndex((size_t) cup);
        SetCupCursorPosition((size_t) p->course);
    }
    gCharacterSelections[0] = (s8) p->chars[0];
    gCharacterSelections[1] = (s8) p->chars[1];
    gIsMirrorMode = 0;
    gMenuSelection = RACING_DUPLICATE; /* anything but START_MENU, so setup_race uses the cup */
    gRandomSeed16 = (u16) p->rng;      /* setup_race uses the RNG for spawn order */
    gGamestateNext = RACING;
}
