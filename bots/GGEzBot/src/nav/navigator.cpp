#include "navigator.h"
#include "../shop/shop.h"
#include "fields.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <vector>
#include "../core/config.h"
#include "../core/fixed_str.h"
#include "../core/trace.h"
#include "../brain/scorers.h"
#include "../strategy/wards.h"

namespace bc = cambc;

Navigator& nav() {
    static Navigator instance;
    return instance;
}

int Navigator::distTo(const World& w, bc::vec2 tile) {
    if (!fromMeValid_ || fromMePos_ != w.pos) {
        bc::bitboard roots;
        roots.clear();
        roots.set(w.pos);
        auto walls = Statics::get().terrain;
        walls.set(w.pos, false);
        fromMe_.compute(walls, roots, fromMeVisited_, static_cast<std::uint16_t>(tune::bfs_radius));
        fromMePos_ = w.pos;
        fromMeValid_ = true;
    }
    // Direct reads (~150 fuel) beat exporting the whole field (2-6k) for the few tiles we query.
    if (!bc::map::is_in_bounds(tile) || !fromMeVisited_.is_set(tile)) return unreachable;
    return fromMe_.get(tile);
}

int Navigator::distToRange(const World& w, bc::vec2 target, int range) {
    if (dist(w.pos, target) <= range) return 0;
    // Chebyshev rings are cheap to scan: walking distance to the ring is at least
    // (chebyshev - range); check the ring tiles nearest us for a real path length.
    int best = unreachable;
    for (int dy = -range; dy <= range; ++dy)
        for (int dx = -range; dx <= range; ++dx) {
            if (std::max(std::abs(dx), std::abs(dy)) != range) continue;
            const bc::vec2 t{target.x + dx, target.y + dy};
            best = std::min(best, distTo(w, t));  // terrain tiles are never reached, so read as unreachable
        }
    return best;
}

bc::bitboard Navigator::blocked(const World& w) const {
    auto b = Statics::get().terrain | w.occupied;
    b.set(w.pos, false);
    return b;
}

bc::bitboard Navigator::standTiles(const World& w, bc::vec2 target, int range) const {
    bc::bitboard tiles;
    tiles.clear();
    tiles.draw_chebyshev(target, static_cast<float>(range));
    auto free = ~blocked(w);
    free.set(w.pos, true);
    return tiles & free;
}

bool Navigator::tryMove(World& w, bc::vec2 dest) {
    auto& me = bc::self();
    try {
        if (me.move_absolute(dest)) {
            FixedStr<16> msg;
            trace().action((msg << 'm' << dest.x << ',' << dest.y).view());
            w.pos = dest;
            return true;
        }
        trace().note("hidden unit");
        w.occupied.set(dest);
    } catch (const bc::error& e) {
        trace().note(std::string("move failed: ") + e.message());
    }
    return false;
}

bool Navigator::tryDash(World& w, const bc::distance_field& field, const bc::bitboard& visited,
                        const bc::bitboard& blockedNow) {
    auto& me = bc::self();
    if (!me.can_cast()) return false;
    for (int slot = 0; slot < 2; ++slot) {
        const auto& ability = w.abilities()[static_cast<std::size_t>(slot)];
        if (!ability.is_dash() || !w.abilityReady(slot)) continue;
        const auto here = field.get(w.pos);
        bc::vec2 best = w.pos;
        auto bestD = here;
        for (auto t : blockedNow.reachable_tiles(w.pos, 2).positions()) {
            if (!visited.is_set(t)) continue;  // never reached: its distance reads 0, not "close"
            const auto d = field.get(t);
            if (d < bestD) { bestD = d; best = t; }
        }
        if (best == w.pos || here - bestD < 2) return false;
        try {
            if (ability.use(best)) {
                w.spentAbility(slot);
                FixedStr<16> msg;
                trace().action((msg << 'd' << best.x << ',' << best.y).view());
                w.pos = best;
                return true;
            }
        } catch (const bc::error& e) {
            trace().note(std::string("dash failed: ") + e.message());
        }
        return false;
    }
    return false;
}

int Navigator::readyDash(World& w) const {
    if (!bc::self().can_cast()) return -1;
    for (int slot = 0; slot < 2; ++slot)
        if (w.abilities()[static_cast<std::size_t>(slot)].is_dash() && w.abilityReady(slot)) return slot;
    return -1;
}

