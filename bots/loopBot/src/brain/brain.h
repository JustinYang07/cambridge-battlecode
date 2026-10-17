#pragma once
// The brain gathers options from every scorer, picks the best one and carries it out.
#include <vector>
#include "../core/world.h"
#include "option.h"

namespace brain {

// keptCamp: last turn's farm or capture plan, re-used instead of scoring every camp when the turn
// is short of fuel or an enemy champion is close (null: no camp plan to keep).
std::vector<Option> think(World& w, const Option* keptCamp = nullptr);
// Best option, with a switching cost on changing our mind and a lock against A/B/A/B flipping.
int choose(const std::vector<Option>& options, World& w);
// Record where this turn left us; detects bouncing between two tiles under one decision.
void noteMovement(World& w);
void execute(World& w, const Option& option);

} // namespace brain
