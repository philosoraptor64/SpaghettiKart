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
| 4 Correctness | partial | to verify (owner) |
| 5 Determinism | done | ReplayCheck Mac == iPhone |
| 6 i64 module | done | SimHeap 128 MB from the arena (2cf3c3f93); to verify (owner) heap per run |
| 7 Online (lockstep) | done | KartNet sessions |
| 8 Rollback | done | SyncTest pass, rollback default |
| 9 Multiplayer features | partial | 2 players; 3-4 to verify (owner) |
| 10 iPhone | done | phones, two-phone sessions |

Status: done / partial / not started / n/a. Updated 2026-11-13 (Studio agent; owners please correct).

Known issues / open:
- to verify (owner)