namespace {
// Bush tiles as one byte per tile, exported once (bitboard reads cost ~30-300 fuel each).
bool isBush(bc::vec2 t) {
    static std::vector<std::uint8_t> bits;
    const auto& st = Statics::get();
    if (bits.empty()) {
        bits.assign(static_cast<std::size_t>(st.width * st.height), 0);
        for (auto b : bc::map::bushes().positions()) bits[static_cast<std::size_t>(b.y * st.width + b.x)] = 1;
    }
    if (t.x < 0 || t.y < 0 || t.x >= st.width || t.y >= st.height) return false;
    return bits[static_cast<std::size_t>(t.y * st.width + t.x)] != 0;
}

// combatBotv3: exposure. Stepping toward a goal (and choosing where to stand), a champion picks
// among the moves that make progress the one least exposed:
//   ranged: not nearer the enemy than our melee front, not beside a bush we cannot see into (anyone
//   may wait in one), farther from the enemy (eight: Stellar and Cassi walked along the central bush
//   wall ahead of our melee, user);
//   melee (with a ranged ally near): in front of our ranged, so the hits land on us first.
// One map per round, a window around us filled on demand: every caller that round shares it.
constexpr int kBig = 1 << 20;

bool meleeDesign(bc::champion_type t) {
    static std::array<std::int8_t, 16> cache{};
    const auto i = static_cast<std::size_t>(t);
    if (i >= cache.size() || i >= bc::champion_designs.size()) return false;  // types newer than the SDK (Heart of Ruin)
    if (cache[i] == 0) cache[i] = bc::champion_designs[i].base_stats().base_attack_range <= 1 ? 1 : 2;
    return cache[i] == 1;
}

// Bush distances come from the bush-ward table (wards::bushDistance), built cheaply by a distance
// field on a turn with fuel to spare. Stamping our own table cost 75k+ on corners and lost the turn.
bool ensureBushList(const World& w) {
    (void)w;
    return wards::bushDistance(w.pos) >= 0;
}
int bushNearAt(bc::vec2 t) {
    const int d = wards::bushDistance(t);
    return d < 0 ? 3 : std::min(d, 3);
}

// combatBotv3: unseen bushes in the middle of the map, and the tiles around them (wider when
// carrying gems): whoever waits in one ambushes anyone walking past. Bushes we can see into (our
// ward on them) are fine. Only bushes within 10 tiles of us; false when there are none. The
// centre bush list is built once (a per-call scan of the area cost ~30k fuel a turn).
bool centreBushZone(const World& w, bc::bitboard& zone) {
    static std::vector<bc::vec2> centre;
    static bool built = false;
    if (tune::centre_bush_ring < 0) return false;
    if (!built) {
        built = true;
        const auto& st = Statics::get();
        const float hx = static_cast<float>(st.width) / 2.0f, hy = static_cast<float>(st.height) / 2.0f;
        for (auto t : bc::map::bushes().positions()) {
            const float cx = std::abs(static_cast<float>(t.x) + 0.5f - hx) / hx, cy = std::abs(static_cast<float>(t.y) + 0.5f - hy) / hy;
            if (std::max(cx, cy) <= tune::centre_bush_region) centre.push_back(t);
        }
    }
    if (centre.empty()) return false;
    const int ring = w.gems >= tune::centre_bush_gems ? tune::centre_bush_ring + 1 : tune::centre_bush_ring;
    bool any = false;
    for (const auto t : centre) {
        if (dist(t, w.pos) > 10 || w.isVisible(t)) continue;
        if (!any) zone.clear();
        zone.draw_chebyshev(t, static_cast<float>(ring));
        any = true;
    }
    return any;
}

struct ExMap {
    static constexpr int R = 5, N = 2 * R + 1, HB = R + 2, HN = 2 * HB + 1;
    bool on = false, melee = false;
    int round = -1;
    bc::vec2 origin{-100, -100};
    std::array<bc::vec2, 12> foes{};
    int nf = 0;
    int front = kBig;        // ranged: our nearest melee ally's distance to the enemy
    int rangedFront = kBig;  // melee: our nearest ranged ally's distance to the enemy
    int scale = 4;           // quarters: health-scaled weight (exposure_hp_scale)
    std::array<bc::vec2, 8> rangedMates{};  // our other ranged champions near (exposure_spread)
    int nr = 0;
    bc::vec2 lastPos{-100, -100};  // the tile we stepped from last round (exposure_no_backtrack)
    // Generation-tagged memo: no clearing each round.
    std::uint16_t gen = 0;
    std::array<std::int16_t, N * N> memo{};
    std::array<std::uint16_t, N * N> memoGen{};
    const World* world = nullptr;  // this round's (for vision into bushes)

    // Distance from t to the nearest bush tile we cannot see into (3: none within 2). Only tiles beside
    // a bush look further, and only for the tiles asked about (stamping every bush cost ~1.9k a round).
    int hiddenBush(bc::vec2 t) const {
        const auto& st = Statics::get();
        const int b = bushNearAt(t);
        if (b == 0 || b >= 3 || !tune::exposure_unseen_only) return b;
        int best = 3;
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
                const int x = t.x + dx, y = t.y + dy, d = std::max(std::abs(dx), std::abs(dy));
                if (d >= best || x < 0 || y < 0 || x >= st.width || y >= st.height) continue;
                if (bushNearAt({x, y}) != 0) continue;  // not a bush
                if (!world->isVisible({x, y})) best = d;
            }
        return best;
    }

    int cost(bc::vec2 t) const {
        int nd = kBig;
        for (int k = 0; k < nf; ++k) nd = std::min(nd, dist(t, foes[static_cast<std::size_t>(k)]));
        if (melee) return rangedFront < kBig && nd >= rangedFront ? tune::melee_cover_weight * (nd - rangedFront + 1) : 0;
        int c = -std::min(nd, 10);
        if (front < kBig && nd < front) c += tune::exposure_front_weight * (front - nd);
        const int hb = hiddenBush(t);
        if (hb > 0 && hb < 3) c += tune::exposure_bush_weight * (3 - hb);  // beside an unseen bush, not in one
        // Area spells (Ryth's stun) punish our ranged standing together.
        if (tune::exposure_spread > 0)
            for (int k = 0; k < nr; ++k)
                if (dist(t, rangedMates[static_cast<std::size_t>(k)]) <= 1) c += tune::exposure_spread;
        c = c * scale / 4;
        if (tune::exposure_no_backtrack > 0 && t == lastPos) c += tune::exposure_no_backtrack;
        return c;
    }
    int at(bc::vec2 t) {
        const int x = t.x - origin.x + R, y = t.y - origin.y + R;
        if (x < 0 || y < 0 || x >= N || y >= N) return cost(t);
        const auto i = static_cast<std::size_t>(y * N + x);
        if (memoGen[i] != gen) {
            memoGen[i] = gen;
            memo[i] = static_cast<std::int16_t>(cost(t));
        }
        return memo[i];
    }
};

