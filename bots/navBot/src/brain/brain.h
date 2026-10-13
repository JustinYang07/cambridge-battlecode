#pragma once
// The brain gathers options from every scorer, picks the best one and carries it out.
#include <vector>
#include "../core/world.h"
#include "option.h"

namespace brain {

std::vector<Option> think(World& w);
int choose(const std::vector<Option>& options);
void execute(World& w, const Option& option);

} // namespace brain
