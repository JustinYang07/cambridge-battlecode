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
void refresh(const World& w);  // re-reads the inventory only where buying is possible (or after a purchase)
// Buy toward the build plan if this champion is allowed to shop right now.
void spend(World& w);
// Keep one altar lock (and optionally a teleport crystal) in the bag; 10 gold each, spawn only.
void buyConsumables(World& w);
bool hasPassive(cambc::item_passive passive);
// yBot: a farming map (large, or medium with a long walk between spawns).
bool isFarmingMap();
// Progress through the build order, for traces: "jungle-str 3/5".
std::string progress();
// What a trip to the shop would buy right now: shards of the current build step we can afford,
// and whether they finish its emblem. Zero for Tavi, who shops anywhere, and at spawn, where
// spend() already bought them.
struct Affordable {
    int shards = 0;
    bool completes = false;
};
Affordable affordable(const World& w);
// Total shards bought so far (emblems count as their shards): a rough "how built are we".
int shardsOwned();

// Navigation found a crystal shortcut worth taking: keep teleport crystals from now on.
void wantCrystals();
bool crystalsWanted();
} // namespace shop
