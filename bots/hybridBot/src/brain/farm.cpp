// Farming camp groups: kill every monster, then hold a spawn tile for the capture.
#include <algorithm>
#include <array>
#include <cmath>
#include "../combat/combat.h"
#include "../core/comms.h"
#include "../core/config.h"
#include "../core/trace.h"
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

// ---- the middle of the map
// On maps with a busy centre (see centreIsBusy), camps toward the geometric centre are worth more than their own reward: the middle is where
// rotations are short and the map is contested. The bonus scales with the camp's radius from the
// centre, each axis measured against half the map's size (so wide maps work too): x max at the
// centre, x1 in the corners.
namespace {
// Radius of a tile from the map centre: each axis against half the map's size, 0 centre .. 1 corner.
float centreRadius(bc::vec2 p) {
    const auto& s = Statics::get();
    const float hx = std::max(static_cast<float>(s.width) / 2.0f, 1.0f), hy = std::max(static_cast<float>(s.height) / 2.0f, 1.0f);
    const float dx = (static_cast<float>(p.x) + 0.5f - hx) / hx, dy = (static_cast<float>(p.y) + 0.5f - hy) / hy;
    return std::min(std::sqrt((dx * dx + dy * dy) / 2.0f), 1.0f);
}

// Only on maps whose centre is busier than their outskirts: more camps per area inside
// tune::centre_zone than outside it (monoliths excluded). Ring maps (elkring) and edge-heavy maps
// have no middle worth pulling toward: there the bonus cost up to ~470 gems/game.
bool centreIsBusy() {
    static int verdict = -1;
    if (verdict >= 0) return verdict == 1;
    int inner = 0, outer = 0;
    for (const auto& g : Statics::get().groups) {
        if (g.hasMonolith || (g.gems == 0 && g.gold == 0)) continue;
        (centreRadius(g.center) < tune::centre_zone ? inner : outer) += 1;
    }
    // Areas in the normalised square [-1,1]^2 (area 4): the inner zone is a disc of radius zone * sqrt(2).
    const float innerArea = 3.14159265f * 2.0f * tune::centre_zone * tune::centre_zone;
    const float outerArea = std::max(4.0f - innerArea, 1e-3f);
    verdict = static_cast<float>(inner) / innerArea > static_cast<float>(outer) / outerArea ? 1 : 0;
    trace().note(verdict ? "map centre is busy: middle bonus on" : "map centre is quiet: middle bonus off");
    return verdict == 1;
}
} // namespace

