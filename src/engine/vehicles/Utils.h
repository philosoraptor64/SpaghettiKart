#pragma once

#include <libultraship.h>

uint32_t CalculateWaypointDistribution(size_t i, uint32_t numVehicles, size_t numWaypoints, uint32_t centerWaypoint);
uint32_t GetVehiclePathPointDistributed(std::vector<uint32_t>& existingTrains, uint32_t numWaypoints);

extern "C" {
#include "camera.h"
}
// The cars', buses' and trucks' lane hint for the player a screen follows (what their Draw did for each screen's
// camera: gPlayerTrackPositionFactorInstruction, read by the race logic). Online every device draws only its own
// players, so the race logic gives the hint for every player's camera instead (KartNet_UpdateShellVisibility ->
// CM_VehicleSteerHints) and Draw leaves it alone.
void VehicleSteerHint(Camera* camera, u16 waypointIndex, s16 someType);
extern "C" int KartNet_IsActive(void);
extern "C" void CM_VehicleSteerHints(void);
