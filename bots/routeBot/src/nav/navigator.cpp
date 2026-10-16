#include "navigator.h"
#include <algorithm>
#include "../core/config.h"
#include "../core/fixed_str.h"
#include "../core/trace.h"

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

bool Navigator::stepToward(World& w, const bc::bitboard& goalsIn, bool allowDash, const bc::bitboard* avoid,
                           bc::vec2 aim) {
    auto& me = bc::self();
    if (goalsIn.is_set(w.pos)) return false;
    auto blockedNow = blocked(w);
    auto goals = goalsIn & ~blockedNow;
    if (goals.is_empty()) {
        trace().note("no free goal tile");
        return false;
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

bool Navigator::stepEscape(World& w) {
    auto& me = bc::self();
    computeEscape(w);
    if (!escapeNeeded_ || !escapeVisited_.is_set(w.pos)) return false;
    const auto blockedNow = blocked(w);
    bool moved = tryDash(w, escape_, escapeVisited_, blockedNow);
    if (!me.can_move()) return moved;
    bc::vec2 best = w.pos;
    auto bestD = escape_.get(w.pos);
    for (auto t : blocked(w).reachable_tiles(w.pos, 1).positions()) {
        if (t == w.pos || !escapeVisited_.is_set(t)) continue;
        const auto d = escape_.get(t);
        if (d < bestD) { bestD = d; best = t; }
    }
    return (best != w.pos && tryMove(w, best)) || moved;
}
