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
void regroupOption(World& w, std::vector<Option>& out);
void hunt(World& w, std::vector<Option>& out);       // survive.cpp: after being stuck, go after enemies  // survive.cpp: join an ally who sees an enemy

float goldValue(const World& w);
// GGEzBot: damage per turn from awake camps whose reach covers the tiles beside this altar.
float campFireAt(const World& w, cambc::vec2 at);
float recallBankTurns(const World& w, int altarIdx);  // bank.cpp: recall-then-walk turns to this altar
// Our factor for this group's capture (<1: a teammate keeps it), and a teammate stands on its tiles now.
float captureShareOf(const World& w, int groupId);
bool allyHoldingOf(const World& w, int groupId);
// Value of one gem: above 1 while the enemy has tributed more than us.
float gemValue(const World& w);
// farm.cpp: every altar is blocked by a live camp group (Statics::altarBlockers): no tribute possible.
bool altarsBlocked(const World& w);
// farm.cpp: a team rush is on for this group (the map's main gem camp, enough of us alive).
bool teamRushOn(const World& w, int groupId);
// combatBotv3: capturing now (on a reward's spawn tiles) with no enemy champion within capture_hold_radius.
bool captureHold(const World& w);
// combatBotv3: a bush within 3 tiles of the group's centre (cached per group).
bool bushBeside(const GroupInfo& g);

} // namespace scorers
