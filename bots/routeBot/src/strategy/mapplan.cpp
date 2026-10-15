#include "mapplan.h"
#include <algorithm>
#include <cmath>
#include "../core/comms.h"
#include "../core/config.h"
#include "../core/profile.h"
#include "../core/statics.h"
#include "../core/trace.h"
#include "../core/world.h"

namespace bc = cambc;

namespace mapplan {

namespace {

constexpr int kFar = 1 << 14;
constexpr std::uint8_t kMagic = 0x52;  // 'R'
constexpr std::uint8_t kVersion = 1;
constexpr std::size_t kHeader = 4 + 32;  // magic, version, flags, group count, centre bitmap

// One BFS, exported so reads are local array lookups instead of host calls.
struct FieldRead {
    std::vector<std::uint16_t> dist;
    std::vector<std::uint8_t> reached;
    int width = 0, height = 0;

    int at(bc::vec2 p) const {
        if (p.x < 0 || p.y < 0 || p.x >= width || p.y >= height) return kFar;
        const auto i = static_cast<std::size_t>(p.y * width + p.x);
        return (reached[i >> 3] >> (i & 7)) & 1 ? dist[i] : kFar;
    }
    int minOver(const std::vector<bc::vec2>& tiles) const {
        int best = kFar;
        for (auto t : tiles) best = std::min(best, at(t));
        return best;
    }
};

struct Analysis {
    bool done = false, failed = false;
    int n = 0;                                   // groups analysed (ids 0..n-1)
    std::vector<int> dOur, dEnemy, dAltar;       // per group
    std::vector<std::vector<std::uint16_t>> dGroup;  // walking distance group -> group
    std::vector<std::uint8_t> farmable, centre;
    Features f;
    unsigned long long maxFieldCost = 30000;  // largest field seen so far (start pessimistic)
    unsigned long long fuelUsed = 0, fuelCompute = 0, fuelExport = 0, fuelStatics = 0, fuelSetup = 0;
    int fields = 0;
} A;

FieldRead build(const bc::bitboard& blocked, const bc::bitboard& roots) {
    const auto& s = Statics::get();
    bc::distance_field field;
    bc::bitboard visited;
    const auto t0 = bc::fuel_remaining();
    field.compute(blocked, roots, visited);
    const auto t1 = bc::fuel_remaining();
    FieldRead f;
    f.width = s.width;
    f.height = s.height;
    f.dist.resize(static_cast<std::size_t>(s.width * s.height));
    field.export_linear_buffer(f.dist);
    f.reached = visited.export_linear_buffer();
    A.fuelCompute += t0 - t1;
    A.fuelExport += t1 - bc::fuel_remaining();
    return f;
}


float value(const GroupInfo& g) { return static_cast<float>(g.gems) + tune::route_lambda * static_cast<float>(g.gold); }

// A typical champion (~50 damage per turn, ~1000 health) cannot take this group alone.
bool needsParty(const GroupInfo& g) {
    if (g.harmless) return false;
    const float kill = g.totalHealth / 50.0f;
    return kill * g.totalStrength > 1000.0f * tune::route_max_hp_use;
}

void computeFeatures() {
    const auto& s = Statics::get();
    auto& f = A.f;
    float total = 0, centre = 0, harmless = 0, gold = 0, splash = 0, party = 0, all = 0;
    float ourSum = 0, enemySum = 0;
    const float band = tune::centre_band * f.spawnToSpawn;
    A.centre.assign(static_cast<std::size_t>(A.n), 0);
    for (int i = 0; i < A.n; ++i) {
        const auto& g = s.groups[static_cast<std::size_t>(i)];
        if (!A.farmable[static_cast<std::size_t>(i)]) continue;
        const float v = value(g);
        const float rate = v / static_cast<float>(std::max(g.respawn, 1));
        total += rate;
        all += v;
        if (g.harmless) harmless += v;
        gold += tune::route_lambda * static_cast<float>(g.gold);
        if (g.adjacentPairs > 0) splash += v;
        if (needsParty(g)) party += v;
        const int o = A.dOur[static_cast<std::size_t>(i)], e = A.dEnemy[static_cast<std::size_t>(i)];
        if (o < kFar && e < kFar && static_cast<float>(std::abs(o - e)) <= band) {
            A.centre[static_cast<std::size_t>(i)] = 1;
            centre += rate;
            ourSum += static_cast<float>(o);
            enemySum += static_cast<float>(e);
            ++f.centreGroups;
        }
    }
    f.incomeRate = total;
    f.centreShare = total > 0 ? centre / total : 0;
    f.harmlessShare = all > 0 ? harmless / all : 0;
    f.goldShare = all > 0 ? gold / all : 0;
    f.splashShare = all > 0 ? splash / all : 0;
    f.partyShare = all > 0 ? party / all : 0;
    if (f.centreGroups) {
        f.centreOurDist = ourSum / static_cast<float>(f.centreGroups);
        f.centreEnemyDist = enemySum / static_cast<float>(f.centreGroups);
    }
}

std::vector<std::vector<bc::vec2>> rings;  // walkable tiles around each altar
std::vector<int> fieldQueue;               // groups still waiting for their own field
std::size_t queueNext = 0;
std::vector<std::uint8_t> hasField;

// Stage 1 (must finish on the first draft turn): statics, both spawn fields and the map
// features. Everything the picks and the centre decision need.
bool runAnalysis() {
    const auto start = bc::fuel_remaining();
    const auto& s = Statics::get();
    A.fuelStatics = start - bc::fuel_remaining();
    A.n = std::min(static_cast<int>(s.groups.size()), kMaxGroups);
    const auto n = static_cast<std::size_t>(A.n);
    A.farmable.assign(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& g = s.groups[i];
        A.farmable[i] = (g.gems > 0 || g.gold > 0) && !g.hasMonolith ? 1 : 0;  // the Monolith has its own decision
    }

    // Altars are never walkable: measure to the walkable ring around them.
    for (auto a : s.altars) {
        bc::bitboard ring;
        ring.clear();
        ring.draw_chebyshev(a, 1.0f);
        rings.push_back((ring & ~s.terrain).positions());
    }

    // Spawn fields. Our terrain blocks the enemy spawn, so theirs uses walls/holes/altars only.
    const auto ours = build(s.terrain, s.allySpawn);
    const auto theirs = build(bc::map::walls() | bc::map::holes() | bc::map::altars(), s.enemySpawn);
    A.fields = 2;
    A.f.spawnToSpawn = static_cast<float>(theirs.minOver(s.allySpawn.positions()));
    if (A.f.spawnToSpawn >= kFar) A.f.spawnToSpawn = static_cast<float>(dist(s.spawnCenter, s.enemySpawnCenter));
    A.dOur.assign(n, kFar);
    A.dEnemy.assign(n, kFar);
    for (std::size_t i = 0; i < n; ++i) {
        A.dOur[i] = ours.minOver(s.groups[i].tiles);
        A.dEnemy[i] = theirs.minOver(s.groups[i].tiles);
    }
    const float band = tune::centre_band * A.f.spawnToSpawn;
    for (const auto& r : rings) {
        const int o = ours.minOver(r), e = theirs.minOver(r);
        if (o < kFar && e < kFar && static_cast<float>(std::abs(o - e)) <= band) A.f.centreAltar = true;
    }
    computeFeatures();

    // Stage 2 queue: our side and the centre only (routes never go further), nearest first.
    A.dGroup.assign(n, std::vector<std::uint16_t>(n, static_cast<std::uint16_t>(kFar)));
    A.dAltar.assign(n, kFar);
    hasField.assign(n, 0);
    for (int i = 0; i < A.n; ++i) {
        const auto k = static_cast<std::size_t>(i);
        if (A.farmable[k] && A.dOur[k] < kFar && static_cast<float>(A.dOur[k]) <= static_cast<float>(A.dEnemy[k]) + band)
            fieldQueue.push_back(i);
    }
    std::sort(fieldQueue.begin(), fieldQueue.end(), [](int a, int b) {
        return A.dOur[static_cast<std::size_t>(a)] < A.dOur[static_cast<std::size_t>(b)];
    });
    A.fuelSetup = start - bc::fuel_remaining() - A.fuelStatics;
    A.fuelUsed = start - bc::fuel_remaining();
    return true;
}

// Stage 2 (any draft turn with fuel to spare): one field per queued group, giving exact
// group -> group and group -> altar distances. Missing ones are estimated (see groupDist).
void continueFields(unsigned long long reserve) {
    const auto& s = Statics::get();
    const auto n = static_cast<std::size_t>(A.n);
    const auto start = bc::fuel_remaining();
    // Only start a field that fits: a draft turn that overruns loses its output.
    while (queueNext < fieldQueue.size() && bc::fuel_remaining() > reserve + A.maxFieldCost) {
        const auto before = bc::fuel_remaining();
        const auto h = static_cast<std::size_t>(fieldQueue[queueNext++]);
        const auto f = build(s.terrain, s.groups[h].tileMask);
        ++A.fields;
        for (std::size_t g = 0; g < n; ++g)
            if (A.farmable[g]) A.dGroup[h][g] = static_cast<std::uint16_t>(std::min(f.minOver(s.groups[g].tiles), kFar));
        int best = kFar;
        for (const auto& r : rings) best = std::min(best, f.minOver(r));
        A.dAltar[h] = best;
        hasField[h] = 1;
        A.maxFieldCost = std::max(A.maxFieldCost, before - bc::fuel_remaining());
    }
    A.fuelUsed += start - bc::fuel_remaining();
}

// Walking distance between groups: exact from either group's field, else centre-to-centre
// straight-line distance with ~25% for walls (the estimate farm scoring uses beyond its BFS).
int groupDist(int h, int g) {
    const auto hi = static_cast<std::size_t>(h), gi = static_cast<std::size_t>(g);
    if (hasField[hi]) return A.dGroup[hi][gi];
    if (hasField[gi]) return A.dGroup[gi][hi];
    const auto& s = Statics::get();
    return static_cast<int>(static_cast<float>(dist(s.groups[hi].center, s.groups[gi].center)) * 1.25f);
}

int altarDist(int g) {
    const auto gi = static_cast<std::size_t>(g);
    if (hasField[gi]) return A.dAltar[gi];
    const auto& s = Statics::get();
    int best = kFar;
    for (auto a : s.altars)
        for (auto t : s.groups[gi].tiles) best = std::min(best, dist(a, t));
    return best >= kFar ? kFar : static_cast<int>(static_cast<float>(best) * 1.25f);
}

// ---- route scoring (R3): value per turn minus risk, for one champion

struct Champ {
    bc::champion_type type;
    float dps, hp, bankTarget;
};

Champ champ(bc::champion_type t) {
    const auto& p = profile(t);
    // No per-type stats in the draft: health scales with the profile's durability (Ryth ~1,190).
    return {t, std::max(p.farmPower, 1.0f), 950.0f * p.survivability, tuning(t).bankTarget};
}

struct Step {
    bool ok = false;
    float value = 0, turns = 0, risk = 0;
};

// The camp's part of a step (kill, capture, risk), before any walking: computed once per
// champion and camp, then planning only adds travel (keeps the draft turn's fuel small).
Step campStep(const Champ& c, int g) {
    const auto& s = Statics::get();
    const auto& grp = s.groups[static_cast<std::size_t>(g)];
    Step st;
    float dps = c.dps;
    // Varg's basic attacks splash every adjacent monster.
    if (c.type == bc::champion_type::varg && grp.campCount > 1)
        dps *= 1.0f + std::min(1.0f, 2.0f * static_cast<float>(grp.adjacentPairs) / static_cast<float>(grp.campCount));
    const float kill = grp.totalHealth / dps;
    const float damage = kill * grp.totalStrength;
    if (damage > tune::route_max_hp_use * c.hp) return st;  // needs a party: not a route camp
    const float s2s = std::max(A.f.spawnToSpawn, 1.0f);
    const float nearEnemy = std::clamp(1.0f - static_cast<float>(A.dEnemy[static_cast<std::size_t>(g)]) / (0.5f * s2s), 0.0f, 1.0f);
    st.ok = true;
    st.value = value(grp);
    st.turns = kill + tune::capture_turns;
    st.risk = damage / c.hp + nearEnemy;
    return st;
}

Step walked(Step st, int travel) {
    if (travel >= kFar) st.ok = false;
    st.turns += static_cast<float>(travel);
    return st;
}

// Built once at the end of the first draft turn (10x fuel): every champion type's camp steps
// and each group's walk to the nearest altar, so later picks only assign and look ahead.
std::array<std::vector<Step>, 12> stepTable;
constexpr std::size_t kSpawnTop = 10, kNearList = 6;
std::array<std::vector<std::pair<float, int>>, 12> spawnTop;  // per type: best first camps from spawn
std::vector<std::vector<std::pair<int, int>>> nearList;       // per group: (walk, group), nearest first
std::array<bool, 12> typeReady{};
std::vector<std::uint8_t> nearReady;
std::vector<int> pool;  // every group a route could use (our side or the centre)
std::vector<int> altarT;
bool tableReady = false;

// score = (gems + lambda gold) / (t_travel + t_kill + 5 + t_to_altar) - w_risk * risk
float scoreSteps(const Step* steps, int count, int lastGroup) {
    float v = 0, t = 0, risk = 0;
    for (int i = 0; i < count; ++i) {
        v += steps[i].value;
        t += steps[i].turns;
        risk = std::max(risk, steps[i].risk);
    }
    t += static_cast<float>(std::min(tableReady ? altarT[static_cast<std::size_t>(lastGroup)] : altarDist(lastGroup), kFar));
    return v / std::max(t, 1.0f) - tune::route_risk_weight * risk;
}

bool candidate(int g, bool contest) {
    const auto i = static_cast<std::size_t>(g);
    if (!A.farmable[i] || A.dOur[i] >= kFar) return false;
    return A.dOur[i] <= A.dEnemy[i] || (contest && A.centre[i]);
}

bool decideCentre(const std::vector<bc::champion_type>& roster, int teamSize) {
    const auto& f = A.f;
    // A centre that holds little of the map's income is never worth contesting, however close
    // or well supplied it is (collision: 12% of income, yet the other three rules all held).
    if (f.centreGroups == 0 || f.centreShare < tune::centre_share) return false;
    int rules = 0;
    if (f.centreOurDist <= f.centreEnemyDist * 1.05f) ++rules;
    if (f.centreAltar) ++rules;
    // Strong early, or more bodies than picks (Cassi & Issac).
    float strength = 0;
    for (auto t : roster) strength += profile(t).farmPower * profile(t).survivability;
    if (!roster.empty()) strength /= static_cast<float>(roster.size());
    if (static_cast<int>(roster.size()) > teamSize || strength >= 50.0f) ++rules;
    return rules >= tune::centre_rules_needed;
}

// ---- gameplay state
Plan readCache;
bool readDone = false;
const Route* mine = nullptr;
int stepIdx = 0;
bool ended = true, followInit = false;

} // namespace

bool analyse() {
    if (A.done) return !A.failed;
    A.done = true;
    try {
        A.failed = !runAnalysis();
    } catch (const bc::error&) {
        A.failed = true;
    }
    return !A.failed;
}

void buildFields(unsigned long long reserve) {
    if (!analysed()) return;
    try {
        continueFields(reserve);
    } catch (const bc::error&) {
        // Missing fields fall back to distance estimates.
    }
}

bool analysed() { return A.done && !A.failed; }

void prepare(unsigned long long floor) {
    if (tableReady || !analysed()) return;
    const auto n = static_cast<std::size_t>(A.n);
    altarT.assign(n, kFar);
    for (std::size_t g = 0; g < n; ++g)
        if (A.farmable[g]) altarT[g] = altarDist(static_cast<int>(g));
    tableReady = true;  // scoreSteps reads altarT from here on
    pool.clear();
    for (int g = 0; g < A.n; ++g)
        if (candidate(g, true)) pool.push_back(g);
    // Each group's nearest pool groups: the lookahead's candidates for the next step. Built while
    // fuel lasts; groups without a list fall back to scanning the pool.
    nearList.assign(n, {});
    nearReady.assign(n, 0);
    for (int h : pool) {
        if (bc::fuel_remaining() < floor) break;
        auto& list = nearList[static_cast<std::size_t>(h)];
        for (int g : pool)
            if (g != h) list.push_back({groupDist(h, g), g});
        std::sort(list.begin(), list.end());
        if (list.size() > kNearList) list.resize(kNearList);
        nearReady[static_cast<std::size_t>(h)] = 1;
    }
}

namespace {
// A champion type's camp steps and best first camps, built the first time it is drafted.
void ensureType(std::size_t t) {
    if (typeReady[t]) return;
    typeReady[t] = true;
    const auto c = champ(static_cast<bc::champion_type>(t));
    stepTable[t].assign(static_cast<std::size_t>(A.n), Step{});
    spawnTop[t].clear();
    for (int g : pool) {
        const auto st = campStep(c, g);
        stepTable[t][static_cast<std::size_t>(g)] = st;
        const auto from = walked(st, A.dOur[static_cast<std::size_t>(g)]);
        if (from.ok) spawnTop[t].push_back({scoreSteps(&from, 1, g), g});
    }
    std::sort(spawnTop[t].begin(), spawnTop[t].end(), [](const auto& x, const auto& y) { return x.first > y.first; });
    if (spawnTop[t].size() > kSpawnTop) spawnTop[t].resize(kSpawnTop);
}
} // namespace

const Features& features() { return A.f; }

float draftBonus(bc::champion_draft pick, int teamSize) {
    if (!analysed()) return 0;
    const auto& f = A.f;
    using D = bc::champion_draft;
    switch (pick) {
    case D::varg: return 3.0f * f.splashShare;
    case D::vesper_vale: return 3.0f * f.goldShare;
    case D::ryth:
    case D::marcella: return 4.0f * std::max(0.0f, f.partyShare - 0.5f);
    case D::tavi:
    case D::ren: return f.centreShare >= tune::centre_share ? 1.0f : 0.0f;  // pick-offs pay where both teams meet
    case D::cassi_and_issac: return teamSize <= 3 ? 1.0f : 0.0f;
    case D::prism: return -2.0f;
    default: return 0;
    }
}

std::vector<bc::champion_type> bodies(const std::vector<bc::champion_draft>& picks) {
    std::vector<bc::champion_type> out;
    for (auto p : picks) {
        using D = bc::champion_draft;
        using T = bc::champion_type;
        switch (p) {
        case D::ryth: out.push_back(T::ryth); break;
        case D::vesper_vale: out.push_back(T::vesper_vale); break;
        case D::vendetta: out.push_back(T::vendetta); break;
        case D::cassi_and_issac: out.push_back(T::cassi); out.push_back(T::issac); break;
        case D::stellar: out.push_back(T::stellar); break;
        case D::prism: out.push_back(T::prism); break;
        case D::marcella: out.push_back(T::marcella); break;
        case D::willow: out.push_back(T::willow); break;
        case D::ren: out.push_back(T::ren); break;
        case D::varg: out.push_back(T::varg); break;
        case D::tavi: out.push_back(T::tavi); break;
        }
    }
    return out;
}

Plan planFor(const std::vector<bc::champion_type>& roster, int teamSize, unsigned long long fuelFloor) {
    Plan p;
    if (!analysed()) return p;
    p.valid = true;
    p.contestCentre = decideCentre(roster, teamSize);
    p.centre = A.centre;
    prepare(0);
    const int n = A.n;
    std::vector<Champ> champs;
    for (auto t : roster) champs.push_back(champ(t));
    auto typeOf = [&](std::size_t c) { return static_cast<std::size_t>(champs[c].type) % 12; };
    for (std::size_t c = 0; c < champs.size(); ++c) ensureType(typeOf(c));
    auto stepOf = [&](std::size_t c, int g, int travel) { return walked(stepTable[typeOf(c)][static_cast<std::size_t>(g)], travel); };
    // Each pair costs ~200 fuel on a 100k pick turn, so everything below reads the tables
    // prepare() built on the first turn and stops early when fuel runs short.

    // R4 assignment: greedy over champion-camp pairs from spawn, best pair first.
    std::vector<std::uint8_t> used(static_cast<std::size_t>(n), 0);
    std::vector<int> first(champs.size(), -1);
    struct Pair { float score; int c, g; };
    std::vector<Pair> pairs;
    for (std::size_t c = 0; c < champs.size(); ++c)
        for (const auto& [score, g] : spawnTop[typeOf(c)])
            if (score > 0 && candidate(g, p.contestCentre)) pairs.push_back({score, static_cast<int>(c), g});
    std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) {
        return a.score != b.score ? a.score > b.score : a.g < b.g;
    });
    std::vector<int> order;  // champions in assignment order
    for (const auto& pr : pairs) {
        if (first[static_cast<std::size_t>(pr.c)] >= 0 || used[static_cast<std::size_t>(pr.g)]) continue;
        first[static_cast<std::size_t>(pr.c)] = pr.g;
        used[static_cast<std::size_t>(pr.g)] = 1;
        order.push_back(pr.c);
    }

    // 2-3 step lookahead from each first camp over its nearest groups: best total value / total time.
    for (int c : order) {
        const auto ci = static_cast<std::size_t>(c);
        // Short of fuel: remaining champions keep just their first camp (a turn that overruns
        // before the pick gets a random pick).
        const bool lookahead = bc::fuel_remaining() > fuelFloor;
        const auto& ch = champs[ci];
        const int g1 = first[ci];
        Step steps[3];
        steps[0] = stepOf(ci, g1, A.dOur[static_cast<std::size_t>(g1)]);
        std::vector<std::pair<float, int>> near;
        auto consider = [&](int g, int walk) {
            if (used[static_cast<std::size_t>(g)] || !candidate(g, p.contestCentre)) return;
            const auto st = stepOf(ci, g, walk);
            if (st.ok) near.push_back({scoreSteps(&st, 1, g), g});
        };
        // No neighbour list (the first turn ran short): the route keeps its first camp only.
        if (lookahead && nearReady[static_cast<std::size_t>(g1)])
            for (const auto& [walk, g] : nearList[static_cast<std::size_t>(g1)]) consider(g, walk);
        std::sort(near.begin(), near.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        if (static_cast<int>(near.size()) > tune::route_lookahead) near.resize(static_cast<std::size_t>(tune::route_lookahead));
        int best2 = -1, best3 = -1;
        float best = scoreSteps(steps, 1, g1);
        for (const auto& [s2, g2] : near) {
            steps[1] = stepOf(ci, g2, groupDist(g1, g2));
            if (!steps[1].ok) continue;
            if (const float sc = scoreSteps(steps, 2, g2); sc > best) { best = sc; best2 = g2; best3 = -1; }
            for (const auto& [s3, g3] : near) {
                if (g3 == g2) continue;
                steps[2] = stepOf(ci, g3, groupDist(g2, g3));
                if (!steps[2].ok) continue;
                if (const float sc = scoreSteps(steps, 3, g3); sc > best) { best = sc; best2 = g2; best3 = g3; }
            }
        }
        Route r;
        r.type = ch.type;
        int carried = 0;
        bool banked = false;
        for (int g : {g1, best2, best3}) {
            if (g < 0) break;
            used[static_cast<std::size_t>(g)] = 1;
            r.steps[static_cast<std::size_t>(r.len++)] = static_cast<std::uint8_t>(g);
            carried += Statics::get().groups[static_cast<std::size_t>(g)].gems;
            // Altar visit once the bag reaches this champion's bank target.
            if (!banked && static_cast<float>(carried) >= ch.bankTarget && r.len < kMaxSteps) {
                r.steps[static_cast<std::size_t>(r.len++)] = kAltarStep;
                banked = true;
            }
        }
        p.routes.push_back(r);
    }
    return p;
}