ExMap& exposureMap(const World& w) {
    static ExMap x;
    static bc::vec2 prevPos{-100, -100};
    static int prevRound = -100;
    if (x.round == w.round && x.origin == w.pos) return x;
    if (x.round != w.round) {
        // The tile we left last round (one step back would undo it).
        x.lastPos = (prevRound == w.round - 1 && prevPos != w.pos) ? prevPos : bc::vec2{-100, -100};
        prevRound = w.round;
        prevPos = w.pos;
    }
    x.round = w.round;
    x.origin = w.pos;
    x.on = false;
    x.nf = 0;
    x.nr = 0;
    x.front = x.rangedFront = kBig;
    x.melee = !w.ranged;
    // Below full health, exposure counts for more (x1 at full, up to x3 near death).
    x.scale = tune::exposure_hp_scale ? 4 + static_cast<int>(8.0f * std::clamp(1.0f - w.hpFrac(), 0.0f, 1.0f)) : 4;
    if (x.melee ? !tune::melee_cover : !tune::ranged_exposure) return x;
    if (!ensureBushList(w)) return x;
    for (const auto& g : w.ghosts)
        if (w.round - g.round <= tune::exposure_ghost_age && dist(g.pos, w.pos) <= tune::exposure_radius && x.nf < static_cast<int>(x.foes.size()))
            x.foes[static_cast<std::size_t>(x.nf++)] = g.pos;
    if (x.nf == 0) return x;
    for (int s = 0; s < w.allyCount && s < 8; ++s) {
        if (s == w.slot) continue;
        const bool m = meleeDesign(w.allyTypes[static_cast<std::size_t>(s)]);
        if (m == x.melee) {
            if (!m && tune::exposure_spread > 0 && x.nr < static_cast<int>(x.rangedMates.size())) {
                const auto q = w.allyPosCached(s);
                if (q.x >= 0 && dist(q, w.pos) <= 3) x.rangedMates[static_cast<std::size_t>(x.nr++)] = q;
            }
            continue;  // ranged look at our melee, melee at our ranged
        }
        const auto p = w.allyPosCached(s);
        if (p.x < 0 || dist(p, w.pos) > (x.melee ? tune::melee_cover_radius : tune::exposure_radius)) continue;
        int d = kBig;
        for (int k = 0; k < x.nf; ++k) d = std::min(d, dist(p, x.foes[static_cast<std::size_t>(k)]));
        (x.melee ? x.rangedFront : x.front) = std::min(x.melee ? x.rangedFront : x.front, d);
    }
    if (x.melee && x.rangedFront >= kBig) return x;  // no ranged ally to cover
    if (++x.gen == 0) {  // wrapped: clear the tags once
        x.memoGen.fill(0);
        x.gen = 1;
    }
    x.world = &w;
    x.on = true;
    return x;
}

// Crossing routes: per goal field, the hole crossings that save the most steps toward it. Decisions
// only read finished lists and queue missing ones; buildCrossings works through the queue from
// spare fuel at the end of the turn (built inline, farm scoring drained the turn: fracture lost
// 1,400 turns a game).
struct Portal { bc::vec2 from, to; int toDist, gain, pid = -1; };  // pid: fields point id of `from`
struct PortalEntry {
    FarField far;
    std::size_t next = 0;
    bool done = false;
    std::vector<Portal> all, portals;
};
std::deque<PortalEntry>& portalCache() {
    static std::deque<PortalEntry> cache;  // stable addresses: callers keep a returned list while scoring
    return cache;
}

// Camp fields by id (scoring asks for every camp each turn); the few others by a scan.
std::vector<PortalEntry*>& portalsByGroup() {
    static std::vector<PortalEntry*> byGroup;
    return byGroup;
}

PortalEntry* findPortalEntry(const FarField& far) {
    if (far.read == &fields::toGroup && far.id >= 0) {
        const auto& byGroup = portalsByGroup();
        return static_cast<std::size_t>(far.id) < byGroup.size() ? byGroup[static_cast<std::size_t>(far.id)] : nullptr;
    }
    for (auto& e : portalCache())
        if (e.far.read == far.read && e.far.id == far.id) return &e;
    return nullptr;
}

// The entry for a goal, queued for buildCrossings if new.
PortalEntry& portalEntry(const FarField& far) {
    if (auto* e = findPortalEntry(far)) return *e;
    auto& cache = portalCache();
    cache.push_back({far});
    if (far.read == &fields::toGroup && far.id >= 0) {
        auto& byGroup = portalsByGroup();
        if (static_cast<std::size_t>(far.id) >= byGroup.size()) byGroup.resize(static_cast<std::size_t>(far.id) + 1, nullptr);
        byGroup[static_cast<std::size_t>(far.id)] = &cache.back();
    }
    return cache.back();
}

// Read only (queue false) never adds an entry: the build queue holds goals we actually weigh.
const std::vector<Portal>* portalsFor(const FarField& far, bool queue = true) {
    auto* e = queue ? &portalEntry(far) : findPortalEntry(far);
    return e && e->done ? &e->portals : nullptr;
}

// Work on one entry: 1 finished, 0 out of fuel, -1 its goal's field is not built yet (skip it).
int buildPortals(PortalEntry& e, unsigned long long reserve) {
    if (e.done) return 1;
    // The goal's field must exist (unbuilt fields read -1 everywhere: an empty list would stick).
    if (e.far.read(e.far.id, Statics::get().spawnCenter) < 0) return -1;
    const std::vector<Statics::Crossing>* cs = nullptr;
    if (!Statics::get().crossings(cs, reserve)) return 0;
    for (; e.next < cs->size(); ++e.next) {
        if ((e.next & 7) == 0 && bc::fuel_remaining() < reserve) return 0;
        const auto& c = (*cs)[e.next];
        const int ga = e.far.read(e.far.id, c.from), gb = e.far.read(e.far.id, c.to);
        if (ga < 0 || gb < 0 || ga >= Navigator::unreachable || gb >= Navigator::unreachable) continue;
        if (ga - gb < tune::crossing_min_gain) continue;
        // A short best-first list as we go (sorting thousands at the end overran the turn).
        const Portal p{c.from, c.to, gb, ga - gb};
        if (e.all.size() < 48) e.all.push_back(p);
        else if (p.gain > e.all.back().gain) e.all.back() = p;
        else continue;
        for (std::size_t k = e.all.size() - 1; k > 0 && e.all[k].gain > e.all[k - 1].gain; --k) std::swap(e.all[k], e.all[k - 1]);
    }
    for (const auto& p : e.all) {
        if (static_cast<int>(e.portals.size()) >= tune::crossing_keep) break;
        bool near = false;
        for (const auto& k : e.portals) near = near || dist(k.from, p.from) <= 2;
        if (!near) e.portals.push_back(p);
    }
    for (auto& p : e.portals) p.pid = fields::registerPoint(p.from);  // walking distance to it: a local read
    e.all.clear();
    e.all.shrink_to_fit();
    e.done = true;
    return 1;
}

