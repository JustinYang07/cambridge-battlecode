#pragma once
// Regrouping when enemies come onto our half.
//
// Vision is not shared, so the champion that sees an enemy on our half within
// tune::regroup_enemy_radius of itself (the spotter) raises a team alert in shared memory (comms
// bytes 6-7: its own tile, one or several enemies, and whether it is squishy) and refreshes it every
// turn that still holds. A squishy spotter (not a tank, or below tune::squishy_hp) calls allies from
// tune::regroup_radius_squishy with a stronger pull; a sturdy one only from tune::regroup_radius.
// Responders walk to it, and while there only farm camps close by. The spotter keeps doing what it
// was doing; the survive scorer backs it off when outnumbered.
//
// Once the enemy leaves the spotter's radius (or dies, or kills it) the alert lapses, and
// responders stand down tune::regroup_end_rounds later and go back to farming and banking.
// Hurt champions (below tune::regroup_min_hp) do not respond; healing comes first.
#include <cambc.h>

class World;

namespace regroup {

// Raise or refresh the alert if we see an enemy on our half, and update whether we respond.
// Call once per turn after comms::refresh().
void update(World& w);
// Responding to an alert this turn (healthy, not the spotter).
bool active();
// Where the spotter was last reported.
cambc::vec2 at();
// Enemies the spotter reported (1, or 2 for two or more).
int enemies();
// The spotter is fragile (not a tank, or hurt).
bool squishySpotter();
// combatBotv3: responding to a rally (an ally falling back from this many enemies); 0 otherwise.
int rallyEnemies();
// A fresh alert (this round, ours or an ally's) was raised within `radius` of `p`: allies may be
// walking to it (comms::regroupers), so a fight there has help on the way.
bool alertNear(cambc::vec2 p, int radius);

} // namespace regroup
