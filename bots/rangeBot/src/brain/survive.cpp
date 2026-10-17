// Survival: heal (walk home or recall), flee from losing fights, and the idle fallback.
#include <algorithm>
#include "../combat/combat.h"
#include "../core/comms.h"
#include "../core/config.h"
#include "../nav/navigator.h"
#include "../shop/shop.h"
#include "../strategy/regroup.h"
#include "scorers.h"

namespace bc = cambc;

namespace scorers {

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
        const bool enemiesAbout = w.enemyPressure(w.pos, tune::enemy_watch_radius + 3) > 0;
        const float start = tune::heal_start_frac + (enemiesAbout ? tune::heal_enemy_margin : 0.0f);
        const float urgent = tune::heal_urgent_frac + (enemiesAbout ? tune::heal_enemy_margin : 0.0f);
        const float urgency = std::clamp((start - f) / (start - urgent), 0.0f, 1.0f);
        o.add("low health", urgency * urgency * 60.0f);
        if (canShop && !(fightNear && f >= tune::heal_start_frac)) {
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
        if (w.gems > 0) o.mul("protect bag", 1.0f + std::min(1.0f, w.gems / 50.0f));
        // Weak with enemies around: get home before they find us.
        if (f < 0.6f && enemiesAbout) o.mul("enemies around", 2.5f);
        o.finish();
        out.push_back(std::move(o));
    }

    const float threat = combat::threatPerTurn(w, w.pos);
    const float pressure = w.enemyPressure(w.pos, 5);
    if (threat > 0 || pressure > 0) {
        // Allies right beside us share the enemy's attention (Varg fled at 90% with Ryth adjacent).
        const float beside = static_cast<float>(1 + w.alliesWithin(w.pos, tune::fight_beside_radius));
        const float turnsToDie = w.hp * beside / std::max(threat, pressure);
        const float outnumbered = pressure / std::max(w.alliedPower(w.pos, 5), 1.0f);
        // Danger rises as death gets close, and with a bag when we are outnumbered.
        const float bagRisk = w.gems >= tune::bag_avoid_gems ? (outnumbered - 1.0f) * 0.8f : 0.0f;
        const float horizon = tune::flee_horizon_turns;
        const float danger = std::clamp(std::max((horizon - turnsToDie) / horizon, bagRisk), 0.0f, 1.0f);
        Option o;
        o.kind = OptionKind::flee;
        o.c.target << "flee";
        o.add("danger", danger * tune::flee_value);
        o.add("bag", static_cast<float>(w.gems) * 1.5f * danger);
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
            const float toDie = w.hp * beside / theirs;
            const float escape = static_cast<float>(nav().escapeTurns(w));
            const float need = escape + tune::retreat_margin_turns;
            if (toDie < need) {
                Option o;
                o.kind = OptionKind::flee;
                o.c.target << "retreat";
                const float danger = std::clamp((need - toDie) / need + 0.5f, 0.0f, 1.0f);
                o.add("predicted death", danger * tune::flee_value);
                o.add("bag", static_cast<float>(w.gems) * 1.5f * danger);
                o.turns("lost farming", 25.0f);
                o.c.note << "die in " << static_cast<int>(toDie) << ", escape " << static_cast<int>(escape);
                // Allies beside us still fighting: leaving them hands the enemy a 4v2 (collision:
                // Ren left at 86% health, Cassi and Issac died, then Ren was chased down alone).
                if (tune::retreat_hold && w.hpFrac() >= tune::retreat_hold_hp) {
                    for (int s = 0; s < comms::alert_byte && s < w.allyCount; ++s)
                        if (s != w.slot && comms::fighting(s) && w.allyPos(s).x >= 0 && dist(w.allyPos(s), w.pos) <= tune::retreat_team_radius) {
                            o.mul("allies still fighting", tune::retreat_hold_factor);
                            break;
                        }
                }
                o.finish();
                out.push_back(std::move(o));
            }
        }
    }
    // An ally beside us is retreating from enemies here: go with it rather than be left outnumbered.
    if (tune::retreat_follow && w.enemyPressure(w.pos, tune::regroup_enemy_radius) > 0) {
        for (int s = 0; s < comms::alert_byte && s < w.allyCount; ++s) {
            if (s == w.slot || !comms::retreating(s) || w.allyPos(s).x < 0 || dist(w.allyPos(s), w.pos) > tune::retreat_team_radius) continue;
            Option o;
            o.kind = OptionKind::flee;
            o.c.target << "retreat";
            o.add("team retreats", 0.8f * tune::flee_value);
            o.add("bag", static_cast<float>(w.gems) * 1.2f);
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
    Option o;
    o.kind = OptionKind::wait;
    o.target = 2;  // regroup (idle is -1)
    o.at = at;
    o.c.target << "regroup";
    o.add("ally sees enemy", tune::regroup_value);
    if (regroup::squishySpotter()) o.mul("squishy ally needs help", tune::regroup_squishy_factor);
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