float centreBonus(const World& w, const GroupInfo& g) {
    if (!centreIsBusy()) return 1.0f;
    const float radius = centreRadius(g.center);
    // Healthier means bolder (none at half health), and never where visible enemies outnumber us.
    const float bold = std::clamp((w.hpFrac() - 0.5f) / 0.5f, 0.0f, 1.0f);
    if (bold <= 0) return 1.0f;
    if (w.enemyPressure(g.center, tune::enemy_watch_radius) > w.alliedPower(g.center, 6)) return 1.0f;
    return 1.0f + (tune::centre_bonus_max - 1.0f) * (1.0f - radius) * bold;
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
    // Endgame: gems still carried at the round limit count for nothing, so a camp whose gems cannot
    // reach an altar in time is worth only its gold (lifetime gold is the first tiebreak).
    bool gemsInTime = true;
    const int maxRounds = static_cast<int>(bc::constants::max_rounds);
    if (g.gems > 0 && w.round > maxRounds - tune::endgame_window) {
        int toAltar = 1 << 20;
        for (int i = 0; i < static_cast<int>(statics.altars.size()); ++i)
            if (const int d = fields::toAltar(i, g.center); d >= 0) toAltar = std::min(toAltar, d);
        const float ticks = static_cast<float>((w.gems + g.gems + 9) / 10);
        const float finish = static_cast<float>(w.round) + w.walkTurns(static_cast<float>(c.travel)) +
                             static_cast<float>(c.wait) + tune::capture_turns + static_cast<float>(toAltar) + ticks;
        gemsInTime = finish < static_cast<float>(maxRounds);
        if (!gemsInTime) o.c.note << "too late to bank; ";
    }
    o.add("gems", gemsInTime ? static_cast<float>(g.gems) : 0.0f);
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
    float killTurns = 0;    // estimated fight length (farm options)
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
        killTurns = fight.turns;
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
        // Party rotation: our party camp is down for a while and this one is up. The party's leader
        // (lowest slot) moves on first; mates follow when a mate already claims the new camp. While
        // rotating, the party is on its way, so we are not "awaiting help".
        bool rotating = false;
        if (tune::party_rotation && need > 1 && g.id != w.lastTarget && w.partyMates != 0) {
            if (comms::claimersMask(g.id, w.slot) & w.partyMates) {
                rotating = true;
                o.mul("follow the party", tune::rotate_bonus);
            } else if (w.lastTarget >= 0 && w.lastTarget < static_cast<int>(w.groups.size())) {
                const auto& from = w.groups[static_cast<std::size_t>(w.lastTarget)];
                // Leader: the lowest slot among mates still at our camp (one off banking cannot lead).
                const unsigned present = w.partyMates & comms::claimersMask(w.lastTarget, w.slot);
                const bool leader = (present & ((1u << w.slot) - 1u)) == 0;
                if (leader && from.status == GroupStatus::respawning && from.respawnAt - w.round >= tune::rotate_min_wait &&
                    mem.status != GroupStatus::respawning) {
                    rotating = true;
                    o.mul("lead the party on", tune::rotate_bonus);
                }
            }
        }
        // Once a fight has started, waiting for help is no longer an option: finish or leave.
        awaiting = need > 1 + claims && !engaged && !rotating;
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
    // Raid: their team is (nearly) wiped, so their half is open until the first of them respawns.
    // Clusters are free and quick (strong pull); monster camps take longer and they may come back
    // mid-fight (mild pull). Only camps we can finish in time, and bolder the healthier we are.
    if (w.enemyBodies >= 2 && w.enemiesAlive <= tune::raid_max_alive && w.enemyRespawnAt > w.round &&
        dist(g.center, statics.enemySpawnCenter) < dist(g.center, statics.spawnCenter)) {
        const float finish = static_cast<float>(w.round) + w.walkTurns(static_cast<float>(c.travel)) +
                             static_cast<float>(c.wait) + killTurns + tune::capture_turns;
        const float bold = std::clamp((w.hpFrac() - 0.5f) / 0.5f, 0.0f, 1.0f);
        if (finish <= static_cast<float>(w.enemyRespawnAt) && bold > 0) {
            const float full = g.harmless ? tune::raid_cluster_factor : tune::raid_camp_factor;
            o.mul("raid: their team is down", 1.0f + (full - 1.0f) * bold);
        }
    }
    // Their whole team is dead with time left to use it: take their nearest camps first. Ours will
    // still be there when the window closes, theirs will not be open again.
    if (w.enemyBodies >= 2 && w.enemiesAlive == 0 && w.enemyRespawnAt - w.round >= tune::raid_min_window &&
        dist(g.center, statics.enemySpawnCenter) >= dist(g.center, statics.spawnCenter)) {
        const float bold = std::clamp((w.hpFrac() - 0.5f) / 0.5f, 0.0f, 1.0f);
        if (bold > 0) o.mul("their team is dead: theirs first", 1.0f - (1.0f - tune::raid_own_half_factor) * bold);
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
    if (const float mid = centreBonus(w, g); mid > 1.001f) o.mul("towards the middle", mid);
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
                                tune::safe_camp_bonus * tune::route_bonus * tune::centre_bonus_max *
                                (w.enemyBodies >= 2 && w.enemiesAlive <= tune::raid_max_alive ? tune::raid_cluster_factor : 1.0f);
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
    // Deathball maps (close spawns): the whole team works one camp. The leader (lowest living slot) takes the nearest
    // camp or cluster that is up, and keeps it until it is captured; everyone else follows the
    // leader's claim, or keeps the last team camp while the leader is busy (banking, fighting).
    if (mapplan::read().deathball) {
        auto findPre = [&](int id) -> int {
            for (int i = 0; i < n; ++i)
                if (pre[static_cast<std::size_t>(i)].group && pre[static_cast<std::size_t>(i)].group->id == id) return i;
            return -1;
        };
        auto usable = [&](int i) {
            if (i < 0) return false;
            const auto& g = *pre[static_cast<std::size_t>(i)].group;
            return !g.hasMonolith && w.groups[static_cast<std::size_t>(g.id)].status != GroupStatus::respawning;
        };
        static int teamCamp = -1;
        const int leader = w.leaderSlot();
        int pick = -1;
        if (leader != w.slot) {
            const int i = findPre(comms::claimedGroup(leader));
            if (i >= 0 && !pre[static_cast<std::size_t>(i)].group->hasMonolith) pick = i;
        }
        if (pick < 0 && usable(findPre(teamCamp))) pick = findPre(teamCamp);
        if (pick < 0) {
            for (int i = 0; i < n; ++i)
                if (usable(i) && (pick < 0 || pre[static_cast<std::size_t>(i)].travel + pre[static_cast<std::size_t>(i)].wait <
                                                 pre[static_cast<std::size_t>(pick)].travel + pre[static_cast<std::size_t>(pick)].wait))
                    pick = i;
        }
        if (pick >= 0) {
            teamCamp = pre[static_cast<std::size_t>(pick)].group->id;
            const auto before = out.size();
            scoreOne(pre[static_cast<std::size_t>(pick)]);
            if (out.size() > before) {
                auto& o = out.back();
                if (o.score < tune::team_target_min_score) {
                    o.score = tune::team_target_min_score;
                    o.c.score = o.score;
                }
                o.c.note << (leader == w.slot ? "team camp (leader)" : "team camp");
                return;
            }
        }
    }
    // The route camp is scored first and does not count against the cap: on a tight turn it took
    // the only slot, and Issac followed a 140-tile gold route for 70 rounds without ever comparing
    // it with the wolves its party was farming beside spawn (rows).
    int routeScored = 0;
    if (const int routeId = mapplan::routeGroup(); routeId >= 0)
        for (int i = 0; i < n; ++i)
            if (pre[static_cast<std::size_t>(i)].group && pre[static_cast<std::size_t>(i)].group->id == routeId) {
                auto c = pre[static_cast<std::size_t>(i)];
                pre[static_cast<std::size_t>(i)].group = nullptr;
                scoreOne(c);
                routeScored = 1;
                break;
            }
    const int cap = routeScored ? std::max(maxScored, tune::route_rivals_scored) : maxScored;
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
        if ((c.bound < best && scored >= tune::trace_candidates) || scored - routeScored >= cap) break;

        scoreOne(c);
    }
    // A party camp allies are working is always worth comparing, even on a tight budget: a tank
    // that only scored its route camp kept walking away while the party waited for it (rows).
    for (int extra = 0, i = 0; i < n && extra < tune::claimed_party_scored; ++i) {
        auto& c = pre[static_cast<std::size_t>(i)];
        if (!c.group || c.group->harmless || comms::claimsOn(c.group->id, w.slot) == 0) continue;
        const auto copy = c;
        c.group = nullptr;
        scoreOne(copy);
        ++extra;
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
