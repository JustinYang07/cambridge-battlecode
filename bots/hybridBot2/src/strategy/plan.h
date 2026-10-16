#pragma once
// Team plan: split the map into regions and give each squad one, so champions spread
// out over the map's resources instead of all chasing the camps nearest spawn.
//
// Everything is derived from the map itself (no per-map hard-coding) and computed
// identically by every allied bot from the same inputs (map, slots, champion types),
// so the team agrees on the plan without spending shared memory:
//   1. need: how many champions each camp group takes (from its real health/strength)
//   2. squads: pairs where the map's value is in group-needing camps, solos elsewhere
//   3. regions: seeds spread by direction from our spawn, camps assigned to the seed
//      nearest by walking distance (walls keep lanes apart), one BFS per region,
//      computed over the first few turns so no turn overruns its fuel
//   4. assignment: tanks lead pair squads, then farmers, then everyone else
#include <array>
#include <string>
#include <vector>
#include "../core/world.h"

namespace strategy {

struct Plan {
    bool ready = false;
    int regions = 0;
    std::vector<int> groupRegion;          // per camp group: region index, -1 = unassigned
    std::vector<int> groupNeed;            // per camp group: champions needed (1..4)
    std::array<int, 8> slotRegion{};       // per allied slot: home region
    std::array<int, 8> regionSize{};       // champions assigned to each region
    std::string summary;                   // for traces
};

// Advance the plan computation; cheap once ready. Call once per turn while alive.
void step(World& w);
const Plan& plan();
// Home region of this champion, or -1 while the plan is still being computed.
int homeRegion(const World& w);

} // namespace strategy