void write(const Plan& p) {
    std::vector<std::uint8_t> bytes(kHeader, 0);
    bytes[0] = kMagic;
    bytes[1] = kVersion;
    bytes[2] = static_cast<std::uint8_t>((p.valid ? 1 : 0) | (p.contestCentre ? 2 : 0));
    bytes[3] = static_cast<std::uint8_t>(std::min<std::size_t>(p.centre.size(), kMaxGroups));
    for (std::size_t i = 0; i < p.centre.size() && i < kMaxGroups; ++i)
        if (p.centre[i]) bytes[4 + i / 8] |= static_cast<std::uint8_t>(1u << (i % 8));
    bytes.push_back(static_cast<std::uint8_t>(p.routes.size()));
    for (const auto& r : p.routes) {
        bytes.push_back(static_cast<std::uint8_t>(r.type));
        bytes.push_back(static_cast<std::uint8_t>(r.len));
        for (int i = 0; i < r.len; ++i) bytes.push_back(r.steps[static_cast<std::size_t>(i)]);
    }
    bc::draft_memory = bytes;
}

std::string describe(const Plan& p) {
    const auto& f = A.f;
    std::string out = analysed() ? "" : "analysis failed; ";
    out += "fuel " + num(static_cast<long long>(A.fuelUsed / 1000)) + "k for " + num(A.fields) + "/" + num(static_cast<long long>(fieldQueue.size()) + 2) + " fields (bfs " + num(static_cast<long long>(A.fuelCompute / 1000)) + "k export " +
           num(static_cast<long long>(A.fuelExport / 1000)) + "k statics " + num(static_cast<long long>(A.fuelStatics / 1000)) + "k setup " +
           num(static_cast<long long>(A.fuelSetup / 1000)) + "k, " + num(A.n) + " groups); s2s " +
           num(static_cast<long long>(f.spawnToSpawn)) + "; centre " + num(f.centreGroups) + " groups " +
           num(static_cast<long long>(f.centreShare * 100)) + "% income" + (f.centreAltar ? " +altar" : "") +
           (p.contestCentre ? " -> CONTEST" : " -> farm own side") + "; splash " +
           num(static_cast<long long>(f.splashShare * 100)) + "% gold " + num(static_cast<long long>(f.goldShare * 100)) +
           "% party " + num(static_cast<long long>(f.partyShare * 100)) + "%";
    for (const auto& r : p.routes) {
        out += "; ";
        out += name(r.type);
        for (int i = 0; i < r.len; ++i) {
            const auto st = r.steps[static_cast<std::size_t>(i)];
            out += i ? " > " : ": ";
            out += st == kAltarStep ? std::string("altar") : "g" + num(st);
        }
    }
    return out;
}

