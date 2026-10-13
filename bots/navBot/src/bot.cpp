#include "bot.h"
#include <algorithm>
#include <cambc.h>
#include "brain/brain.h"
#include "strategy/plan.h"
#include "core/comms.h"
#include "core/config.h"
#include "core/fixed_str.h"
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
    if (!plan_ || planAge_ >= tune::replan_every) return true;
    if (w.beingHit || w.incoming > 0 || !w.enemies.empty() || w.gems != planGems_) return true;
    if (w.hpFrac() < 0.6f) return true;
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
    shop::refresh();
    shop::spend(w);
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
        // Commit to a plan like a human would: rescoring every option costs most of a turn's
        // fuel, so only replan when something relevant changed.
        std::vector<Option> options;
        int chosen = -1;
        if (needsReplan(w)) {
            options = brain::think(w);
            chosen = brain::choose(options);
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
        }
        fuel::mark("execute");

        // Trace the chosen option first, then the best alternatives (partial selection, no sort of all).
        const Candidate* top[Trace::kMaxCandidates];
        int n = 0;
        if (chosen >= 0) top[n++] = &options[static_cast<std::size_t>(chosen)].c;
        while (n < std::min(Trace::kMaxCandidates, tune::trace_candidates)) {
            int best = -1;
            for (int i = 0; i < static_cast<int>(options.size()); ++i) {
                const auto* cand = &options[static_cast<std::size_t>(i)].c;
                if (std::find(top, top + n, cand) != top + n) continue;
                if (best < 0 || options[static_cast<std::size_t>(i)].score > options[static_cast<std::size_t>(best)].score) best = i;
            }
            if (best < 0) break;
            top[n++] = &options[static_cast<std::size_t>(best)].c;
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
    const auto report = fuel::report();
    if (lostOne || round_ % 50 == 0) {
        // The previous turn overran: say what it was doing so the lab can show the culprit.
        trace().note("LOST prev: " + prevReport_);
        trace().forceFull();
    }
    if (tune::trace_fuel_profile) trace().note(report);
    const auto beforeFlush = bc::fuel_remaining();
    const bool fullTrace = trace().willWriteFull();
    trace().flush(fuel::split() ? 0 : fuel::used());
    prevReport_ = report + " flushAt " + num(static_cast<long long>(beforeFlush / 1000)) + "k flushCost " +
                  num(static_cast<long long>((beforeFlush - bc::fuel_remaining()) / 1000)) + "k" + (fullTrace ? " full" : "") +
                  " build " + num(trace().lastFullBuild_) + " write " + num(trace().lastFullWrite_) + " bytes " + num(trace().lastFullBytes_);
}
