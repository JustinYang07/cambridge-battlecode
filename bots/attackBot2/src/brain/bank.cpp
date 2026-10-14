// Banking: walk to an altar and tribute the bag. Worth more the fuller the bag,
// the closer the altar, the more danger we are in, and near the end of the game.
#include <algorithm>
#include <array>
#include <cmath>
#include "../combat/combat.h"
#include "../core/config.h"
#include "../nav/fields.h"
#include "../nav/navigator.h"
#include "scorers.h"

namespace bc = cambc;

namespace scorers {

void bank(World& w, std::vector<Option>& out) {
    if (w.gems < tune::bank_min_gems) return;
    const auto& altars = Statics::get().altars;
    // Walking distance per altar: a local read once the altar's field exists, otherwise a ring of
    // host reads (~1-2k), so then only the 3 closest by straight line are measured.
    std::array<std::pair<int, int>, 3> nearest{{{-1, 0}, {-1, 0}, {-1, 0}}};  // (altar, travel)
    for (int i = 0; i < static_cast<int>(altars.size()); ++i) {
        const int exact = fields::toAltar(i, w.pos);
        std::pair<int, int> carry{i, exact >= 0 ? exact : dist(w.pos, altars[static_cast<std::size_t>(i)]) - 1};
        if (exact >= Navigator::unreachable) continue;
        for (auto& k : nearest) {  // insertion into a 3-slot list sorted by (estimated) travel
            if (carry.first < 0) break;
            if (k.first < 0 || carry.second < k.second) std::swap(k, carry);
        }
    }
    int bestIdx = -1, bestTravel = Navigator::unreachable;
    for (auto [k, estimate] : nearest) {
        if (k < 0 || estimate >= bestTravel) continue;  // walking never beats the estimate
        const auto i = static_cast<std::size_t>(k);
        try {
            if (bc::vision::has_altar_lock(altars[i]) && bc::vision::altar_lock_at(altars[i]).is_enemy()) continue;
        } catch (const bc::error&) {
        }
        const int exact = fields::toAltar(k, w.pos);
        const int d = exact >= 0 ? exact : nav().distToRange(w, altars[i], 1);
        if (d < bestTravel) { bestTravel = d; bestIdx = k; }
    }
    if (bestIdx < 0) return;

    Option o;
    o.kind = OptionKind::bank;
    o.target = bestIdx;
    o.at = altars[static_cast<std::size_t>(bestIdx)];
    o.c.target << 'a' << bestIdx;
    const float gems = static_cast<float>(w.gems);
    o.add("bag", gems);
    o.turns("travel", w.walkTurns(static_cast<float>(bestTravel)));
    o.turns("tribute", std::ceil(gems / 10.0f));
    // Alone, a full bag is an invitation: bank at a lower bag size.
    const bool alone = w.alliesWithin(w.pos, 6) == 0;
    const float target = w.role().bankTarget * (alone ? tune::alone_bank_factor : 1.0f);
    o.mul("bag fullness", std::pow(std::min(gems / target, 1.5f), 1.5f));
    if (alone) o.c.note << "alone";
    if (w.teamGems + w.gems >= 1000) o.mul("wins the game", 20.0f);
    // The game ends when either team reaches 1000: gems still carried then count for nothing.
    else if (w.enemyGems >= tune::race_end_gems) o.mul("enemy about to win", tune::race_end_bank_boost);
    if (w.round > 1850) o.mul("score before time", 4.0f);
    const float pressure = w.enemyPressure(w.pos, tune::enemy_watch_radius);
    // Too weak to cross enemy ground with a bag: recalling home keeps the gems, a walk may not.
    if (pressure > 0 && w.hpFrac() < 0.4f) o.mul("too weak to cross", 0.3f);
    if (pressure > 0) {
        const float ratio = pressure / std::max(w.alliedPower(w.pos, 5), 1.0f);
        o.mul("enemies near", tune::danger_bank_boost * std::clamp(ratio, 0.5f, 3.0f));
    }
    if (w.lastOption == static_cast<int>(OptionKind::bank)) o.mul("committed", 1.2f);
    // Same test the executor uses before tributing: a stronger enemy close to the altar means
    // we would only walk up, back off and walk up again. Pick something else instead.
    const auto at = altars[static_cast<std::size_t>(bestIdx)];
    const int tributeTurns = std::max(1, (w.gems + 9) / 10);
    bool enemyClose = false;
    for (const auto& gh : w.ghosts)
        if (w.round == gh.round && dist(gh.pos, at) <= gh.range + 2 + tributeTurns) enemyClose = true;
    if (enemyClose && tributeTurns > 1 && w.enemyPressure(at, tune::enemy_watch_radius) > w.alliedPower(at, 5))
        o.mul("unsafe altar", 0.2f);
    // Our camp is respawning: bank in the dead time instead of standing guard with a full bag.
    if (w.lastTarget >= 0 && w.lastOption == static_cast<int>(OptionKind::farm) &&
        w.lastTarget < static_cast<int>(w.groups.size())) {
        const auto& mem = w.groups[static_cast<std::size_t>(w.lastTarget)];
        const int wait = mem.status == GroupStatus::respawning ? mem.respawnAt - w.round : 0;
        if (wait >= bestTravel * 2) o.mul("bank while camp respawns", 3.0f);
    }
    o.finish();
    out.push_back(std::move(o));
}

} // namespace scorers
