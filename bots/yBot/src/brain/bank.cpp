// Banking: walk to an altar and tribute the bag. Worth more the fuller the bag,
// the closer the altar, the more danger we are in, and near the end of the game.
#include <algorithm>
#include <array>
#include <cmath>
#include "../combat/combat.h"
#include "../core/config.h"
#include "../nav/fields.h"
#include "../nav/navigator.h"
#include "../shop/shop.h"
#include "../strategy/mapplan.h"
#include "scorers.h"

namespace bc = cambc;

namespace scorers {

// Turns to bank at `altarIdx` by recalling home and walking from spawn (a large number when not
// worth considering: off, already home, or the altar's field is not built yet).
float recallBankTurns(const World& w, int altarIdx) {
    if (!tune::bank_by_recall || w.atSpawn || altarIdx < 0) return 1e9f;
    const int fromSpawn = fields::toAltar(altarIdx, Statics::get().spawnCenter);
    if (fromSpawn < 0 || fromSpawn >= Navigator::unreachable) return 1e9f;
    return tune::recall_turns + w.walkTurns(static_cast<float>(fromSpawn));
}

// Exposure of an altar, in walking tiles added to its cost: towards the enemy spawn (a centre
// altar about half, one on their side the full amount) and enemies seen near it, both growing with
// the bag (corners: every tribute went to the centre altar, where we fought and died).
float altarExposure(const World& w, int altarIdx) {
    if (tune::altar_exposure_tiles <= 0 && tune::altar_enemy_tiles <= 0) return 0;
    const auto& st = Statics::get();
    const auto at = st.altars[static_cast<std::size_t>(altarIdx)];
    const float ours = static_cast<float>(dist(at, st.spawnCenter));
    const float theirs = static_cast<float>(dist(at, st.enemySpawnCenter));
    const float depth = ours / std::max(ours + theirs, 1.0f);  // 0 at our spawn, 1 at theirs
    const float bagScale = 1.0f + static_cast<float>(w.gems) / tune::altar_exposure_bag;
    float tiles = tune::altar_exposure_tiles * std::clamp((depth - 0.35f) / 0.3f, 0.0f, 1.0f);
    if (w.enemyPressure(at, tune::enemy_watch_radius) > 0) tiles += tune::altar_enemy_tiles;
    return tiles * bagScale;
}

// yBot: an enemy altar lock on altar i (two vision calls), re-read every altar_lock_every rounds: bank
// scoring asked for every altar each turn (~11k a turn on lanes), and locks are rare and last.
bool enemyLock(const World& w, int i) {
    static std::array<int, 16> readRound = [] { std::array<int, 16> a{}; a.fill(-1000); return a; }();
    static std::array<bool, 16> locked{};
    const auto at = Statics::get().altars[static_cast<std::size_t>(i)];
    const bool cache = i >= 0 && i < static_cast<int>(readRound.size());
    if (cache && w.round - readRound[static_cast<std::size_t>(i)] < tune::altar_lock_every) return locked[static_cast<std::size_t>(i)];
    bool l = false;
    try {
        l = bc::vision::has_altar_lock(at) && bc::vision::altar_lock_at(at).is_enemy();
    } catch (const bc::error&) {
    }
    if (cache) {
        readRound[static_cast<std::size_t>(i)] = w.round;
        locked[static_cast<std::size_t>(i)] = l;
    }
    return l;
}

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
    // An enemy altar lock is not a wall: tributing against it is converted into a basic attack on
    // the lock, and three remove it (rules). Skipping locked altars lost elkring with a 720-gem
    // lead: routeBot locked both altars, and we sat on 900 banked + a winning bag for 190 rounds.
    // A locked altar costs the turns to break the lock instead.
    int bestIdx = -1, bestTravel = Navigator::unreachable, bestCost = Navigator::unreachable;
    bool bestLocked = false;
    for (auto [k, estimate] : nearest) {
        if (k < 0 || estimate >= bestCost) continue;  // walking never beats the estimate
        const auto i = static_cast<std::size_t>(k);
        if (!w.mayInvade() && w.theirHalf(altars[i])) continue;  // no invading without a numbers lead
        if (w.invadeWithoutMelee(altars[i])) continue;           // nor, ranged, without our melee there
        const bool locked = enemyLock(w, k);
        if (locked && !tune::break_altar_locks) continue;
        const int exact = fields::toAltar(k, w.pos);
        const int d = exact >= 0 ? exact : nav().distToRange(w, altars[i], 1);
        const int cost = d + (locked ? tune::lock_break_turns : 0) + static_cast<int>(altarExposure(w, k));
        if (cost < bestCost) { bestCost = cost; bestTravel = d; bestIdx = k; bestLocked = locked; }
    }
    if (bestIdx < 0) return;

