#pragma once
// Walking distances to every camp group and altar, from terrain alone (terrain never changes).
//
// Scoring used to run a fresh BFS from our tile on every replanning turn, then read camp and
// altar-ring tiles through the host (~200 fuel each). Instead, one field per group and per altar
// ring is computed once, a few per turn from spare fuel at the end of turns, and exported, so a
// distance becomes a local array read. Until a field is ready, callers fall back to the BFS.
#include <cambc.h>

namespace fields {

// Compute pending fields while more than `reserve` fuel is left. Call at the end of a turn.
void step(unsigned long long reserve);
// Walking distance from `from` to the nearest tile of the group / to a tile beside the altar.
// -1 while the field is not computed yet; Navigator::unreachable when there is no path.
int toGroup(int groupId, cambc::vec2 from);
int toAltar(int altarIndex, cambc::vec2 from);
// loopBot: walking distance to our spawn (heal and shop trips).
int toSpawn(int /*unused*/, cambc::vec2 from);
// Build a missing field now (~3-12k, big maps 55k): the walk that needs it is this turn.
void ensureGroup(int groupId);
// The most a field has cost so far: gate on-demand builds with it.
unsigned long long buildCost();
void ensureAltar(int altarIndex);
void ensureSpawn(int /*unused*/ = 0);

// Fields rooted at single tiles (hole-crossing launch tiles): registerPoint returns an id for the
// tile (the same id for the same tile), built from spare fuel in step() like the others.
int registerPoint(cambc::vec2 tile);
int toPoint(int id, cambc::vec2 from);
void ensurePoint(int id);

} // namespace fields
