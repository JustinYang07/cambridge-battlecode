#include "brain.h"
#include <algorithm>
#include <array>
#include "scorers.h"
#include "../core/config.h"
#include "../core/comms.h"
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
    // combatBotv3: one call per fight. Allies near us (within team_call_radius) claiming a fight or
    // a retreat vote; the majority is the team's call and we follow it (join their target, or
    // retreat without fighting). Badly hurt, we may still leave a fight.
    const bool hurt = w.hpFrac() < tune::commit_break_hp;
    int callFight = -1;  // enemy the team fights (-1 none)
    bool callRetreat = false;
    if (tune::team_fight_call && !w.enemies.empty()) {
        int retreatVotes = 0, fightVotes = 0;
        std::array<int, 16> votes{};
        for (int sl = 0; sl < w.allyCount && sl < 8; ++sl) {
            if (sl == w.slot) continue;
            const auto p = w.allyPosCached(sl);
            if (p.x < 0 || dist(p, w.pos) > tune::team_call_radius) continue;
            if (comms::retreating(sl)) {
                ++retreatVotes;
            } else if (const int e = comms::enemyOf(sl); e >= 0 && e < 16) {
                ++fightVotes;
                ++votes[static_cast<std::size_t>(e)];
            }
        }
        if (fightVotes > retreatVotes) {
            for (int e = 0; e < 16; ++e)
                if (votes[static_cast<std::size_t>(e)] > 0 &&
                    (callFight < 0 || votes[static_cast<std::size_t>(e)] > votes[static_cast<std::size_t>(callFight)]))
                    callFight = e;
        } else if (retreatVotes > fightVotes) {
            callRetreat = true;
        }
    }
    const bool teamCall = callFight >= 0 || callRetreat;
    const bool committed = tune::fight_commit_rounds > 0 && !teamCall && w.round < w.commitUntil && !hurt;
    int best = -1;
    float bestScore = -1;
    for (int i = 0; i < static_cast<int>(options.size()); ++i) {
        const auto& o = options[static_cast<std::size_t>(i)];
        const auto key = keyOf(o);
        float s = o.score;
        if (key == last) s *= tune::switch_hysteresis;            // changing our mind has a cost
        if (w.lockTurns > 0 && !emergency && key == w.lock) s = 1e9f;  // locked after flip-flopping
        if (w.stuckTurns > 0 && key == w.stuck) s *= tune::stuck_factor;  // it left us bouncing in place
        if (teamCall) {
            if (callRetreat && o.kind == OptionKind::fight) continue;
            if (callRetreat && o.kind == OptionKind::flee) s = std::max(s, 5e8f);
            if (callFight >= 0 && !hurt && o.kind == OptionKind::fight && o.target == callFight) s = std::max(s, 5e8f);
        } else if (committed) {
            if (w.commitKind == static_cast<int>(OptionKind::flee) && o.kind == OptionKind::fight) continue;
            if (w.commitKind == static_cast<int>(OptionKind::fight) && o.kind == OptionKind::fight && o.target == w.commitTarget)
                s = std::max(s, 5e8f);
        }
        if constexpr (kTracing) {
            // Lab exploration overrides locks: a declined fight is never picked, a forced one always.
            if (w.exploreMode == 1 && o.kind == OptionKind::fight) continue;
            if ((w.exploreMode == 2 || w.exploreMode == 3) && i == w.exploreForced) s = 2e9f;
        }
        if (s > bestScore) { bestScore = s; best = i; }
    }
    if (best < 0) return best;
    // Start a commitment when we enter a fight or a flight (it runs out, then we decide afresh).
    if (tune::fight_commit_rounds > 0) {
        const auto& b = options[static_cast<std::size_t>(best)];
        const bool fightNow = b.kind == OptionKind::fight, fleeNow = b.kind == OptionKind::flee;
        const bool same = w.round < w.commitUntil && w.commitKind == static_cast<int>(b.kind) && (fleeNow || w.commitTarget == b.target);
        if ((fightNow || fleeNow) && !same) {
            w.commitKind = static_cast<int>(b.kind);
            w.commitTarget = b.target;
            w.commitUntil = w.round + tune::fight_commit_rounds;
        }
    }
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
