// Farming camp groups: kill every monster, then hold a spawn tile for the capture.
#include <algorithm>
#include <array>
#include "../combat/combat.h"
#include "../core/comms.h"
#include "../core/config.h"
#include "../core/fuel.h"
#include "../nav/fields.h"
#include "../nav/navigator.h"
#include "../shop/shop.h"
#include "../strategy/mapplan.h"
#include "../strategy/plan.h"
#include "../strategy/regroup.h"
#include "scorers.h"

namespace bc = cambc;

namespace scorers {


// Early, gold buys items that speed up everything after; late, it only breaks ties.
// Opening: monsters >= gold > gems. Endgame: gems >= monsters > gold.
float goldValue(const World& w) {
    float v;
    if (w.round < tune::opening_rounds) v = tune::gold_value_opening;
    else if (w.round < tune::late_game_round)
        v = tune::gold_value_opening + (tune::gold_value_late - tune::gold_value_opening) *
                static_cast<float>(w.round - tune::opening_rounds) / static_cast<float>(tune::late_game_round - tune::opening_rounds);
    else v = tune::gold_value_late;
    return w.kit.goldBonus ? v * 1.4f : v;  // Vesper Vale earns 20% more from every gold drop
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
    // Long walks cost more than their turns: a far camp is often contested or respawned by the
    // time we arrive, and walking past nearer camps leaves them to the enemy (collision: walking
    // 33 tiles past two elks to a richer corner camp).
    o.turns("travel", w.walkTurns(static_cast<float>(c.travel)) * tune::travel_weight);
    if (c.wait > 0) o.turns("respawn wait", static_cast<float>(c.wait) * tune::respawn_wait_weight);
    // routeBot (plan R3): the gems still have to reach an altar. One trip serves a whole bag,
    // so each camp pays its share of the walk from it to the nearest altar.
    if (g.gems > 0 && tune::altar_time_weight > 0) {
        int toAltar = 1 << 20;
        for (int i = 0; i < static_cast<int>(statics.altars.size()); ++i)
            if (const int d = fields::toAltar(i, g.center); d >= 0) toAltar = std::min(toAltar, d);
        if (toAltar < Navigator::unreachable)
            o.turns("to altar (share)", tune::altar_time_weight * static_cast<float>(toAltar) *
                                            std::min(1.0f, static_cast<float>(g.gems) / w.role().bankTarget));
    }

    bool awaiting = false;  // holding outside a party camp for allies
    const int claims = comms::claimsOn(g.id, w.slot);
    // Allies whose claim outranks ours: nearer to the camp, or as near and an earlier slot. Only they
    // keep us off a solo camp; a claim from across the map no longer does (it blocked the nearest
    // champion in 188 of 513 walk-pasts in the v1 A/B).
    const unsigned owners = comms::ownersMask(g.id, w.slot, dist(w.pos, g.center));
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
        // Seen, not assumed: an ally is already beside the fight and the monsters are not hitting
        // us, so they are on it. Vendetta used to recall from lanes' elk at half health while Varg
        // took over the hits right behind it.
        const bool allyTanks = !w.beingHit && w.allyTanking(g.id);
        const float share = allyTanks ? 0.05f : role.leadsPull ? 1.0f : tankLeads ? 0.05f : role.partyAbsorb;
        // With k attackers the fight is k times shorter and we absorb a share of the hits,
        // so per-head damage is solo * share / k (what campFight computes, without re-simulating).
        auto damageFor = [&](int k) {
            return k == 1 ? solo.damage : solo.damage * share / static_cast<float>(k);
        };
        int need = 1;
        while (need < tune::max_team_per_camp && damageFor(need) > budget) ++need;
        const int k = std::max(need, 1 + claims);
        const auto fight = k == 1 ? solo : combat::campFight(w, g, left, k, share);
        if (allyTanks) o.c.note << "ally tanking";
        else if (tankLeads) o.c.note << "tank soaks";
        if (role.followsParty && claims > 0) o.mul("support joins party", 1.5f);
        o.need = need;
        // Joining an ally who is waiting on a camp it cannot take without us unlocks the whole reward
        // for the team, not just our cut (Issac idled 30 turns on a respawn while Ren held beside an
        // elk for help on elkring).
        const bool unlocksAlly = claims > 0 && need > claims && need <= 1 + claims;
        if (k > 1 && !unlocksAlly) o.mul("our share", 1.0f / static_cast<float>(k));
        if (unlocksAlly) o.c.note << "ally needs us";
        o.turns("kill time", fight.turns);
        o.turns("capture", tune::capture_turns);
        o.expectDamage = fight.damage;

        const bool engaged = w.engagedGroup == g.id;
        // Only charge the walk-out when monsters are already hitting us. Charging it whenever we
        // stood inside a sleeping camp's range made the verdict flip with every step in and out
        // (Varg alternated farm / heal-first on collision for 50+ rounds).
        const float escapeCost = engaged ? static_cast<float>(nav().escapeTurns(w)) * solo.hitting : 0.0f;
        if (fight.damage > budget) {
            o.mul("too strong", 0.05f);
        } else if (fight.damage + reserve + escapeCost > w.hp) {
            if (engaged) o.mul("would not survive", 0.02f);  // disengage now; heal will win
            else {
                // Not doable at this health: this is really a reason to go home first.
                o.turns("heal first", 5.0f + static_cast<float>(dist(statics.spawnCenter, g.center)));
                o.mul("needs a trip home", tune::heal_first_factor);
                o.kind = OptionKind::heal;
            }
        }
        // Once a fight has started, waiting for help is no longer an option: finish or leave.
        awaiting = need > 1 + claims && !engaged;
        if (awaiting) {
            o.mul("awaiting help", 0.7f);
            o.turns("gather", 8.0f);
            // Help that never arrives: stop camping this group.
            if (w.lastTarget == g.id && w.waitingTurns > (claims > 0 ? tune::max_party_wait_claimed : tune::max_party_wait))
                o.mul("help not coming", 0.3f);
        }
        if (need > 1 && claims > 0) o.mul("join ally", 1.3f);
        // Under-levelled: build strength on wolves and clusters before taking elks.
        if (need > 1 && shop::shardsOwned() < tune::underpowered_shards) o.mul("underpowered", tune::underpowered_factor);
        if (need == 1 && owners) o.mul("nearer ally claimed", tune::ally_claim_factor);
        if (w.gems > 0 && fight.damage > 0)
            o.mul("bag at risk", std::max(0.4f, 1.0f - tune::carry_risk_per_gem * w.gems * fight.damage / std::max(w.hp, 1.0f)));
    } else {
        o.turns("capture", tune::capture_turns);
        if (owners) o.mul("nearer ally capturing", tune::ally_claim_factor);
        // An enemy on the spawn tiles contests the capture: progress resets every round, so the
        // reward is not 5 turns away (both teams' Vargs sat on packs' cluster trading hits for 14
        // turns, scoring capture 2109 until one died). Winning the fight is the way to the reward.
        for (const auto& e : w.enemies)
            if (e.hp > 0 && g.tileMask.is_set(e.pos)) {
                o.mul("contested by enemy", tune::contested_capture_factor);
                o.c.note << "contested";
                break;
            }
        // Standing on the tiles while enemies and monsters hit us: can we last the capture?
        const float incoming = combat::threatPerTurn(w, w.pos) + w.incoming;
        if (incoming > 0 && incoming * tune::capture_turns >= w.hp) o.mul("would not survive", 0.02f);
    }

