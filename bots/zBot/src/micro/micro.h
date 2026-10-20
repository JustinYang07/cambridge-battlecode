#pragma once
// xBot: XSquare-style micro. With enemy champions close, every tile we could end the turn on (stay,
// or one of the 8 neighbours) gets a few per-tile numbers (damage we deal, damage we can take next
// turn, how far ahead of our front line it is, unseen bushes beside it, ...), and one score picks
// the tile. This replaces a chain of separate rules (kite, hold, front line, fall back) that each
// looked at one thing and whose order decided the move.
//
// Ranged champions either move or attack in a turn, so "stay and shoot" and "step" are the two
// kinds of choice. Melee champions walk and hit in one turn, so micro picks the tile they hit from
// once an enemy is in reach of one; their approach (path search, safe dash) stays with the fight code.
#include <cambc.h>
#include <optional>
#include "../core/world.h"

namespace micro {

enum class Intent {
    engage,  // the brain chose to fight `target`: shoot it (or whatever is in range), approach if safe
    hold,    // another plan (farm walk, regroup, idle) with enemies close: shoot what is in range,
             // step only when the tile we are on is clearly worse than a neighbour
};

struct Request {
    Intent intent = Intent::engage;
    std::optional<cambc::champion> target;  // engage: the enemy the brain picked
    cambc::vec2 at{-1, -1};                 // engage: where that enemy is
};

// Enemy champions close enough that the tile choice matters (within our range + 3).
bool applies(const World& w);
// Choose and play this turn's tile (and attack). False when micro has no opinion (hold: the
// current plan's own walk should run); the turn is then untouched.
bool run(World& w, const Request& r);

} // namespace micro
