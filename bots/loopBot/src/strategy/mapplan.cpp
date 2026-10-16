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
constexpr std::uint8_t kVersion = 3;
constexpr std::uint8_t kOpeningMarker = 0xA5;  // loopBot: opening camp follows
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

// ---- farming loops (draft): cyclic orienteering over camp groups
//
// A loop is a cycle a party repeats all game. Its rate is gems per round:
//   rate = sum(value) / max(walk + sum(kill + capture) + bank + heal, max respawn + capture)
// so a lap shorter than a camp's respawn gains nothing (the party would wait). Each camp's value is
// weighted by how far toward the enemy it sits (our side counts in full). Solved by cheapest
// insertion and then local search (2-opt, moving one camp, dropping, swapping in an unused camp),
// for 1, 2 and 3 parallel loops; the best total wins. Every step is small and the state persists,
// so solving stops at a fuel floor and resumes on the next draft turn. Draft fuel is dear (heap
// allocations especially), so tours are fixed arrays and insertions are scored from running sums.
namespace {

struct LoopCand {
    int g = 0;
    float value = 0, gems = 0, resp = 0;
    int dA = 0, dO = 0;
    std::array<float, 7> kill{}, dmg{};  // by party size 1..6: turns to kill, damage per head
    std::array<std::uint8_t, 7> ok{};    // a party this size can take it (its tank survives)
};

struct Tour {
    int n = 0;
    std::array<int, kMaxLoopLen> v{};  // indices into LS.c
};

// Running sums of a tour, enough to score it (and to score one insertion into it) in O(1).
struct Sums {
    float val = 0, gems = 0, serv = 0, dmg = 0, resp = 0, walk = 0, detour = 1e9f;
    int dO = kFar;
};

struct Solution {
    bool done = false;
    int k = 0;
    float total = 0;
    std::array<int, kMaxLoops> m{};      // bodies per loop
    std::array<Tour, kMaxLoops> tours;
    std::array<float, kMaxLoops> rate{};
};

struct LoopSolver {
    bool started = false, ready = false;
    int bodies = 0;
    float dps = 50, hp = 950, bankT = 50;
    std::vector<LoopCand> c;
    int nc = 0;
    std::vector<std::uint16_t> d;  // nc x nc walking distances
    int k = 1, phase = 0, sweeps = 0, move = 0;
    bool cycleImproved = false;
    Solution cur;
    std::array<Sums, kMaxLoops> sums;
    std::vector<std::uint8_t> used;
    // Per loop and candidate: cheapest extra walk to insert it, and the edge (by its start camp).
    std::array<std::vector<int>, kMaxLoops> insAdd, insFrom;
    std::array<Solution, kMaxLoops + 1> best;  // finished solutions by k
    unsigned long long insertCost = 15000, sweepCost = 15000, fuel = 0, setupFuel = 0;
    int evals = 0;
} LS;

int dd(int a, int b) { return LS.d[static_cast<std::size_t>(a * LS.nc + b)]; }
const LoopCand& cand(int i) { return LS.c[static_cast<std::size_t>(i)]; }
std::size_t party(int m) { return static_cast<std::size_t>(std::clamp(m, 1, 6)); }

// Detour to bank from the edge a -> b.
float edgeDetour(int a, int b) { return static_cast<float>(cand(a).dA + cand(b).dA - dd(a, b)); }

Sums sumsOf(const Tour& t, int m) {
    Sums s;
    const auto mi = party(m);
    const float capCost = m > 1 ? tune::loop_handoff_turns : tune::capture_turns;
    for (int i = 0; i < t.n; ++i) {
        const auto& x = cand(t.v[static_cast<std::size_t>(i)]);
        s.val += x.value;
        s.gems += x.gems;
        s.serv += x.kill[mi] + capCost;
        s.dmg += x.dmg[mi];
        s.resp = std::max(s.resp, x.resp);
        s.dO = std::min(s.dO, x.dO);
        if (t.n > 1) {
            const int a = t.v[static_cast<std::size_t>(i)], b = t.v[static_cast<std::size_t>((i + 1) % t.n)];
            s.walk += static_cast<float>(dd(a, b));
            s.detour = std::min(s.detour, edgeDetour(a, b));
        } else {
            s.detour = 2.0f * static_cast<float>(x.dA);
        }
    }
    return s;
}

float rateOf(const Sums& s, int m) {
    if (s.val <= 0) return 0;
    ++LS.evals;
    const float mf = static_cast<float>(m);
    // The bag is split across the party; each banks once it holds its bank target.
    const float bank = std::min(1.0f, s.gems / (mf * LS.bankT)) * std::max(s.detour, 0.0f) + s.gems / (10.0f * mf);
    // Damage taken per lap, paid back by trips home (recall, then the walk back).
    const float heal = s.dmg / (tune::loop_heal_hp * LS.hp) * (tune::recall_turns + static_cast<float>(std::min(s.dO, 200)));
    const float lap = std::max(s.walk + s.serv + bank + heal, s.resp + tune::capture_turns);
    return s.val / std::max(lap, 1.0f);
}

float rateOf(const Tour& t, int m) { return rateOf(sumsOf(t, m), m); }

void insertAt(Tour& t, int pos, int x) {
    for (int i = t.n; i > pos; --i) t.v[static_cast<std::size_t>(i)] = t.v[static_cast<std::size_t>(i - 1)];
    t.v[static_cast<std::size_t>(pos)] = x;
    ++t.n;
}

void eraseAt(Tour& t, int pos) {
    for (int i = pos; i + 1 < t.n; ++i) t.v[static_cast<std::size_t>(i)] = t.v[static_cast<std::size_t>(i + 1)];
    --t.n;
}

void beginK(int k) {
    LS.k = k;
    LS.phase = 0;
    LS.sweeps = 0;
    LS.move = 0;
    LS.cycleImproved = false;
    LS.cur = Solution{};
    LS.cur.k = k;
    for (int l = 0; l < k && l < kMaxLoops; ++l) LS.cur.m[static_cast<std::size_t>(l)] = LS.bodies / k + (l < LS.bodies % k ? 1 : 0);
    LS.sums = {};
    LS.used.assign(static_cast<std::size_t>(LS.nc), 0);
    for (auto& v : LS.insAdd) v.assign(static_cast<std::size_t>(LS.nc), 0);
    for (auto& v : LS.insFrom) v.assign(static_cast<std::size_t>(LS.nc), -1);
}

// Cheapest gap for candidate ci in tour t (tours of 2+ camps): extra walk, and the gap's start camp.
void scanGap(const Tour& t, int ci, int& add, int& from) {
    add = 1 << 30;
    from = -1;
    for (int i = 0; i < t.n; ++i) {
        const int a = t.v[static_cast<std::size_t>(i)], b = t.v[static_cast<std::size_t>((i + 1) % t.n)];
        const int x = dd(a, ci) + dd(ci, b) - dd(a, b);
        if (x < add) { add = x; from = a; }
    }
}

// After inserting x between a and b in loop l: only gaps a->x and x->b are new; candidates whose
// best gap was a->b (now gone) rescan.
void updateGaps(int l, int a, int x, int b) {
    const auto li = static_cast<std::size_t>(l);
    const auto& t = LS.cur.tours[li];
    for (int ci = 0; ci < LS.nc; ++ci) {
        const auto c = static_cast<std::size_t>(ci);
        if (LS.used[c]) continue;
        if (t.n == 2 || LS.insFrom[li][c] == a || LS.insFrom[li][c] < 0) {
            scanGap(t, ci, LS.insAdd[li][c], LS.insFrom[li][c]);
            continue;
        }
        const int ax = dd(a, ci) + dd(ci, x) - dd(a, x), xb = dd(x, ci) + dd(ci, b) - dd(x, b);
        if (ax < LS.insAdd[li][c]) { LS.insAdd[li][c] = ax; LS.insFrom[li][c] = a; }
        if (xb < LS.insAdd[li][c]) { LS.insAdd[li][c] = xb; LS.insFrom[li][c] = x; }
    }
}

// Position just after camp `from` in t.
int posAfter(const Tour& t, int from) {
    for (int i = 0; i < t.n; ++i)
        if (t.v[static_cast<std::size_t>(i)] == from) return i + 1;
    return t.n;
}

void startLoops(const std::vector<bc::champion_type>& predicted) {
    LS.started = true;
    const auto& s = Statics::get();
    LS.bodies = std::clamp(static_cast<int>(predicted.size()), 1, 6);
    float dps = 0, hp = 0, bank = 0;
    for (auto t : predicted) {
        const auto ch = champ(t);
        dps += ch.dps;
        hp += ch.hp;
        bank += ch.bankTarget;
    }
    const float nb = static_cast<float>(std::max<std::size_t>(predicted.size(), 1));
    LS.dps = std::max(dps / nb, 10.0f) * tune::loop_dps_scale;
    LS.hp = std::max(hp / nb, 300.0f);
    LS.bankT = std::max(bank / nb, 20.0f);

    // Candidates: value per turn from spawn, our side weighted in full and the enemy's side less.
    std::vector<std::pair<float, int>> rank;
    std::vector<float> weight(static_cast<std::size_t>(A.n), 0);
    for (int g = 0; g < A.n; ++g) {
        const auto gi = static_cast<std::size_t>(g);
        if (!A.farmable[gi] || A.dOur[gi] >= kFar) continue;
        const float o = static_cast<float>(A.dOur[gi]);
        const float r = A.dEnemy[gi] >= kFar ? 0.0f : o / std::max(o + static_cast<float>(A.dEnemy[gi]), 1.0f);
        const float t = std::clamp((r - tune::loop_near_side) / (tune::loop_far_side - tune::loop_near_side), 0.0f, 1.0f);
        weight[gi] = 1.0f - (1.0f - tune::loop_far_weight) * t;
        const auto& grp = s.groups[gi];
        rank.push_back({weight[gi] * value(grp) / (o + grp.totalHealth / LS.dps + tune::capture_turns), g});
    }
    std::sort(rank.begin(), rank.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
    if (static_cast<int>(rank.size()) > tune::loop_candidates) rank.resize(static_cast<std::size_t>(tune::loop_candidates));
    for (const auto& [score, g] : rank) {
        const auto gi = static_cast<std::size_t>(g);
        const auto& grp = s.groups[gi];
        LoopCand x;
        x.g = g;
        x.value = weight[gi] * value(grp);
        x.gems = static_cast<float>(grp.gems);
        x.resp = static_cast<float>(grp.respawn);
        x.dA = std::min(altarDist(g), 200);
        x.dO = A.dOur[gi];
        const float soloKill = grp.totalHealth / LS.dps;
        for (int m = 1; m <= 6; ++m) {
            const auto mi = static_cast<std::size_t>(m);
            // Parties do not kill m times faster: walking in and spreading out costs a few turns.
            x.kill[mi] = m == 1 ? soloKill : tune::loop_party_overhead + soloKill / static_cast<float>(m);
            // Monsters hit the nearest champion: the party's tank takes most of it.
            const float tankHit = grp.harmless ? 0.0f : x.kill[mi] * grp.totalStrength * (m > 1 ? tune::party_focus_share : 1.0f);
            x.dmg[mi] = grp.harmless ? 0.0f : x.kill[mi] * grp.totalStrength / static_cast<float>(m);
            x.ok[mi] = tankHit <= tune::route_max_hp_use * LS.hp ? 1 : 0;
        }
        LS.c.push_back(x);
    }
    LS.nc = static_cast<int>(LS.c.size());
    LS.d.assign(static_cast<std::size_t>(LS.nc * LS.nc), 0);
    for (int a = 0; a < LS.nc; ++a)
        for (int b = a + 1; b < LS.nc; ++b) {
            const auto v = static_cast<std::uint16_t>(std::min(groupDist(cand(a).g, cand(b).g), 2000));
            LS.d[static_cast<std::size_t>(a * LS.nc + b)] = v;
            LS.d[static_cast<std::size_t>(b * LS.nc + a)] = v;
        }
    beginK(1);
}

// One insertion: the unused camp, loop and position that raise the rate most. False if none does.
// Scored from the tour's running sums (the detour is optimistic: the replaced edge is not removed).
bool insertStep() {
    float bestGain = 1e-5f;
    int bc_ = -1, bl = -1, bp = -1;
    for (int l = 0; l < LS.k; ++l) {
        const auto li = static_cast<std::size_t>(l);
        const auto& t = LS.cur.tours[li];
        if (t.n >= kMaxLoopLen) continue;
        const int m = LS.cur.m[li];
        const auto mi = party(m);
        const float capCost = m > 1 ? tune::loop_handoff_turns : tune::capture_turns;
        const auto& s = LS.sums[li];
        const float base = LS.cur.rate[li];
        for (int ci = 0; ci < LS.nc; ++ci) {
            const auto& x = cand(ci);
            if (LS.used[static_cast<std::size_t>(ci)] || !x.ok[mi]) continue;
            Sums ns = s;
            ns.val += x.value;
            ns.gems += x.gems;
            ns.serv += x.kill[mi] + capCost;
            ns.dmg += x.dmg[mi];
            ns.resp = std::max(ns.resp, x.resp);
            ns.dO = std::min(ns.dO, x.dO);
            int pos = t.n;
            if (t.n == 0) {
                ns.detour = 2.0f * static_cast<float>(x.dA);
            } else if (t.n == 1) {
                const int a = t.v[0];
                ns.walk = static_cast<float>(2 * dd(a, ci));
                ns.detour = std::min(edgeDetour(a, ci), edgeDetour(ci, a));
            } else {
                const int a = LS.insFrom[li][static_cast<std::size_t>(ci)];
                if (a < 0) continue;
                pos = posAfter(t, a);
                const int b = t.v[static_cast<std::size_t>(pos % t.n)];
                ns.walk += static_cast<float>(LS.insAdd[li][static_cast<std::size_t>(ci)]);
                ns.detour = std::min({ns.detour, edgeDetour(a, ci), edgeDetour(ci, b)});
            }
            const float gain = rateOf(ns, m) - base;
            if (gain > bestGain) { bestGain = gain; bc_ = ci; bl = l; bp = pos; }
        }
    }
    if (bc_ < 0) return false;
    const auto li = static_cast<std::size_t>(bl);
    auto& t = LS.cur.tours[li];
    insertAt(t, bp, bc_);
    LS.used[static_cast<std::size_t>(bc_)] = 1;
    LS.sums[li] = sumsOf(t, LS.cur.m[li]);
    LS.cur.rate[li] = rateOf(LS.sums[li], LS.cur.m[li]);
    if (t.n >= 2) updateGaps(bl, t.v[static_cast<std::size_t>((bp + t.n - 1) % t.n)], bc_, t.v[static_cast<std::size_t>((bp + 1) % t.n)]);
    return true;
}

// One kind of local-search move over every loop (0: 2-opt, 1: move one camp, 2: drop a camp,
// 3: swap in an unused camp). Split up so each step stays small on a 100k pick turn.
bool improveSweep(int kind) {
    bool improved = false;
    for (int l = 0; l < LS.k; ++l) {
        const auto li = static_cast<std::size_t>(l);
        auto& t = LS.cur.tours[li];
        const int m = LS.cur.m[li];
        float r = LS.cur.rate[li];
        auto tryKeep = [&](const Tour& alt) {
            const float nr = rateOf(alt, m);
            if (nr > r + 1e-6f) { r = nr; t = alt; improved = true; return true; }
            return false;
        };
        auto at = [&](int i) { return t.v[static_cast<std::size_t>(((i % t.n) + t.n) % t.n)]; };
        // 2-opt: reverse t[i..j]; scored only when it shortens the walk.
        for (int i = 0; kind == 0 && t.n >= 4 && i < t.n - 1; ++i)
            for (int j = i + 1; j < t.n; ++j) {
                if (i == 0 && j == t.n - 1) continue;
                if (dd(at(i - 1), at(j)) + dd(at(i), at(j + 1)) - dd(at(i - 1), at(i)) - dd(at(j), at(j + 1)) >= 0) continue;
                Tour alt = t;
                std::reverse(alt.v.begin() + i, alt.v.begin() + j + 1);
                tryKeep(alt);
            }
        // Or-opt: move one camp to the gap where it costs least, when that shortens the walk.
        for (int i = 0; kind == 1 && t.n >= 3 && i < t.n; ++i) {
            const int x = at(i);
            const int saved = dd(at(i - 1), x) + dd(x, at(i + 1)) - dd(at(i - 1), at(i + 1));
            Tour rest = t;
            eraseAt(rest, i);
            int bestAdd = saved, bestPos = -1;
            for (int j = 0; j < rest.n; ++j) {
                const int u = rest.v[static_cast<std::size_t>(j)], v = rest.v[static_cast<std::size_t>((j + 1) % rest.n)];
                const int add = dd(u, x) + dd(x, v) - dd(u, v);
                if (add < bestAdd) { bestAdd = add; bestPos = j + 1; }
            }
            if (bestPos < 0) continue;
            insertAt(rest, bestPos, x);
            tryKeep(rest);
        }
        // Drop a camp whose time costs more than it brings.
        for (int i = t.n - 1; kind == 2 && t.n > 1 && i >= 0; --i) {
            const int x = t.v[static_cast<std::size_t>(i)];
            Tour alt = t;
            eraseAt(alt, i);
            if (tryKeep(alt)) LS.used[static_cast<std::size_t>(x)] = 0;
        }
        // Swap an unused camp in for the loop camp nearest it.
        const auto mi = party(m);
        for (int ci = 0; kind == 3 && ci < LS.nc && t.n > 0; ++ci) {
            if (LS.used[static_cast<std::size_t>(ci)] || !cand(ci).ok[mi]) continue;
            int near = 0;
            for (int i = 1; i < t.n; ++i)
                if (dd(ci, t.v[static_cast<std::size_t>(i)]) < dd(ci, t.v[static_cast<std::size_t>(near)])) near = i;
            Tour alt = t;
            const int out = alt.v[static_cast<std::size_t>(near)];
            alt.v[static_cast<std::size_t>(near)] = ci;
            if (tryKeep(alt)) {
                LS.used[static_cast<std::size_t>(out)] = 0;
                LS.used[static_cast<std::size_t>(ci)] = 1;
            }
        }
        if (!improved) continue;
        LS.cur.rate[li] = r;
        LS.sums[li] = sumsOf(t, m);
        for (int ci = 0; t.n >= 2 && ci < LS.nc; ++ci)
            if (!LS.used[static_cast<std::size_t>(ci)]) scanGap(t, ci, LS.insAdd[li][static_cast<std::size_t>(ci)], LS.insFrom[li][static_cast<std::size_t>(ci)]);
    }
    return improved;
}

void finishK() {
    auto& s = LS.cur;
    s.total = 0;
    for (int l = 0; l < LS.k; ++l) s.total += s.rate[static_cast<std::size_t>(l)];
    s.total *= std::pow(tune::loop_split_factor, static_cast<float>(LS.k - 1));
    s.done = true;
    LS.best[static_cast<std::size_t>(LS.k)] = s;
    if (LS.k >= std::min(LS.bodies, kMaxLoops)) LS.ready = true;
    else beginK(LS.k + 1);
}

// The best finished solution with at most `maxK` loops (the unfinished k = 1 if nothing finished).
const Solution* bestLoops(int maxK) {
    const Solution* pick = nullptr;
    for (int k = 1; k <= kMaxLoops && k <= maxK; ++k) {
        const auto& s = LS.best[static_cast<std::size_t>(k)];
        if (s.done && (!pick || s.total > pick->total)) pick = &s;
    }
    if (!pick && LS.started && LS.k == 1) pick = &LS.cur;
    return pick;
}

} // namespace

void solveLoops(const std::vector<bc::champion_type>& predicted, unsigned long long floor) {
    if (!tune::loops || !analysed()) return;
    const auto start = bc::fuel_remaining();
    try {
        if (!LS.started) {
            if (predicted.empty()) return;
            startLoops(predicted);
            LS.setupFuel = start - bc::fuel_remaining();
        }
        // Each step only starts when the most it has cost so far still fits above the floor.
        while (!LS.ready && bc::fuel_remaining() > floor + (LS.phase == 0 ? LS.insertCost : LS.sweepCost)) {
            const auto before = bc::fuel_remaining();
            if (LS.phase == 0) {
                if (!insertStep()) LS.phase = 1;
                LS.insertCost = std::max(LS.insertCost, before - bc::fuel_remaining());
            } else {
                // Moves 0-3 are local search, 4 tries another insertion; a full cycle without any
                // gain (or tune::loop_max_sweeps cycles) finishes this number of loops.
                const bool better = LS.move < 4 ? improveSweep(LS.move) : insertStep();
                LS.cycleImproved = LS.cycleImproved || better;
                if (++LS.move == 5) {
                    LS.move = 0;
                    if (!LS.cycleImproved || ++LS.sweeps >= tune::loop_max_sweeps) finishK();
                    LS.cycleImproved = false;
                }
                LS.sweepCost = std::max(LS.sweepCost, before - bc::fuel_remaining());
            }
        }
    } catch (const bc::error&) {
        LS.ready = true;  // keep whatever finished
    }
    LS.fuel += start - bc::fuel_remaining();
}

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

namespace {
std::vector<std::array<std::uint8_t, 3>> neighbourCache;
int neighbourNext = 0;
} // namespace

// Each farmable group's kNeighbours nearest farmable groups by walking distance (exact where the
// draft built fields, estimated elsewhere), while fuel lasts above `floor`; resumes on the next
// pick. Groups still missing use the in-game straight-line fallback.
void buildNeighbours(unsigned long long floor) {
    if (!analysed()) return;
    if (neighbourCache.empty()) neighbourCache.assign(static_cast<std::size_t>(A.n), {255, 255, 255});
    auto& out = neighbourCache;
    for (int h = neighbourNext; h < A.n; neighbourNext = ++h) {
        if (bc::fuel_remaining() < floor) return;
        if (!A.farmable[static_cast<std::size_t>(h)]) continue;
        std::array<std::pair<int, int>, 3> best;
        best.fill({kFar, 255});
        for (int g = 0; g < A.n; ++g) {
            if (g == h || !A.farmable[static_cast<std::size_t>(g)]) continue;
            const int d = groupDist(h, g);
            if (d >= best[2].first) continue;
            best[2] = {d, g};
            for (int i = 2; i > 0 && best[static_cast<std::size_t>(i)].first < best[static_cast<std::size_t>(i - 1)].first; --i)
                std::swap(best[static_cast<std::size_t>(i)], best[static_cast<std::size_t>(i - 1)]);
        }
        for (std::size_t i = 0; i < 3; ++i) out[static_cast<std::size_t>(h)][i] = static_cast<std::uint8_t>(best[i].second);
    }
}

Plan planFor(const std::vector<bc::champion_type>& roster, int teamSize, unsigned long long fuelFloor) {
    Plan p;
    if (!analysed()) return p;
    p.valid = true;
    p.neighbours = neighbourCache;
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

    // Farming loops: the best solution for this many bodies; tanks spread first, one per loop,
    // then every champion joins the loop with the most room left.
    if (const auto* sol = bestLoops(static_cast<int>(roster.size()))) {
        const int k = sol->k;
        std::array<int, kMaxLoops> room{};
        const int nb = static_cast<int>(roster.size());
        for (int l = 0; l < k; ++l) room[static_cast<std::size_t>(l)] = nb / k + (l < nb % k ? 1 : 0);
        for (int l = 0; l < k; ++l) {
            Loop lp;
            lp.bodies = room[static_cast<std::size_t>(l)];
            const auto& t = sol->tours[static_cast<std::size_t>(l)];
            for (int i = 0; i < t.n; ++i) lp.camps.push_back(static_cast<std::uint8_t>(LS.c[static_cast<std::size_t>(t.v[static_cast<std::size_t>(i)])].g));
            p.loops.push_back(lp);
        }
        std::vector<std::size_t> order2(roster.size());
        for (std::size_t i = 0; i < order2.size(); ++i) order2[i] = i;
        std::stable_sort(order2.begin(), order2.end(), [&](std::size_t a, std::size_t b) {
            const auto& pa = profile(roster[a]);
            const auto& pb = profile(roster[b]);
            return pa.roles.tank * pa.survivability > pb.roles.tank * pb.survivability;
        });
        for (auto i : order2) {
            int l = 0;
            for (int j = 1; j < k; ++j)
                if (room[static_cast<std::size_t>(j)] > room[static_cast<std::size_t>(l)]) l = j;
            --room[static_cast<std::size_t>(l)];
            p.loopOf.push_back({roster[i], static_cast<std::uint8_t>(l)});
        }
    }

    // Opening camp (loopBot): on round 1 nobody has claimed anything yet, so a party camp beside
    // spawn looked "too strong" to the first champions to move, and they walked off to a far
    // cluster (rows: Cassi and Issac took the gold 30 tiles away for 10 turns while Varg went to
    // the wolves). The draft sees the whole roster: name the camp the champions without a route
    // open on together. Cautious: a camp that needs a party, near spawn, on our side, that those
    // champions can kill with the tank surviving; the best value per turn of walk + kill + capture.
    {
        // The crew: champions without a route, and those whose route starts far from spawn (rows:
        // Varg's route went to the gold in the far corner; the wolves need it as their tank).
        std::vector<int> crew;
        for (std::size_t c = 0; c < champs.size(); ++c) {
            const int g1 = first[c];
            if (g1 < 0 || A.dOur[static_cast<std::size_t>(g1)] > tune::opening_camp_radius) crew.push_back(static_cast<int>(c));
        }
        if (static_cast<int>(crew.size()) >= 2) {
            float dps = 0, tankHp = 0;
            for (int c : crew) {
                dps += champs[static_cast<std::size_t>(c)].dps;
                tankHp = std::max(tankHp, champs[static_cast<std::size_t>(c)].hp);
            }
            float bestScore = 0;
            for (int g = 0; g < n; ++g) {
                const auto gi = static_cast<std::size_t>(g);
                const auto& grp = Statics::get().groups[gi];
                if (!A.farmable[gi] || grp.harmless || !needsParty(grp) || !candidate(g, p.contestCentre) ||
                    A.dOur[gi] > tune::opening_camp_radius)
                    continue;
                // Clearly ours: a camp about as near them pulls both teams into a fight there
                // (eight's central wolves: 27-33 of our deaths in the A/B).
                if (A.dOur[gi] + tune::opening_safe_margin > A.dEnemy[gi]) continue;
                const float kill = grp.totalHealth / std::max(dps, 1.0f);
                if (kill * grp.totalStrength * tune::party_focus_share > tune::route_max_hp_use * tankHp) continue;
                const float sc = value(grp) / (static_cast<float>(A.dOur[gi]) + kill + tune::capture_turns);
                if (sc > bestScore) {
                    bestScore = sc;
                    p.openingCamp = g;
                    p.openingParty = static_cast<int>(crew.size());
                }
            }
            // Crew members with a (far) route open on the camp instead.
            if (p.openingCamp >= 0)
                for (int c : crew)
                    p.routes.erase(std::remove_if(p.routes.begin(), p.routes.end(),
                                                  [&](const Route& r) { return r.type == champs[static_cast<std::size_t>(c)].type; }),
                                   p.routes.end());
        }
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
    bytes.push_back(static_cast<std::uint8_t>(p.loops.size()));
    for (const auto& l : p.loops) {
        bytes.push_back(static_cast<std::uint8_t>(l.bodies));
        bytes.push_back(static_cast<std::uint8_t>(std::min<std::size_t>(l.camps.size(), kMaxLoopLen)));
        for (std::size_t i = 0; i < l.camps.size() && i < kMaxLoopLen; ++i) bytes.push_back(l.camps[i]);
    }
    bytes.push_back(static_cast<std::uint8_t>(p.loopOf.size()));
    for (const auto& [t, l] : p.loopOf) {
        bytes.push_back(static_cast<std::uint8_t>(t));
        bytes.push_back(l);
    }
    bytes.push_back(static_cast<std::uint8_t>(std::min<std::size_t>(p.neighbours.size(), kMaxGroups)));
    for (std::size_t i = 0; i < p.neighbours.size() && i < kMaxGroups; ++i)
        for (auto v : p.neighbours[i]) bytes.push_back(v);
    // Opening camp: a marker first, since the memory may be zero-padded (0 would read as group 0).
    bytes.push_back(kOpeningMarker);
    bytes.push_back(p.openingCamp >= 0 ? static_cast<std::uint8_t>(p.openingCamp) : 255);
    bytes.push_back(static_cast<std::uint8_t>(p.openingParty));
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
    out += "; neighbours " + num(neighbourNext) + "/" + num(A.n);
    if (LS.started) {
        out += "; loops fuel " + num(static_cast<long long>(LS.fuel / 1000)) + "k (setup " + num(static_cast<long long>(LS.setupFuel / 1000)) + "k, insert " +
               num(static_cast<long long>(LS.insertCost / 1000)) + "k, sweep " + num(static_cast<long long>(LS.sweepCost / 1000)) + "k) evals " + num(LS.evals) +
               " cands " + num(LS.nc) + " k" + num(LS.k) + " phase " + num(LS.phase) + " totals";
        for (int k = 1; k <= kMaxLoops; ++k) {
            const auto& s = LS.best[static_cast<std::size_t>(k)];
            out += " k" + num(k) + "=" + (s.done ? num(static_cast<long long>(s.total * 1000)) : std::string("-"));
        }
        for (std::size_t l = 0; l < p.loops.size(); ++l) {
            out += "; loop" + num(static_cast<long long>(l)) + " x" + num(p.loops[l].bodies);
            for (std::size_t i = 0; i < p.loops[l].camps.size(); ++i) out += (i ? ">g" : ": g") + num(p.loops[l].camps[i]);
        }
    }
    for (const auto& r : p.routes) {
        out += "; ";
        out += name(r.type);
        for (int i = 0; i < r.len; ++i) {
            const auto st = r.steps[static_cast<std::size_t>(i)];
            out += i ? " > " : ": ";
            out += st == kAltarStep ? std::string("altar") : "g" + num(st);
        }
    }
    if (p.openingCamp >= 0) out += "; opening g" + num(p.openingCamp) + " x" + num(p.openingParty);
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
        const int loops = std::min<int>(at(pos++), kMaxLoops);
        for (int l = 0; l < loops; ++l) {
            Loop lp;
            lp.bodies = at(pos++);
            const int len = std::min<int>(at(pos++), kMaxLoopLen);
            for (int i = 0; i < len; ++i) lp.camps.push_back(at(pos++));
            readCache.loops.push_back(lp);
        }
        const int owners = at(pos++);
        for (int i = 0; i < owners; ++i) {
            const auto t = static_cast<bc::champion_type>(at(pos++));
            readCache.loopOf.push_back({t, at(pos++)});
        }
        const int nb = at(pos++);
        readCache.neighbours.assign(static_cast<std::size_t>(nb), {255, 255, 255});
        for (int i = 0; i < nb; ++i)
            for (std::size_t j = 0; j < 3; ++j) readCache.neighbours[static_cast<std::size_t>(i)][j] = at(pos++);
        if (pos + 2 < m.size() && at(pos) == kOpeningMarker) {
            readCache.openingCamp = at(pos + 1) == 255 ? -1 : at(pos + 1);
            readCache.openingParty = at(pos + 2);
        }
    } catch (const std::exception&) {
        // Truncated or foreign memory: whatever decoded so far stands; the brain covers the rest.
    }
    return readCache;
}

bool isNeighbour(int groupId, int other) {
    const auto& p = read();
    if (groupId < 0 || other < 0) return false;
    if (groupId < static_cast<int>(p.neighbours.size()) && p.neighbours[static_cast<std::size_t>(groupId)][0] != 255) {
        for (auto v : p.neighbours[static_cast<std::size_t>(groupId)])
            if (v == other) return true;
        return false;
    }
    // No draft list: the kNeighbours nearest by straight-line distance, computed once per group.
    static std::vector<std::array<int, 3>> local;
    const auto& s = Statics::get();
    if (local.empty()) local.assign(s.groups.size(), {-2, -2, -2});
    auto& mine = local[static_cast<std::size_t>(groupId)];
    if (mine[0] == -2) {
        std::array<std::pair<int, int>, 3> best;
        best.fill({kFar, -1});
        for (const auto& g : s.groups) {
            if (g.id == groupId || g.hasMonolith || (g.gems == 0 && g.gold == 0)) continue;
            const int d = dist(g.center, s.groups[static_cast<std::size_t>(groupId)].center);
            if (d >= best[2].first) continue;
            best[2] = {d, g.id};
            for (int i = 2; i > 0 && best[static_cast<std::size_t>(i)].first < best[static_cast<std::size_t>(i - 1)].first; --i)
                std::swap(best[static_cast<std::size_t>(i)], best[static_cast<std::size_t>(i - 1)]);
        }
        for (std::size_t i = 0; i < 3; ++i) mine[i] = best[i].second;
    }
    return mine[0] == other || mine[1] == other || mine[2] == other;
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

bool routeActive() { return followInit && !ended; }

bool openingActive(const World& w) {
    static bool done = false;
    const auto& p = read();
    if (done || p.openingCamp < 0 || p.openingCamp >= static_cast<int>(w.groups.size())) return false;
    // Over once it has been captured (it respawns) or the opening is past.
    if (w.groups[static_cast<std::size_t>(p.openingCamp)].status == GroupStatus::respawning || w.round > tune::opening_camp_rounds) {
        done = true;
        return false;
    }
    return !routeActive();
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
