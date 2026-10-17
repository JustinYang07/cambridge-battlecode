#pragma once
// Following the draft's farming loop (mapplan::Loop) once the opening route is over.
//
// The loop's current camp gets a strong bonus in farm scoring and camps off the loop a penalty;
// the option brain still decides fights, healing, banking and regroups, and comes back to the
// loop afterwards. The party stays in step through the claims in shared memory: each member
// follows the lowest slot of its loop that is working a ready camp on it, otherwise walks the
// loop itself from where it last was. A camp that is down for longer than tune::loop_max_wait
// after we could reach it is skipped.
//
// Capture hand-off: when a loop camp's monsters are dead, its weakest damage dealer among the
// party there stays to capture (and takes the whole reward) while the others walk on to the next
// camp and start the kill. With enemies about, the whole party holds the capture.
#include <string>

class World;

namespace loop {

// Pick this turn's loop camp. Call once per turn while alive and acting, after mapplan::follow.
void update(World& w);
// Camp group the loop wants now (-1: no loop, or the opening route still runs).
int target();
// The group is on our loop.
bool onLoop(int groupId);
// We should hold this loop camp's capture; false when a loop mate there takes it and we move on.
bool capturer(const World& w, int groupId);
// The loop camp is too strong for us right now: skip it for tune::loop_skip_rounds.
void tooStrong(const World& w, int groupId);
std::string text();

} // namespace loop
