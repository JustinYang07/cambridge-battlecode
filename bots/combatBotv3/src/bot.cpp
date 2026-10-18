#include "bot.h"
#include <cstdint>
#include <variant>
#include <string>
#include <algorithm>
#include <cambc.h>
#include "brain/brain.h"
#include "strategy/cohesion.h"
#include "strategy/loop.h"
#include "strategy/wards.h"
#include "strategy/mapplan.h"
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

// ---- lab fight data (traced builds only)
// Deterministic per-round, per-slot coin in [0, 1).
float labCoin(int round, int slot) {
    std::uint32_t h = static_cast<std::uint32_t>(round) * 2654435761u ^ static_cast<std::uint32_t>(slot + 1) * 40503u ^ 0x9e3779b9u;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    return static_cast<float>(h & 0xffffu) / 65536.0f;
}

// A new contact, the same rule the fight dataset uses (a fight row due this turn, none in the
// previous 10 rounds), flips the fight decision with probability tune::fight_explore_rate for 10
// rounds: decline the fight the scorer wants, or take the best fight it would not.
void exploreFights(World& w, std::vector<Option>& options) {
    w.exploreMode = 0;
    w.exploreForced = -1;
    const auto& f = w.bestFight;
    if (f.round != w.round || f.dist > 8) return;  // no fight row this turn
    // Lab mode: take every fight where we are at least even in numbers around the target (visible),
    // so the data has no selection bias in that range.
    if (tune::fight_explore_even && f.us >= f.them) {
        for (int i = 0; i < static_cast<int>(options.size()); ++i) {
            const auto& o = options[static_cast<std::size_t>(i)];
            if (o.kind == OptionKind::fight && o.target == f.enemy) {
                w.exploreMode = 3;
                w.exploreForced = i;
                trace().note("explore: even, took the fight");
                return;
            }
        }
    }
    if (tune::fight_explore_rate > 0 && w.round - w.lastContactRound >= 10 && labCoin(w.round, w.slot) < tune::fight_explore_rate)
        w.exploreUntil = w.round + 10;
    w.lastContactRound = w.round;
    if (w.round >= w.exploreUntil) return;
    int best = -1, bestFight = -1;
    for (int i = 0; i < static_cast<int>(options.size()); ++i) {
        const auto& o = options[static_cast<std::size_t>(i)];
        if (best < 0 || o.score > options[static_cast<std::size_t>(best)].score) best = i;
        if (o.kind == OptionKind::fight && (bestFight < 0 || o.score > options[static_cast<std::size_t>(bestFight)].score)) bestFight = i;
    }
    if (bestFight < 0) return;
    if (options[static_cast<std::size_t>(best)].kind == OptionKind::fight) {
        w.exploreMode = 1;
        trace().note("explore: declined the fight");
    } else {
        w.exploreMode = 2;
        w.exploreForced = bestFight;
        trace().note("explore: took the fight");
    }
}

// One @F row per champion turn near a scored fight target (features the bot sees now):
// [r,u,enemy,chose fight,explore,edge%,us,them,ghosts,our hp,their hp,our dps,their dps,
//  my hp%,target hp%,their hp%,my bag,target bag,their bags,my stun ready,ally stuns ready,
//  enemy stunners,to our spawn,to their spawn,monster dps,dist,tx,ty,win%]
void logFight(World& w, const std::vector<Option>& options, int chosen) {
    const auto& f = w.bestFight;
    if (f.round != w.round || f.dist > 8) return;
    const bool choseFight = chosen >= 0 && options[static_cast<std::size_t>(chosen)].kind == OptionKind::fight;
    int myStun = 0;
    for (int k = 0; k < 2; ++k) {
        const auto& a = w.abilities()[static_cast<std::size_t>(k)];
        const auto& info = a.info();
        const auto* aoe = info ? std::get_if<bc::ability_aoe_damage>(&*info) : nullptr;
        if ((a.is_cc() || (aoe && aoe->duration > 0)) && w.abilityReady(k)) myStun = 1;
    }
    const bc::vec2 t{f.tx, f.ty};
    const auto& st = Statics::get();
    auto i = [](float v) { return std::to_string(static_cast<long>(v)); };
    std::string row = "@F[";
    row += std::to_string(w.round) + ',' + std::to_string(w.slot) + ',' + std::to_string(f.enemy) + ',' + (choseFight ? "1" : "0") + ',' +
           std::to_string(w.exploreMode) + ',' + i(f.edge * 100.0f) + ',' + std::to_string(f.us) + ',' + std::to_string(f.them) + ',' +
           std::to_string(f.ghosts) + ',' + i(f.ourHp) + ',' + i(f.theirHp) + ',' + i(f.ourDps) + ',' + i(f.theirDps) + ',' +
           i(w.hpFrac() * 100.0f) + ',' + i(f.tgtHpFrac * 100.0f) + ',' + i(f.theirHpFrac * 100.0f) + ',' + std::to_string(w.gems) + ',' +
           std::to_string(f.tgtBag) + ',' + std::to_string(f.theirBags) + ',' + std::to_string(myStun) + ',' +
           std::to_string(w.alliesStunReadyNear(t, 6)) + ',' + std::to_string(f.enemyStuns) + ',' + std::to_string(dist(t, st.spawnCenter)) + ',' +
           std::to_string(dist(t, st.enemySpawnCenter)) + ',' + i(f.monster) + ',' + std::to_string(f.dist) + ',' + std::to_string(f.tx) + ',' +
           std::to_string(f.ty) + ',' + i(f.winChance * 100.0f) + ']';
    trace().raw(row);
}
bool isGroupPlan(const Option& o) { return o.kind == OptionKind::farm || o.kind == OptionKind::capture; }
} // namespace

