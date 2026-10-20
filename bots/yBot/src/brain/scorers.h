#pragma once
#include <array>
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
// yBot: the fight network's chance we lose more champions than them near us within 15 rounds
// (-1: no enemy within 8 tiles). Computed once per turn.
float nnFightBad(const World& w);
// yBot: top teams' fight policy: chance they would retreat or recall here (-1: no enemy within 6).
float nnPolicyLeave(const World& w);
std::array<float, 4> nnPolicy(const World& w);  // hold, engage, retreat, recall (-1s: no enemy within 6)
void idle(World& w, std::vector<Option>& out);      // survive.cpp: fallback positioning
void regroupOption(World& w, std::vector<Option>& out);
void hunt(World& w, std::vector<Option>& out);       // survive.cpp: after being stuck, go after enemies  // survive.cpp: join an ally who sees an enemy

float goldValue(const World& w);
float recallBankTurns(const World& w, int altarIdx);  // bank.cpp: recall-then-walk turns to this altar
// Our factor for this group's capture (<1: a teammate keeps it), and a teammate stands on its tiles now.
float captureShareOf(const World& w, int groupId);
bool allyHoldingOf(const World& w, int groupId);
// Value of one gem: above 1 while the enemy has tributed more than us.
float gemValue(const World& w);
// farm.cpp: a team rush is on for this group (the map's main gem camp, enough of us alive).
bool teamRushOn(const World& w, int groupId);
// combatBotv3: capturing now (on a reward's spawn tiles) with no enemy champion within capture_hold_radius.
bool captureHold(const World& w);
// combatBotv3: a bush within 3 tiles of the group's centre (cached per group).
bool bushBeside(const GroupInfo& g);

} // namespace scorers
