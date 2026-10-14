#include "plan.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include "../core/config.h"
#include "../core/fixed_str.h"
#include "../core/trace.h"

namespace bc = cambc;

namespace strategy {

namespace {

Plan state;
std::vector<int> candidates;          // groups taking part in the split
std::vector<int> seeds;               // seed group per region
std::vector<std::vector<int>> seedDist;  // seedDist[region][candidate index]
int stage = 0;                        // 0: setup, 1..regions: BFS per seed, then finalise

// Champions a group needs: a front champion (~1100 hp, the first-drafted tank in a party)
// soaks every hit while each attacker adds ~75 damage per turn.
int groupNeed(const GroupInfo& g) {
    if (g.harmless) return 1;
    constexpr float dps = 75.0f, budget = 1100.0f * 0.8f;
    float damage = 0, strength = g.totalStrength;
    for (const auto& m : g.killOrder) {
        damage += m.health / dps * strength;
        strength -= m.strength;
    }
    // With k attackers the fight is k times shorter and a front tank soaks the hits.
    for (int k = 1; k <= 4; ++k)
        if (damage / static_cast<float>(k) <= budget) return k;
    return 5;
}

void setup(const World& w) {
    const auto& s = Statics::get();
    state.groupNeed.assign(s.groups.size(), 1);
    state.groupRegion.assign(s.groups.size(), -1);
    candidates.clear();
    float pairValue = 0, totalValue = 0;
    bc::vec2 enemyCenter{};
    {
        long ex = 0, ey = 0, n = 0;
        for (auto p : s.enemySpawn.positions()) { ex += p.x; ey += p.y; ++n; }
        if (n) enemyCenter = {static_cast<int>(ex / n), static_cast<int>(ey / n)};
    }
    for (const auto& g : s.groups) {
        const int need = groupNeed(g);
        state.groupNeed[static_cast<std::size_t>(g.id)] = need;
        if (g.gems == 0 && g.gold == 0) continue;
        if (need > 3) continue;  // monoliths and the like are late-game team objectives
        // Only our half: camps nearer the enemy spawn are contested and a long walk.
        if (dist(g.center, enemyCenter) < dist(g.center, s.spawnCenter)) continue;
        candidates.push_back(g.id);
        const float v = static_cast<float>(g.gems) + 0.2f * static_cast<float>(g.gold);
        totalValue += v;
        if (need >= 2) pairValue += v;
    }

    // Squads follow the map: squad size is the value-weighted champions-per-camp the map
    // demands (wolf-pack maps ~3, gem-cluster maps ~1), so spread-out maps split into many
    // small squads and pack-heavy maps keep the team together.
    const int bodies = std::max(1, w.allyCount);
    float needValue = 0;
    for (int id : candidates) {
        const auto& g = s.groups[static_cast<std::size_t>(id)];
        needValue += static_cast<float>(state.groupNeed[static_cast<std::size_t>(id)]) *
                     (static_cast<float>(g.gems) + 0.2f * static_cast<float>(g.gold));
    }
    const float avgNeed = totalValue > 0 ? needValue / totalValue : 1.0f;
    (void)pairValue;
    const int squadSize = std::clamp(static_cast<int>(std::lround(avgNeed)), 1, bodies);
    // Round the squad count down (5 bodies in squads of ~2 gives 3-2, not 2-2-1): leftover solos
    // were picked off by enemies on small maps, and rounding to nearest tested worse (-132 vs -2 gems).
    const int squads = std::max(1, std::min<int>(bodies / squadSize, static_cast<int>(candidates.size())));
    state.regions = squads;

    // Seeds: sort candidates by direction from our spawn, cut into equal-value sectors,
    // and take the most valuable (distance-discounted) group of each sector.
    struct Item { int id; float angle, weight; };
    std::vector<Item> items;
    for (int id : candidates) {
        const auto& g = s.groups[static_cast<std::size_t>(id)];
        const float dx = static_cast<float>(g.center.x - s.spawnCenter.x), dy = static_cast<float>(g.center.y - s.spawnCenter.y);
        const float d = static_cast<float>(dist(g.center, s.spawnCenter));
        const float v = static_cast<float>(g.gems) + 0.2f * static_cast<float>(g.gold);
        items.push_back({id, std::atan2(dy, dx), v / (1.0f + d / 25.0f)});
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        return a.angle != b.angle ? a.angle < b.angle : a.id < b.id;
    });
    // Contiguous angular chunks of roughly equal weight, never leaving a chunk empty.
    const float total = std::accumulate(items.begin(), items.end(), 0.0f, [](float acc, const Item& i) { return acc + i.weight; });
    const float target = total / static_cast<float>(squads);
    seeds.clear();
    float inChunk = 0, bestWeight = -1;
    int best = -1;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto& it = items[i];
        if (it.weight > bestWeight) { bestWeight = it.weight; best = it.id; }
        inChunk += it.weight;
        const int chunksLeft = squads - static_cast<int>(seeds.size()) - 1;  // after closing this one
        const int itemsLeft = static_cast<int>(items.size() - i - 1);
        const bool close = chunksLeft > 0 && (inChunk >= target || itemsLeft <= chunksLeft);
        if (close || i + 1 == items.size()) {
            seeds.push_back(best);
            inChunk = 0;
            bestWeight = -1;
            best = -1;
        }
    }
    state.regions = static_cast<int>(seeds.size());
    seedDist.assign(seeds.size(), std::vector<int>(candidates.size(), 1 << 20));
}