bool dashTo(World& w, int slot, bc::vec2 to) {
    try {
        if (w.abilities()[static_cast<std::size_t>(slot)].use(to)) {
            w.spentAbility(slot);
            FixedStr<16> msg;
            trace().action((msg << 'd' << to.x << ',' << to.y).view());
            w.pos = to;
            return true;
        }
    } catch (const bc::error& e) {
        trace().note(std::string("dash failed: ") + e.message());
    }
    return false;
}
}  // namespace

int Navigator::crossingTravel(World& w, const FarField& far, int walk, bool* hasCrossings, bool evaluate) {
    if (hasCrossings) *hasCrossings = false;
    if (!tune::crossing_routes || !tune::crossing_scoring || walk <= tune::crossing_min_gain || w.reserveDash) return walk;
    int dslot = -1;
    for (int slot = 0; slot < 2; ++slot)
        if (w.abilities()[static_cast<std::size_t>(slot)].is_dash()) dslot = slot;
    if (dslot < 0) return walk;
    const auto* ps = portalsFor(far, evaluate);
    if (!ps || ps->empty()) return walk;
    if (hasCrossings) *hasCrossings = true;
    if (!evaluate) {
        // An optimistic bound with no host reads: straight-line distance to a crossing never beats
        // walking there (a looser bound let far camps past the pruning: more scoring, more fuel).
        int bound = walk;
        for (const auto& p : *ps) bound = std::min(bound, dist(p.from, w.pos) + p.toDist);
        return bound;
    }
    const int cd = std::max(0, static_cast<int>(w.abilities()[static_cast<std::size_t>(dslot)].cooldown()));
    int best = walk;
    for (const auto& p : *ps) {
        if (p.toDist >= walk || dist(p.from, w.pos) + p.toDist + tune::crossing_margin >= best) continue;
        const int wa = fields::toPoint(p.pid, w.pos);
        if (wa < 0 || wa >= unreachable) continue;
        best = std::min(best, wa + std::max(0, cd - wa) + p.toDist + tune::crossing_margin);
    }
    return best;
}

bool Navigator::enterPocket(World& w, const GroupInfo& g) {
    if (w.pos == g.dashFrom) {
        if (const int slot = readyDash(w); slot >= 0 && !w.occupied.is_set(g.dashTo)) return dashTo(w, slot, g.dashTo);
        trace().action("hold");  // waiting for the dash cooldown
        return true;
    }
    bc::bitboard goal;
    goal.clear();
    goal.set(g.dashFrom);
    noDash_ = true;  // the dash is for the pocket: do not spend it walking to the dash tile
    const bool moved = stepToward(w, goal, false, &w.danger);
    noDash_ = false;
    return moved;
}

bool Navigator::leavePocket(World& w, const GroupInfo& g) {
    const auto& st = Statics::get();
    if (const int slot = readyDash(w); slot >= 0) {
        // Any legal dash out from here, else walk to the landing tile first.
        bc::vec2 best{-1, -1};
        for (int dx = -2; dx <= 2; ++dx)
            for (int dy = -2; dy <= 2; ++dy) {
                const bc::vec2 t{w.pos.x + dx, w.pos.y + dy};
                if (!st.reach.is_set(t) || w.occupied.is_set(t) || !st.dashLegal(w.pos, t)) continue;
                if (best.x < 0 || dist(t, st.spawnCenter) < dist(best, st.spawnCenter)) best = t;
            }
        if (best.x >= 0) return dashTo(w, slot, best);
    }
    if (w.pos == g.dashTo) {
        trace().action("hold");  // waiting for the dash cooldown
        return true;
    }
    bc::bitboard goal;
    goal.clear();
    goal.set(g.dashTo);
    noDash_ = true;  // the dash is the way out of the pocket
    const bool moved = stepToward(w, goal, false);
    noDash_ = false;
    return moved;
}

