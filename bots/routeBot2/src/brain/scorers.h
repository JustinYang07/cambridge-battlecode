#pragma once
// Each scorer proposes options of one kind. Add a new behaviour by writing a
// scorer and registering it in brain.cpp.
#include <vector>
#include "../core/world.h"
#include "option.h"

namespace scorers {

void farm(World& w, std::vector<Option>& out);      // farm.cpp: clear and capture camp groups
void bank(World& w, std::vector<Option>& out);      // bank.cpp: tribute carried gems
void survive(World& w, std::vector<Option>& out);   // survive.cpp: heal, flee
void fight(World& w, std::vector<Option>& out);     // fight.cpp: attack enemy champions
void idle(World& w, std::vector<Option>& out);      // survive.cpp: fallback positioning

float goldValue(const World& w);

} // namespace scorers