    // Spread out: prefer camps in our squad's region, leave other squads' regions to them.
    const int home = strategy::homeRegion(w);
    const int region = home >= 0 ? strategy::plan().groupRegion[static_cast<std::size_t>(g.id)] : -1;
    if (g.id == mapplan::routeGroup()) {
        o.mul("route", tune::route_bonus);
        o.c.note << "route";
    }
    if (home >= 0 && region >= 0) {
        if (region == home) o.mul("home region", tune::home_region_bonus);
        else o.mul("other squad's region", tune::other_region_factor);
    }
    // Regrouping: stay with the spotter. Camps beside it are fine, the rest wait for the fight.
    if (regroup::active() && dist(g.center, regroup::at()) > tune::regroup_leash) o.mul("regrouping", tune::regroup_farm_factor);
    if (w.engagedGroup >= 0 && w.engagedGroup != g.id) {
        // Walking away from monsters that are hitting us costs health on the way out.
        o.turns("leave fight", static_cast<float>(nav().escapeTurns(w)));
        o.mul("abandon fight", tune::abandon_factor);
    }
    // Gem and gold clusters: quick, free and safe, a good use of time between bigger camps.
    // Waiting for a respawn is neither a quick safe kill nor finishing what we started: those
    // bonuses made a respawning gem cluster outscore every wolf (expanse), so they skip waits.
    const bool waiting = c.wait > 0;
    if (g.harmless && c.travel <= tune::safe_camp_radius && !waiting) o.mul("safe camp nearby", tune::safe_camp_bonus);
    if (c.wait > tune::idle_wait_turns) o.mul("idle wait", tune::idle_wait_factor);
    // Opening gold: items bought early speed up everything after; Vesper earns 20% more of it.
    if (g.gems == 0 && g.gold > 0 && w.round < tune::opening_rounds)
        o.mul("opening gold", w.kit.goldBonus ? tune::opening_gold_bonus * 1.3f : tune::opening_gold_bonus);
    if (mem.enemyNearRound >= 0 && w.round - mem.enemyNearRound <= 10) o.mul("enemy nearby", tune::contested_camp_factor);
    // Enemy champions around the camp: worse the more we carry and the more outnumbered we are.
    const float pressure = w.enemyPressure(g.center, tune::enemy_watch_radius);
    if (pressure > 0 && w.gems >= tune::bag_avoid_gems) {
        // Only a real bag in front of a stronger enemy group is worth giving up a camp for.
        const float ratio = pressure / std::max(w.alliedPower(g.center, 6), 1.0f);
        if (ratio > 1.0f) o.mul("contested", 1.0f / (1.0f + (ratio - 1.0f) * static_cast<float>(w.gems) / 40.0f));
    }
    // Holding for help earns no "committed" bonus: it made a 100-turn wait on corners stick.
    if (!waiting && !awaiting && w.lastTarget == g.id && (w.lastOption == static_cast<int>(OptionKind::farm) ||
                                             w.lastOption == static_cast<int>(OptionKind::capture))) {
        // Finish what we started, all the more when we have already put damage into it.
        const float done = std::clamp(1.0f - mem.healthLeft, 0.0f, 1.0f);
        o.mul("committed", tune::engaged_bonus + tune::focus_bonus_per_damage * done);
    }
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
    const float maxMultiplier = (tune::engaged_bonus + tune::focus_bonus_per_damage) * 1.5f * tune::home_region_bonus *
                                tune::safe_camp_bonus * tune::route_bonus;
    const float gv = goldValue(w);
    for (const auto& g : statics.groups) {
        if (n >= kMax) break;
        const auto& mem = w.groups[static_cast<std::size_t>(g.id)];
        if (g.gems == 0 && g.gold == 0) continue;
        const int reach = mem.status == GroupStatus::reward ? 0 : w.range;
        // Exact walking distance once its field exists (a tighter bound prunes more groups),
        // otherwise Chebyshev distance, which walking never beats.
        int cheb = fields::toGroup(g.id, w.pos);
        if (cheb < 0) {
            cheb = 1 << 20;
            for (auto t : g.tiles) cheb = std::min(cheb, dist(w.pos, t));
        }
        cheb = std::max(0, cheb - reach);
        const int wait = mem.status == GroupStatus::respawning ? std::max(0, mem.respawnAt - w.round - cheb) : 0;
        if (wait > 60) continue;  // too far in the future to plan around
        float reward = static_cast<float>(g.gems) + static_cast<float>(g.gold) * gv;
        // Keep the bound honest for the opening-gold bonus (applied per group, not to all).
        if (g.gems == 0 && g.gold > 0 && w.round < tune::opening_rounds) reward *= tune::opening_gold_bonus * 1.3f;
        float minKill = 0;
        if (mem.status != GroupStatus::reward) {
            const float left = mem.status == GroupStatus::alive && mem.seenRound >= 0 ? mem.healthLeft : 1.0f;
            minKill = g.totalHealth * left /
                      (std::max(combat::campDamagePerTurn(w, g), 1.0f) * static_cast<float>(tune::max_team_per_camp));
        }
        // Include the respawn wait: without it a respawning camp's bound looked huge, it took
        // the few fully-scored slots, and nearby wolves were never scored at all (expanse).
        // max(cheb, respawn) never exceeds the real travel + wait, so the bound stays valid.
        const float time = static_cast<float>(cheb + wait) + tune::capture_turns + minKill;
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
    bool bestNeedsHelp = false;
    // Fully score one candidate (walking distance, fight estimate) and add it to the options.
    auto scoreOne = [&](Candidate c) {
        const auto& g = *c.group;
        const auto& mem = w.groups[static_cast<std::size_t>(g.id)];
        const int reach = mem.status == GroupStatus::reward ? 0 : w.range;
        int travel = Navigator::unreachable;
        if (const int d = fields::toGroup(g.id, w.pos); d >= 0) {
            // Precomputed field: one local read. Aim at the straight-line nearest tile.
            if (d < Navigator::unreachable) travel = std::max(0, d - reach);
            int nearestD = 1 << 20;
            for (auto t : g.tiles)
                if (dist(w.pos, t) < nearestD) { nearestD = dist(w.pos, t); c.nearest = t; }
        } else {
            for (auto t : g.tiles) {
                const int dt = nav().distTo(w, t);
                if (dt < Navigator::unreachable && std::max(0, dt - reach) < travel) {
                    travel = std::max(0, dt - reach);
                    c.nearest = t;
                }
            }
        }
        // Beyond the BFS radius: estimate from straight-line distance (walls add ~25%).
        if (travel >= Navigator::unreachable) travel = static_cast<int>(static_cast<float>(c.travel) * 1.25f) + 4;
        if (c.travel > tune::bfs_radius * 2) return;
        c.travel = travel;
        c.wait = mem.status == GroupStatus::respawning ? std::max(0, mem.respawnAt - w.round - travel) : 0;
        auto o = scoreGroup(w, c);
        if (o.score > best) {
            // Waiting on help that has not claimed it: a solo option nearby may be better.
            bestNeedsHelp = o.kind == OptionKind::farm && o.need > 1 + comms::claimsOn(g.id, w.slot);
        }
        best = std::max(best, o.score);
        ++scored;
        out.push_back(std::move(o));
    };
    if (const int routeId = mapplan::routeGroup(); routeId >= 0)
        for (int i = 0; i < n; ++i)
            if (pre[static_cast<std::size_t>(i)].group && pre[static_cast<std::size_t>(i)].group->id == routeId) {
                auto c = pre[static_cast<std::size_t>(i)];
                pre[static_cast<std::size_t>(i)].group = nullptr;
                scoreOne(c);
                break;
            }
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

        scoreOne(c);
    }
    // The bound ignores whether we can take a camp: when every group it ranked first turned out
    // infeasible (corners' rich elk groups, "too strong" for one champion) the best score sat below
    // idling and Varg stood at spawn for 539 turns with a wolf 7 tiles away never scored. Then
    // also score the nearest groups not yet looked at.
    // Also when the best is a party camp nobody else is coming to (Cassi and Issac held at an elk
    // for 30+ turns on collision while the wolves they could solo were never scored).
    if (best < tune::idle_rescue_score || bestNeedsHelp)
        for (int extra = 0; extra < tune::idle_rescue_groups; ++extra) {
            int pick = -1;
            for (int i = 0; i < n; ++i)
                if (pre[static_cast<std::size_t>(i)].group &&
                    (pick < 0 || pre[static_cast<std::size_t>(i)].travel < pre[static_cast<std::size_t>(pick)].travel))
                    pick = i;
            if (pick < 0) break;
            auto c = pre[static_cast<std::size_t>(pick)];
            pre[static_cast<std::size_t>(pick)].group = nullptr;
            scoreOne(c);
        }
}

} // namespace scorers
