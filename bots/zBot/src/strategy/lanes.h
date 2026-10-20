#pragma once
// yBot farming lanes (as Intercontinental Fish on expanse): on farming maps each champion owns a few
// camps on our half and works them alone (a single elk with a partner), cycling them as they
// respawn, instead of every champion re-picking the best camp each turn and the team bunching.
//
// The assignment is worked out once, the same way by every champion from the map alone (no
// messages): camps one or two of us can take (monster strength up to tune::lane_max_strength) on
// our half, nearest first, each to the champion whose lane is nearest and least loaded. Farm
// scoring then favours our lane's camps and leaves other lanes' camps to their owners while our
// own lane has a camp to work.
class World;

namespace lanes {

// Work out the lanes (first call) and this turn's state. Call once per turn before scoring.
void update(World& w);
bool active();
// The camp is in our lane.
bool mine(int groupId);
// The camp is in another champion's lane (and not ours).
bool others(int groupId);
// Our lane has a camp up now or back soon: stay on it.
bool laneBusy();

} // namespace lanes