const Plan& read() {
    if (readDone) return readCache;
    readDone = true;
    try {
        const auto& m = bc::draft_memory;
        auto at = [&](std::size_t i) { return static_cast<std::uint8_t>(m[i]); };
        if (m.size() <= kHeader || at(0) != kMagic || at(1) != kVersion) return readCache;
        readCache.valid = at(2) & 1;
        readCache.contestCentre = at(2) & 2;
        const int groups = at(3);
        readCache.centre.assign(static_cast<std::size_t>(groups), 0);
        for (int i = 0; i < groups; ++i)
            readCache.centre[static_cast<std::size_t>(i)] = (at(4 + static_cast<std::size_t>(i) / 8) >> (i % 8)) & 1;
        std::size_t pos = kHeader;
        const int routes = at(pos++);
        for (int r = 0; r < routes; ++r) {
            Route route;
            route.type = static_cast<bc::champion_type>(at(pos++));
            route.len = std::min<int>(at(pos++), kMaxSteps);
            for (int i = 0; i < route.len; ++i) route.steps[static_cast<std::size_t>(i)] = at(pos++);
            readCache.routes.push_back(route);
        }
    } catch (const std::exception&) {
        // Truncated or foreign memory: whatever decoded so far stands; the brain covers the rest.
    }
    return readCache;
}

