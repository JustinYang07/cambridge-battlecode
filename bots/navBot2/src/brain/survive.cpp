// Survival: heal (walk home or recall), flee from losing fights, and the idle fallback.
#include <algorithm>
#include "../combat/combat.h"
#include "../core/config.h"
#include "../nav/navigator.h"
#include "scorers.h"

namespace bc = cambc;

namespace scorers {

void survive(World& w, std::vector<Option>& out) {
    const float f = w.hpFrac();
    if (f < 0.999f) {
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
        // A recall is also a shopping trip once we can afford an emblem.
        // Only worth the trip when hurt anyway or already close to spawn.
        if (w.gold >= tune::shop_gold) o.add("shopping", static_cast<float>(w.gold) * goldValue(w) * 0.1f);
        // Recall (5 turns) plus half the walk back to where the work is.
        o.turns("trip", w.atSpawn ? 1.0f : 5.0f + 0.5f * static_cast<float>(dist(w.pos, Statics::get().spawnCenter)));
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
        const float turnsToDie = w.hp / std::max(threat, pressure);
        const float outnumbered = pressure / std::max(w.alliedPower(w.pos, 5), 1.0f);
        // Danger rises as death gets close, and with a bag when we are outnumbered.
        const float bagRisk = w.gems >= tune::bag_avoid_gems ? (outnumbered - 1.0f) * 0.8f : 0.0f;
        const float danger = std::clamp(std::max((10.0f - turnsToDie) / 10.0f, bagRisk), 0.0f, 1.0f);
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
}

} // namespace scorers
