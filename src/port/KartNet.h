#ifndef KART_NET_H
#define KART_NET_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * KartNet: 2-player netplay, first version (delay-based lockstep over UDP).
 *
 * Host and guest exchange a match config (track, cc, characters, RNG seed),
 * jump straight into the race, then exchange one controller input per game
 * frame. Each side runs frame N only once it has both players' inputs for N;
 * local inputs are scheduled DELAY frames ahead to hide the round trip. Every
 * 30 frames both sides hash the race state and compare (desync check).
 *
 * Settings (environment variables; on iOS also lines KEY=VALUE in
 * Documents/netplay.cfg):
 *   MK64_NET=1              enable
 *   MK64_NET_HOST=1         this side hosts (player 1); otherwise guest (player 2)
 *   MK64_NET_BIND=ip:port   local UDP address (default 0.0.0.0:7101 host, :0 guest)
 *   MK64_NET_PEER=ip:port   the host's address (guest only)
 *   MK64_NET_SERVER=host:port  n64net room server: find the peer by room code
 *                           (then BIND/PEER are optional); MK64_NET_ROOM=ABCD,
 *                           MK64_NET_PASSWORD, MK64_NET_RELAY=1, MK64_NET_WAIT_SECS
 *   MK64_NET_ROLLBACK=1     rollback netcode (n64net_input); _WINDOW=8, _FROM=60
 *   MK64_NET_DELAY=2        minimum input delay in frames (host decides)
 *   MK64_NET_ADAPTIVE=1     raise the delay while the network is slow (0 = fixed)
 *   MK64_NET_MAX_DELAY=6    upper limit for the adaptive delay (rollback: 3; n64net auto delay)
 *   MK64_NET_LEGACY=1       lockstep through KartNet's own input exchange (old path, fallback)
 *   MK64_NET_SIM_LATENCY/_JITTER (ms), _LOSS (%), _SPIKE (ms for _SPIKE_LEN ms
 *                           every _SPIKE_EVERY s): simulated bad network (tests)
 *   MK64_NET_MODE=gp|vs     Grand Prix (6 CPUs) or Versus (host decides)
 *   MK64_NET_CUP/COURSE/CC  track and engine class (host decides)
 *   MK64_NET_CHAR=n         this side's character (0 Mario .. 7 Bowser)
 *   MK64_NET_SEED=n         RNG seed (host; default from the clock, sent to guest)
 *   MK64_NET_LOG=path       sync log (relative paths go to Documents on iOS)
 *   MK64_NET_FRAMES=n       quit after n netplay frames (tests)
 *   MK64_NET_BOT=n          scripted driver with seed n instead of the controller (tests)
 *   MK64_NET_TRACE_LAKITU=1 log the start countdown state (tests)
 */

void KartNet_BeginFrame(void);    /* before the gamestate switch */
void KartNet_OverrideInput(void); /* right after osContGetReadData */
void KartNet_EndFrame(void);      /* after game_state_handler */
int KartNet_IsActive(void);
/* The app is going to / coming back from the background (iOS): tells the peer
 * to wait instead of timing out. */
void KartNet_SetPaused(int paused);
/* Netplay view: each device shows only its own player, full screen
 * (MK64_NET_VIEW=full, the default; =split keeps the split screen).
 * main.c draws the split views as usual (they update per-screen state that
 * must match on both sides), then calls KartNet_RenderLocalView with the
 * display-list position from before them. */
int KartNet_WantsLocalView(void);
int KartNet_LocalPlayer(void); /* 0 host, 1 guest */
void KartNet_RenderLocalView(void* splitStart); /* a Gfx* */
int KartNet_SkipSplitRender(void);
/* Rollback time sync: this displayed frame runs no game logic (1 = hold). */
extern int gKartNetHoldFrame;
/* Start of each race frame: deterministic "shell on screen" bits (netplay). */
void KartNet_UpdateShellVisibility(void);
/* Zero the drawing-only fields in a copy of gPlayers (sync hash, SyncTest). */
void KartNet_MaskDrawingFields(void* players); /* MK64_NET_VIEW=single experiment */

#ifdef __cplusplus
}
#endif

#endif
