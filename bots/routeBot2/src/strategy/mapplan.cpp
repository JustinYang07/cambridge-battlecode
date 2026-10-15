#include "mapplan.h"
#include <algorithm>
#include <cmath>
#include "../core/comms.h"
#include "../core/config.h"
#include "../core/profile.h"
#include "../core/statics.h"
#include "../core/trace.h"
#include "../core/world.h"
#include "../shop/shop.h"

namespace bc = cambc;

namespace mapplan {

namespace {

constexpr int kFar = 1 << 14;
constexpr std::uint8_t kMagic = 0x52;  // 'R'
constexpr std::uint8_t kVersion = 2;
constexpr std::size_t kHeader = 4 + 32;  // magic, version, flags, group count, centre bitmap; then depths, lanes

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


// ---- lanes: which camps each group of champions clears, and in what order

bool ourHalf(int g) {
    const auto i = static_cast<std::size_t>(g);
    return A.farmable[i] && A.dOur[i] < kFar && A.dOur[i] <= A.dEnemy[i];
}

// 0 at our spawn, 255 at theirs (by walking distance); 128 is halfway.
std::uint8_t depthOf(int g) {
    const auto i = static_cast<std::size_t>(g);
    const float o = static_cast<float>(std::min(A.dOur[i], kFar)), e = static_cast<float>(std::min(A.dEnemy[i], kFar));
    return static_cast<std::uint8_t>(std::clamp(o / std::max(o + e, 1.0f) * 255.0f, 0.0f, 255.0f));
}

// Turns a typical champion spends on a camp: kill (~50 damage per turn) plus capture.
float workOf(int g) {
    return Statics::get().groups[static_cast<std::size_t>(g)].totalHealth / 50.0f + tune::capture_turns;
}

std::vector<int> deepTour, baseTour;  // clear paths, built by prepare()
std::vector<int> centreTour;          // our side of the centre band: the duo's, only when we contest it
std::vector<std::uint8_t> depthBytes;
bool lanesReady = false;
std::string laneNote;                 // "nearest-neighbour" or "by distance (short of fuel)"

// Order a set of camps into a path: from spawn, always the nearest camp not yet visited.
// Below `floor` fuel the rest is ordered by distance from spawn (no pairwise distances).
std::vector<int> tour(std::vector<int> set, unsigned long long floor) {
    std::vector<int> out;
    int at = -1;
    while (!set.empty()) {
        if (bc::fuel_remaining() < floor) {
            std::sort(set.begin(), set.end(), [](int a, int b) { return A.dOur[static_cast<std::size_t>(a)] < A.dOur[static_cast<std::size_t>(b)]; });
            out.insert(out.end(), set.begin(), set.end());
            laneNote = "by distance (short of fuel)";
            break;
        }
        std::size_t best = 0;
        int bestD = kFar + 1;
        for (std::size_t k = 0; k < set.size(); ++k) {
            const int d = at < 0 ? A.dOur[static_cast<std::size_t>(set[k])] : groupDist(at, set[k]);
            if (d < bestD) { bestD = d; best = k; }
        }
        at = set[best];
        out.push_back(at);
        set.erase(set.begin() + static_cast<std::ptrdiff_t>(best));
    }
    return out;
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

bool linked(bc::champion_type t) { return t == bc::champion_type::cassi || t == bc::champion_type::issac; }

// ---- gameplay state
Plan readCache;
bool readDone = false;
const Route* mine = nullptr;
unsigned partnerMask = 0;  // allied slots clearing the same lane
int stepIdx = 0;
bool ended = true, followInit = false, suspended = false, idleLoop = false;
int spare = -1;  // our loop is down: another camp on our half that is up

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
    if (lanesReady || !analysed()) return;
    lanesReady = true;
    laneNote = "nearest-neighbour";
    const auto& s = Statics::get();
    depthBytes.assign(static_cast<std::size_t>(A.n), 128);
    // Centre-band camps are contested: they stay out of the loops unless the draft decides to
    // contest the centre (lanes: 78% of income in the band; looping through it fed kills).
    std::vector<int> half, centre;
    for (int g = 0; g < A.n; ++g) {
        depthBytes[static_cast<std::size_t>(g)] = depthOf(g);
        if (!ourHalf(g)) continue;
        if (A.centre[static_cast<std::size_t>(g)]) centre.push_back(g);
        else half.push_back(g);
    }
    // The duo's share of the work is its share of our bodies (teamSize + 1 for Cassi & Issac, who
    // lead both rosters). Camps one champion cannot take alone always go to the duo; the rest fill
    // the deep lane from the deepest camp inward.
    const float bodiesGuess = static_cast<float>(std::max(2, s.teamSize + 1));
    float total = 0;
    for (int g : half) total += workOf(g);
    const float deepTarget = total * std::min(1.0f, 2.0f / bodiesGuess);
    std::vector<int> deep, rest;
    float deepWork = 0;
    for (int g : half)
        if (needsParty(s.groups[static_cast<std::size_t>(g)])) { deep.push_back(g); deepWork += workOf(g); }
        else rest.push_back(g);
    std::sort(rest.begin(), rest.end(), [](int a, int b) { return depthBytes[static_cast<std::size_t>(a)] > depthBytes[static_cast<std::size_t>(b)]; });
    std::vector<int> base;
    for (int g : rest)
        if (deepWork < deepTarget) { deep.push_back(g); deepWork += workOf(g); }
        else base.push_back(g);
    deepTour = tour(deep, floor);
    baseTour = tour(base, floor);
    centreTour = tour(centre, floor);
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

Plan planFor(const std::vector<bc::champion_type>& roster, int teamSize) {
    Plan p;
    if (!analysed()) return p;
    prepare(0);
    p.valid = true;
    p.contestCentre = decideCentre(roster, teamSize);
    p.centre = A.centre;
    p.depth = depthBytes;

    // The duo: the toughest champion plus the best melee damage dealer. Cassi & Issac die
    // together, so they stay one base unit rather than splitting across lanes.
    int tank = -1, dps = -1;
    float tankScore = -1, dpsScore = -1;
    for (int i = 0; i < static_cast<int>(roster.size()); ++i) {
        if (linked(roster[static_cast<std::size_t>(i)])) continue;
        const auto& pr = profile(roster[static_cast<std::size_t>(i)]);
        const float sc = pr.roles.tank * pr.survivability;
        if (sc > tankScore) { tankScore = sc; tank = i; }
    }
    for (int i = 0; i < static_cast<int>(roster.size()); ++i) {
        if (i == tank || linked(roster[static_cast<std::size_t>(i)])) continue;
        const auto& pr = profile(roster[static_cast<std::size_t>(i)]);
        const float sc = pr.farmPower * (1.0f + pr.roles.duelist + 0.5f * pr.roles.farmer - pr.roles.caster);
        if (sc > dpsScore) { dpsScore = sc; dps = i; }
    }
    std::vector<int> duo;
    for (int i : {tank, dps})
        if (i >= 0) duo.push_back(i);
    // Base units: everyone else, Cassi & Issac together.
    std::vector<std::vector<int>> units;
    int linkedUnit = -1;
    for (int i = 0; i < static_cast<int>(roster.size()); ++i) {
        if (std::find(duo.begin(), duo.end(), i) != duo.end()) continue;
        if (linked(roster[static_cast<std::size_t>(i)])) {
            if (linkedUnit < 0) { linkedUnit = static_cast<int>(units.size()); units.push_back({}); }
            units[static_cast<std::size_t>(linkedUnit)].push_back(i);
        } else {
            units.push_back({i});
        }
    }
    auto addRoute = [&](int idx, int lane, const std::vector<int>& path) {
        Route r;
        r.type = roster[static_cast<std::size_t>(idx)];
        r.lane = lane;
        for (int g : path) {
            if (r.len >= kMaxSteps) break;
            r.steps[static_cast<std::size_t>(r.len++)] = static_cast<std::uint8_t>(g);
        }
        p.routes.push_back(r);
    };
    // Nobody for the base, or no base camps (every camp needs a party, as on elkring): everyone
    // clears the whole half together.
    if (units.empty() || duo.empty() || baseTour.empty()) {
        auto all = deepTour;
        all.insert(all.end(), baseTour.begin(), baseTour.end());
        if (p.contestCentre) all.insert(all.end(), centreTour.begin(), centreTour.end());
        for (int i = 0; i < static_cast<int>(roster.size()); ++i) addRoute(i, 0, all);
        return p;
    }
    auto deepLane = deepTour;
    if (p.contestCentre) deepLane.insert(deepLane.end(), centreTour.begin(), centreTour.end());
    for (int i : duo) addRoute(i, 0, deepLane);
    // Base lane: contiguous stretches of the path with equal work, one per unit.
    float total = 0;
    for (int g : baseTour) total += workOf(g);
    const float share = total / static_cast<float>(units.size());
    std::size_t at = 0;
    for (std::size_t u = 0; u < units.size(); ++u) {
        std::vector<int> stretch;
        float work = 0;
        const bool last = u + 1 == units.size();
        while (at < baseTour.size() && (last || work < share || stretch.empty())) {
            work += workOf(baseTour[at]);
            stretch.push_back(baseTour[at++]);
        }
        // A unit left without camps (tiny base lane) shares the nearest stretch instead.
        if (stretch.empty()) stretch = baseTour;
        for (int i : units[u]) addRoute(i, static_cast<int>(u) + 1, stretch);
    }
    return p;
}

void write(const Plan& p) {
    std::vector<std::uint8_t> bytes(kHeader, 0);
    bytes[0] = kMagic;
    bytes[1] = kVersion;
    bytes[2] = static_cast<std::uint8_t>((p.valid ? 1 : 0) | (p.contestCentre ? 2 : 0));
    const auto n = std::min<std::size_t>(p.centre.size(), kMaxGroups);
    bytes[3] = static_cast<std::uint8_t>(n);
    for (std::size_t i = 0; i < n; ++i)
        if (p.centre[i]) bytes[4 + i / 8] |= static_cast<std::uint8_t>(1u << (i % 8));
    for (std::size_t i = 0; i < n; ++i) bytes.push_back(i < p.depth.size() ? p.depth[i] : 128);
    bytes.push_back(static_cast<std::uint8_t>(p.routes.size()));
    for (const auto& r : p.routes) {
        bytes.push_back(static_cast<std::uint8_t>(r.type));
        bytes.push_back(static_cast<std::uint8_t>(r.lane));
        bytes.push_back(static_cast<std::uint8_t>(r.len));
        for (int i = 0; i < r.len; ++i) bytes.push_back(r.steps[static_cast<std::size_t>(i)]);
    }
    bc::draft_memory = bytes;
}

std::string describe(const Plan& p) {
    const auto& f = A.f;
    std::string out = analysed() ? "" : "analysis failed; ";
    out += "fuel " + num(static_cast<long long>(A.fuelUsed / 1000)) + "k for " + num(A.fields) + "/" +
           num(static_cast<long long>(fieldQueue.size()) + 2) + " fields, " + num(A.n) + " groups; s2s " +
           num(static_cast<long long>(f.spawnToSpawn)) + "; centre " + num(f.centreGroups) + " groups " +
           num(static_cast<long long>(f.centreShare * 100)) + "% income" + (f.centreAltar ? " +altar" : "") +
           (p.contestCentre ? " -> CONTEST" : " -> farm own side") + "; splash " +
           num(static_cast<long long>(f.splashShare * 100)) + "% gold " + num(static_cast<long long>(f.goldShare * 100)) +
           "% party " + num(static_cast<long long>(f.partyShare * 100)) + "%; lanes " + laneNote;
    int lastLane = -1;
    for (const auto& r : p.routes) {
        if (r.lane == lastLane) {
            out += "+";
            out += name(r.type);
            continue;
        }
        if (lastLane >= 0) out += "]";
        lastLane = r.lane;
        out += r.lane == 0 ? "; deep: " : "; base: ";
        for (int i = 0; i < r.len; ++i) out += (i ? ">g" : "g") + num(r.steps[static_cast<std::size_t>(i)]);
        out += " [";
        out += name(r.type);
    }
    if (lastLane >= 0) out += "]";
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
        readCache.depth.assign(static_cast<std::size_t>(groups), 128);
        for (int i = 0; i < groups; ++i) readCache.depth[static_cast<std::size_t>(i)] = at(pos++);
        const int routes = at(pos++);
        for (int r = 0; r < routes; ++r) {
            Route route;
            route.type = static_cast<bc::champion_type>(at(pos++));
            route.lane = at(pos++);
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
    idleLoop = false;
    trace().note(std::string("clear loop ended: ") + why);
}
} // namespace

bool clearing() { return !ended; }
bool downtime() { return !ended && idleLoop; }

void follow(World& w) {
    if (!followInit) {
        followInit = true;
        for (const auto& r : read().routes)
            if (r.type == w.type) mine = &r;
        ended = mine == nullptr || mine->len == 0;
        if (mine)
            for (int s = 0; s < w.allyCount; ++s) {
                if (s == w.slot) continue;
                for (const auto& r : read().routes)
                    if (r.type == w.allyTypes[static_cast<std::size_t>(s)] && r.lane == mine->lane) partnerMask |= 1u << s;
            }
        trace().note("clear loop: " + routeText());
    }
    suspended = false;
    if (ended) return;
    // Scaled: items bought, time to contest. Late: the race is on, the brain decides.
    if (shop::shardsOwned() >= tune::contest_shards) return endRoute("scaled");
    if (w.round > tune::clear_max_round) return endRoute("late game");
    const auto& s = Statics::get();

    // The next camp on the loop that is up, or back by the time we walk there. Taken camps
    // (respawning) are skipped, so capturing one moves us on. A gem or gold cluster our partner
    // is already on is left to them (a capture is split, not sped up); camps that need the pair
    // keep us together. Another lane's champion on a camp means it is theirs now.
    const int len = mine->len;
    int pick = -1, soonest = -1, soonestAt = 1 << 30;
    for (int k = 0; k < len; ++k) {
        const int idx = (stepIdx + k) % len;
        const int g = mine->steps[static_cast<std::size_t>(idx)];
        if (g >= static_cast<int>(w.groups.size())) continue;
        const auto& grp = s.groups[static_cast<std::size_t>(g)];
        const auto& mem = w.groups[static_cast<std::size_t>(g)];
        const unsigned claimers = comms::claimersMask(g, w.slot);
        if (claimers & ~partnerMask) continue;
        if ((claimers & partnerMask) && grp.harmless) continue;
        if (mem.status == GroupStatus::respawning) {
            if (mem.respawnAt < soonestAt) { soonestAt = mem.respawnAt; soonest = idx; }
            if (mem.respawnAt - w.round > dist(w.pos, grp.center) + tune::clear_wait_slack) continue;
        }
        pick = idx;
        break;
    }
    const bool wasIdle = idleLoop;
    const int prevSpare = spare;
    spare = -1;
    idleLoop = false;
    if (pick < 0) {
        // Our loop is down. Remember the camp back first, for when it returns.
        if (soonest >= 0) stepIdx = soonest;
        // Clearing our half comes first: the nearest camp on our half that is up and nobody has claimed.
        const auto& depth = read().depth;
        int bestD = 1 << 20;
        for (const auto& grp : s.groups) {
            if (grp.id >= static_cast<int>(depth.size()) || depth[static_cast<std::size_t>(grp.id)] > 128) continue;
            if ((grp.gems == 0 && grp.gold == 0) || grp.hasMonolith) continue;
            if (isCentre(grp.id) && !read().contestCentre) continue;
            const auto& mem = w.groups[static_cast<std::size_t>(grp.id)];
            if (mem.status == GroupStatus::respawning && mem.respawnAt - w.round > dist(w.pos, grp.center) + tune::clear_wait_slack) continue;
            if (comms::claimersMask(grp.id, w.slot) & ~partnerMask) continue;
            const int d = dist(w.pos, grp.center);
            if (d < bestD) { bestD = d; spare = grp.id; }
        }
        if (spare >= 0) {
            if (w.hpFrac() < tune::route_end_hp || w.enemyPressure(s.groups[static_cast<std::size_t>(spare)].center, tune::route_enemy_radius) > 0)
                suspended = true;
            if (spare != prevSpare) trace().note("clear loop down: clearing g" + num(spare) + " on our half");
        } else {
            // Downtime: our whole half is down. Bank or roam the middle (healthy), else heal first.
            idleLoop = true;
            if (!wasIdle) trace().note("our half is down: bank or roam the middle");
        }
        return;
    }
    if (pick != stepIdx) {
        stepIdx = pick;
        trace().note("clear loop: next " + routeText());
    }
    // Fights and heals come first: no pull toward the camp while an enemy is near it or we are hurt.
    const int g = mine->steps[static_cast<std::size_t>(stepIdx)];
    if (w.hpFrac() < tune::route_end_hp || w.enemyPressure(s.groups[static_cast<std::size_t>(g)].center, tune::route_enemy_radius) > 0)
        suspended = true;
}

int routeGroup() {
    if (ended || suspended || idleLoop || !mine || stepIdx >= mine->len) return -1;
    if (spare >= 0) return spare;
    return mine->steps[static_cast<std::size_t>(stepIdx)];
}

float enemyHalfFactor(int groupId) {
    const auto& p = read();
    if (groupId < 0 || groupId >= static_cast<int>(p.depth.size())) return 1.0f;
    const float depth = static_cast<float>(p.depth[static_cast<std::size_t>(groupId)]) / 255.0f;
    if (depth <= 0.5f) return 1.0f;
    // Linear from 1 at halfway down to the floor at their spawn.
    const float floor = clearing() && !downtime() ? tune::enemy_half_factor : tune::enemy_half_factor_scaled;
    return 1.0f - (depth - 0.5f) / 0.5f * (1.0f - floor);
}

bc::vec2 middle() {
    const auto& s = Statics::get();
    return {(s.spawnCenter.x + s.enemySpawnCenter.x) / 2, (s.spawnCenter.y + s.enemySpawnCenter.y) / 2};
}

std::string routeText() {
    if (!mine) return read().valid ? "none" : "no plan";
    std::string out = mine->lane == 0 ? "deep " : "base ";
    for (int k = 0; k < mine->len; ++k) {
        const int idx = (stepIdx + k) % mine->len;
        out += (k ? ">g" : "g") + num(mine->steps[static_cast<std::size_t>(idx)]);
    }
    return out;
}

} // namespace mapplan
