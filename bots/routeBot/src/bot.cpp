#include "bot.h"
#include <algorithm>
#include <cambc.h>
#include "brain/brain.h"
#include "strategy/mapplan.h"
#include "strategy/plan.h"
#include "strategy/regroup.h"
#include "core/comms.h"
#include "core/config.h"
#include "core/fixed_str.h"
#include "nav/fields.h"
#include "core/fuel.h"
#include "core/profile.h"
#include "core/trace.h"
#include "nav/navigator.h"
#include "shop/shop.h"

namespace bc = cambc;

namespace {
bool isGroupPlan(const Option& o) { return o.kind == OptionKind::farm || o.kind == OptionKind::capture; }
} // namespace

bool Bot::needsReplan(const World& w) const {
    if (!plan_ || planAge_ >= tune::replan_every || w.forceReplan) return true;
    if (w.beingHit || w.incoming > 0 || !w.enemies.empty() || w.gems != planGems_) return true;
    if (w.hpFrac() < 0.6f) return true;
    if (regroup::active()) return true;  // the spotter moves and the fight develops: rescore every turn
    // Holding for a party: rescore every turn so we leave as soon as something better turns up.
    if (w.waitingTurns > 0) return true;
    // Only steady, long-running plans are worth keeping; everything else is re-evaluated.
    if (!(isGroupPlan(*plan_) || plan_->kind == OptionKind::bank)) return true;
    if (isGroupPlan(*plan_) && plan_->target >= 0 &&
        w.groups[static_cast<std::size_t>(plan_->target)].status != planStatus_)
        return true;
    return false;
}

