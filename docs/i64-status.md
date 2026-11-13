# Mario Kart 64 in i64: status

ROM: Mario Kart 64 (USA), SHA-1 579c48e211ae952530ffc8738709f078d5dd215e  
Route: A, HarbourMasters SpaghettiKart (libultraship)  
Owner: MacBook agent  
Process and gate definitions: https://github.com/philosoraptor64/n64net/blob/main/docs/new-game-process.md

| Gate | Status | Evidence |
|---|---|---|
| 0 Intake | done |  |
| 1 Code map | n/a | existing port |
| 2 Builds | done |  |
| 3 Boots and plays | done |  |
| 4 Correctness | partial | user-played GP/VS/Battle on Mac and phones (2026-11-08/09: drift sparks, HUD, minimap, race start, battle balloons fixed); lightning flash never seen on a phone |
| 5 Determinism | done | ReplayCheck Mac == iPhone |
| 6 i64 module | done | SimHeap 128 MB from the arena (2cf3c3f93); heap per run flat (Mac, 3 runs: 18.4 -> 18.9 -> 19.0 MB); heap during play 63 MB after the LUS texture-buffer fix (6fa2d9249) |
| 7 Online (lockstep) | done | KartNet sessions |
| 8 Rollback | done | SyncTest pass, rollback default |
| 9 Multiplayer features | partial | 2 players online (one per device); 3-4 players and several players per device not built (i64 docs/sessions-more-than-two.md) |
| 10 iPhone | done | phones, two-phone sessions |

Status: done / partial / not started / n/a. Updated 2026-11-13 (Studio agent; corrected by the MacBook agent, owner).

Known issues / open:
- 3-4 players / local split screen online (design pending the owner's go)
- footprint left after a run ~160 MB on the Mac (allocator fragmentation + Metal surfaces), ~150 MB on an iPhone before the 2026-11-13 fixes: re-measure on the phones
- lightning flash in the full per-device view: unverified on a phone
- the home-screen picture includes the "Online" overlay button
