#include "statics.h"
#include <algorithm>
#include "json.h"

namespace bc = cambc;

std::string num(long long v) {
    std::string out;
    Json::integer(out, v);
    return out;
}

std::string fmt(bc::vec2 p) {
    std::string out = "(";
    Json::integer(out, p.x);
    out += ',';
    Json::integer(out, p.y);
    out += ')';
    return out;
}

Statics& Statics::get() {
    static Statics instance;
    if (!instance.ready_) instance.init();
    return instance;
}

const Statics::TileCamp* Statics::campAt(bc::vec2 tile) const {
    if (tile.x < 0 || tile.y < 0 || tile.x >= width || tile.y >= height) return nullptr;
    const auto& c = tileCamps_[static_cast<std::size_t>(tile.y * width + tile.x)];
    return c.groupId >= 0 ? &c : nullptr;
}

const GroupInfo* Statics::groupAt(bc::vec2 tile) const {
    const auto* c = campAt(tile);
    return c ? &groups[static_cast<std::size_t>(c->groupId)] : nullptr;
}

static const char* shortName(bc::camp_type t) {
    switch (t) {
    case bc::camp_type::wolf: return "wolf";
    case bc::camp_type::elk: return "elk";
    case bc::camp_type::gem_cluster: return "gem";
    case bc::camp_type::gold_cluster: return "gold";
    case bc::camp_type::monolith: return "monolith";
    }
    return "?";
}

void Statics::init() {
    ready_ = true;
    width = bc::map::width();
    height = bc::map::height();
    allySpawn = bc::map::spawn_region(bc::team::ally);
    enemySpawn = bc::map::spawn_region(bc::team::enemy);
    terrain = bc::map::walls() | bc::map::holes() | bc::map::altars() | enemySpawn;
    {
        long ex = 0, ey = 0, en = 0;
        for (auto p : enemySpawn.positions()) { ex += p.x; ey += p.y; ++en; }
        if (en) enemySpawnCenter = {static_cast<int>(ex / en), static_cast<int>(ey / en)};
    }
    altars = bc::map::altars().positions();
    teamSize = static_cast<int>(bc::draft::team_size());

    const auto spawnTiles = allySpawn.positions();
    long sx = 0, sy = 0;
    for (auto p : spawnTiles) { sx += p.x; sy += p.y; }
    if (!spawnTiles.empty())
        spawnCenter = {static_cast<int>(sx / static_cast<long>(spawnTiles.size())),
                       static_cast<int>(sy / static_cast<long>(spawnTiles.size()))};

    tileCamps_.assign(static_cast<std::size_t>(width * height), TileCamp{});
    int id = 0;
    for (const auto& group : bc::map::camp_groups()) {
        GroupInfo g{.id = id++, .handle = group};
        g.tileMask.clear();
        g.reach.clear();
        g.gems = static_cast<int>(group.gem_value());
        g.gold = static_cast<int>(group.gold_value());
        g.respawn = group.max_respawn_timer();
        g.harmless = true;
        long cx = 0, cy = 0;
        for (const auto& camp : group.camps()) {
            const auto p = camp.spawn_position();
            g.tiles.push_back(p);
            g.tileMask.set(p);
            cx += p.x;
            cy += p.y;
            const auto stats = camp.base_stats();
            g.totalHealth += stats.base_health;
            g.totalStrength += stats.base_strength;
            if (stats.base_strength > 0) {
                g.harmless = false;
                g.reach.draw_chebyshev(p, static_cast<float>(stats.base_attack_range));
            }
            g.killOrder.push_back({stats.base_health, stats.base_strength});
            tileCamps_[static_cast<std::size_t>(p.y * width + p.x)] = {g.id, stats.base_health, stats.base_strength, camp};
            if (camp.is_monolith()) g.hasMonolith = true;
            if (!g.label.empty()) g.label += '+';
            g.label += shortName(camp.kind());
        }
        g.campCount = static_cast<int>(g.tiles.size());
        std::sort(g.killOrder.begin(), g.killOrder.end(), [](const CampMember& a, const CampMember& b) {
            return a.strength != b.strength ? a.strength > b.strength : a.health < b.health;
        });
        for (std::size_t i = 0; i < g.tiles.size(); ++i)
            for (std::size_t j = i + 1; j < g.tiles.size(); ++j)
                if (dist(g.tiles[i], g.tiles[j]) <= 1) ++g.adjacentPairs;
        if (g.campCount) g.center = {static_cast<int>(cx / g.campCount), static_cast<int>(cy / g.campCount)};
        g.label += "@" + fmt(g.tiles.empty() ? bc::vec2{} : g.tiles[0]);
        groups.push_back(std::move(g));
    }
    // Which groups can be walked to at all: one flood fill from our spawn over the terrain (ambush
    // has camps in pockets sealed by walls and holes, reachable only by dashing; champions used to
    // stand still for 1,500 rounds trying to path to one). Runs once, on the first turn's budget.
    try {
        bc::distance_field field;
        bc::bitboard visited;
        field.compute(terrain, allySpawn, visited);
        const auto bits = visited.export_linear_buffer();
        for (auto& g : groups) {
            bool any = false;
            for (auto t : g.tiles) {
                const auto i = static_cast<std::size_t>(t.y * width + t.x);
                if ((bits[i >> 3] >> (i & 7)) & 1) { any = true; break; }
            }
            g.reachable = any;
        }
    } catch (const bc::error&) {
        // Keep every group reachable rather than lose them all.
    }
}
