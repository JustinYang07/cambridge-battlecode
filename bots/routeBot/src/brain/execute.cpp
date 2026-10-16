// Turning the chosen option into this turn's move / attack / cast actions.
#include <algorithm>
#include "../combat/combat.h"
#include "../core/comms.h"
#include "../core/config.h"
#include "../core/fixed_str.h"
#include "../core/trace.h"
#include "../nav/navigator.h"
#include "brain.h"

namespace bc = cambc;

namespace brain {

namespace {

bool groupMemberInRange(const World& w, int groupId) {
    for (const auto& c : w.camps)
        if (c.groupId == groupId && c.hp > 0 && dist(c.pos, w.pos) <= w.range) return true;
    return false;
}

bool groupVisiblyCleared(const World& w, const GroupInfo& g) {
    if (!w.groups[static_cast<std::size_t>(g.id)].visibleNow) return false;
    for (const auto& c : w.camps)
        if (c.groupId == g.id && c.hp > 0) return false;
    return true;
}

int alliesNear(const World& w, const GroupInfo& g) {
    int n = 0;
    for (const auto& a : w.allies) n += dist(a.pos, g.center) <= 6;
    return n;
}

void farmGroup(World& w, const GroupInfo& g, int need);
void capture(World& w, const GroupInfo& g);

// While waiting for the party, work a gem/gold cluster beside the camp instead of standing idle
// (Marcella held 40 turns beside lanes' north elk next to three gem clusters). Our claim stays on
// the big camp so the allies keep coming. Sleeping monsters do not attack, so the camp's own reach
// is fine; other monsters' reach is not.
bool farmWhileWaiting(World& w, const GroupInfo& g) {
    const auto& statics = Statics::get();
    const GroupInfo* best = nullptr;
    int bestDist = 1 << 20;
    for (const auto& other : statics.groups) {
        if (!other.harmless || dist(other.center, g.center) > tune::wait_farm_radius) continue;
        const auto& mem = w.groups[static_cast<std::size_t>(other.id)];
        if (mem.status == GroupStatus::respawning) continue;
        if ((other.tileMask & w.dangerExcept(g.id)).is_empty() == false) continue;
        if (comms::ownersMask(other.id, w.slot, dist(other.center, w.pos))) continue;  // a nearer ally is on it
        const int d = dist(other.center, w.pos);
        if (d < bestDist) { bestDist = d; best = &other; }
    }
    if (!best) return false;
    trace().note("waiting: farming a cluster nearby");
    if (w.groups[static_cast<std::size_t>(best->id)].status == GroupStatus::reward) capture(w, *best);
    else farmGroup(w, *best, 1);
    return true;
}

// Wait just outside monster range until enough allies have arrived to pull the group.
bool waitForParty(World& w, const GroupInfo& g, int need) {
    const auto& mem = w.groups[static_cast<std::size_t>(g.id)];
    // Readiness has hysteresis: allies hovering at the edge of the count radius used to flip
    // it every turn, so two champions stepped in and out waiting on each other.
    const bool committed = w.partyGroup == g.id && w.round <= w.partyReadyUntil;
    const bool partyReady = need <= 1 || committed || alliesNear(w, g) >= need - 1;
    if (partyReady && !committed && need > 1) {
        w.partyGroup = g.id;
        w.partyReadyUntil = w.round + tune::party_commit_turns;
    }
    // A fight under way does not wait for help: we are taking its hits, or an ally is beside the
    // damaged camp. Damage alone is not enough, since camps do not heal: Vendetta found lanes'
    // elk at 55% from its own earlier attempt and went back in alone three times.
    const bool underWay = (w.engagedGroup == g.id && w.beingHit) || (mem.healthLeft < 0.99f && alliesNear(w, g) > 0);
    if (underWay || partyReady) {
        w.waitingTurns = 0;
        return false;
    }
    ++w.waitingTurns;
    if (farmWhileWaiting(w, g)) return true;
    bc::bitboard ring, inner;
    ring.clear();
    inner.clear();
    for (auto t : g.tiles) {
        ring.draw_chebyshev(t, static_cast<float>(tune::monster_range + 2));
        inner.draw_chebyshev(t, static_cast<float>(tune::monster_range));
    }
    auto goals = ring & ~inner & ~w.danger;  // never wait inside another monster's reach
    FixedStr<24> msg;
    trace().note((msg << "need " << (need - 1 - alliesNear(w, g)) << " more").view());
    if (!goals.is_set(w.pos)) nav().stepToward(w, goals, false, &w.danger, g.center);
    else trace().action("hold");
    return true;
}

void farmGroup(World& w, const GroupInfo& g, int need) {
    combat::Focus focus{.groupId = g.id};
    const auto status = w.groups[static_cast<std::size_t>(g.id)].status;
    if (status == GroupStatus::respawning && g.tileMask.is_set(w.pos)) {
        trace().note("off spawn tile");
        auto goals = nav().standTiles(w, g.center, 2) & ~g.tileMask & ~w.danger;
        nav().stepToward(w, goals, false);
        return;
    }
    if (waitForParty(w, g, need)) return;
    if (groupMemberInRange(w, g.id)) {
        combat::act(w, focus, true);
    } else {
        // Aim at members that are still standing; dead members' tiles are empty and standing
        // next to one leaves us out of reach of the rest (a melee champion would idle forever).
        bc::bitboard goals;
        goals.clear();
        bool anyLive = false;
        for (const auto& c : w.camps)
            if (c.groupId == g.id && c.hp > 0) {
                goals |= nav().standTiles(w, c.pos, w.range);
                anyLive = true;
            }
        // Nothing in sight (bushes hide camps from range: Stellar stood 3 tiles from a cluster
        // behind a bush row for 333 turns, "in range" of tiles it could not see): walk up beside it.
        if (!anyLive)
            for (auto t : g.tiles) goals |= nav().standTiles(w, t, 1);
        // Never stand on a spawn tile unless capturing: an occupied tile stops the whole group respawning.
        goals = goals & ~g.tileMask;
        // Monsters hit the nearest champion: ranged allies stand at full range so whoever is
        // closer (ideally the tank) soaks the hits.
        if (w.ranged) {
            bc::bitboard closeIn;
            closeIn.clear();
            for (auto t : g.tiles) closeIn.draw_chebyshev(t, static_cast<float>(w.range - 1));
            const auto far = goals & ~closeIn;
            if (!far.is_empty()) goals = far;
        }
        auto avoid = w.dangerExcept(g.id);
        if (w.gems >= tune::bag_avoid_gems) avoid |= w.enemyReach();
        // Casters hit from 4 tiles: spend the turn casting rather than walking into range.
        combat::castFromRange(w, focus);
        nav().stepToward(w, goals, dist(w.pos, g.center) > 4, &avoid, g.center);
        combat::act(w, focus, false);
    }
    // Last monster just died: start standing on the capture tiles straight away.
    // Not while it is respawning: then the "step off the spawn tile" rule above fires next turn
    // and the two bounced champions between tiles for 100+ rounds.
    if (status != GroupStatus::respawning && groupVisiblyCleared(w, g) && !g.tileMask.is_set(w.pos) &&
        bc::self().can_move()) {
        trace().note("cleared");
        nav().stepToward(w, g.tileMask, false);
    }
}

void capture(World& w, const GroupInfo& g) {
    if (!g.tileMask.is_set(w.pos)) {
        nav().stepToward(w, g.tileMask, dist(w.pos, g.center) > 3, &w.danger, g.center);
        w.captureTurns = 0;
    } else {
        trace().action("cap");
        // A capture tile within tribute range of an altar: captures resolve before channels, and a
        // tribute banks the whole bag when it completes, including gems captured mid-channel. Start
        // a tribute timed to finish just as the capture does, and the reward is banked with it.
        if (w.lastTarget == g.id && w.lastOption == static_cast<int>(OptionKind::capture)) ++w.captureTurns;
        else w.captureTurns = 1;
        const int ticks = std::max(1, (w.gems + 9) / 10);
        if (w.gems > 0 && w.captureTurns + ticks >= static_cast<int>(tune::capture_turns)) {
            for (auto a : Statics::get().altars)
                if (dist(a, w.pos) <= bc::constants::submit_gems_range) {
                    try {
                        bc::self().submit_gems(a);
                        trace().note("tribute timed with the capture beside an altar");
                        return;
                    } catch (const bc::error&) {
                    }
                    break;
                }
        }
    }
    combat::act(w, combat::Focus{}, false);
}

void bank(World& w, int altarIdx) {
    auto& me = bc::self();
    const auto altar = Statics::get().altars[static_cast<std::size_t>(altarIdx)];
    // A tribute is a stationary channel only a stun or a kill cancels (damage does not): never start
    // one an enemy stunner can reach in time, or that the enemies about could kill us during.
    // Judged at the altar (not at our tile) so the verdict does not flip as we step toward it.
    const bool unsafe = w.tributeAtRisk(altar, w.gems);
    if (unsafe) {
        // Keep our distance instead of walking up and backing off every other turn.
        trace().note("unsafe to tribute: holding");
        if (dist(w.pos, altar) <= 1 || w.enemyPressure(w.pos, 3) > 0) {
            if (!nav().stepEscape(w)) nav().stepToward(w, Statics::get().allySpawn, true);
        }
        combat::act(w, combat::Focus{}, false);
        return;
    }
    auto avoid = w.danger;
    if (w.gems >= tune::bag_avoid_gems) avoid |= w.enemyReach();
    if (dist(w.pos, altar) > 1) nav().stepToward(w, altar, 1, dist(w.pos, altar) > 3, &avoid);
    if (dist(w.pos, altar) <= 1) {
        try {
            me.submit_gems(altar);
            FixedStr<24> msg;
            trace().action((msg << 't' << w.gems).view());
            return;
        } catch (const bc::error& e) {
            trace().note(std::string("tribute failed: ") + e.message());
        }
    }
    combat::act(w, combat::Focus{}, false);
}

std::vector<bc::vec2> nearbyThreats(const World& w) {
    std::vector<bc::vec2> threats;
    for (const auto& c : w.camps)
        if (c.strength > 0 && dist(c.pos, w.pos) <= tune::monster_range + 1) threats.push_back(c.pos);
    for (const auto& e : w.enemies)
        if (dist(e.pos, w.pos) <= e.range + 3) threats.push_back(e.pos);
    return threats;
}

void heal(World& w) {
    auto& me = bc::self();
    if (w.atSpawn) {
        trace().action("rest");
        return;
    }
    const int walk = nav().distTo(w, Statics::get().spawnCenter);
    const auto avoid = w.danger | w.enemyReach();
    if (walk <= tune::recall_turns) {  // walking home is no slower than the recall channel
        nav().stepToward(w, Statics::get().allySpawn, true, &avoid, Statics::get().spawnCenter);
        return;
    }
    // Enemy champions do chase: a 5-turn recall in their sight just gets interrupted, so walk
    // home along a route around their reach. One consistent move: previously "escape their
    // reach" and "walk home" alternated whenever an enemy stood between us and home, and the
    // champion bounced in place next to it (Varg on packs).
    bool enemyClose = false;
    for (const auto& g : w.ghosts)
        if (w.round - g.round <= 1 && dist(g.pos, w.pos) <= g.range + 5) enemyClose = true;
    if (enemyClose && !(w.stuckTurns > 0 && !w.beingHit)) {
        trace().note("enemy close: walking home around it");
        nav().stepToward(w, Statics::get().allySpawn, true, &avoid, Statics::get().spawnCenter);
        combat::act(w, combat::Focus{}, false);
        return;
    }
    // Monsters do not chase: get out of their reach first, then recall (damage would cancel it).
    const int leftGroup = w.engagedGroup;
    if (w.incoming > 0 && nav().escapeTurns(w) > 0) {
        if (!nav().stepEscape(w)) nav().stepAway(w, nearbyThreats(w), tune::monster_range);
        if (nav().escapeTurns(w) > 0) return;
        // An ally still beside the camp takes over the hits once we are out of reach: look again
        // next turn (rejoining may beat the trip home) instead of committing to a 5-turn recall.
        if (w.allyTanking(leftGroup)) {
            trace().note("ally tanking: rethinking before recall");
            combat::act(w, combat::Focus{}, false);
            return;
        }
    }
    try {
        me.recall();
        trace().action("rc");
    } catch (const bc::error& e) {
        trace().note(std::string("recall failed: ") + e.message());
        nav().stepToward(w, Statics::get().allySpawn, true);
    }
}

void flee(World& w) {
    std::vector<bc::vec2> threats;
    for (const auto& e : w.enemies) threats.push_back(e.pos);
    if (!nav().stepEscape(w) && !nav().stepAway(w, threats, 6)) nav().stepToward(w, Statics::get().allySpawn, true);
    combat::act(w, combat::Focus{}, false);
}

void fight(World& w, const Option& o) {
    combat::Focus focus{.enemy = o.enemy};
    if (dist(w.pos, o.at) <= w.range) {
        combat::act(w, focus, true);
    } else {
        nav().stepToward(w, o.at, w.range, true);
        combat::act(w, focus, false);
    }
}

} // namespace

void execute(World& w, const Option& o) {
    // Whatever we are doing, cancelling an enemy tribute or recall is worth a stun.
    combat::interruptChannels(w);
    combat::useAltarLock(w);
    const auto& statics = Statics::get();
    const bool farming = o.kind == OptionKind::farm || o.kind == OptionKind::capture;
    if (o.kind == OptionKind::fight) comms::claimEnemy(w.slot, o.target);
    else if (o.kind == OptionKind::wait && o.target == 2) comms::claimRegroup(w.slot);
    else if (farming) comms::claimGroup(w.slot, o.target, dist(w.pos, statics.groups[static_cast<std::size_t>(o.target)].center));
    else comms::clear(w.slot);

    switch (o.kind) {
    case OptionKind::farm: farmGroup(w, statics.groups[static_cast<std::size_t>(o.target)], o.need); break;
    case OptionKind::capture: capture(w, statics.groups[static_cast<std::size_t>(o.target)]); break;
    case OptionKind::bank: bank(w, o.target); break;
    case OptionKind::heal: heal(w); break;
    case OptionKind::flee: flee(w); break;
    case OptionKind::fight: fight(w, o); break;
    case OptionKind::wait:
        if (o.target == 2) nav().stepToward(w, o.at, tune::regroup_arrive - 1, true, &w.danger);  // regroup
        else if (!w.atSpawn) nav().stepToward(w, statics.allySpawn, false, nullptr, statics.spawnCenter);
        combat::act(w, combat::Focus{}, false);
        break;
    }
    combat::support(w);

    // Chaining: the camp we worked last turn is dead now (killed or captured): start the chain from it.
    if ((w.lastOption == static_cast<int>(OptionKind::farm) || w.lastOption == static_cast<int>(OptionKind::capture)) &&
        w.lastTarget >= 0 && w.lastTarget < static_cast<int>(w.groups.size()) && w.chainFrom != w.lastTarget) {
        const auto st = w.groups[static_cast<std::size_t>(w.lastTarget)].status;
        if (st == GroupStatus::reward || st == GroupStatus::respawning) {
            w.chainFrom = w.lastTarget;
            w.chainRound = w.round;
        }
    }
    if (w.lastOption == static_cast<int>(o.kind) && w.lastTarget == o.target) ++w.turnsOnOption;
    else w.turnsOnOption = 0;
    w.lastOption = static_cast<int>(o.kind);
    w.lastTarget = o.target;
}

} // namespace brain
