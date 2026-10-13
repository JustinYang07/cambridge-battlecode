// Fighting enemy champions when we expect to win: their bag and the kill gold
// are the reward, time-to-kill (plus the walk) is the cost.
#include <algorithm>
#include "../combat/combat.h"
#include "../core/config.h"
#include "../nav/navigator.h"
#include "scorers.h"

namespace scorers {

void fight(World& w, std::vector<Option>& out) {
    const float myDps = std::max(combat::championDamagePerTurn(w), 1.0f);
    for (const auto& e : w.enemies) {
        const float theirDps = std::max(e.strength + e.focus * 1.5f / tune::spell_cooldown, 1.0f);
        const float turnsToKill = e.hp / myDps;
        const float turnsToDie = w.hp / theirDps;
        Option o;
        o.kind = OptionKind::fight;
        o.enemy = e.handle;
        o.at = e.pos;
        o.c.target << 'e' << static_cast<int>(static_cast<cambc::handle_champion_t>(e.handle));
        o.add("their bag", static_cast<float>(e.gems));
        o.add("kill gold", tune::kill_gold_value * goldValue(w));
        o.turns("travel", static_cast<float>(std::max(0, dist(w.pos, e.pos) - w.range)));
        o.turns("kill time", turnsToKill);
        const float edge = turnsToDie / std::max(turnsToKill, 0.1f);
        o.mul("win margin", std::clamp((edge - 1.0f) / (tune::fight_win_margin - 1.0f), 0.0f, 1.5f));
        if (e.channeling) o.mul("interrupt channel", 1.5f);
        o.mul("role appetite", w.role().fightAppetite);
        o.c.note << "kill in " << static_cast<int>(turnsToKill) << ", die in " << static_cast<int>(turnsToDie);
        o.finish();
        out.push_back(std::move(o));
    }
}

} // namespace scorers