bool Bot::needsReplan(const World& w) const {
    if (!plan_ || planAge_ >= tune::replan_every || w.forceReplan) return true;
    if (w.beingHit || w.incoming > 0 || !w.enemies.empty() || w.gems != planGems_) return true;
    if (w.hpFrac() < 0.6f) return true;
    if (regroup::active()) return true;  // the spotter moves and the fight develops: rescore every turn
    // Holding for a party: rescore every turn so we leave as soon as something better turns up.
    if (w.waitingTurns > 0) return true;
    // A party camp we skipped for want of help gained a claimer: the party is forming (rows: Cassi
    // and Issac walked to the far gold for 10 turns while Varg was already on the wolves).
    for (int i = 0; i < awaitedCount_; ++i)
        if (comms::claimsOn(awaited_[static_cast<std::size_t>(i)].group, w.slot) > awaited_[static_cast<std::size_t>(i)].claims)
            return true;
    // Capturing: rescore every turn (the turn is idle anyway) so a second capturer hands off as soon
    // as a teammate stands on the tiles.
    if (plan_->kind == OptionKind::capture) return true;
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
        // Tanks act first (rules: within a team, higher priority acts first, then draft order). Its
        // camp claim is then visible to everyone the same round, and it reaches camps first to
        // take the monsters' hits. Takes effect from round 2.
        if (tune::tank_priority != 0 && isTank(w.type)) {
            try {
                me.set_priority(tune::tank_priority);
            } catch (const bc::error&) {
            }
        }
        trace().configure(static_cast<int>(bc::champions(bc::team::ally).size()), w.slot, name(w.type));
        if (w.slot == 0) trace().legend();
        if (w.slot == 0) {
            std::string sealed;
            for (const auto& g : Statics::get().groups)
                if (!g.reachable) sealed += " " + g.label;
            w.pendingNotes.push_back("unreachable groups:" + (sealed.empty() ? std::string(" none") : sealed) +
                                     (Statics::get().usedDraftBlock ? "; statics from draft" : "; statics computed"));
        }
    }
    trace().beginTurn(w.round, w.slot, name(w.type));
    trace().state(w.pos.x, w.pos.y, w.hp, w.maxHp, w.gems, w.gold, w.alive);
    if (!w.pendingNotes.empty()) {
        for (const auto& n : w.pendingNotes) trace().note(n);
        w.pendingNotes.clear();
        trace().forceFull();
    }

    // First tribute done: the bag emptied while we were banking (alive, so not a death).
    if (w.alive && w.prevGems > 0 && w.gems == 0 && w.lastOption == static_cast<int>(OptionKind::bank)) w.hasTributed = true;
    w.prevGems = w.gems;
    // Stuck watchdog: standing still while idle or holding for help (lanes: all six idle at spawn
    // for 1,700 rounds; stampede: one champion held an elk camp alone for 750).
    if (w.alive && !me.is_recalling() && !me.is_submitting_gems() && !me.is_teleporting()) {
        // Not stuck while there is something to hit, or while banking or healing.
        bool busy = w.lastOption == static_cast<int>(OptionKind::bank) || w.lastOption == static_cast<int>(OptionKind::heal) ||
                    w.spellClearRound >= w.round - 1;  // casting at a camp from range, waiting on the cooldown
        for (const auto& c : w.camps) busy = busy || (c.hp > 0 && dist(c.pos, w.pos) <= w.range);
        for (const auto& e : w.enemies) busy = busy || (e.hp > 0 && dist(e.pos, w.pos) <= w.range + 1);
        if (!busy && w.pos.x == w.stillPos.x && w.pos.y == w.stillPos.y) {
            if (++w.stillTurns >= (w.waitingTurns > 0 ? tune::still_hold_turns : tune::still_turns)) {
                if (w.lastOption == static_cast<int>(OptionKind::farm) || w.lastOption == static_cast<int>(OptionKind::capture))
                    w.stuckGroup = w.lastTarget;
                w.stuckUntil = w.round + tune::stuck_cooloff;
                w.huntUntil = w.round + tune::hunt_turns;
                w.stillTurns = 0;
                w.forceReplan = true;
                w.pendingNotes.push_back("stuck: dropping camp, hunting");
            }
        } else {
            w.stillTurns = 0;
            w.stillPos = w.pos;
        }
    }
    comms::refresh();
    // Capture board (teams of 5 bodies or fewer): we just captured a group -> tell the team; a
    // teammate did -> it respawns from now (rows: the far wolves sat respawned 18-44 rounds before
    // anyone came, nobody far away knew when they would be back).
    if (tune::capture_board && w.allyCount <= comms::capture_byte) {
        static int gemsBefore = 0;  // w.prevGems is already this turn's count here
        const int gained = w.gems - gemsBefore;
        gemsBefore = w.alive ? w.gems : 0;
        if (w.alive && gained > 0) {
            if (const auto* g = Statics::get().groupAt(w.pos); g && g->gems > 0 && gained >= g->gems / 6) {
                comms::announceCapture(g->id);
                auto& mem = w.groups[static_cast<std::size_t>(g->id)];
                mem.status = GroupStatus::respawning;
                mem.respawnAt = w.round + g->respawn;
                mem.healthLeft = 0;
            }
        }
        if (const int c = comms::newCapture(); c >= 0 && c < static_cast<int>(w.groups.size())) {
            auto& mem = w.groups[static_cast<std::size_t>(c)];
            mem.status = GroupStatus::respawning;
            mem.respawnAt = w.round + Statics::get().groups[static_cast<std::size_t>(c)].respawn;
            mem.healthLeft = 0;
        }
    }
    cohesion::update(w);
    const bool wasRegrouping = regroup::active();
    regroup::update(w);
    if (regroup::active() != wasRegrouping) w.forceReplan = true;
    shop::refresh(w);
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
        const int routeBefore = mapplan::routeGroup();
        mapplan::follow(w);
        if (mapplan::routeGroup() != routeBefore) w.forceReplan = true;
        const int loopBefore = loop::target();
        loop::update(w);
        if (loop::target() != loopBefore) w.forceReplan = true;
        // Commit to a plan like a human would: rescoring every option costs most of a turn's
        // fuel, so only replan when something relevant changed.
        std::vector<Option> options;
        int chosen = -1;
        fuel::mark("route");
        // Short of fuel already (a crowded fight, a big map): keep last turn's plan instead of
        // rescoring everything, unless we are in real danger. A turn that overruns loses the next.
        // Never keep a farm plan whose camp is down (killed and waiting for capture, or respawning),
        // and never for long: lanes' turns are tight every turn, and six champions kept "farm" on
        // three dead wolf camps for 500 rounds, stepping on and off the tiles, never capturing.
        static int tightStreak = 0;
        const bool planStale = plan_ && plan_->kind == OptionKind::farm && plan_->target >= 0 &&
                               plan_->target < static_cast<int>(w.groups.size()) &&
                               w.groups[static_cast<std::size_t>(plan_->target)].status != GroupStatus::alive;
        const bool tight = tune::think_floor > 0 && plan_ && bc::fuel_remaining() < tune::think_floor &&
                           w.hpFrac() >= tune::lock_emergency_hp && !planStale && tightStreak < tune::think_floor_max_streak;
        tightStreak = tight ? tightStreak + 1 : 0;
        if (tight) trace().note("fuel tight: kept plan");
        // combatBotv2: the first turn builds the static map; scoring every option on top of it
        // overran the 200k budget on big maps and lost rounds 2-3. With the fuel short, take the
        // draft plan's camp for this champion (route, loop or opening camp) and score from turn 2.
        int planned = -1;
        bool idleFirst = false;
        if (first && !plan_ && tune::first_turn_plan_floor > 0 && bc::fuel_remaining() < tune::first_turn_plan_floor) {
            const bool opening = mapplan::openingActive(w);
            planned = mapplan::routeGroup() >= 0 ? mapplan::routeGroup()
                      : loop::target() >= 0      ? loop::target()
                      : opening                  ? mapplan::read().openingCamp
                                                 : -1;
            if (planned >= 0 && planned < static_cast<int>(w.groups.size())) {
                Option o;
                o.kind = OptionKind::farm;
                o.target = planned;
                o.at = Statics::get().groups[static_cast<std::size_t>(planned)].center;
                o.need = opening && planned == mapplan::read().openingCamp ? std::max(1, mapplan::read().openingParty) : 1;
                o.c.note << "first turn: draft plan";
                options.push_back(o);
                chosen = 0;
                plan_ = o;
                planAge_ = 0;
                planGems_ = w.gems;
                planStatus_ = w.groups[static_cast<std::size_t>(planned)].status;
                w.forceReplan = true;  // score properly next turn
            } else {
                // No draft camp for us: wait this round and score next turn. Overrunning instead
                // finishes the same turn a round late, so nothing is lost, and the lost turn goes.
                planned = -1;
                idleFirst = true;
                w.forceReplan = true;
                trace().note("first turn: no plan camp, wait");
            }
        }
        if (planned < 0 && !idleFirst && !tight && needsReplan(w)) {
            w.forceReplan = false;
            const bool campPlan = plan_ && (plan_->kind == OptionKind::farm || plan_->kind == OptionKind::capture);
            options = brain::think(w, campPlan ? &*plan_ : nullptr);
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
            if constexpr (kTracing) exploreFights(w, options);
            chosen = brain::choose(options, w);
            if constexpr (kTracing) logFight(w, options, chosen);
            awaitedCount_ = 0;
            for (std::size_t i = 0; i < options.size() && awaitedCount_ < static_cast<int>(awaited_.size()); ++i) {
                const auto& o = options[i];
                if (static_cast<int>(i) == chosen || o.kind != OptionKind::farm || o.target < 0) continue;
                const int claims = comms::claimsOn(o.target, w.slot);
                if (o.need > 1 + claims) awaited_[static_cast<std::size_t>(awaitedCount_++)] = {o.target, claims};
            }
            if (chosen >= 0) {
                plan_ = options[static_cast<std::size_t>(chosen)];
                planAge_ = 0;
                planGems_ = w.gems;
                planStatus_ = plan_->target >= 0 && isGroupPlan(*plan_)
                                  ? w.groups[static_cast<std::size_t>(plan_->target)].status
                                  : GroupStatus::unknown;
            }
        } else if (planned < 0 && !idleFirst) {
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
            // Party rotation bookkeeping: who is working this party camp with us. Moving to another
            // party camp keeps the mates (so they can follow); a solo camp ends the party.
            if (pick.kind == OptionKind::farm || pick.kind == OptionKind::capture) {
                if (pick.target == w.partyCamp) w.partyMates |= comms::claimersMask(pick.target, w.slot);
                else {
                    if (pick.kind == OptionKind::farm && pick.need <= 1) w.partyMates = 0;
                    w.partyCamp = pick.target;
                }
            }
            brain::execute(w, pick);
            brain::noteMovement(w);
            wards::update(w);
        }
        fuel::mark("execute");

        // Trace the chosen option, then the best option of each other kind (so a rejected fight
        // or bank is visible even when several farm options outscore it), then the best of the rest.
        if constexpr (kTracing) {
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
        } else {
            (void)changed;
        }
        fuel::mark("trace");
    }
    // Every champion gets a turn every round (dead or channelling included), so a jump of more
    // than one round means the previous turn overran its fuel and swallowed this one.
    const bool lostOne = !first && roundAtStart - lastRound_ > 1;
    if (lostOne) lostTurns_ += roundAtStart - lastRound_ - 1;
    lastRound_ = roundAtStart;
    (void)fuelAtStart;
    trace().lostTurns(lostTurns_);
    if (kTracing && (lostOne || round_ % 50 == 0)) {
        // The previous turn overran: say what it was doing so the lab can show the culprit.
        trace().note("LOST prev: " + fuel::keptReport() + " flushAt " + num(static_cast<long long>(prevFlushAt_ / 1000)) +
                     "k flushCost " + num(static_cast<long long>(prevFlushCost_ / 1000)) + "k" +
                     (prevFullTrace_ ? " full" : "") + " build " + num(prevBuild_) + " write " + num(prevWrite_) +
                     " bytes " + num(prevBytes_));
        trace().forceFull();
    }
    if constexpr (kTracing) {
        if (tune::trace_fuel_profile) trace().note(fuel::report());
        const auto beforeFlush = bc::fuel_remaining();
        prevFullTrace_ = trace().willWriteFull();
        trace().flush(fuel::split() ? 0 : fuel::used());
        prevFlushAt_ = beforeFlush;
        prevFlushCost_ = beforeFlush - bc::fuel_remaining();
        prevBuild_ = trace().lastFullBuild_;
        prevWrite_ = trace().lastFullWrite_;
        prevBytes_ = trace().lastFullBytes_;
    }
    fuel::mark("flush");
    // Spare fuel at the end of the turn builds the static distance fields scoring reads.
    if (!fuel::split()) fields::step(tune::fuel_reserve_fields);
    fuel::mark("fields");
    // Kept after everything, so a turn that overran shows its whole split next turn.
    fuel::keep();
}
