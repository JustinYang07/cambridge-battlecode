// Survival: heal (walk home or recall), flee from losing fights, and the idle fallback.
#include <algorithm>
#include "../combat/combat.h"
#include "../core/comms.h"
#include "../core/config.h"
#include "../core/statics.h"
#include "../nav/navigator.h"
#include "../shop/shop.h"
#include "../strategy/regroup.h"
#include "nnrecall.h"
#include <array>
#include <cmath>
#include "scorers.h"

namespace bc = cambc;

namespace scorers {

// yBot: logit of the top team recalling now (lab/nn/recall_dataset.py: same features, same order).
float nnRecallLogit(const World& w) {
    static std::array<float, 4> hpAgo{};
    static int hpRound = -100;
    if (w.round - hpRound != 1) hpAgo.fill(w.hp);
    hpRound = w.round;
    const float dmg = std::max(0.0f, hpAgo[2] - w.hp) / std::max(w.maxHp, 1.0f);
    hpAgo[3] = hpAgo[2]; hpAgo[2] = hpAgo[1]; hpAgo[1] = hpAgo[0]; hpAgo[0] = w.hp;
    const auto& st = Statics::get();
    int da = 1 << 20;
    for (auto a : st.altars) da = std::min(da, dist(a, w.pos));
    int en = 0, al = 0;
    for (const auto& e : w.enemies) en += e.hp > 0 && dist(e.pos, w.pos) <= 8;
    for (const auto& a : w.allies) al += dist(a.pos, w.pos) <= 6;
    bool campNear = false;
    for (const auto& c : w.camps) campNear = campNear || (c.hp > 0 && c.strength > 0 && dist(c.pos, w.pos) <= c.range);  // "a monster can hit us"
    const bool justCap = w.lastOption == static_cast<int>(OptionKind::capture);
    const std::array<float, nnrecall::kIn> x{
        w.hpFrac(), static_cast<float>(w.gems) / 60.0f, static_cast<float>(dist(w.pos, st.spawnCenter)) / 40.0f,
        static_cast<float>(da) / 40.0f, static_cast<float>(en) / 3.0f, static_cast<float>(al) / 3.0f,
        static_cast<float>(w.round) / 1000.0f, w.maxHp / 1500.0f, dmg * 10.0f, static_cast<float>(w.gold) / 300.0f,
        campNear ? 1.0f : 0.0f, justCap ? 1.0f : 0.0f};
    return nnrecall::logit(x);
}

void survive(World& w, std::vector<Option>& out) {
    const float f = w.hpFrac();
    // Shopping: once we can afford shards, a trip home to spend the gold gets a bonus that keeps
    // growing every round the gold sits unspent (items are how we scale), and resets once bought.
    static int affordableSince = -1;
    const auto buy = shop::affordable(w);
    bool canShop = tune::shop_trips && (buy.completes || buy.shards >= tune::shop_min_shards) &&
                   (!tune::shop_after_first_tribute || w.hasTributed);
    // A fight about to happen is no time to shop (collision: Varg recalled at full health to spend
    // 90 gold as their team arrived, and the three left behind died). The gold keeps its urgency.
    bool fightNear = false;
    if (tune::shop_no_fight && canShop) {
        for (const auto& e : w.enemies)
            if (e.hp > 0 && dist(e.pos, w.pos) <= tune::shop_enemy_radius) fightNear = true;
        if (regroup::active() && dist(regroup::at(), w.pos) <= tune::shop_alert_radius) fightNear = true;
    }
    if (!canShop) affordableSince = -1;
    else if (affordableSince < 0) affordableSince = w.round;
    if (f < 0.999f || canShop) {
        Option o;
        o.kind = OptionKind::heal;
        o.at = Statics::get().spawnCenter;
        o.c.target << "home";
        // Health only matters once it is low: nothing above ~45%, dominant by ~20%.
        // Monsters do not chase, so ~20% is fine against them; enemy champions do, so with
        // enemies about we head home while we still have the health to get there.
        // yBot raid: their team is down with time before the first respawn: no enemy margin, and
        // health only matters once it is low (stay and farm their open half).
        const bool raid = tune::raid_heal && w.enemyBodies >= 2 && w.enemiesAlive <= tune::raid_max_alive &&
                          w.enemyRespawnAt - w.round >= tune::raid_heal_window;
        const bool enemiesAbout = !raid && w.enemyPressure(w.pos, tune::enemy_watch_radius + 3) > 0;
        const float pairMargin = tune::pair_lives && w.pairChampion() ? tune::pair_heal_margin : 0.0f;
        const float start = raid ? tune::raid_heal_start : tune::heal_start_frac + (enemiesAbout ? tune::heal_enemy_margin : 0.0f);
        const float urgent = tune::heal_urgent_frac + (enemiesAbout ? tune::heal_enemy_margin : 0.0f) + pairMargin;
        float urgency = std::clamp((start - f) / std::max(start - urgent, 0.05f), 0.0f, 1.0f);
        // combatBotv2: tanking a camp (no enemy champions about), go by the damage we are taking,
        // not by health fraction: stay until death is ~3 turns past the walk out of the monsters'
        // reach (rows: Ren left the third wolves at 35% with ~6 turns of health to spare).
        if (tune::late_heal && !enemiesAbout && w.engagedGroup >= 0 && w.hitRate > 1.0f) {
            const float spare = w.hp / w.hitRate - static_cast<float>(nav().escapeTurns(w)) - tune::death_margin_turns;
            urgency = spare <= 0 ? 1.0f : spare >= tune::late_heal_window ? 0.0f : 1.0f - spare / tune::late_heal_window;
            o.c.note << "turns to spare " << static_cast<int>(spare) << "; ";
        }
        o.add("low health", urgency * urgency * 60.0f);
        // yBot: shopping joins a trip home we make for health; on its own it does not send us home
        // (83% of our recalls were heal trips at ~65% health, most with shopping; Fish goes home at
        // ~35-40% and shops then). Completing an emblem we have waited long for still may.
        const bool shopTrip = f < tune::shop_trip_hp || (buy.completes && affordableSince >= 0 && w.round - affordableSince >= tune::shop_trip_wait);
        if (canShop && shopTrip && !(fightNear && f >= tune::heal_start_frac)) {
            o.add("shopping", static_cast<float>(buy.shards) * tune::shop_shard_value * (buy.completes ? 1.5f : 1.0f));
            const float waited = static_cast<float>(w.round - affordableSince);
            o.mul("gold waiting to be spent", std::min(1.0f + waited * tune::shop_urgency_per_round, tune::shop_urgency_max));
            o.c.note << "shop " << buy.shards << (buy.completes ? " (emblem)" : "") << "; ";
        }
        // Healing itself takes the recall's fixed turns (distance does not matter to a recall), or
        // the walk home when that is shorter. Then we walk back from spawn to where the work is.
        if (w.atSpawn) {
            o.turns("rest", 1.0f);
        } else {
            const float walkHome = w.walkTurns(static_cast<float>(dist(w.pos, Statics::get().spawnCenter)));
            o.turns(walkHome <= tune::recall_turns ? "walk home" : "recall", std::min(walkHome, tune::recall_turns));
            // Weighted like a farm's travel, so the two walks compare on the same scale.
            o.turns("back to work", walkHome * tune::travel_weight);
        }
        if (f < w.role().healThreshold) o.mul("critical", 2.0f);
        if (w.incoming > 0) {
            // Monsters are hitting us: can we still walk out before we drop?
            const float turnsLeft = w.hp / w.incoming;
            const float escape = static_cast<float>(nav().escapeTurns(w));
            if (turnsLeft < escape + 4.0f) o.mul("about to die", 4.0f);
        }
        if (w.gems > 0) o.mul("protect bag", 1.0f + std::min(1.0f, w.gems / (50.0f * tune::gem_scale)));
        // yBot: when the top team would go home (lab/nn recall classifier): boost, or damp a trip
        // they would not make (our recall after every capture).
        if (tune::nn_recall_gamma > 0 && !w.atSpawn) {
            const float f = std::clamp(std::exp(tune::nn_recall_gamma * (nnRecallLogit(w) - nnrecall::kThreshold)), tune::nn_recall_min, tune::nn_recall_max);
            o.mul("top team recall", f);
        }
        // Weak with enemies around: get home before they find us.
        if (f < 0.6f && enemiesAbout) o.mul("enemies around", 2.5f);
        if (raid) o.c.note << "raid: heal late; ";
        o.finish();
        out.push_back(std::move(o));
    }

    const float threat = combat::threatPerTurn(w, w.pos);
    const float pressure = w.enemyPressure(w.pos, 5);
    if (threat > 0 || pressure > 0) {
        // Allies right beside us share the enemy's attention (Varg fled at 90% with Ryth adjacent).
        const float beside = static_cast<float>(1 + w.alliesWithin(w.pos, tune::fight_beside_radius));
        const float pairRisk = tune::pair_lives && w.pairChampion() ? tune::pair_risk : 1.0f;  // two lives
        const float turnsToDie = w.hp * beside / std::max(threat, pressure) / pairRisk;
        const float outnumbered = pressure / std::max(w.alliedPower(w.pos, 5), 1.0f);
        // Danger rises as death gets close, and with a bag when we are outnumbered.
        const float bagRisk = w.gems >= tune::bag_avoid_gems ? (outnumbered - 1.0f) * 0.8f : 0.0f;
        const float horizon = tune::flee_horizon_turns;
        const float danger = std::clamp(std::max((horizon - turnsToDie) / horizon, bagRisk), 0.0f, 1.0f);
        Option o;
        o.kind = OptionKind::flee;
        o.c.target << "flee";
        o.add("danger", danger * tune::flee_value);
        o.add("bag", static_cast<float>(w.gems) / tune::gem_scale * 1.5f * danger);
        o.turns("lost farming", 25.0f);
        // Stand together: with allies beside us matching the enemies here, running gets us chased
        // and hit from range while the others lose a fighter (57% of our deaths came in even or
        // better fights, 76% with an ally within 6). Keep fighting unless badly hurt.
        if (tune::stand_together && pressure > 0 && f > tune::stand_min_hp &&
            w.alliedPower(w.pos, tune::fight_beside_radius + 2) >= pressure)
            o.mul("stand with allies", tune::stand_flee_factor);
        o.c.note << "die in " << static_cast<int>(turnsToDie);
        o.finish();
        out.push_back(std::move(o));
    }

    // Predicted retreat: enemy champions near enough to reach us could kill us before we get out
    // of their reach (plus a margin), and our side here is weaker. Leave now, not at low health.
    if (tune::predicted_retreat) {
        const float theirs = w.enemyPressure(w.pos, tune::regroup_enemy_radius);
        const float ours = w.alliedPower(w.pos, tune::fight_beside_radius + 2);
        if (theirs > 0 && theirs > ours) {
            const float beside = static_cast<float>(1 + w.alliesWithin(w.pos, tune::fight_beside_radius));
            const float toDie = w.hp * beside / theirs / (tune::pair_lives && w.pairChampion() ? tune::pair_risk : 1.0f);
            const float escape = static_cast<float>(nav().escapeTurns(w));
            const float need = escape + tune::retreat_margin_turns;
            if (toDie < need) {
                Option o;
                o.kind = OptionKind::flee;
                o.c.target << "retreat";
                const float danger = std::clamp((need - toDie) / need + 0.5f, 0.0f, 1.0f);
                o.add("predicted death", danger * tune::flee_value);
                o.add("bag", static_cast<float>(w.gems) / tune::gem_scale * 1.5f * danger);
                o.turns("lost farming", 25.0f);
                o.c.note << "die in " << static_cast<int>(toDie) << ", escape " << static_cast<int>(escape);
                // Allies beside us still fighting: leaving them hands the enemy a 4v2 (collision:
                // Ren left at 86% health, Cassi and Issac died, then Ren was chased down alone).
                if (tune::retreat_hold && w.hpFrac() >= tune::retreat_hold_hp) {
                    for (int s = 0; s < comms::alert_byte && s < w.allyCount; ++s)
                        if (s != w.slot && comms::fighting(s) && w.allyPosCached(s).x >= 0 && dist(w.allyPosCached(s), w.pos) <= tune::retreat_team_radius) {
                            o.mul("allies still fighting", tune::retreat_hold_factor);
                            break;
                        }
                }
                o.finish();
                out.push_back(std::move(o));
            }
        }
    }
    // combatBotv3: team-fight forecast. A stronger group walking toward us is a retreat before
    // contact, not after the first hits (eight, v3 vs v2 r31-38: Ren farmed a camp with Stellar and
    // Cassi behind him while their Ryth, Varg, Issac, Stellar and Cassi walked in from 6-9 tiles;
    // he left at 380 health and Stellar was caught, 750 -> 145). Whole sides compared: each side's
    // health over the other's damage is how long it lasts; we leave when we would clearly lose.
    // Allies beside us follow the retreat (retreat_follow).
    if (tune::forecast_retreat && w.alive) {
        float theirHp = 0, theirDps = 0;
        int theirCount = 0;
        bool closing = false, contact = false;
        const auto& st = Statics::get();
        for (const auto& e : w.enemies) {
            if (e.hp <= 0 || dist(e.pos, w.pos) > tune::forecast_radius) continue;
            if (dist(e.pos, st.enemySpawnCenter) <= tune::enemy_spawn_radius) continue;  // healing at home
            theirHp += e.hp;
            theirDps += std::max(e.strength + e.focus * 1.5f / tune::spell_cooldown, 1.0f);
            ++theirCount;
            closing = closing || w.enemyApproaching(e.index, e.pos) || dist(e.pos, w.pos) <= e.range + 2;
            contact = contact || dist(e.pos, w.pos) <= e.range + tune::forecast_contact_slack;
        }
        // Before contact only: once we are in their reach, leaving hands them free hits and the
        // team peels off one at a time (5v6 retreats lost eight 204 vs 1118, 12 deaths vs 4).
        if (theirCount >= tune::forecast_min_enemies && closing && !contact) {
            const float myDps = std::max(combat::championDamagePerTurn(w), 1.0f);
            const auto ours = w.ourSide(w.pos, tune::forecast_ally_radius, -1, myDps);
            // Monsters on us count against us too (a camp hitting our front while they arrive).
            const float campDps = w.incoming > 0 ? w.incoming : 0.0f;
            const float weLast = ours.hp / std::max(theirDps + campDps, 1.0f);
            const float theyLast = theirHp / std::max(ours.dps, 1.0f);
            const float edge = weLast / std::max(theyLast, 0.1f);
            if (edge < tune::forecast_edge && theirCount > ours.count) {
                Option o;
                o.kind = OptionKind::flee;
                o.c.target << "retreat";
                const float danger = std::clamp((tune::forecast_edge - edge) / tune::forecast_edge + 0.4f, 0.0f, 1.0f);
                o.add("outmatched", danger * tune::flee_value);
                o.add("bag", static_cast<float>(w.gems) / tune::gem_scale * 1.5f * danger);
                o.turns("lost farming", 25.0f);
                o.c.note << ours.count << "v" << theirCount << " edge x10 " << static_cast<int>(edge * 10.0f);
                o.finish();
                out.push_back(std::move(o));
            }
        }
    }
    // combatBotv3: ranged follow the front. A melee ally near us retreating with enemies about leaves
    // us without a front line: go with it now (eight r33: Ren retreated ahead of their five, Stellar
    // kept farming and was caught).
    if (tune::ranged_follow_front && w.ranged) {
        bool enemiesNear = false;
        for (const auto& e : w.enemies)
            enemiesNear = enemiesNear || (e.hp > 0 && dist(e.pos, w.pos) <= tune::forecast_radius &&
                                          dist(e.pos, Statics::get().enemySpawnCenter) > tune::enemy_spawn_radius);
        if (enemiesNear)
            for (int s = 0; s < comms::alert_byte && s < w.allyCount; ++s) {
                const auto t = static_cast<std::size_t>(w.allyTypes[static_cast<std::size_t>(s)]);
                const bool melee = t < bc::champion_designs.size() && bc::champion_designs[t].base_stats().base_attack_range <= 1;
                if (s == w.slot || !comms::retreating(s) || !melee) continue;
                const auto p = w.allyPosCached(s);
                if (p.x < 0 || dist(p, w.pos) > tune::retreat_team_radius) continue;
                Option o;
                o.kind = OptionKind::flee;
                o.c.target << "retreat";
                o.add("front retreats", tune::front_retreat_value);
                o.add("bag", static_cast<float>(w.gems) / tune::gem_scale * 1.5f);
                o.turns("lost farming", 1.0f);
                o.c.note << "with slot " << s;
                o.finish();
                out.push_back(std::move(o));
                break;
            }
    }
    // An ally beside us is retreating from enemies here: go with it rather than be left outnumbered.
    if (tune::retreat_follow && w.enemyPressure(w.pos, tune::regroup_enemy_radius) > 0) {
        for (int s = 0; s < comms::alert_byte && s < w.allyCount; ++s) {
            if (s == w.slot || !comms::retreating(s) || w.allyPosCached(s).x < 0 || dist(w.allyPosCached(s), w.pos) > tune::retreat_team_radius) continue;
            Option o;
            o.kind = OptionKind::flee;
            o.c.target << "retreat";
            o.add("team retreats", 0.8f * tune::flee_value);
            o.add("bag", static_cast<float>(w.gems) / tune::gem_scale * 1.2f);
            o.turns("lost farming", 25.0f);
            o.c.note << "with slot " << s;
            o.finish();
            out.push_back(std::move(o));
            break;
        }
    }
}

void regroupOption(World& w, std::vector<Option>& out) {
    if (!regroup::active()) return;
    const auto at = regroup::at();
    const int d = dist(w.pos, at);
    if (d <= tune::regroup_arrive) return;  // grouped: farm near the spotter or fight
    // combatBotv3: finish the capture we are standing on first (it takes a few rounds).
    if (captureHold(w)) return;
    Option o;
    o.kind = OptionKind::wait;
    o.target = 2;  // regroup (idle is -1)
    o.at = at;
    o.c.target << "regroup";
    o.add("ally sees enemy", tune::regroup_value);
    if (regroup::squishySpotter()) o.mul("squishy ally needs help", tune::regroup_squishy_factor);
    if (const int n = regroup::rallyEnemies(); n > 1) o.mul("rally: team falling back", 1.0f + tune::rally_factor * static_cast<float>(n - 1));
    o.turns("walk", w.walkTurns(static_cast<float>(d)));
    o.c.note << (regroup::enemies() > 1 ? "enemies" : "an enemy") << (regroup::squishySpotter() ? " near a squishy ally" : " near an ally");
    o.finish();
    out.push_back(std::move(o));
}

void hunt(World& w, std::vector<Option>& out) {
    if (!w.hunting()) return;
    // The most recently seen enemy, else their spawn.
    cambc::vec2 at = Statics::get().enemySpawnCenter;
    int seen = -1;
    for (const auto& g : w.ghosts)
        if (g.round > seen && w.round - g.round <= tune::hunt_turns) { seen = g.round; at = g.pos; }
    Option o;
    o.kind = OptionKind::wait;
    o.target = 3;  // hunt (idle is -1, regroup 2)
    o.at = at;
    o.c.target << "hunt";
    o.add("stuck: go hunt", tune::hunt_value);
    o.turns("turn", 100.0f);
    o.c.note << (seen >= 0 ? "last seen enemy" : "their spawn");
    o.finish();
    out.push_back(std::move(o));
}

void idle(World& w, std::vector<Option>& out) {
    // Always available so the brain never stalls: fall back toward spawn.
    Option o;
    o.kind = OptionKind::wait;
    o.at = Statics::get().spawnCenter;
    o.c.target << "idle";
    o.add("nothing better", 1.0f);
    o.turns("turn", 100.0f);  // scores 1: anything useful beats it
    o.finish();
    out.push_back(std::move(o));
}

} // namespace scorers