void bfsFromSeed(int r) {
    const auto& s = Statics::get();
    const auto& g = s.groups[static_cast<std::size_t>(seeds[static_cast<std::size_t>(r)])];
    bc::distance_field field;
    bc::bitboard visited;
    field.compute(s.terrain, g.tileMask, visited);
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto& other = s.groups[static_cast<std::size_t>(candidates[i])];
        int best = 1 << 20;
        for (auto t : other.tiles)
            if (visited.is_set(t)) best = std::min(best, static_cast<int>(field.get(t)));
        seedDist[static_cast<std::size_t>(r)][i] = best;
    }
}

void finalise(const World& w) {
    const auto& s = Statics::get();
    const int regions = state.regions;
    std::vector<float> pairDemand(static_cast<std::size_t>(regions), 0.0f), value(static_cast<std::size_t>(regions), 0.0f);
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        int bestR = 0;
        for (int r = 1; r < regions; ++r)
            if (seedDist[static_cast<std::size_t>(r)][i] < seedDist[static_cast<std::size_t>(bestR)][i]) bestR = r;
        const int id = candidates[i];
        state.groupRegion[static_cast<std::size_t>(id)] = bestR;
        const auto& g = s.groups[static_cast<std::size_t>(id)];
        const float v = static_cast<float>(g.gems) + 0.2f * static_cast<float>(g.gold);
        value[static_cast<std::size_t>(bestR)] += v;
        if (state.groupNeed[static_cast<std::size_t>(id)] >= 2) pairDemand[static_cast<std::size_t>(bestR)] += v;
    }

    // Squad sizes: regions with the most group-needing value get the pairs.
    const int bodies = std::max(1, w.allyCount);
    int pairs = std::max(0, bodies - regions);  // bodies beyond one per region are the second members
    std::vector<int> order(static_cast<std::size_t>(regions));
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return pairDemand[static_cast<std::size_t>(a)] != pairDemand[static_cast<std::size_t>(b)]
                   ? pairDemand[static_cast<std::size_t>(a)] > pairDemand[static_cast<std::size_t>(b)]
                   : value[static_cast<std::size_t>(a)] > value[static_cast<std::size_t>(b)];
    });
    state.regionSize.fill(0);
    for (int r : order) state.regionSize[static_cast<std::size_t>(r)] = 1;
    for (int k = 0; k < pairs; ++k) state.regionSize[static_cast<std::size_t>(order[static_cast<std::size_t>(k % regions)])] += 1;

    // Champions: tanks lead the squads with the most demand, farmers fill the rest, then others.
    std::vector<int> slots(static_cast<std::size_t>(bodies));
    std::iota(slots.begin(), slots.end(), 0);
    auto rank = [&](int slot) {
        const auto& p = profile(w.allyTypes[static_cast<std::size_t>(slot)]);
        return p.roles.tank * 3.0f + p.roles.farmer * 2.0f + p.roles.duelist + p.roles.caster * 0.5f;
    };
    std::stable_sort(slots.begin(), slots.end(), [&](int a, int b) { return rank(a) > rank(b); });
    std::array<int, 8> filled{};
    // First pass: one champion per region (best-ranked to the most demanding region).
    std::size_t next = 0;
    for (int r : order)
        if (next < slots.size()) {
            state.slotRegion[static_cast<std::size_t>(slots[next++])] = r;
            ++filled[static_cast<std::size_t>(r)];
        }
    // Second pass: fill remaining capacity, most demanding regions first.
    for (; next < slots.size(); ++next) {
        int target = order[0];
        for (int r : order)
            if (filled[static_cast<std::size_t>(r)] < state.regionSize[static_cast<std::size_t>(r)]) { target = r; break; }
        state.slotRegion[static_cast<std::size_t>(slots[next])] = target;
        ++filled[static_cast<std::size_t>(target)];
    }

    FixedStr<120> msg;
    msg << "plan: " << regions << " regions, squads";
    for (int r = 0; r < regions; ++r) msg << ' ' << state.regionSize[static_cast<std::size_t>(r)];
    msg << "; home r" << state.slotRegion[static_cast<std::size_t>(w.slot)];
    state.summary = std::string(msg.view());
    state.ready = true;
    trace().note(state.summary);
    if (w.slot == 0) trace().plan(state.regions, state.groupRegion, state.slotRegion, bodies);
}

} // namespace

void step(World& w) {
    // Run as many stages as the turn can afford (the first turn has a doubled budget, so the
    // plan is usually ready before anyone moves and nobody wanders off in a wrong direction).
    while (!state.ready && bc::fuel_remaining() >= tune::fuel_for_planning) {
        if (stage == 0) {
            setup(w);
            stage = 1;
        } else if (stage <= state.regions) {
            bfsFromSeed(stage - 1);
            ++stage;
        } else {
            finalise(w);
        }
    }
}

const Plan& plan() { return state; }

int homeRegion(const World& w) {
    return state.ready ? state.slotRegion[static_cast<std::size_t>(w.slot)] : -1;
}

} // namespace strategy
