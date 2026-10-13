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
bool hasPassive(cambc::item_passive passive);
// Progress through the build order, for traces: "jungle-str 3/5".
std::string progress();

} // namespace shop
