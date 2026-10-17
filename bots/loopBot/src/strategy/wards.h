#pragma once
// Wards on the halfway line between our territory and theirs: walkable tiles about as far from
// their spawn as from ours (within tune::ward_line_width). Crossing it, a champion with a free
// ward charge places one there, the tile nearest the map centre within reach: enemies coming
// onto our half are seen before they arrive. Each champion keeps one ward active at a time
// (game rule), so in practice it is the first crossing. Skips tiles near a ward we know of and
// backs off when out of charges.
// Bush wards (tune::ward_bushes): moving toward a bush tile we cannot see into, one ward goes on
// it, so whoever hides there is seen before we arrive.
class World;

namespace wards {

// Try to place a ward this turn. Call after the turn's action, while alive and not channelling.
void update(World& w);

} // namespace wards