void Bot::play_turn() {
    const auto fuelAtStart = bc::fuel_remaining();
    const auto roundAtStart = static_cast<int>(bc::round_number());
    round_ = roundAtStart;
    fuel::beginTurn();
    auto& me = bc::self();
    const bool first = !initialised_;
    initialised_ = true;

    World& w = world_;
    w.refresh(first);
    if (first) {
        trace().configure(static_cast<int>(bc::champions(bc::team::ally).size()), w.slot, name(w.type));
        if (w.slot == 0) trace().legend();
    }
    trace().beginTurn(w.round, w.slot, name(w.type));
    trace().state(w.pos.x, w.pos.y, w.hp, w.maxHp, w.gems, w.gold, w.alive);

    comms::refresh();
    const bool wasRegrouping = regroup::active();
    regroup::update(w);
    if (regroup::active() != wasRegrouping) w.forceReplan = true;
    shop::refresh();
    shop::spend(w);
    shop::buyConsumables(w);
    fuel::mark("shop");

    if (!w.alive) {
        comms::clear(w.slot);
        trace().note("dead");
    } else if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) {
        comms::clear(w.slot);
        trace().action(me.is_recalling() ? "ch:rc" : me.is_submitting_gems() ? "ch:t" : "ch:tp");
    } else {
        nav().beginTurn();
        if (!strategy::plan().ready) {
            strategy::step(w);
            if (strategy::plan().ready) trace().forceFull();
        }
        const int routeBefore = mapplan::routeGroup();
        mapplan::follow(w);
        if (mapplan::routeGroup() != routeBefore) w.forceReplan = true;
        // Commit to a plan like a human would: rescoring every option costs most of a turn's
        // fuel, so only replan when something relevant changed.
        std::vector<Option> options;
        int chosen = -1;
        if (needsReplan(w)) {
            w.forceReplan = false;
            options = brain::think(w);
            // Vision changed (an enemy appeared or vanished, e.g. walking out of bushes): record the
            // full option breakdown even if the choice stays the same, or the decision explorer
            // keeps showing options scored before the new information.
            unsigned seen = 0;
            for (const auto& e : w.enemies)
                if (e.index >= 0 && e.index < 32) seen |= 1u << e.index;
            if (seen != seenEnemies_) {
                seenEnemies_ = seen;
                trace().forceFull();
            }
            chosen = brain::choose(options, w);
            if (chosen >= 0) {
                plan_ = options[static_cast<std::size_t>(chosen)];
                planAge_ = 0;
                planGems_ = w.gems;
                planStatus_ = plan_->target >= 0 && isGroupPlan(*plan_)
                                  ? w.groups[static_cast<std::size_t>(plan_->target)].status
                                  : GroupStatus::unknown;
            }
        } else {
            ++planAge_;
            options.push_back(*plan_);
            options.back().c.note.clear();
            options.back().c.note << "kept plan (" << planAge_ << ")";
            chosen = 0;
        }
        bool changed = true;
        if (chosen >= 0) {
            const auto& pick = options[static_cast<std::size_t>(chosen)];
            changed = w.lastOption != static_cast<int>(pick.kind) || w.lastTarget != pick.target;
            brain::execute(w, pick);
            brain::noteMovement(w);
        }
        fuel::mark("execute");

        // Trace the chosen option, then the best option of each other kind (so a rejected fight
        // or bank is visible even when several farm options outscore it), then the best of the rest.
        const Candidate* top[Trace::kMaxCandidates];
        int n = 0;
        if (chosen >= 0) top[n++] = &options[static_cast<std::size_t>(chosen)].c;
        // Every kind gets its best option recorded (a losing fight was cut when four slots went to
        // capture, heal, flee and farm); then pad with the next best up to trace_candidates.
        for (int pass = 0; pass < 2; ++pass) {
            const int limit = pass == 0 ? Trace::kMaxCandidates : std::min(Trace::kMaxCandidates, tune::trace_candidates);
            while (n < limit) {
                int best = -1;
                for (int i = 0; i < static_cast<int>(options.size()); ++i) {
                    const auto& o = options[static_cast<std::size_t>(i)];
                    if (std::find(top, top + n, &o.c) != top + n) continue;
                    // First pass: only kinds not yet shown.
                    if (pass == 0 && std::any_of(top, top + n, [&](const Candidate* c) { return c->kind == o.c.kind; })) continue;
                    if (best < 0 || o.score > options[static_cast<std::size_t>(best)].score) best = i;
                }
                if (best < 0) break;
                top[n++] = &options[static_cast<std::size_t>(best)].c;
            }
        }
        trace().candidates(top, n, changed);
        fuel::mark("trace");
    }
    // Every champion gets a turn every round (dead or channelling included), so a jump of more
    // than one round means the previous turn overran its fuel and swallowed this one.
    const bool lostOne = !first && roundAtStart - lastRound_ > 1;
    if (lostOne) lostTurns_ += roundAtStart - lastRound_ - 1;
    lastRound_ = roundAtStart;
    (void)fuelAtStart;
    trace().lostTurns(lostTurns_);
    if (lostOne || round_ % 50 == 0) {
        // The previous turn overran: say what it was doing so the lab can show the culprit.
        trace().note("LOST prev: " + fuel::keptReport() + " flushAt " + num(static_cast<long long>(prevFlushAt_ / 1000)) +
                     "k flushCost " + num(static_cast<long long>(prevFlushCost_ / 1000)) + "k" +
                     (prevFullTrace_ ? " full" : "") + " build " + num(prevBuild_) + " write " + num(prevWrite_) +
                     " bytes " + num(prevBytes_));
        trace().forceFull();
    }
    if (tune::trace_fuel_profile) trace().note(fuel::report());
    const auto beforeFlush = bc::fuel_remaining();
    prevFullTrace_ = trace().willWriteFull();
    trace().flush(fuel::split() ? 0 : fuel::used());
    fuel::keep();
    prevFlushAt_ = beforeFlush;
    prevFlushCost_ = beforeFlush - bc::fuel_remaining();
    prevBuild_ = trace().lastFullBuild_;
    prevWrite_ = trace().lastFullWrite_;
    prevBytes_ = trace().lastFullBytes_;
    // Spare fuel at the end of the turn builds the static distance fields scoring reads.
    if (!fuel::split()) fields::step(tune::fuel_reserve_fields);
}
