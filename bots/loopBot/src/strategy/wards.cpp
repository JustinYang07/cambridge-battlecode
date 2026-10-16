#include "wards.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include <cambc.h>
#include "../core/config.h"
#include "../core/fixed_str.h"
#include "../core/statics.h"
#include "../core/trace.h"
#include "../core/world.h"

namespace bc = cambc;

namespace wards {

namespace {
std::vector<std::uint8_t> lineBits;  // halfway-line tiles, one byte per tile
std::vector<bc::vec2> placed;        // our own wards, so we do not stack them
int nextTry = 0;
bool init = false, done = false;  // tune::ward_once: one ward per champion, then no more attempts

// Radius of a tile from the map centre: each axis against half the map's size, 0 centre .. 1 corner.
float centreRadius(bc::vec2 p, int width, int height) {
    const float hx = std::max(static_cast<float>(width) / 2.0f, 1.0f), hy = std::max(static_cast<float>(height) / 2.0f, 1.0f);
    const float dx = (static_cast<float>(p.x) + 0.5f - hx) / hx, dy = (static_cast<float>(p.y) + 0.5f - hy) / hy;
    return std::min(std::sqrt((dx * dx + dy * dy) / 2.0f), 1.0f);
}

void setup(const World& w) {
    init = true;
    const auto& s = Statics::get();
    lineBits.assign(static_cast<std::size_t>(s.width * s.height), 0);
    // Exported once: bitboard reads cost ~30-300 fuel each, a byte array read is free.
    const auto blocked = s.terrain.export_linear_buffer();
    int count = 0;
    for (int y = 0; y < s.height; ++y)
        for (int x = 0; x < s.width; ++x) {
            const auto i = static_cast<std::size_t>(y * s.width + x);
            if ((blocked[i >> 3] >> (i & 7)) & 1) continue;
            const bc::vec2 t{x, y};
            if (std::abs(dist(t, s.spawnCenter) - dist(t, s.enemySpawnCenter)) > tune::ward_line_width) continue;
            lineBits[i] = 1;
            ++count;
        }
    if (w.slot == 0) {
        FixedStr<160> msg;
        msg << "wards: " << count << " halfway tiles; charges " << static_cast<int>(bc::detail::cached_game_settings.ward_charges) << " cooldown "
            << bc::constants::ward_cooldown << " lifetime " << bc::constants::ward_lifetime << " place "
            << static_cast<int>(bc::constants::ward_placement_radius) << " vision "
            << static_cast<int>(bc::constants::ward_vision_radius) << " active " << static_cast<int>(bc::constants::active_wards);
        trace().note(msg.view());
    }
}
} // namespace

void update(World& w) {
    if (!tune::wards || !w.alive) return;
    if (!init) setup(w);
    if (done || w.round < nextTry || (w.round + w.slot) % 2 != 0) return;
    const auto& s = Statics::get();
    const int reach = static_cast<int>(bc::constants::ward_placement_radius);
    const int spacing = std::max(2, static_cast<int>(bc::constants::ward_vision_radius));
    bc::vec2 best{-1, -1};
    float bestR = 2.0f;
    for (int dy = -reach; dy <= reach; ++dy)
        for (int dx = -reach; dx <= reach; ++dx) {
            const bc::vec2 t{w.pos.x + dx, w.pos.y + dy};
            if (t.x < 0 || t.y < 0 || t.x >= s.width || t.y >= s.height) continue;
            if (!lineBits[static_cast<std::size_t>(t.y * s.width + t.x)]) continue;
            if (static_cast<float>(t.chebyshev_distance(w.pos)) > bc::constants::ward_placement_radius) continue;
            bool covered = false;
            for (auto p : placed)
                if (dist(p, t) < spacing) covered = true;
            if (covered) continue;
            const float r = centreRadius(t, s.width, s.height);
            if (r < bestR) { bestR = r; best = t; }
        }
    if (best.x < 0) return;
    try {
        if (bc::vision::has_ward(best, bc::team::ally)) {
            placed.push_back(best);  // a teammate's: treat as ours for spacing
            return;
        }
        if (bc::self().place_ward(best)) {
            placed.push_back(best);
            if (tune::ward_once) done = true;
            FixedStr<32> msg;
            trace().note((msg << "ward at " << best.x << ',' << best.y).view());
            trace().forceFull();
        }
    } catch (const bc::error& e) {
        trace().note(std::string("ward failed: ") + e.message());
        // Out of charges: wait for one to recharge. Anything else: try again shortly.
        const bool noCharge = e.code() == bc::error_code::invalid_action_place_ward_no_charges;
        if (tune::ward_once && e.code() == bc::error_code::invalid_action_place_ward_cant_place) done = true;
        nextTry = w.round + (noCharge ? std::max(bc::constants::ward_cooldown, 4) : 4);
        if (noCharge) placed.clear();  // old wards may have expired by the time we have a charge again
    }
}

} // namespace wards
