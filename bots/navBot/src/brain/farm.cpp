// Farming camp groups: kill every monster, then hold a spawn tile for the capture.
#include <algorithm>
#include <array>
#include "../combat/combat.h"
#include "../core/comms.h"
#include "../core/config.h"
#include "../core/fuel.h"
#include "../nav/navigator.h"
#include "../strategy/plan.h"
#include "scorers.h"

namespace bc = cambc;

namespace scorers {


float goldValue(const World& w) {
    return w.round >= tune::late_game_round ? tune::gold_value_late : tune::gold_value_early;
}

namespace {

struct Candidate {
    const GroupInfo* group;
    int travel, wait;
    bc::vec2 nearest;
    float bound;  // optimistic score: no option for this group can score higher
};

Option scoreGroup(World& w, const Candidate& c) {
    const auto& statics = Statics::get();
    const auto& g = *c.group;
    const auto& mem = w.groups[static_cast<std::size_t>(g.id)];
    Option o;
    o.kind = mem.status == GroupStatus::reward ? OptionKind::capture : OptionKind::farm;
    o.target = g.id;
    o.at = c.nearest;
    o.c.target << 'g' << g.id;
    o.add("gems", static_cast<float>(g.gems));
    o.add("gold", static_cast<float>(g.gold) * goldValue(w));
    o.turns("travel", static_cast<float>(c.travel));
    if (c.wait > 0) o.turns("respawn wait", static_cast<float>(c.wait) * tune::respawn_wait_weight);

    const int claims = comms::claimsOn(g.id, w.slot);
    if (o.kind == OptionKind::farm) {
        const float left = mem.status == GroupStatus::alive && mem.seenRound >= 0 ? mem.healthLeft : 1.0f;
        // Health we must keep to walk out of reach afterwards (monsters do not chase).
        const auto solo = combat::campFight(w, g, left, 1);
        const float reserve = solo.hitting * (tune::escape_margin_turns + 1.0f);
        const auto& role = w.role();
        const float budget = w.maxHp * role.maxFightHpUse - reserve;
        // Monsters hit the nearest champion, ties going to the earliest draft slot (measured from
        // 727 party fights). A tank stands adjacent and was drafted first, so it takes nearly every
        // hit while the rest of the party, ranged allies at full range especially, takes almost none.
        const bool tankLeads = !role.leadsPull && w.tankAmong(comms::claimersMask(g.id, w.slot));
        const float share = role.leadsPull ? 1.0f : tankLeads ? 0.05f : role.partyAbsorb;
        // With k attackers the fight is k times shorter and we absorb a share of the hits,
        // so per-head damage is solo * share / k (what campFight computes, without re-simulating).
        auto damageFor = [&](int k) {
            return k == 1 ? solo.damage : solo.damage * share / static_cast<float>(k);
        };
        int need = 1;
        while (need < tune::max_team_per_camp && damageFor(need) > budget) ++need;
        const int k = std::max(need, 1 + claims);
        const auto fight = k == 1 ? solo : combat::campFight(w, g, left, k, share);
        if (tankLeads) o.c.note << "tank soaks";
        if (role.followsParty && claims > 0) o.mul("support joins party", 1.5f);
        o.need = need;
        if (k > 1) o.mul("our share", 1.0f / static_cast<float>(k));
        o.turns("kill time", fight.turns);
        o.turns("capture", tune::capture_turns);
        o.expectDamage = fight.damage;

        const bool engaged = w.engagedGroup == g.id;
        const float escapeCost = static_cast<float>(nav().escapeTurns(w)) * solo.hitting;
        if (fight.damage > budget) {
            o.mul("too strong", 0.05f);
        } else if (fight.damage + reserve + escapeCost > w.hp) {
            if (engaged) o.mul("would not survive", 0.02f);  // disengage now; heal will win
            else o.turns("heal first", 5.0f + static_cast<float>(dist(statics.spawnCenter, g.center)));
        }
        // Once a fight has started, waiting for help is no longer an option: finish or leave.
        if (need > 1 + claims && !engaged) {
            o.mul("awaiting help", 0.7f);
            o.turns("gather", 8.0f);
            // Help that never arrives: stop camping this group.
            if (w.lastTarget == g.id && w.waitingTurns > tune::max_party_wait)
                o.mul("help not coming", 0.3f);
        }
        if (need > 1 && claims > 0) o.mul("join ally", 1.3f);
        if (need == 1 && claims > 0) o.mul("ally claimed", tune::ally_claim_factor);
        if (w.gems > 0 && fight.damage > 0)
            o.mul("bag at risk", std::max(0.4f, 1.0f - tune::carry_risk_per_gem * w.gems * fight.damage / std::max(w.hp, 1.0f)));
    } else {
        o.turns("capture", tune::capture_turns);
        if (claims > 0) o.mul("ally capturing", tune::ally_claim_factor);
    }

    // Spread out: prefer camps in our squad's region, leave other squads' regions to them.
    const int home = strategy::homeRegion(w);
    const int region = home >= 0 ? strategy::plan().groupRegion[static_cast<std::size_t>(g.id)] : -1;
    if (home >= 0 && region >= 0) {
        if (region == home) o.mul("home region", tune::home_region_bonus);
        else o.mul("other squad's region", tune::other_region_factor);
    }
    if (w.engagedGroup >= 0 && w.engagedGroup != g.id) {
        // Walking away from monsters that are hitting us costs health on the way out.
        o.turns("leave fight", static_cast<float>(nav().escapeTurns(w)));
        o.mul("abandon fight", 0.7f);
    }
    if (mem.enemyNearRound >= 0 && w.round - mem.enemyNearRound <= 10) o.mul("enemy nearby", 0.6f);
    // Enemy champions around the camp: worse the more we carry and the more outnumbered we are.
    const float pressure = w.enemyPressure(g.center, tune::enemy_watch_radius);
    if (pressure > 0 && w.gems >= tune::bag_avoid_gems) {
        // Only a real bag in front of a stronger enemy group is worth giving up a camp for.
        const float ratio = pressure / std::max(w.alliedPower(g.center, 6), 1.0f);
        if (ratio > 1.0f) o.mul("contested", 1.0f / (1.0f + (ratio - 1.0f) * static_cast<float>(w.gems) / 40.0f));
    }
    if (w.lastTarget == g.id && (w.lastOption == static_cast<int>(OptionKind::farm) ||
                                 w.lastOption == static_cast<int>(OptionKind::capture)))
        o.mul("committed", tune::engaged_bonus);
    o.finish();
    return o;
}

} // namespace

void farm(World& w, std::vector<Option>& out) {
    // Branch and bound: an optimistic bound per group (Chebyshev distance never exceeds
    // walking distance, the kill uses the largest party), then fully score groups in
    // bound order until no remaining bound can beat the best real score. The walking
    // distance BFS is only consulted for the few groups that get scored.
    const auto& statics = Statics::get();
    constexpr int kMax = 64;
    std::array<Candidate, kMax> pre;
    int n = 0;
    // Largest product of '*' factors any option can get (committed, join/support party, home region).
    const float maxMultiplier = tune::engaged_bonus * 1.5f * tune::home_region_bonus;
    const float gv = goldValue(w);
    for (const auto& g : statics.groups) {
        if (n >= kMax) break;
        const auto& mem = w.groups[static_cast<std::size_t>(g.id)];
        if (g.gems == 0 && g.gold == 0) continue;
        const int reach = mem.status == GroupStatus::reward ? 0 : w.range;
        int cheb = 1 << 20;
        for (auto t : g.tiles) cheb = std::min(cheb, dist(w.pos, t));
        cheb = std::max(0, cheb - reach);
        const int wait = mem.status == GroupStatus::respawning ? std::max(0, mem.respawnAt - w.round - cheb) : 0;
        if (wait > 60) continue;  // too far in the future to plan around
        const float reward = static_cast<float>(g.gems) + static_cast<float>(g.gold) * gv;
        float minKill = 0;
        if (mem.status != GroupStatus::reward) {
            const float left = mem.status == GroupStatus::alive && mem.seenRound >= 0 ? mem.healthLeft : 1.0f;
            minKill = g.totalHealth * left /
                      (std::max(combat::campDamagePerTurn(w, g), 1.0f) * static_cast<float>(tune::max_team_per_camp));
        }
        const float time = static_cast<float>(cheb) + tune::capture_turns + minKill;
        pre[static_cast<std::size_t>(n++)] = {&g, cheb, 0, g.center, reward / std::max(time, 1.0f) * 100.0f * maxMultiplier};
    }
    fuel::mark("farm.pre");

    float best = 0;
    int scored = 0;
    // Degrade gracefully when the turn is already expensive (big maps, crowded fights).
    const auto fuelLeft = bc::fuel_remaining();
    // Leave ~50k for executing, tracing and slack; spend the rest on scoring (~6k per group).
    const auto spare = fuelLeft > tune::fuel_for_full_scoring - 10000 ? fuelLeft - (tune::fuel_for_full_scoring - 10000) : 0;
    const int maxScored = std::clamp(static_cast<int>(spare / tune::fuel_per_scored_group), 1, tune::max_scored_groups);
    for (int round = 0; round < n; ++round) {
        // Selection instead of a full sort: we usually stop after a handful.
        int pick = -1;
        for (int i = 0; i < n; ++i)
            if (pre[static_cast<std::size_t>(i)].group && (pick < 0 || pre[static_cast<std::size_t>(i)].bound > pre[static_cast<std::size_t>(pick)].bound))
                pick = i;
        if (pick < 0) break;
        auto c = pre[static_cast<std::size_t>(pick)];
        pre[static_cast<std::size_t>(pick)].group = nullptr;
        // Stop when nothing left can win, and never score more than a handful (~6k fuel each).
        if ((c.bound < best && scored >= tune::trace_candidates) || scored >= maxScored) break;

        // Exact walking distance now that the group is worth a closer look.
        const auto& g = *c.group;
        const auto& mem = w.groups[static_cast<std::size_t>(g.id)];
        const int reach = mem.status == GroupStatus::reward ? 0 : w.range;
        int travel = Navigator::unreachable;
        for (auto t : g.tiles) {
            const int d = nav().distTo(w, t);
            if (d < Navigator::unreachable && std::max(0, d - reach) < travel) {
                travel = std::max(0, d - reach);
                c.nearest = t;
            }
        }
        // Beyond the BFS radius: estimate from straight-line distance (walls add ~25%).
        if (travel >= Navigator::unreachable) travel = static_cast<int>(static_cast<float>(c.travel) * 1.25f) + 4;
        if (c.travel > tune::bfs_radius * 2) continue;
        c.travel = travel;
        c.wait = mem.status == GroupStatus::respawning ? std::max(0, mem.respawnAt - w.round - travel) : 0;
        auto o = scoreGroup(w, c);
        best = std::max(best, o.score);
        ++scored;
        out.push_back(std::move(o));
    }
}

} // namespace scorers
