#pragma once
// Regrouping when enemies come onto our half.
//
// Vision is not shared, so the champion that sees an enemy on our half (the spotter) raises a
// team alert in shared memory (comms bytes 6-7: its own tile and how many enemies it sees) and
// refreshes it every turn it still sees them. Allies within tune::regroup_radius of the spotter
// respond: they walk to it, and while there only farm camps close by. The spotter keeps doing
// what it was doing; the survive scorer backs it off when outnumbered.
//
// The regroup lasts until the fight resolves: a kill, a death or a retreat all end with nobody
// seeing the enemy, so it ends tune::regroup_end_rounds after the alert was last refreshed.
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
// Enemies the spotter reported (1..4).
int enemies();
// A fresh alert (this round, ours or an ally's) was raised within `radius` of `p`: allies may be
// walking to it (comms::regroupers), so a fight there has help on the way.
bool alertNear(cambc::vec2 p, int radius);

} // namespace regroup
