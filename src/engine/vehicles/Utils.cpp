#include <libultraship.h>
#include "Utils.h"

extern "C" {
#include "macros.h"
#include "defines.h"
#include "code_80005FD0.h"
}

uint32_t CalculateWaypointDistribution(size_t i, uint32_t numVehicles, size_t numWaypoints, uint32_t centerWaypoint) {
    return (uint32_t)(((i * numWaypoints) / numVehicles) + centerWaypoint) % numWaypoints;
}

uint32_t GetVehiclePathPointDistributed(std::vector<uint32_t>& existingTrains, uint32_t numWaypoints) {
    if (existingTrains.empty()) {
        return 0; // first train at start
    }

    // Sort trains along path
    std::sort(existingTrains.begin(), existingTrains.end());

    if (existingTrains.size() == 1) {
        // Place train halfway around the path
        return (existingTrains[0] + numWaypoints / 2) % numWaypoints;
    }

    uint32_t bestGap = 0;
    uint32_t bestPos = 0;

    for (size_t i = 0; i < existingTrains.size(); i++) {
        uint32_t start = existingTrains[i];
        uint32_t end = existingTrains[(i + 1) % existingTrains.size()];
        uint32_t gap = (end + numWaypoints - start) % numWaypoints;

        if (gap > bestGap) {
            bestGap = gap;
            bestPos = (start + gap / 2) % numWaypoints;
        }
    }

    return bestPos;
}


#include "Car.h"
#include "Bus.h"
#include "Truck.h"
#include "TankerTruck.h"
#include "engine/World.h"
extern "C" {
#include "camera.h"
#include "code_80005FD0.h"
extern s8 gPlayerCount;
}

void VehicleSteerHint(Camera* camera, u16 waypointIndex, s16 someType) {
    const s32 waypointCount = gPathCountByPathIndex[0];
    if (!(gPlayers[camera->playerId].speed < 1.6666666666666667)) {
        for (s32 i = 0; i < 0x18; i += 3) {
            if (((sSomeNearestPathPoint + i) % waypointCount) == waypointIndex) {
                gPlayerTrackPositionFactorInstruction[camera->playerId].target =
                    player_track_position_factor_vehicle(someType, gTrackPositionFactor[camera->playerId], waypointIndex);
                return;
            }
        }
    }
}

extern "C" void CM_VehicleSteerHints(void) {
    for (s32 s = 0; s < gPlayerCount && s < 4; s++) {
        Camera* camera = &camera1[s];
        for (auto& actor : GetWorld()->Actors) {
            AActor* a = actor.get();
            if (auto* car = dynamic_cast<ACar*>(a)) {
                VehicleSteerHint(camera, car->WaypointIndex, car->SomeType);
            } else if (auto* bus = dynamic_cast<ABus*>(a)) {
                VehicleSteerHint(camera, bus->WaypointIndex, bus->SomeType);
            } else if (auto* truck = dynamic_cast<ATruck*>(a)) {
                VehicleSteerHint(camera, truck->WaypointIndex, truck->SomeType);
            } else if (auto* tanker = dynamic_cast<ATankerTruck*>(a)) {
                VehicleSteerHint(camera, tanker->WaypointIndex, tanker->SomeType);
            }
        }
    }
}