bool Navigator::stepByField(World& w, const FarField& far, const bc::bitboard& blockedNow, const bc::bitboard* avoid,
                            bool allowDash, bc::vec2 aim) {
    const auto& st = Statics::get();
    int here = far.read(far.id, w.pos);
    if (here < 0 || here >= unreachable || here == 0) return false;
    const bool useAvoid = avoid && w.stuckTurns == 0 && !avoid->is_set(w.pos);
    auto value = [&](bc::vec2 t) {
        if (t.x < 0 || t.y < 0 || t.x >= st.width || t.y >= st.height) return unreachable;
        if (blockedNow.is_set(t) || (useAvoid && avoid->is_set(t))) return unreachable;
        const int d = far.read(far.id, t);
        return d < 0 ? unreachable : d;
    };
    // Equal steps: toward the goal's centre, then a fixed compass order, so a lap repeats its tiles.
    auto better = [&](bc::vec2 t, int d, bc::vec2 best, int bestD) {
        if (d != bestD) return d < bestD;
        if (aim.x >= 0) {
            const auto a = (t - aim).length_squared(), b = (best - aim).length_squared();
            if (a != b) return a < b;
        }
        return false;  // keep the earlier one in compass order
    };
    // A walk step on down the field after a dash (the dash is a cast: the walk is still ours).
    auto walkOn = [&]() {
        if (!tune::dash_then_walk || !bc::self().can_move()) return;
        static constexpr std::array<bc::vec2, 8> kSteps{{{0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}}};
        const int now = value(w.pos);
        bc::vec2 step{-1, -1};
        int stepD = now;
        for (const auto dir : kSteps) {
            const bc::vec2 t = w.pos + dir;
            const int d = value(t);
            if (d < stepD && !w.occupied.is_set(t)) { stepD = d; step = t; }
        }
        if (step.x >= 0) tryMove(w, step);
    };
    // Crossing routes (dashers, no enemy in sight): walk to a hole crossing and dash over it when that
    // beats walking round, waiting there for the dash if it is cooling down (counted in the cost).
    if (tune::crossing_routes && far.read != &fields::toPoint && here > tune::crossing_min_gain && w.enemies.empty() && !w.reserveDash &&
        bc::fuel_remaining() > tune::crossing_fuel_floor) {
        int dslot = -1;
        for (int slot = 0; slot < 2; ++slot)
            if (w.abilities()[static_cast<std::size_t>(slot)].is_dash()) dslot = slot;
        const std::vector<Portal>* ps = dslot >= 0 ? portalsFor(far) : nullptr;
        if (ps && !ps->empty()) {
            const int cd = std::max(0, static_cast<int>(w.abilities()[static_cast<std::size_t>(dslot)].cooldown()));
            const Portal* best = nullptr;
            int bestCost = here - tune::crossing_margin;
            for (const auto& p : *ps) {
                // Straight-line distance never beats the walk there: skip without the search (each
                // walking distance from us costs a 48-tile search whenever we have moved).
                if (p.toDist >= here || dist(p.from, w.pos) + p.toDist >= bestCost) continue;
                const int wa = fields::toPoint(p.pid, w.pos);
                if (wa < 0 || wa >= unreachable) continue;  // its field is not built yet, or no way there
                // Waiting at the crossing re-plans every turn (fuel): only go for one we reach with the
                // dash (nearly) ready.
                if (cd - wa > tune::crossing_max_wait) continue;
                const int cost = wa + std::max(0, cd - wa) + p.toDist;
                if (cost < bestCost) { bestCost = cost; best = &p; }
            }
            if (best) {
                if (w.pos == best->from) {
                    if (readyDash(w) >= 0 && !w.occupied.is_set(best->to) && dashTo(w, dslot, best->to)) {
                        trace().note("crossing");
                        walkOn();
                        return true;
                    }
                    trace().action("hold");
                    trace().note("crossing: waiting for the dash");
                    return true;
                }
                noDash_ = true;  // keep the dash for the crossing
                const bool moved = stepByField(w, FarField{&fields::toPoint, &fields::ensurePoint, best->pid}, blockedNow, avoid, false, best->from);
                noDash_ = false;
                if (moved) {
                    trace().note("to a crossing");
                    return true;
                }
            }
        }
    }
    // Crystal flash: a teleport crystal lands up to tp_range tiles away, over walls and holes, after a
    // tp_crystal_time channel (shortcut: the spawn box's south side is a row of holes, and walking out
    // the north gap and round cost ~25 steps; the top team buys ~40 crystals a game there). Taken when
    // it saves tp_min_gain steps net of the channel, with nobody about to interrupt it. The first time
    // one would have paid off, the shop starts keeping crystals.
    // Fuel: the 49-tile scan cost 14-18k on every walking step (big world: ~170 lost turns a game
    // per champion) and the bag query (a host call) up to 35k. The shop counts the crystals it
    // buys; the scan runs only with one in the bag, or now and then to find out whether shortcuts
    // pay off on this map.
    const int known = shop::crystalsHeld();
    const bool tpDiscover = !shop::crystalsWanted() && (w.round + w.slot) % 8 == 0;
    if (tune::tp_flash && w.enemies.empty() && !w.beingHit && w.incoming <= 0 && here > tune::tp_min_gain && (known > 0 || tpDiscover)) {
        // Field reads only; one host check on the chosen landing.
        bc::vec2 best{-1, -1};
        int bestD = unreachable;
        const int payoff = here - static_cast<int>(bc::constants::tp_crystal_time) - tune::tp_min_gain;
        for (int dy = -tune::tp_range; dy <= tune::tp_range; ++dy)
            for (int dx = -tune::tp_range; dx <= tune::tp_range; ++dx) {
                const bc::vec2 t{w.pos.x + dx, w.pos.y + dy};
                if (std::max(std::abs(dx), std::abs(dy)) < 2) continue;
                if (t.x < 0 || t.y < 0 || t.x >= st.width || t.y >= st.height) continue;
                const int d = far.read(far.id, t);
                if (d < 0 || d >= bestD || d > payoff) continue;
                bestD = d;
                best = t;
            }
        // One host check, on the chosen landing only (a blocked one waits for the next step).
        if (best.x >= 0 && (blockedNow.is_set(best) || (useAvoid && avoid->is_set(best)))) best = {-1, -1};
        if (best.x >= 0 && here - bestD - static_cast<int>(bc::constants::tp_crystal_time) >= tune::tp_min_gain) {
            shop::wantCrystals();
            try {
                if (known > 0 && bc::self().consumables()[bc::consumable_type::tp_crystal].use(best)) {
                    shop::crystalUsed();
                    FixedStr<24> msg;
                    trace().action((msg << "tp" << best.x << ',' << best.y).view());
                    trace().note("crystal shortcut");
                    return true;
                }
            } catch (const bc::error&) {
            }
        }
    }
    // Hole hop: a dash crosses holes the field has to walk round, so 2 tiles of dash can save many
    // steps. Take one that gains 3+ steps (4+ with enemies about: the dash may be needed to escape),
    // or a plain 2-step gain when no enemy is in sight.
    if (const int slot = noDash_ ? -1 : readyDash(w); slot >= 0) {
        bc::vec2 best{-1, -1};
        int bestD = unreachable;
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
                const bc::vec2 t{w.pos.x + dx, w.pos.y + dy};
                if ((dx == 0 && dy == 0) || std::max(std::abs(dx), std::abs(dy)) < 2) continue;
                const int d = value(t);
                if (d >= unreachable || !st.dashLegal(w.pos, t)) continue;
                if (best.x < 0 || better(t, d, best, bestD)) { best = t; bestD = d; }
            }
        const int gain = best.x >= 0 ? here - bestD : 0;
        const int need = tune::dash_always && allowDash ? 2
                         : w.enemies.empty()        ? (allowDash ? 2 : tune::hop_min_gain)
                                                    : tune::hop_min_gain + 1;
        if (gain >= need) {
            if (gain > 2) trace().note("hole hop");
            if (dashTo(w, slot, best)) {
                // The dash is a cast: the walk is still ours this turn.
                walkOn();
                return true;
            }
        }
    }
    if (!bc::self().can_move()) return false;
    static constexpr std::array<bc::vec2, 8> kCompass{{{0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}}};
    auto& ex = exposureMap(w);
    for (int attempt = 0; attempt < 2; ++attempt) {
        bc::vec2 best{-1, -1};
        int bestD = here, bestScore = 1 << 30;
        for (const auto dir : kCompass) {
            const bc::vec2 t = w.pos + dir;
            const int d = value(t);
            if (d >= here) continue;
            if (ex.on) {
                // Progress first (4 per step), exposure second: a step less progress for a much safer tile.
                const int sc = d * 4 + ex.at(t);
                if (best.x < 0 || sc < bestScore || (sc == bestScore && better(t, d, best, bestD))) { best = t; bestD = d; bestScore = sc; }
            } else if (best.x < 0 || better(t, d, best, bestD)) { best = t; bestD = d; }
        }
        if (best.x < 0) return false;  // blocked or only into danger: let the search route round it
        if (tryMove(w, best)) return true;  // tryMove marks a hidden blocker occupied: retry once
    }
    return false;
}

