#include "territory.h"
#include <algorithm>
#include <array>
#include <vector>
#include "mapplan.h"
#include "../combat/combat.h"
#include "../core/comms.h"
#include "../core/config.h"
#include "../core/fixed_str.h"
#include "../core/statics.h"
#include "../core/trace.h"
#include "../core/world.h"

namespace territory {

namespace {
bool init = false;
int region = -1;
int nextReplan = 0;
int regions = 0;
int switchedRound = -1000;   // last region change (tune::territory_min_stay)
bool arrived = false;        // we have reached our region since the last change
std::array<int, 8> allyRegion{};  // per slot: the region the ally works (plan, then its claims)
std::vector<int> regionOfGroup;  // per group: region id or -1
struct Region {
    float rate = 0;              // value per round its camps give back
    cambc::vec2 centre{0, 0};
    float side = 0;              // 0 at our spawn .. 1 at theirs (straight-line)
    int seenRound = -1000;       // last round an enemy champion was seen in it
    int deathRound = -1000;      // last round one of us died in it (ours only)
};
std::array<Region, mapplan::kMaxRegions> R{};

float value(const GroupInfo& g) { return static_cast<float>(g.gems) / tune::gem_scale + tune::route_lambda / tune::gold_scale * static_cast<float>(g.gold); }

// The region a tile belongs to: the nearest region camp within territory_near.
int regionAt(cambc::vec2 p) {
    const auto& s = Statics::get();
    int best = -1, bestD = tune::territory_near + 1;
    for (std::size_t g = 0; g < regionOfGroup.size(); ++g) {
        if (regionOfGroup[g] < 0) continue;
        const int d = dist(p, s.groups[g].center);
        if (d < bestD) { bestD = d; best = regionOfGroup[g]; }
    }
    return best;
}

void setup(World& w) {
    init = true;
    const auto& p = mapplan::read();
    const auto& s = Statics::get();
    regionOfGroup.assign(s.groups.size(), -1);
    std::array<int, mapplan::kMaxRegions> members{};
    std::array<long, mapplan::kMaxRegions> sx{}, sy{};
    for (std::size_t g = 0; g < p.regionOf.size() && g < s.groups.size(); ++g) {
        const int r = p.regionOf[g];
        if (r >= mapplan::kMaxRegions) continue;
        regionOfGroup[g] = r;
        regions = std::max(regions, r + 1);
        const auto& grp = s.groups[g];
        auto& reg = R[static_cast<std::size_t>(r)];
        reg.rate += value(grp) / static_cast<float>(std::max(grp.respawn, 1));
        sx[static_cast<std::size_t>(r)] += grp.center.x;
        sy[static_cast<std::size_t>(r)] += grp.center.y;
        ++members[static_cast<std::size_t>(r)];
    }
    for (int r = 0; r < regions; ++r) {
        auto& reg = R[static_cast<std::size_t>(r)];
        const auto m = std::max(members[static_cast<std::size_t>(r)], 1);
        reg.centre = {static_cast<int>(sx[static_cast<std::size_t>(r)] / m), static_cast<int>(sy[static_cast<std::size_t>(r)] / m)};
        const float o = static_cast<float>(dist(reg.centre, s.spawnCenter)), e = static_cast<float>(dist(reg.centre, s.enemySpawnCenter));
        reg.side = o / std::max(o + e, 1.0f);
    }
    allyRegion.fill(-1);
    for (const auto& [t, r] : p.regionFor) {
        if (r >= regions) continue;
        if (t == w.type) region = r;
        for (int sl = 0; sl < w.allyCount && sl < 8; ++sl)
            if (sl != w.slot && w.allyTypes[static_cast<std::size_t>(sl)] == t) allyRegion[static_cast<std::size_t>(sl)] = r;
    }
    nextReplan = tune::territory_first_replan;
    FixedStr<48> msg;
    trace().note((msg << "territory: r" << region << " of " << regions).view());
}

// Champions like us a camp needs: the fewest whose tank outlasts the clear.
int partyNeed(const World& w, const GroupInfo& g) {
    if (g.harmless) return 1;
    const float dps = std::max(combat::campDamagePerTurn(w, g), 1.0f);
    for (int m = 1; m <= 5; ++m) {
        const float kill = g.totalHealth / (dps * static_cast<float>(m));
        if (kill * g.totalStrength * (m > 1 ? tune::party_focus_share : 1.0f) <= tune::route_max_hp_use * w.maxHp) return m;
    }
    return 6;
}

// What this region is worth to us now: what the champions there with us can take (party camps
// count once enough of us are there), shared among them; less where enemies were seen or we died
// lately, less on their side unless most of their team is down, and less for the walk there.
float worth(const World& w, int r, const std::array<int, mapplan::kMaxRegions>& allies) {
    const auto& reg = R[static_cast<std::size_t>(r)];
    const auto& s = Statics::get();
    const int k = 1 + allies[static_cast<std::size_t>(r)];
    float take = 0;
    for (std::size_t g = 0; g < regionOfGroup.size(); ++g)
        if (regionOfGroup[g] == r && partyNeed(w, s.groups[g]) <= k) take += value(s.groups[g]) / static_cast<float>(std::max(s.groups[g].respawn, 1));
    float v = take / static_cast<float>(k);
    const bool theyAreDown = w.enemyBodies > 0 && w.enemiesAlive * 2 < w.enemyBodies;
    if (!theyAreDown) {
        const float t = std::clamp((reg.side - 0.5f) / 0.2f, 0.0f, 1.0f);
        v *= 1.0f - (1.0f - tune::territory_far_weight) * t;
    }
    if (w.round - reg.seenRound <= tune::territory_contest_rounds) v *= tune::territory_contested_factor;
    if (w.round - reg.deathRound <= tune::territory_death_rounds) v *= tune::territory_contested_factor;
    return v * tune::territory_stay / (tune::territory_stay + static_cast<float>(dist(w.pos, reg.centre)));
}

void replan(World& w, const char* why) {
    // Allies by the region they work: the plan's, then whatever their camp claims show (shared
    // memory has no room for a region byte; claims only exist once they are at work there).
    std::array<int, mapplan::kMaxRegions> allies{};
    for (int sl = 0; sl < w.allyCount && sl < 8; ++sl)
        if (sl != w.slot && allyRegion[static_cast<std::size_t>(sl)] >= 0) ++allies[static_cast<std::size_t>(allyRegion[static_cast<std::size_t>(sl)])];
    int best = region;
    float bestV = region >= 0 ? worth(w, region, allies) : 0.0f;
    const float keep = bestV;
    for (int r = 0; r < regions; ++r) {
        const float v = worth(w, r, allies);
        if (v > bestV) { bestV = v; best = r; }
    }
    if (best != region && bestV > keep * tune::territory_switch_margin) {
        FixedStr<64> msg;
        trace().note((msg << "territory: r" << region << " -> r" << best << " (" << why << ")").view());
        region = best;
        switchedRound = w.round;
        arrived = false;
    }
}
} // namespace

void update(World& w) {
    if (!tune::territory) return;
    if (!init) setup(w);
    if (regions == 0) return;
    // Sightings: an enemy champion inside a region marks it contested.
    bool mineContested = false;
    for (const auto& e : w.enemies) {
        if (e.hp <= 0) continue;
        const int r = regionAt(e.pos);
        if (r < 0) continue;
        R[static_cast<std::size_t>(r)].seenRound = w.round;
        mineContested = mineContested || r == region;
    }
    // Back from a death (a gap in the turns we act): the region we were last in cost us a life.
    static int lastActed = -1;
    static cambc::vec2 lastPos{-1, -1};
    if (lastActed >= 0 && w.round - lastActed > tune::territory_death_gap && lastPos.x >= 0) {
        const int r = regionAt(lastPos);
        if (r >= 0) R[static_cast<std::size_t>(r)].deathRound = w.round;
    }
    lastActed = w.round;
    lastPos = w.pos;
    for (int sl = 0; sl < w.allyCount && sl < comms::alert_byte; ++sl) {
        if (sl == w.slot) continue;
        const int g = comms::claimOf(sl);
        if (g >= 0 && g < static_cast<int>(regionOfGroup.size()) && regionOfGroup[static_cast<std::size_t>(g)] >= 0)
            allyRegion[static_cast<std::size_t>(sl)] = regionOfGroup[static_cast<std::size_t>(g)];
    }
    if (region >= 0 && regionAt(w.pos) == region) arrived = true;
    // No switching back and forth: a region keeps us territory_min_stay rounds, a death there aside;
    // the scheduled look waits until we have got there at least once.
    const bool diedThere = region >= 0 && w.round - R[static_cast<std::size_t>(region)].deathRound <= 1;
    const bool settled = w.round - switchedRound >= tune::territory_min_stay;
    if (diedThere) replan(w, "died there");
    else if (mineContested && settled) replan(w, "enemies here");
    else if (w.round >= nextReplan && settled && arrived) replan(w, "scheduled");
    if (w.round >= nextReplan) nextReplan = w.round + tune::territory_replan;
}

int mine() { return tune::territory ? region : -1; }

int regionOf(int groupId) {
    if (groupId < 0 || groupId >= static_cast<int>(regionOfGroup.size())) return -1;
    return regionOfGroup[static_cast<std::size_t>(groupId)];
}

float travelFactor(const World& w, const GroupInfo& g) {
    if (!tune::territory || region < 0 || regionOf(g.id) != region || arrived) return 1.0f;
    return tune::territory_travel_share;
}

float farmFactor(const World& w, const GroupInfo& g) {
    if (!tune::territory || region < 0) return 1.0f;
    if (regionOf(g.id) == region) return tune::territory_bonus;
    if (dist(w.pos, g.center) <= tune::territory_near) return 1.0f;
    return tune::territory_off;
}

} // namespace territory
