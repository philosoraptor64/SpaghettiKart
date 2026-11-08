#ifndef RACE_START_H
#define RACE_START_H

#ifdef __cplusplus
extern "C" {
#endif

/* Everything needed to jump straight into a race without the menus. Used by
 * ReplayCheck and by netplay (KartNet), which must set up identical races. */
typedef struct {
    int mode;        /* GRAND_PRIX, VERSUS, ... */
    int playerCount; /* human players: 1 or 2 */
    int cup;         /* 0 Mushroom .. 3 Special */
    int course;      /* 0..3 within the cup */
    int cc;          /* CC_50 .. CC_150 */
    int chars[2];    /* characters of the human players */
    unsigned int rng; /* gRandomSeed16 at race setup (spawn order, CPU picks) */
} RaceStartParams;

/* Request the race; it starts when the gamestate switches on this frame. */
void RaceStart_Request(const RaceStartParams* p);

#ifdef __cplusplus
}
#endif

#endif
