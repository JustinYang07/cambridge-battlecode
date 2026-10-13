#pragma once
// One Bot per champion bot instance. Folders:
//   core/    world snapshot, static map facts, tuning, traces, team comms, champion profiles
//   roster/  draft picks
//   nav/     pathfinding and movement
//   combat/  damage estimates, targeting, attacks, abilities
//   shop/    build orders and purchasing
//   brain/   option scorers (farm, bank, survive, fight), chooser, executor
#include <string>
#include <optional>
#include "brain/option.h"
#include "core/world.h"

class Bot {
public:
    void play_turn();

private:
    bool needsReplan(const World& w) const;
    std::optional<Option> plan_;   // last full decision, kept while nothing relevant changes
    int planAge_ = 0, planGems_ = 0;
    GroupStatus planStatus_ = GroupStatus::unknown;

    World world_;
    bool initialised_ = false;
    int lastRound_ = 0;
    int round_ = 0;
    int lostTurns_ = 0;   // turns lost to fuel overruns (shown in traces)
    std::string prevReport_;
};
