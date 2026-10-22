#include "lanes.h"
#include <algorithm>
#include <array>
#include <vector>
#include "../core/config.h"
#include "../core/profile.h"
#include "../core/statics.h"
#include "../core/trace.h"
#include "../core/world.h"
#include "../shop/shop.h"

namespace lanes {

namespace {
bool init = false, on = false, busy = false;
std::vector<unsigned> owners;  // per group: bit per slot owning it
int me = -1;

void assign(const World& w) {
    const auto& st = Statics::get();
    owners.assign(st.groups.size(), 0u);
    const int bodies = std::min(w.allyCount, 8);
    if (!tune::lanes || bodies < 2 || !shop::isFarmingMap()) return;
    std::vector<int> camps;
    for (const auto& g : st.groups) {
        if (g.harmless || !g.reachable || g.gems <= 0 || g.totalStrength > tune::lane_max_strength) continue;
        if (dist(g.center, st.spawnCenter) >= dist(g.center, st.enemySpawnCenter)) continue;
        camps.push_back(g.id);
    }
    if (static_cast<int>(camps.size()) < bodies) return;
    std::sort(camps.begin(), camps.end(), [&](int a, int b) {
        const int da = dist(st.groups[static_cast<std::size_t>(a)].center, st.spawnCenter);
        const int db = dist(st.groups[static_cast<std::size_t>(b)].center, st.spawnCenter);
        return da != db ? da < db : a < b;
    });
    // Stellar on steal duty (big far camps by spells) keeps out of the lanes.
    unsigned skip = 0;
    if (tune::stellar_steal_large)
        for (int s = 0; s < bodies; ++s)
            if (stellarLike(w.allyTypes[static_cast<std::size_t>(s)])) skip |= 1u << s;
    std::array<float, 8> load{};
    std::array<float, 8> cx{}, cy{};
    std::array<int, 8> count{};
    for (int s = 0; s < bodies; ++s) { cx[static_cast<std::size_t>(s)] = static_cast<float>(st.spawnCenter.x); cy[static_cast<std::size_t>(s)] = static_cast<float>(st.spawnCenter.y); }
    for (const int id : camps) {
        const auto& g = st.groups[static_cast<std::size_t>(id)];
        const int k = g.totalStrength > tune::lane_solo_strength ? 2 : 1;
        // Busy turns per visit for each of the k owners: kill plus capture.
        const float work = g.totalHealth / (tune::lane_dps * static_cast<float>(k)) + tune::capture_turns;
        unsigned picked = 0;
        for (int j = 0; j < k; ++j) {
            int best = -1;
            float bestCost = 1e9f;
            for (int s = 0; s < bodies; ++s) {
                if (((picked | skip) >> s) & 1u) continue;
                const auto si = static_cast<std::size_t>(s);
                const float d = std::max(std::abs(cx[si] - static_cast<float>(g.center.x)), std::abs(cy[si] - static_cast<float>(g.center.y)));
                float cost = load[si] * tune::lane_load_weight + d + static_cast<float>(count[si]) * tune::lane_count_weight;
                // Camps stronger than a single wolf go to sturdy champions (Fish: Varg solos the wolf
                // pair; our Cassi judged it "need 2" and waited there alone).
                if (g.totalStrength > tune::lane_frail_strength)
                    cost += std::max(0.0f, 1.0f - profile(w.allyTypes[si]).survivability) * tune::lane_frail_pref;
                // A pair camp wants a tank in it.
                if (k == 2 && j == 0 && !isTank(w.allyTypes[si])) cost += tune::lane_tank_pref;
                if (cost < bestCost) { bestCost = cost; best = s; }
            }
            if (best < 0) break;
            picked |= 1u << best;
            const auto bi = static_cast<std::size_t>(best);
            load[bi] += work;
            cx[bi] = (cx[bi] * static_cast<float>(count[bi]) + static_cast<float>(g.center.x)) / static_cast<float>(count[bi] + 1);
            cy[bi] = (cy[bi] * static_cast<float>(count[bi]) + static_cast<float>(g.center.y)) / static_cast<float>(count[bi] + 1);
            ++count[bi];
        }
        owners[static_cast<std::size_t>(id)] = picked;
    }
    on = true;
}
} // namespace

void update(World& w) {
    if (!init) {
        init = true;
        me = w.slot;
        assign(w);
        if (on) {
            std::string t = "lane:";
            for (std::size_t i = 0; i < owners.size(); ++i)
                if ((owners[i] >> me) & 1u) t += " g" + num(static_cast<long long>(i));
            trace().note(t);
        }
    }
    busy = false;
    if (!on) return;
    for (std::size_t i = 0; i < owners.size() && i < w.groups.size(); ++i) {
        if (!((owners[i] >> me) & 1u)) continue;
        const auto& mem = w.groups[i];
        if (mem.status == GroupStatus::respawning) {
            const int walk = dist(w.pos, Statics::get().groups[i].center);
            if (mem.respawnAt - w.round <= walk + tune::lane_wait) busy = true;
        } else {
            busy = true;  // alive, unknown, or waiting for capture
        }
    }
}

bool active() { return on; }
bool mine(int groupId) { return on && groupId >= 0 && groupId < static_cast<int>(owners.size()) && ((owners[static_cast<std::size_t>(groupId)] >> me) & 1u); }
bool others(int groupId) {
    return on && groupId >= 0 && groupId < static_cast<int>(owners.size()) && owners[static_cast<std::size_t>(groupId)] != 0 && !mine(groupId);
}
bool laneBusy() { return busy; }

} // namespace lanes