bool Navigator::stepToward(World& w, const bc::bitboard& goalsIn, bool allowDash, const bc::bitboard* avoid,
                           bc::vec2 aim, FarField far) {
    // xBot: dash whenever it gets us there faster (2+ tiles nearer), except to save it for a pocket.
    // Not with an enemy champion near: in fights the fight rules (safe dash, keeping it to escape) decide.
    if (tune::dash_always && !noDash_) {
        bool enemyNear = false;
        for (const auto& e : w.enemies) enemyNear = enemyNear || (e.hp > 0 && dist(e.pos, w.pos) <= 6);
        if (!enemyNear) allowDash = true;
    }
    // combatBotv2: outnumbered, keep the dash for the way out (fleeing dashes via stepHome/stepEscape).
    if (allowDash && w.reserveDash) allowDash = false;
    auto& me = bc::self();
    if (goalsIn.is_set(w.pos)) return false;
    // Inside a sealed pocket with somewhere to go outside it: out the way we came (a dash).
    if (const auto* pg = Statics::get().pocketAt(w.pos); pg && pg->pocket && (goalsIn & *pg->pocket).is_empty()) return leavePocket(w, *pg);
    auto blockedNow = blocked(w);
    auto goals = goalsIn & ~blockedNow;
    if (goals.is_empty()) {
        trace().note("no free goal tile");
        return false;
    }
    // combatBotv2: walk around the tiles of a dead camp a teammate is capturing (not our goal):
    // standing on one as the capture completes takes half its reward (rows: the party walking on to
    // the next wolves crossed the keeper's tiles). Avoided like danger: only while a route around exists.
    bc::bitboard merged;
    if (tune::avoid_teammate_capture) {
        bool any = false;
        for (const auto& g : Statics::get().groups) {
            if (dist(g.center, w.pos) > tune::capture_avoid_radius || w.groups[static_cast<std::size_t>(g.id)].status != GroupStatus::reward) continue;
            if (!(goalsIn & g.tileMask).is_empty() || !scorers::allyHoldingOf(w, g.id)) continue;
            if (!any) merged = avoid ? *avoid : bc::bitboard{};
            merged |= g.tileMask;
            any = true;
        }
        if (any) avoid = &merged;
    }
    // combatBotv3: unseen centre bushes near us (zone built once here, used by both walks below).
    bc::bitboard bushes;
    const bool haveBushes = tune::centre_bush_ring >= 0 && w.stuckTurns == 0 && bc::fuel_remaining() >= tune::nav_safe_floor &&
                            centreBushZone(w, bushes);
    // Set paths: far from the goal, walk the precomputed field (built now if it is missing). Its
    // step never enters a bush zone; with no step around one, the search below decides.
    if (tune::set_paths && far.read && aim.x >= 0 && dist(w.pos, aim) > tune::field_walk_tiles) {
        // Build it only when the most a field has cost still fits (big world: 55k a field; a fixed
        // 35k gate overran 234 turns a game): otherwise the search below walks this turn.
        if (far.read(far.id, w.pos) < 0 && far.ensure && bc::fuel_remaining() > tune::field_build_fuel + fields::buildCost()) far.ensure(far.id);
        bc::bitboard farAvoid;
        if (haveBushes) farAvoid = (avoid ? *avoid | bushes : bushes) & ~goals;
        if (stepByField(w, far, blockedNow, haveBushes ? &farAvoid : avoid, allowDash, aim)) return true;
    }
    bc::distance_field field;
    bc::bitboard visited, here;
    here.clear();
    here.set(w.pos);
    // xBot: the searches treat units as walls only near us: farther ones will have moved by the
    // time we get there, and a unit across a corridor sent every search round a long detour or into
    // the "units block every route" retry. The step itself still avoids every occupied tile.
    auto searchWalls = blockedNow;
    if (tune::unit_wall_radius >= 0) {
        bc::bitboard nearUs;
        nearUs.clear();
        nearUs.draw_chebyshev(w.pos, static_cast<float>(tune::unit_wall_radius));
        searchWalls = Statics::get().terrain | (w.occupied & nearUs);
        searchWalls.set(w.pos, false);
    }
    // xBot: with a known aim, a search stops at the straight-line distance plus a detour allowance.
    // A route that exists is found before that; a search that fails (no safe route) stops there
    // instead of flooding everything reachable. The plain search below retries uncapped.
    const auto cap = aim.x >= 0 && tune::search_detour >= 0
                         ? static_cast<std::uint16_t>(std::min(dist(w.pos, aim) + tune::search_detour, 60000))
                         : bc::distance_field::unreachable;
    // While a decision is marked stuck (we were bouncing between two tiles), ignore the avoid
    // layer: a safe route and the plain fallback pointing opposite ways caused exactly that.
    // Short of fuel: skip the safe-route search (one path search fewer) rather than overrun.
    // combatBotv3: route around unseen centre bushes (plus the usual avoid zones) when the detour
    // is short; otherwise fall through to the usual avoid handling.
    bool bushRoute = false;
    {
        if (haveBushes) {
            auto zone = (avoid ? *avoid | bushes : bushes) & ~goals;
            if (zone.is_set(w.pos)) {
                bc::bitboard bubble;
                bubble.clear();
                bubble.draw_chebyshev(w.pos, 1.0f);
                zone = zone & ~bubble;
            }
            auto walls = searchWalls | zone;
            walls.set(w.pos, false);
            field.compute_to(walls, goals, here, visited, cap);
            if (visited.is_set(w.pos)) {
                bc::distance_field plain;
                bc::bitboard plainVisited;
                plain.compute_to(searchWalls, goals, here, plainVisited, cap);
                const int detour = plainVisited.is_set(w.pos) ? static_cast<int>(field.get(w.pos)) - static_cast<int>(plain.get(w.pos)) : 0;
                if (detour <= (w.gems >= tune::centre_bush_gems ? tune::centre_bush_detour_gems : tune::centre_bush_detour)) {
                    blockedNow = blockedNow | zone;
                    bushRoute = true;
                    if (detour > 0) trace().note("around a centre bush");
                }
            }
            if (!bushRoute) visited.clear();
        }
    }
    if (!bushRoute && avoid && w.stuckTurns == 0 && bc::fuel_remaining() >= tune::nav_safe_floor) {
        // First try a route that never enters the avoided zones. If we are already inside one,
        // free a small bubble around us so the safe route can lead out of it; otherwise every
        // step from in here fell back to the plain route, which pointed back the way we came.
        auto zone = *avoid & ~goals;
        if (zone.is_set(w.pos)) {
            bc::bitboard bubble;
            bubble.clear();
            bubble.draw_chebyshev(w.pos, 2.0f);
            zone = zone & ~bubble;
        }
        auto walls = searchWalls | zone;
        walls.set(w.pos, false);
        field.compute_to(walls, goals, here, visited, cap);
        if (visited.is_set(w.pos)) blockedNow = blockedNow | zone;
        else trace().note("no safe route");
    }
    if (!visited.is_set(w.pos)) field.compute_to(searchWalls, goals, here, visited);
    if (!visited.is_set(w.pos)) {
        // Units block every route: path through them on terrain alone and hope they move.
        field.compute_to(Statics::get().terrain, goals, here, visited);
        if (!visited.is_set(w.pos)) {
            trace().note("goal unreachable");
            return false;
        }
    }

    bool moved = false;
    if (allowDash) moved = tryDash(w, field, visited, blockedNow);
    if (!me.can_move()) return moved;

    const auto origin = w.pos;
    auto& ex = exposureMap(w);
    for (int attempt = 0; attempt < 2; ++attempt) {
        bc::vec2 best = w.pos;
        auto bestD = field.get(w.pos);
        const auto hereD = bestD;
        int bestScore = 1 << 30;
        auto stepWalls = blockedNow | w.occupied;
        stepWalls.set(w.pos, false);
        for (auto t : stepWalls.reachable_tiles(w.pos, 1).positions()) {
            // compute_to stops once it reaches us, so tiles beyond its frontier were never
            // written and read as distance 0: without this check we walked *away* from goals.
            if (t == w.pos || !visited.is_set(t)) continue;
            const auto d = field.get(t);
            if (ex.on) {
                if (d >= hereD) continue;  // progress only
                const int sc = static_cast<int>(d) * 4 + ex.at(t);
                if (sc < bestScore || (sc == bestScore && aim.x >= 0 && (t - aim).length_squared() < (best - aim).length_squared())) {
                    bestScore = sc;
                    bestD = d;
                    best = t;
                }
            } else if (d < bestD || (d == bestD && best != w.pos && aim.x >= 0 &&
                                     (t - aim).length_squared() < (best - aim).length_squared())) {
                bestD = d;
                best = t;
            }
        }
        if (best == w.pos) break;
        if (tryMove(w, best)) return true;  // tryMove marks hidden blockers occupied, so retry once
    }
    return moved || w.pos != origin;
}

