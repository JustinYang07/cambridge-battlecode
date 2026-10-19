#pragma once
// Team cohesion: the whole team farms around one leader, like the ladder's top teams.
//
// The leader is the active champion (out farming or fighting: its shared-memory claim is set)
// with the highest damage (profile farmPower; ties to the lower slot). Everyone computes the same
// answer from the shared claims, so no extra bytes are needed. Followers get a soft pull: the
// leader's camp and its nearest camps score more, camps elsewhere less. Banking, healing and
// recalls stay individual; a champion doing them drops out (its claim clears) and rejoins after.
// Fights are taken as a group: with less than half the active team near us, fight scores drop.
#include <cambc.h>

class World;

namespace cohesion {

// Recompute the leader and its camp. Call once per turn after comms::refresh().
void update(const World& w);
// Allied slot leading the team (-1: none active).
int leader();
bool isLeader(const World& w);
// Camp group the leader is working (-1: none, or we lead).
int leaderCamp();
// Active champions on our team (us included).
int activeCount();

} // namespace cohesion