    Option o;
    o.kind = OptionKind::bank;
    o.target = bestIdx;
    o.at = altars[static_cast<std::size_t>(bestIdx)];
    o.c.target << 'a' << bestIdx;
    const float gems = static_cast<float>(w.gems);
    o.add("bag", gems * gemValue(w));
    // Recall first when that is quicker: 5 turns home, then the walk from spawn (rows: walking the
    // bag from the far wolves took ~40 rounds; their team recalled and banked in ~25).
    float travel = w.walkTurns(static_cast<float>(bestTravel));
    float viaHome = recallBankTurns(w, bestIdx);
    // After a recall the nearest altar is the one nearest spawn, not the one nearest us.
    if (tune::bank_recall_any_altar && viaHome < 1e8f) {
        for (int i = 0; i < static_cast<int>(altars.size()); ++i) {
            if (i == bestIdx || (!w.mayInvade() && w.theirHalf(altars[static_cast<std::size_t>(i)])) ||
                w.invadeWithoutMelee(altars[static_cast<std::size_t>(i)]))
                continue;
            if (enemyLock(w, i)) continue;
            if (const float t = recallBankTurns(w, i) + altarExposure(w, i) - altarExposure(w, o.target); t < viaHome) {
                viaHome = t;
                o.target = i;
                o.at = altars[static_cast<std::size_t>(i)];
                o.c.target.clear();
                o.c.target << 'a' << i;
            }
        }
    }
    if (viaHome + tune::bank_recall_margin < travel) {
        travel = viaHome;
        o.c.note << "recall first; ";
        // The recall also heals us fully on spawn and opens the shop: one trip for all three.
        if (tune::bank_recall_combo) {
            const float missing = std::clamp(1.0f - w.hpFrac(), 0.0f, 1.0f);
            if (missing > 0.05f) o.add("heals on the way", missing * missing * 60.0f);
            if (const auto buy = shop::affordable(w); buy.shards >= tune::shop_min_shards || buy.completes)
                o.add("shops on the way", static_cast<float>(buy.shards) * tune::shop_shard_value * 0.5f);
        }
    }
    o.turns("travel", travel);
    if (bestLocked) {
        o.turns("break their lock", static_cast<float>(tune::lock_break_turns));
        o.c.note << "locked: breaking it";
    }
    o.turns("tribute", std::ceil(gems / 10.0f));
    // Alone, a full bag is an invitation: bank at a lower bag size.
    const bool alone = w.alliesWithin(w.pos, 6) == 0;
    const float early = w.round < tune::early_bank_round ? tune::early_bank_factor : 1.0f;
    const float target = w.role().bankTarget * tune::bank_target_scale * early * (alone ? tune::alone_bank_factor : 1.0f);
    o.mul("bag fullness", std::pow(std::min(gems / target, 1.5f), 1.5f));
    if (alone) o.c.note << "alone";
    if (w.teamGems + w.gems >= 1000) o.mul("wins the game", 20.0f);
    // The game ends when either team reaches 1000: gems still carried then count for nothing.
    else if (w.enemyGems >= tune::race_end_gems) o.mul("enemy about to win", tune::race_end_bank_boost);
    // Endgame, tick-exact: the tribute completes at the start of our turn after its last tick
    // (ceil(bag / 10) ticks, the first on the turn we arrive). Past the round limit it is worthless;
    // close to it, bank everything now.
    {
        const int maxRounds = static_cast<int>(bc::constants::max_rounds);
        const float done = static_cast<float>(w.round) + w.walkTurns(static_cast<float>(bestTravel)) +
                           static_cast<float>((w.gems + 9) / 10);
        if (done >= static_cast<float>(maxRounds)) o.mul("cannot finish before the end", 0.01f);
        else if (w.round > maxRounds - tune::endgame_window / 2) o.mul("score before time", 4.0f);
    }
    const float pressure = w.enemyPressure(w.pos, tune::enemy_watch_radius);
    // Too weak to cross enemy ground with a bag: recalling home keeps the gems, a walk may not.
    if (pressure > 0 && w.hpFrac() < 0.4f) o.mul("too weak to cross", 0.3f);
    if (pressure > 0) {
        const float ratio = pressure / std::max(w.alliedPower(w.pos, 5), 1.0f);
        o.mul("enemies near", tune::danger_bank_boost * std::clamp(ratio, 0.5f, 3.0f));
    }
    if (w.lastOption == static_cast<int>(OptionKind::bank)) o.mul("committed", 1.2f);
    // Passing an altar on the path anyway: top up, the detour costs nearly nothing (hybridBot2: +161).
    if (bestTravel <= tune::passing_altar_tiles && gems >= tune::passing_altar_min_gems) o.mul("passing the altar", tune::passing_altar_bonus);
    if (mapplan::routeWantsBank(w)) o.mul("route altar visit", tune::route_bonus);
    // Same test the executor uses before tributing: a stunner or a killing blow could cancel it, so
    // we would only walk up, back off and walk up again. Pick something else instead.
    const auto at = altars[static_cast<std::size_t>(bestIdx)];
    if (w.tributeAtRisk(at, w.gems)) o.mul("tribute would be cancelled", 0.2f);
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