bool Navigator::stepToward(World& w, bc::vec2 target, int range, bool allowDash, const bc::bitboard* avoid) {
    auto goals = standTiles(w, target, range);
    return stepToward(w, goals, allowDash, avoid, target);
}

bool Navigator::saferStep(World& w, const bc::bitboard& keepIn) {
    if (!bc::self().can_move()) return false;
    auto& ex = exposureMap(w);
    if (!ex.on) return false;
    const int here = ex.at(w.pos);
    auto stepWalls = blocked(w) | w.occupied;
    stepWalls.set(w.pos, false);
    bc::vec2 best = w.pos;
    int bestC = here;
    for (auto t : stepWalls.reachable_tiles(w.pos, 1).positions()) {
        if (t == w.pos || !keepIn.is_set(t) || w.danger.is_set(t)) continue;
        const int c = ex.at(t);
        if (c < bestC) { bestC = c; best = t; }
    }
    if (best == w.pos || here - bestC < tune::exposure_step_gain) return false;
    if (!tryMove(w, best)) return false;
    trace().note("safer tile");
    return true;
}

bool Navigator::stepAway(World& w, const std::vector<bc::vec2>& threats, int keepRange) {
    auto& me = bc::self();
    if (!me.can_move() || threats.empty()) return false;
    auto score = [&](bc::vec2 t) {
        int nearest = 1 << 20;
        for (auto th : threats) nearest = std::min(nearest, dist(t, th));
        // Prefer escaping toward spawn when distances tie.
        return std::min(nearest, keepRange + 1) * 100 - dist(t, Statics::get().spawnCenter);
    };
    bc::vec2 best = w.pos;
    int bestScore = score(w.pos);
    for (auto t : blocked(w).reachable_tiles(w.pos, 1).positions()) {
        const int s = score(t);
        if (s > bestScore) { bestScore = s; best = t; }
    }
    return best != w.pos && tryMove(w, best);
}

