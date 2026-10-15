#pragma once
// Purchasing: follow a per-role build order of emblems (5 shards each).
#include <cambc.h>
#include "../core/profile.h"
#include "../core/world.h"

namespace shop {

enum class Family : std::uint8_t { monster, power, durability, treasure, jungle, home, bruiser };

struct Step {
    Family family;
    Stat stat;
};

// Read the inventory once per turn; cheap queries below use this snapshot.
void refresh();
// Buy toward the build plan if this champion is allowed to shop right now.
void spend(World& w);
// Keep one altar lock (and optionally a teleport crystal) in the bag; 10 gold each, spawn only.
void buyConsumables(World& w);
bool hasPassive(cambc::item_passive passive);
// Progress through the build order, for traces: "jungle-str 3/5".
std::string progress();
// Total shards bought so far (emblems count as their shards): a rough "how built are we".
int shardsOwned();

} // namespace shop
