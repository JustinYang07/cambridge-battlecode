#include "brain.h"
#include "scorers.h"
#include "../core/config.h"
#include "../core/trace.h"
#include "../core/fuel.h"

namespace brain {

std::vector<Option> think(World& w) {
    std::vector<Option> options;
    options.reserve(Statics::get().groups.size() + 8);
    scorers::farm(w, options);
    fuel::mark("think.farm");
    scorers::bank(w, options);
    scorers::survive(w, options);
    scorers::fight(w, options);
    scorers::idle(w, options);
    scorers::regroupOption(w, options);
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
    // Journey commitment: farming or walking to a camp, another camp must be clearly better.
    // Farm scores swing 20-40% from turn to turn as we walk, so the plain hysteresis let rivals
    // win often: 60% of the walking that never reached its camp was a switch to another camp
    // that was still there. A camp that stops being worth it (claimed by a nearer ally, contested,
    // not survivable) collapses by 4-20x, so those switches still happen; fights, flight, healing
    // and banking compete with the plain hysteresis.
    bool onJourney = false;
    for (const auto& o : options)
        if (keyOf(o) == last && (o.kind == OptionKind::farm || o.kind == OptionKind::capture)) onJourney = true;
    int best = -1;
    float bestScore = -1;
    for (int i = 0; i < static_cast<int>(options.size()); ++i) {
        const auto& o = options[static_cast<std::size_t>(i)];
        const auto key = keyOf(o);
        float s = o.score;
        if (key == last) s *= tune::switch_hysteresis;            // changing our mind has a cost
        else if (onJourney && (o.kind == OptionKind::farm || o.kind == OptionKind::capture)) s /= tune::farm_switch_margin;
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
