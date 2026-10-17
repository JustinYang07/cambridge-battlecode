#include "navigator.h"
#include "fields.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include "../core/config.h"
#include "../core/fixed_str.h"
#include "../core/trace.h"
#include "../brain/scorers.h"

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

bool Navigator::enterPocket(World& w, const GroupInfo& g) {
    if (w.pos == g.dashFrom) {
        if (const int slot = readyDash(w); slot >= 0 && !w.occupied.is_set(g.dashTo)) return dashTo(w, slot, g.dashTo);
        trace().action("hold");  // waiting for the dash cooldown
        return true;
    }
    bc::bitboard goal;
    goal.clear();
    goal.set(g.dashFrom);
    return stepToward(w, goal, false, &w.danger);
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
    return stepToward(w, goal, false);
}

bool Navigator::stepByField(World& w, const FarField& far, const bc::bitboard& blockedNow, const bc::bitboard* avoid,
                            bool allowDash, bc::vec2 aim) {
    const auto& st = Statics::get();
    const int here = far.read(far.id, w.pos);
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
    // Hole hop: a dash crosses holes the field has to walk round, so 2 tiles of dash can save many
    // steps. Take one that gains 3+ steps (4+ with enemies about: the dash may be needed to escape),
    // or a plain 2-step gain when no enemy is in sight.
    if (const int slot = readyDash(w); slot >= 0) {
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
        const int need = w.enemies.empty() ? (allowDash ? 2 : tune::hop_min_gain) : tune::hop_min_gain + 1;
        if (gain >= need) {
            if (gain > 2) trace().note("hole hop");
            if (dashTo(w, slot, best)) return true;
        }
    }
    if (!bc::self().can_move()) return false;
    static constexpr std::array<bc::vec2, 8> kCompass{{{0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}}};
    for (int attempt = 0; attempt < 2; ++attempt) {
        bc::vec2 best{-1, -1};
        int bestD = here;
        for (const auto dir : kCompass) {
            const bc::vec2 t = w.pos + dir;
            const int d = value(t);
            if (d < here && (best.x < 0 || better(t, d, best, bestD))) { best = t; bestD = d; }
        }
        if (best.x < 0) return false;  // blocked or only into danger: let the search route round it
        if (tryMove(w, best)) return true;  // tryMove marks a hidden blocker occupied: retry once
    }
    return false;
}

bool Navigator::stepToward(World& w, const bc::bitboard& goalsIn, bool allowDash, const bc::bitboard* avoid,
                           bc::vec2 aim, FarField far) {
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
    // Set paths: far from the goal, walk the precomputed field (built now if it is missing).
    if (tune::set_paths && far.read && aim.x >= 0 && dist(w.pos, aim) > tune::field_walk_tiles) {
        if (far.read(far.id, w.pos) < 0 && far.ensure && bc::fuel_remaining() > tune::field_build_fuel) far.ensure(far.id);
        if (stepByField(w, far, blockedNow, avoid, allowDash, aim)) return true;
    }
    bc::distance_field field;
    bc::bitboard visited, here;
    here.clear();
    here.set(w.pos);
    // While a decision is marked stuck (we were bouncing between two tiles), ignore the avoid
    // layer: a safe route and the plain fallback pointing opposite ways caused exactly that.
    // Short of fuel: skip the safe-route search (one path search fewer) rather than overrun.
    if (avoid && w.stuckTurns == 0 && bc::fuel_remaining() >= tune::nav_safe_floor) {
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
        auto walls = blockedNow | zone;
        walls.set(w.pos, false);
        field.compute_to(walls, goals, here, visited);
        if (visited.is_set(w.pos)) blockedNow = walls;
        else trace().note("no safe route");
    }
    if (!visited.is_set(w.pos)) field.compute_to(blockedNow, goals, here, visited);
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
    for (int attempt = 0; attempt < 2; ++attempt) {
        bc::vec2 best = w.pos;
        auto bestD = field.get(w.pos);
        auto stepWalls = blockedNow | w.occupied;
        stepWalls.set(w.pos, false);
        for (auto t : stepWalls.reachable_tiles(w.pos, 1).positions()) {
            // compute_to stops once it reaches us, so tiles beyond its frontier were never
            // written and read as distance 0: without this check we walked *away* from goals.
            if (t == w.pos || !visited.is_set(t)) continue;
            const auto d = field.get(t);
            if (d < bestD || (d == bestD && best != w.pos && aim.x >= 0 &&
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
        if (tune::flee_bush_bonus > 0 && nearest > 2 && nearest <= tune::flee_bush_chase && bc::map::bushes().is_set(t))
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
    trace().note(tune::flee_bush_bonus > 0 && bc::map::bushes().is_set(walk) ? "fleeing home: into a bush" : "fleeing home");
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
