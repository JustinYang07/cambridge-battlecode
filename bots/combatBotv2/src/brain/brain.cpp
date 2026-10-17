#include "brain.h"
#include "scorers.h"
#include "../core/config.h"
#include "../core/trace.h"
#include "../core/fuel.h"

namespace brain {

std::vector<Option> think(World& w, const Option* keptCamp) {
    std::vector<Option> options;
    options.reserve(Statics::get().groups.size() + 8);
    // Farm scoring is the most expensive scorer (~10k bounds + ~6k per scored camp). In a fight
    // the choice is fight, flee or the camp we were already on; a turn already short of fuel
    // would overrun and lose the next (swirl: every champion lost every other turn for 50 rounds).
    bool skipFarm = tune::farm_floor > 0 && cambc::fuel_remaining() < tune::farm_floor;
    if (!skipFarm && tune::farm_skip_enemy_reach >= 0)
        for (const auto& e : w.enemies)
            // An enemy on or by its own spawn heals there and is not fought: it must not stop us
            // farming (rushbait-v2: spawns 4 tiles apart, all ten champions stood still all game).
            if (e.hp > 0 && dist(e.pos, w.pos) <= std::max(w.range, e.range) + tune::farm_skip_enemy_reach &&
                !(tune::farm_skip_ignore_spawn && dist(e.pos, Statics::get().enemySpawnCenter) <= tune::enemy_spawn_radius))
                skipFarm = true;
    // combatBotv2: with an enemy about, survival and fight scoring go first and farm scoring gets
    // what is left (it stops at tune::farm_reserve): overrunning turns in fights spent ~25k on
    // farm scoring before reaching the fight. (No scorer reads another's options.)
    bool fighting = false;
    if (tune::fight_first)
        for (const auto& e : w.enemies)
            fighting = fighting || (e.hp > 0 && dist(e.pos, Statics::get().enemySpawnCenter) > tune::enemy_spawn_radius);
    if (fighting) {
        scorers::survive(w, options);
        fuel::mark("think.survive");
        scorers::fight(w, options);
        fuel::mark("think.fight");
    }
    if (!skipFarm) scorers::farm(w, options);
    else if (keptCamp) {
        options.push_back(*keptCamp);
        options.back().c.note.clear();
        options.back().c.note << "camp kept (farm not rescored)";
    }
    fuel::mark("think.farm");
    scorers::bank(w, options);
    fuel::mark("think.bank");
    if (!fighting) {
        scorers::survive(w, options);
        fuel::mark("think.survive");
        scorers::fight(w, options);
        fuel::mark("think.fight");
    }
    scorers::idle(w, options);
    scorers::regroupOption(w, options);
    scorers::hunt(w, options);
    fuel::mark("think.other");
    return options;
}

namespace {
std::pair<int, std::string> keyOf(const Option& o) { return {static_cast<int>(o.kind), std::string(o.c.target.view())}; }
} // namespace

int choose(const std::vector<Option>& options, World& w) {
    const auto& last = w.history[0];
    // Emergencies override any lock: badly hurt, or being hit while outnumbered by enemies.
    const bool emergency = w.hpFrac() < tune::lock_emergency_hp ||
                           (w.beingHit && w.enemyPressure(w.pos, 5) > w.alliedPower(w.pos, 5));
    int best = -1;
    float bestScore = -1;
    for (int i = 0; i < static_cast<int>(options.size()); ++i) {
        const auto& o = options[static_cast<std::size_t>(i)];
        const auto key = keyOf(o);
        float s = o.score;
        if (key == last) s *= tune::switch_hysteresis;            // changing our mind has a cost
        if (w.lockTurns > 0 && !emergency && key == w.lock) s = 1e9f;  // locked after flip-flopping
        if (w.stuckTurns > 0 && key == w.stuck) s *= tune::stuck_factor;  // it left us bouncing in place
        if (s > bestScore) { bestScore = s; best = i; }
    }
    if (best < 0) return best;
    if (w.lockTurns > 0) --w.lockTurns;
    if (w.stuckTurns > 0) --w.stuckTurns;

    // Shift history and look for A, B, A, B: commit to the newest choice for a few turns.
    for (std::size_t i = w.history.size() - 1; i > 0; --i) w.history[i] = w.history[i - 1];
    w.history[0] = keyOf(options[static_cast<std::size_t>(best)]);
    const auto& h = w.history;
    if (w.lockTurns == 0 && h[0] == h[2] && h[1] == h[3] && h[0] != h[1] && h[3].first >= 0) {
        w.lock = h[0];
        w.lockTurns = tune::flip_lock_turns;
        trace().note("flip-flopping: locked decision");
    }
    return best;
}

void noteMovement(World& w) {
    // Same decision, but we keep stepping back and forth between two tiles: that decision is
    // not making progress (an executor undoing its own step). Penalise it for a while.
    for (std::size_t i = w.posHistory.size() - 1; i > 0; --i) w.posHistory[i] = w.posHistory[i - 1];
    w.posHistory[0] = w.pos;
    const auto& p = w.posHistory;
    if (p[0] != p[1] && p[0] == p[2] && p[2] == p[4] && p[1] == p[3] && p[3] == p[5] && w.stuckTurns == 0 &&
        w.history[0] == w.history[1] && w.history[1] == w.history[2]) {
        w.stuck = w.history[0];
        w.stuckTurns = tune::stuck_turns;
        w.forceReplan = true;
        trace().note("bouncing between two tiles: penalising this decision");
    }
}

} // namespace brain