void Navigator::computeEscape(const World& w) {
    if (escapeRound_ == w.round && escapePos_ == w.pos) return;
    escapeRound_ = w.round;
    escapePos_ = w.pos;
    auto danger = w.danger;  // monster reach, computed once in World::refresh
    for (const auto& e : w.enemies) danger.draw_chebyshev(e.pos, static_cast<float>(e.range + 1));
    escapeNeeded_ = danger.is_set(w.pos);
    if (!escapeNeeded_) return;
    const auto blockedNow = blocked(w);
    const auto safe = ~(danger | blockedNow);
    bc::bitboard here;
    here.clear();
    here.set(w.pos);
    escape_.compute_to(blockedNow, safe, here, escapeVisited_, 16);
}

int Navigator::escapeTurns(const World& w) {
    computeEscape(w);
    if (!escapeNeeded_) return 0;
    return escapeVisited_.is_set(w.pos) ? escape_.get(w.pos) : 16;
}

bool Navigator::stepHome(World& w) {
    if (fields::toSpawn(0, w.pos) < 0) {
        if (bc::fuel_remaining() <= tune::field_build_fuel) return false;
        fields::ensureSpawn(0);
    }
    const auto& st = Statics::get();
    const auto blockedNow = blocked(w);
    // Home distance plus a heavy cost per enemy (and awake monster) that could hit the tile:
    // running home around them, not through them and not just out of the nearest reach.
    auto cost = [&](bc::vec2 t) {
        if (t.x < 0 || t.y < 0 || t.x >= st.width || t.y >= st.height || (t != w.pos && blockedNow.is_set(t))) return 1 << 20;
        const int home = fields::toSpawn(0, t);
        if (home < 0 || home >= unreachable) return 1 << 20;
        int c = home;
        int nearest = 1 << 20;
        for (const auto& e : w.enemies)
            if (e.hp > 0) {
                if (dist(e.pos, t) <= e.range + 1) c += tune::flee_reach_cost;
                nearest = std::min(nearest, dist(e.pos, t));
            }
        if (w.danger.is_set(t)) c += tune::flee_reach_cost;
        // A bush hides us from enemies outside it: worth a few steps of home distance while they
        // chase, unless one is close enough to be in or beside the same bush.
        if (tune::flee_bush_bonus > 0 && nearest > 2 && nearest <= tune::flee_bush_chase && isBush(t))
            c -= tune::flee_bush_bonus;
        return c;
    };
    const int here = cost(w.pos);
    if (here >= (1 << 20) || fields::toSpawn(0, w.pos) == 0) return false;
    static constexpr std::array<bc::vec2, 8> kCompass{{{0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}}};
    bc::vec2 walk{-1, -1};
    int walkCost = here;
    for (const auto dir : kCompass) {
        const bc::vec2 t = w.pos + dir;
        const int c = cost(t);
        if (c < walkCost) { walkCost = c; walk = t; }
    }
    // A dash that beats the best step by 2+ (two tiles home, or out of their reach) is worth it.
    if (const int slot = readyDash(w); slot >= 0) {
        bc::vec2 best{-1, -1};
        int bestCost = walkCost;
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) < 2) continue;
                const bc::vec2 t{w.pos.x + dx, w.pos.y + dy};
                const int c = cost(t);
                if (c < bestCost && st.dashLegal(w.pos, t)) { bestCost = c; best = t; }
            }
        if (best.x >= 0 && bestCost + tune::flee_dash_gain <= walkCost && dashTo(w, slot, best)) {
            trace().note("fleeing home: dash");
            // The dash is a cast: the walk is still ours this turn (unless the dash used it).
            if (tune::flee_dash_then_walk && bc::self().can_move()) stepHome(w);
            return true;
        }
    }
    if (walk.x < 0 || !bc::self().can_move()) return false;
    trace().note(tune::flee_bush_bonus > 0 && isBush(walk) ? "fleeing home: into a bush" : "fleeing home");
    return tryMove(w, walk);
}

bool Navigator::stepEscape(World& w) {
    auto& me = bc::self();
    computeEscape(w);
    if (!escapeNeeded_ || !escapeVisited_.is_set(w.pos)) return false;
    const auto blockedNow = blocked(w);
    bool moved = tryDash(w, escape_, escapeVisited_, blockedNow);
    if (!me.can_move()) return moved;
    bc::vec2 best = w.pos;
    auto bestD = escape_.get(w.pos);
    // Equally quick ways out: take the one toward home (contest: Vesper Vale escaped deeper into
    // their half, the enemies standing between it and our spawn).
    auto home = [](bc::vec2 t) {
        const int d = fields::toSpawn(0, t);
        return d < 0 ? unreachable : d;
    };
    int bestHome = home(w.pos);
    for (auto t : blocked(w).reachable_tiles(w.pos, 1).positions()) {
        if (t == w.pos || !escapeVisited_.is_set(t)) continue;
        const auto d = escape_.get(t);
        const int h = tune::flee_homeward ? home(t) : 0;
        if (d < bestD || (d == bestD && best != w.pos && h < bestHome)) { bestD = d; best = t; bestHome = h; }
    }
    return (best != w.pos && tryMove(w, best)) || moved;
}

void buildCrossings(unsigned long long reserve) {
    if (!tune::crossing_routes) return;
    // The per-tile fuel checks inside still let one call cost 66k on big world (103 overruns a
    // game): start a call only when the most one has cost so far fits above the reserve.
    static unsigned long long maxCost = 0;
    const auto before = bc::fuel_remaining();
    if (before < reserve + maxCost) return;
    for (auto& e : portalCache())
        if (buildPortals(e, reserve) == 0) break;  // out of fuel: next turn
    const auto now = bc::fuel_remaining();
    if (before > now) maxCost = std::max(maxCost, before - now);
}
