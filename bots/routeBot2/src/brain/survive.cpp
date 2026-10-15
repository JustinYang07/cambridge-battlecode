// Survival: heal (walk home or recall), flee from losing fights, and the idle fallback.
#include <algorithm>
#include "../combat/combat.h"
#include "../core/config.h"
#include "../nav/navigator.h"
#include "../shop/shop.h"
#include "../strategy/mapplan.h"
#include "scorers.h"

namespace bc = cambc;

namespace scorers {

void survive(World& w, std::vector<Option>& out) {
    const float f = w.hpFrac();
    // A trip home is also a shopping trip: worth it on its own once we can buy enough shards
    // (items are how we scale before contesting), even at full health.
    const auto buy = shop::affordable(w);
    const bool shopTrip = tune::shop_trips && (buy.completes || buy.shards >= tune::shop_min_shards);
    if (f < 0.999f || shopTrip) {
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
        if (tune::shop_trips && buy.shards > 0)
            o.add("shopping", static_cast<float>(buy.shards) * tune::shop_shard_value * (buy.completes ? 1.5f : 1.0f));
        if (shopTrip) o.c.note << "shop " << buy.shards << (buy.completes ? " (emblem)" : "");
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
        // Our loop is down and we are too hurt to bank or roam safely: heal now, go after.
        if (mapplan::downtime() && f < tune::downtime_min_hp) o.mul("heal before roaming", tune::downtime_heal_boost);
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
        o.add("danger", danger * 150.0f);
        o.add("bag", static_cast<float>(w.gems) * 1.5f * danger);
        o.turns("lost farming", 25.0f);
        o.c.note << "die in " << static_cast<int>(turnsToDie);
        o.finish();
        out.push_back(std::move(o));
    }
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

    // Our loop is down and we are healthy: head for the middle, where farm and fights are, instead
    // of standing at a respawning camp. Anything real (a camp, a bank, a fight) still beats it.
    if (mapplan::downtime() && w.hpFrac() >= tune::downtime_min_hp) {
        Option r;
        r.kind = OptionKind::wait;
        r.target = 1;  // roam (idle is -1)
        r.at = mapplan::middle();
        r.c.target << "roam middle";
        r.add("loop down: roam", tune::downtime_roam_score);
        r.turns("turn", 100.0f);
        r.finish();
        out.push_back(std::move(r));
    }
}

} // namespace scorers
