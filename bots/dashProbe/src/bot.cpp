// Dash probe: can a dash cross walls or holes into a sealed pocket, and back out?
#include <cambc.h>
#include "bot.h"
#include <iostream>
#include <variant>

using cambc::vec2;
namespace bc = cambc;

static bc::bitboard terrainNow() {
    return bc::map::walls() | bc::map::holes() | bc::map::altars() | bc::map::spawn_region(bc::team::enemy);
}

static void say(const char* what, vec2 a, vec2 b, const std::string& extra = "") {
    std::cout << "PROBE r" << bc::round_number() << ' ' << what << " from " << a.x << ',' << a.y << " to " << b.x << ',' << b.y
              << ' ' << extra << std::endl;
}

static bool stepTo(const bc::bitboard& goals) {
    auto& me = bc::self();
    if (!me.can_move() || goals.is_set(me.position())) return false;
    auto blocked = terrainNow() | bc::vision::champion_tiles() | bc::vision::camp_tiles();
    blocked.set(me.position(), false);
    bc::distance_field field;
    bc::bitboard visited, here;
    here.clear();
    here.set(me.position());
    field.compute_to(blocked & ~goals, goals & ~blocked, here, visited);
    if (!visited.is_set(me.position())) return false;
    auto step = me.position();
    for (auto t : blocked.reachable_tiles(step, 1).positions())
        if (visited.is_set(t) && field.get(t) < field.get(step)) step = t;
    return step != me.position() && me.move_absolute(step);
}

void Bot::play_turn() {
    auto& me = bc::self();
    if (!me.is_alive()) return;
    int dashSlot = -1;
    const auto abilities = me.abilities();
    for (int s = 0; s < 2; ++s)
        if (abilities[static_cast<std::size_t>(s)].is_dash()) {
            dashSlot = s;
            if (const auto* d = std::get_if<bc::ability_dash>(&*abilities[static_cast<std::size_t>(s)].info())) { range = static_cast<float>(d->range); cd = d->max_cooldown; }
        }
    if (dashSlot < 0) return;
    const auto& dash = abilities[static_cast<std::size_t>(dashSlot)];
    if (!init) {
        init = true;
        bc::distance_field f;
        f.compute(terrainNow(), bc::map::spawn_region(bc::team::ally), reach);
        pocket = ~terrainNow() & ~reach;
        std::cout << "PROBE dash range " << range << " cooldown " << cd << " pocket tiles " << pocket.count() << std::endl;
    }
    const auto pos = me.position();
    if (phase == 0) {
        if (pocket.is_set(pos)) { phase = 1; say("INSIDE POCKET", pos, pos); return; }
        // Pocket tiles within dash range of where we stand: try each (nearest first).
        if (!dash.is_on_cooldown() && me.can_cast()) {
            for (int r = 1; r <= static_cast<int>(range) + 1; ++r)
                for (auto t : pocket.positions()) {
                    if (static_cast<int>(pos.chebyshev_distance(t)) != r) continue;
                    try {
                        const bool ok = dash.use(t);
                        say(ok ? "dash returned true" : "dash returned false", pos, t, "now at " + std::to_string(me.position().x) + "," + std::to_string(me.position().y));
                        if (pocket.is_set(me.position())) { phase = 1; say("INSIDE POCKET", pos, me.position()); }
                        return;
                    } catch (const bc::error& e) {
                        say("dash error", pos, t, e.message());
                    }
                }
        }
        // Walk to reachable tiles within dash range of the pocket.
        // Stand on reachable tiles with a hole between us and the pocket (ambush: holes at 34-36,47).
        bc::bitboard edge;
        edge.clear();
        for (auto t : pocket.positions()) edge.draw_chebyshev(t, range);
        bc::bitboard nearHole;
        nearHole.clear();
        for (auto h : bc::map::holes().positions()) nearHole.draw_chebyshev(h, 1.0f);
        auto goals = edge & reach & nearHole;
        if (goals.is_empty()) goals = edge & reach;
        stepTo(goals);
    } else if (phase == 1) {
        if (!pocket.is_set(pos)) { phase = 2; say("OUT OF POCKET", pos, pos); return; }
        if (!dash.is_on_cooldown() && me.can_cast()) {
            for (int r = static_cast<int>(range); r >= 1; --r)
                for (auto t : reach.positions()) {
                    if (static_cast<int>(pos.chebyshev_distance(t)) != r) continue;
                    try {
                        const bool ok = dash.use(t);
                        say(ok ? "dash out true" : "dash out false", pos, t, "now at " + std::to_string(me.position().x) + "," + std::to_string(me.position().y));
                        return;
                    } catch (const bc::error& e) {
                        (void)e;
                    }
                }
        }
    }
}
