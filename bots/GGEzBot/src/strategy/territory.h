#pragma once
// Territories (user): the draft splits the map's camps into regions and gives each champion the
// region where it adds the most gems per round (mapplan::planTerritories). In game the region's
// camps score higher, camps far outside it lower, and the choice is revisited every
// tune::territory_replan rounds or when enemies show up in it: regions where enemies were seen
// recently count less, regions on their side count in full while most of their team is down, and
// each region's worth is shared among the allies already farming there.
struct GroupInfo;
class World;

namespace territory {

// Once per turn while alive: read the plan, note enemy sightings, re-pick the region when due.
void update(World& w);
// Our region (-1: none, or territories off).
int mine();
// Region of a camp group (-1: none).
int regionOf(int groupId);
// Farm score factor for this group: x territory_bonus in our region, x territory_off for camps
// outside it and farther than territory_near from us, 1 otherwise.
float farmFactor(const World& w, const GroupInfo& g);
// Share of the walk a camp in our region pays while we have not got there yet: the walk to a
// region is paid once for all its camps (in full, a camp 60 tiles off scored ~10x below one 5 off
// and no bonus could make up for it).
float travelFactor(const World& w, const GroupInfo& g);

} // namespace territory