bool isCentre(int groupId) {
    const auto& p = read();
    return groupId >= 0 && groupId < static_cast<int>(p.centre.size()) && p.centre[static_cast<std::size_t>(groupId)];
}

namespace {
void endRoute(const char* why) {
    if (ended) return;
    ended = true;
    trace().note(std::string("route ended: ") + why);
}
} // namespace

void follow(World& w) {
    if (!followInit) {
        followInit = true;
        for (const auto& r : read().routes)
            if (r.type == w.type) mine = &r;
        ended = mine == nullptr || mine->len == 0;
        trace().note("route: " + routeText());
    }
    if (ended) return;
    if (w.round > tune::route_max_round) return endRoute("opening over");
    if (w.hpFrac() < tune::route_end_hp) return endRoute("low health");
    const auto& s = Statics::get();
    while (stepIdx < mine->len) {
        const auto step = mine->steps[static_cast<std::size_t>(stepIdx)];
        if (step == kAltarStep) {
            if (w.gems > 0) return;
            ++stepIdx;
            continue;
        }
        if (step >= w.groups.size()) { ++stepIdx; continue; }
        // Captured (by us or anyone): on to the next step.
        if (w.groups[step].status == GroupStatus::respawning) {
            ++stepIdx;
            trace().note("route: next " + routeText());
            continue;
        }
        // A nearer ally took it over: the assignment no longer holds, the brain picks.
        if (comms::ownersMask(step, w.slot, dist(w.pos, s.groups[step].center))) {
            ++stepIdx;
            continue;
        }
        if (w.enemyPressure(s.groups[step].center, tune::route_enemy_radius) > 0) return endRoute("enemy near target");
        return;
    }
    endRoute("done");
}

int routeGroup() {
    if (ended || !mine || stepIdx >= mine->len) return -1;
    const auto step = mine->steps[static_cast<std::size_t>(stepIdx)];
    return step == kAltarStep ? -1 : step;
}

bool routeWantsBank(const World& w) {
    return !ended && mine && stepIdx < mine->len && mine->steps[static_cast<std::size_t>(stepIdx)] == kAltarStep &&
           w.gems > 0;
}

std::string routeText() {
    if (!mine) return read().valid ? "none" : "no plan";
    std::string out;
    for (int i = stepIdx; i < mine->len; ++i) {
        const auto st = mine->steps[static_cast<std::size_t>(i)];
        if (!out.empty()) out += " > ";
        out += st == kAltarStep ? std::string("altar") : "g" + num(st);
    }
    return out.empty() ? "done" : out;
}

} // namespace mapplan
