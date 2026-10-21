#include "statics.h"
#include <algorithm>
#include <array>
#include "config.h"
#include "json.h"
#include "trace.h"

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
    const auto k = tileCampIndex_[static_cast<std::size_t>(tile.y * width + tile.x)];
    return k ? &tileCamps_[k - 1u] : nullptr;
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

// Draft block layout (all little-endian): magic, version, group count, stat-type count; per stat
// type health u16, strength u16, attack range u8, camp kind u8; per group handle u16, gems u16,
// gold u16, respawn u16, camp count u8, then per camp handle u16, x u8, y u8, stat type u8. Draft
// memory's last two bytes hold the block's offset (0: none), so the plan's own layout is untouched.
namespace {
struct CampSrc {
    std::uint32_t id;
    bc::vec2 pos;
    int health, strength, range, kind;
};
struct GroupSrc {
    std::uint32_t id;
    int gems, gold, respawn;
    std::size_t first, count;  // range in the shared camp list (one allocation, not one per group)
};
struct MapSrc {
    std::vector<GroupSrc> groups;
    std::vector<CampSrc> camps;
    // Dash-in groups found by the draft: group, dash-from tile, dash-to tile.
    bool dashKnown = false;
    std::vector<std::array<int, 5>> dash;
    bool empty() const { return groups.empty(); }
};

// The camp facts from draft memory, or nothing if there is no valid block for this map.
MapSrc fromDraft() {
    MapSrc out;
    try {
        const auto& m = bc::draft_memory;
        if (m.size() < 1024) return {};
        const std::size_t end = m.size() - 2;
        // Raw reads: the snapshot's operator[] is bounds-checked (~25 fuel a byte, ~26k a block).
        const auto* d = reinterpret_cast<const std::uint8_t*>(m.data());
        auto b = [d](std::size_t i) { return static_cast<int>(d[i]); };
        auto u16 = [&](std::size_t i) { return b(i) | (b(i + 1) << 8); };
        const std::size_t off = static_cast<std::size_t>(u16(end));
        if (off == 0 || off + 4 > end || b(off) != Statics::kPackMagic || b(off + 1) != Statics::kPackVersion) return {};
        const std::size_t groups = static_cast<std::size_t>(b(off + 2)), types = static_cast<std::size_t>(b(off + 3));
        std::size_t at = off + 4;
        if (at + types * 6 > end) return {};
        std::array<std::array<int, 4>, 256> stat;  // not zeroed (4 KB: ~4k fuel); only [0, types) is read
        for (std::size_t k = 0; k < types; ++k, at += 6) stat[k] = {u16(at), u16(at + 2), b(at + 4), b(at + 5)};
        out.groups.reserve(groups);
        out.camps.reserve(255);
        for (std::size_t gi = 0; gi < groups; ++gi) {
            if (at + 9 > end) return {};
            auto& g = out.groups.emplace_back();
            g.id = static_cast<std::uint32_t>(u16(at));
            g.gems = u16(at + 2);
            g.gold = u16(at + 4);
            g.respawn = u16(at + 6);
            const std::size_t n = static_cast<std::size_t>(b(at + 8));
            at += 9;
            if (at + n * 5 > end) return {};
            g.first = out.camps.size();
            g.count = n;
            for (std::size_t c = 0; c < n; ++c, at += 5) {
                const auto k = static_cast<std::size_t>(b(at + 4));
                if (k >= types) return {};
                const auto& t = stat[k];
                out.camps.push_back({static_cast<std::uint32_t>(u16(at)), {b(at + 2), b(at + 3)}, t[0], t[1], t[2], t[3]});
            }
        }
        // Dash-in groups (version 3): count, then group u8, from x, y, to x, y.
        if (at + 1 > end) return {};
        const std::size_t dashes = static_cast<std::size_t>(b(at++));
        if (at + dashes * 5 > end) return {};
        for (std::size_t k = 0; k < dashes; ++k, at += 5) {
            if (static_cast<std::size_t>(b(at)) >= groups) return {};
            out.dash.push_back({b(at), b(at + 1), b(at + 2), b(at + 3), b(at + 4)});
        }
        out.dashKnown = true;
        // Against the engine: the group count and the first camp's tile (two host calls). A stale
        // or foreign block falls back to the engine queries.
        std::uint32_t count = 0;
        bc::detail::check(bc::detail::sdk_map_num_camp_groups(&count));
        if (count != out.groups.size()) return {};
        if (!out.camps.empty()) {
            bc::detail::camp_info info{};
            bc::detail::check(bc::detail::sdk_map_camp_info(out.camps[0].id, &info));
            if (info.x != out.camps[0].pos.x || info.y != out.camps[0].pos.y || info.type != out.camps[0].kind) return {};
        }
    } catch (const std::exception&) {
        return {};
    }
    return out;
}

// The same facts from the engine (draft, or no usable block).
MapSrc fromEngine() {
    MapSrc out;
    const auto live = bc::map::camp_groups();
    out.groups.reserve(live.size());
    for (const auto& group : live) {
        auto& g = out.groups.emplace_back();
        g.id = static_cast<std::uint32_t>(static_cast<bc::handle_camp_group_t>(group));
        g.gems = static_cast<int>(group.gem_value());
        g.gold = static_cast<int>(group.gold_value());
        g.respawn = group.max_respawn_timer();
        g.first = out.camps.size();
        g.count = group.camps().size();
        for (const auto& camp : group.camps()) {
            const auto st = camp.base_stats();
            out.camps.push_back({static_cast<std::uint32_t>(static_cast<bc::handle_camp_t>(camp)), camp.spawn_position(),
                               static_cast<int>(st.base_health), static_cast<int>(st.base_strength), st.base_attack_range,
                               static_cast<int>(camp.kind())});
            // Health and strength are whole numbers in the game data; keep exact values if not.
            if (static_cast<float>(out.camps.back().health) != st.base_health || static_cast<float>(out.camps.back().strength) != st.base_strength)
                out.camps.back().health = -1;  // marks the block unpackable
        }
    }
    return out;
}

std::vector<std::uint8_t> pack(const MapSrc& map) {
    const auto& src = map.groups;
    std::vector<std::array<int, 4>> types;
    std::vector<std::uint8_t> body;
    auto put16 = [&](int v) {
        body.push_back(static_cast<std::uint8_t>(v & 255));
        body.push_back(static_cast<std::uint8_t>((v >> 8) & 255));
    };
    auto fits16 = [](long long v) { return v >= 0 && v < 65536; };
    if (src.size() >= 256) return {};
    for (const auto& g : src) {
        if (!fits16(g.id) || !fits16(g.gems) || !fits16(g.gold) || !fits16(g.respawn) || g.count >= 256) return {};
        put16(static_cast<int>(g.id));
        put16(g.gems);
        put16(g.gold);
        put16(g.respawn);
        body.push_back(static_cast<std::uint8_t>(g.count));
        for (std::size_t ci = g.first; ci < g.first + g.count; ++ci) {
            const auto& c = map.camps[ci];
            if (!fits16(c.id) || !fits16(c.health) || !fits16(c.strength) || c.range < 0 || c.range > 255 || c.kind < 0 || c.kind > 255 ||
                c.pos.x < 0 || c.pos.x > 255 || c.pos.y < 0 || c.pos.y > 255)
                return {};
            const std::array<int, 4> t{c.health, c.strength, c.range, c.kind};
            std::size_t k = 0;
            while (k < types.size() && types[k] != t) ++k;
            if (k == types.size()) types.push_back(t);
            if (k >= 256) return {};
            put16(static_cast<int>(c.id));
            body.push_back(static_cast<std::uint8_t>(c.pos.x));
            body.push_back(static_cast<std::uint8_t>(c.pos.y));
            body.push_back(static_cast<std::uint8_t>(k));
        }
    }
    std::vector<std::uint8_t> out{Statics::kPackMagic, Statics::kPackVersion, static_cast<std::uint8_t>(src.size()),
                                  static_cast<std::uint8_t>(types.size())};
    for (const auto& t : types) {
        out.push_back(static_cast<std::uint8_t>(t[0] & 255));
        out.push_back(static_cast<std::uint8_t>(t[0] >> 8));
        out.push_back(static_cast<std::uint8_t>(t[1] & 255));
        out.push_back(static_cast<std::uint8_t>(t[1] >> 8));
        out.push_back(static_cast<std::uint8_t>(t[2]));
        out.push_back(static_cast<std::uint8_t>(t[3]));
    }
    out.insert(out.end(), body.begin(), body.end());
    return out;
}
} // namespace

void Statics::appendPacked(std::vector<std::uint8_t>& bytes) const {
    const std::size_t cap = 1024;
    if (bytes.size() > cap - 2) return;
    std::size_t off = 0;
    if (!packed_.empty() && bytes.size() + packed_.size() <= cap - 2) {
        off = bytes.size();
        bytes.insert(bytes.end(), packed_.begin(), packed_.end());
    }
    bytes.resize(cap, 0);
    bytes[cap - 2] = static_cast<std::uint8_t>(off & 255);
    bytes[cap - 1] = static_cast<std::uint8_t>(off >> 8);
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

    tileCampIndex_.alloc(static_cast<std::size_t>(width * height));
    tileCampIndex_.zero();
    // combatBotv2: the engine's camp-group queries and our copies of them cost ~190k on a 42-group
    // map (lanes), every champion, on a 200k first turn. The draft bot runs the same setup on its
    // 1M-fuel turn and leaves a compact copy in draft memory (appendPacked); champions build the
    // groups and the camp handles (just engine ids) from it without asking the engine.
    const bool draft = bc::is_draft();
    MapSrc src = draft ? MapSrc{} : fromDraft();
    usedDraftBlock = !src.empty();
    if (src.empty()) src = fromEngine();
    if (draft) {
        bool exact = true;
        for (const auto& c : src.camps) exact = exact && c.health >= 0;
        packed_ = exact ? pack(src) : std::vector<std::uint8_t>{};
    }
    groups.reserve(src.groups.size());
    tileCamps_.reserve(255);
    int id = 0;
    std::vector<bc::camp> handles;
    for (const auto& s : src.groups) {
        auto& g = groups.emplace_back();
        g.id = id++;
        g.gems = s.gems;
        g.gold = s.gold;
        g.respawn = s.respawn;
        g.harmless = true;
        g.tiles.reserve(s.count);
        g.killOrder.reserve(s.count);
        handles.clear();
        handles.reserve(s.count);
        long cx = 0, cy = 0;
        for (std::size_t ci = s.first; ci < s.first + s.count; ++ci) {
            const auto& c = src.camps[ci];
            const auto p = c.pos;
            const auto health = static_cast<float>(c.health), strength = static_cast<float>(c.strength);
            const auto camp = bc::detail::handle_access::make_camp(static_cast<bc::handle_camp_t>(c.id), static_cast<bc::handle_camp_group_t>(s.id),
                                                                    static_cast<bc::camp_type>(c.kind), p);
            handles.push_back(camp);
            g.tiles.push_back(p);
            g.tileMask.set(p);
            cx += p.x;
            cy += p.y;
            g.totalHealth += health;
            g.totalStrength += strength;
            if (strength > 0) {
                g.harmless = false;
                g.reach.draw_chebyshev(p, static_cast<float>(c.range));
                g.maxRange = std::max(g.maxRange, c.range);
            }
            g.killOrder.push_back({health, strength});
            if (tileCamps_.size() < 255) {
                tileCamps_.push_back({g.id, health, strength, c.range, camp});
                tileCampIndex_[static_cast<std::size_t>(p.y * width + p.x)] = static_cast<std::uint8_t>(tileCamps_.size());
            } else {
                campsCapped_ = true;
            }
            if (c.kind == static_cast<int>(bc::camp_type::monolith)) g.hasMonolith = true;
            if constexpr (kTracing) {  // labels are for traces only
                if (!g.label.empty()) g.label += '+';
                g.label += shortName(static_cast<bc::camp_type>(c.kind));
            }
        }
        g.handle = bc::detail::handle_access::make_camp_group(static_cast<bc::handle_camp_group_t>(s.id), static_cast<std::uint32_t>(s.gold),
                                                              static_cast<std::uint32_t>(s.gems), s.respawn, std::move(handles));
        handles = {};
        g.campCount = static_cast<int>(g.tiles.size());
        std::sort(g.killOrder.begin(), g.killOrder.end(), [](const CampMember& a, const CampMember& b) {
            return a.strength != b.strength ? a.strength > b.strength : a.health < b.health;
        });
        for (std::size_t i = 0; i < g.tiles.size(); ++i)
            for (std::size_t j = i + 1; j < g.tiles.size(); ++j)
                if (dist(g.tiles[i], g.tiles[j]) <= 1) ++g.adjacentPairs;
        if (g.campCount) g.center = {static_cast<int>(cx / g.campCount), static_cast<int>(cy / g.campCount)};
        if constexpr (kTracing) g.label += "@" + fmt(g.tiles.empty() ? bc::vec2{} : g.tiles[0]);
    }
    // Which groups can be walked to at all: one flood fill from our spawn over the terrain (ambush
    // has camps in pockets sealed by walls and holes, reachable only by dashing; champions used to
    // stand still for 1,500 rounds trying to path to one). Runs once, on the first turn's budget.
    reach = ~terrain;
    try {
        bc::distance_field field;
        bc::bitboard visited;
        field.compute(terrain, allySpawn, visited);
        reach = visited;
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
    // Pockets a dash can enter: flood fill from each sealed group's neighbouring tiles, then the
    // landing tile nearest the group with a legal dash from a tile we can walk to. combatBotv2: the
    // draft runs the search (~230k on ambush) and passes the dash tiles; champions only build the
    // pocket boards.
    dashBlock = terrain & ~bc::map::holes();
    std::vector<int> dashOf(groups.size(), -1);
    for (std::size_t k = 0; k < src.dash.size(); ++k)
        if (static_cast<std::size_t>(src.dash[k][0]) < groups.size()) dashOf[static_cast<std::size_t>(src.dash[k][0])] = static_cast<int>(k);
    // Draft: 01islands seals most camps on islands, and this search cost ~900k of the 1M first
    // turn (the pick then went random). The landing search only visits pocket tiles near walkable
    // ones and reads byte tables (bitboard calls per tile cost ~30-300 fuel each); the draft also
    // stops searching at a fuel floor.
    std::vector<std::uint8_t> reachBits, terrainBits, blockBits;
    bc::bitboard nearReach;
    auto bitAt = [&](const std::vector<std::uint8_t>& bits, int x, int y) {
        const auto i = static_cast<std::size_t>(y * width + x);
        return ((bits[i >> 3] >> (i & 7)) & 1) != 0;
    };
    auto inside = [&](int x, int y) { return x >= 0 && y >= 0 && x < width && y < height; };
    // dashLegal on the byte tables.
    auto legal = [&](bc::vec2 from, bc::vec2 to) {
        const int d = dist(from, to);
        if (d < 1 || d > 2 || !inside(to.x, to.y) || bitAt(terrainBits, to.x, to.y)) return false;
        if (d == 1) return true;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy) {
                const bc::vec2 m{from.x + dx, from.y + dy};
                if (!inside(m.x, m.y) || dist(m, to) > 1) continue;
                if (!bitAt(blockBits, m.x, m.y)) return true;
            }
        return false;
    };
    try {
        for (auto& g : groups) {
            if (g.reachable || g.tiles.empty()) continue;
            if (src.dashKnown && dashOf[static_cast<std::size_t>(g.id)] < 0) continue;
            if (draft && bc::fuel_remaining() < tune::draft_dash_floor) break;
            bc::bitboard seeds;
            seeds.clear();
            for (auto t : g.tiles) seeds.draw_chebyshev(t, 1.0f);
            seeds = seeds & ~terrain & ~g.tileMask;
            if (seeds.is_empty()) continue;
            bc::distance_field field;
            bc::bitboard pocket;
            field.compute(terrain, seeds, pocket);
            pocket = pocket & ~reach;
            if (src.dashKnown) {
                const auto& d = src.dash[static_cast<std::size_t>(dashOf[static_cast<std::size_t>(g.id)])];
                g.dashFrom = {d[1], d[2]};
                g.dashTo = {d[3], d[4]};
                g.dashIn = true;
                g.pocket = pocket | g.tileMask;
                continue;
            }
            int best = 1 << 20;
            if (reachBits.empty()) {
                reachBits = reach.export_linear_buffer();
                terrainBits = terrain.export_linear_buffer();
                blockBits = dashBlock.export_linear_buffer();
                // Tiles within 2 of a walkable tile (a superset if shifts wrap): only these can be landings.
                bc::bitboard row = reach;
                for (int k : {1, 2, -1, -2}) row |= reach.shift_right(k);
                nearReach = row;
                for (int k : {1, 2, -1, -2}) nearReach |= row.shift_up(k);
            }
            for (auto p : (pocket & nearReach).positions()) {
                if (onGroup(g, p)) continue;
                // Walkable tiles within 2, in positions() order (row-major), as the bitboard gave them.
                for (int y = p.y - 2; y <= p.y + 2; ++y)
                    for (int x = p.x - 2; x <= p.x + 2; ++x) {
                        if (!inside(x, y) || !bitAt(reachBits, x, y)) continue;
                        const bc::vec2 e{x, y};
                        if (!legal(e, p)) continue;
                        const int cost = dist(p, g.center) * 4 + dist(e, spawnCenter);
                        if (cost < best) { best = cost; g.dashFrom = e; g.dashTo = p; }
                    }
            }
            if (best < (1 << 20)) {
                g.dashIn = true;
                g.pocket = pocket | g.tileMask;  // only sealed groups allocate one
            }
        }
    } catch (const bc::error&) {
        for (auto& g : groups) g.dashIn = false;
    }
    if (draft && !packed_.empty()) {
        std::vector<std::uint8_t> dash;
        for (const auto& g : groups)
            if (g.dashIn && g.id < 256 && g.dashFrom.x >= 0 && g.dashFrom.x < 256 && g.dashFrom.y >= 0 && g.dashFrom.y < 256 &&
                g.dashTo.x >= 0 && g.dashTo.x < 256 && g.dashTo.y >= 0 && g.dashTo.y < 256)
                dash.insert(dash.end(), {static_cast<std::uint8_t>(g.id), static_cast<std::uint8_t>(g.dashFrom.x), static_cast<std::uint8_t>(g.dashFrom.y),
                                         static_cast<std::uint8_t>(g.dashTo.x), static_cast<std::uint8_t>(g.dashTo.y)});
        if (dash.size() / 5 < 256) {
            packed_.push_back(static_cast<std::uint8_t>(dash.size() / 5));
            packed_.insert(packed_.end(), dash.begin(), dash.end());
        } else {
            packed_.clear();
        }
    }
}

void Statics::dashTables() const {
    if (!wallBits_.empty()) return;
    wallBits_ = dashBlock.export_linear_buffer();
    holeBits_ = (terrain & ~dashBlock).export_linear_buffer();
}

void Statics::buildHops(unsigned long long reserve) const {
    if (hopsDone_ || width <= 0) return;
    dashTables();
    auto bit = [&](const std::vector<std::uint8_t>& bits, int x, int y) {
        if (x < 0 || y < 0 || x >= width || y >= height) return true;  // off the map: like a wall
        const auto i = static_cast<std::size_t>(y * width + x);
        return ((bits[i >> 3] >> (i & 7)) & 1) != 0;
    };
    auto hole = [&](int x, int y) { return x >= 0 && y >= 0 && x < width && y < height && bit(holeBits_, x, y); };
    auto walkable = [&](int x, int y) { return !bit(wallBits_, x, y) && !hole(x, y); };
    const int total = width * height;
    while (hopScan_ < total) {
        if ((hopScan_ & 63) == 0 && bc::fuel_remaining() < reserve) return;  // resume next turn
        const int ax = hopScan_ % width, ay = hopScan_ / width;
        ++hopScan_;
        if (!walkable(ax, ay)) continue;
        for (int ty = ay - 2; ty <= ay + 2; ++ty)
            for (int tx = ax - 2; tx <= ax + 2; ++tx) {
                if (std::max(std::abs(tx - ax), std::abs(ty - ay)) != 2 || !walkable(tx, ty)) continue;
                // Tiles beside both ends: a hop only when none is walkable (else it is a 2-step walk)
                // and one is a hole the dash passes over (walls block a dash).
                bool walkMid = false, holeMid = false;
                for (int my = ay - 1; my <= ay + 1; ++my)
                    for (int mx = ax - 1; mx <= ax + 1; ++mx) {
                        if (std::max(std::abs(mx - tx), std::abs(my - ty)) > 1) continue;
                        if (walkable(mx, my)) walkMid = true;
                        else if (hole(mx, my)) holeMid = true;
                    }
                if (!walkMid && holeMid && hops.size() < 4096) hops.push_back({{ax, ay}, {tx, ty}});
            }
    }
    hopsDone_ = true;
}

int Statics::dashLine(bc::vec2 a, bc::vec2 b, bc::vec2* hopFrom, bc::vec2* hopTo) const {
    dashTables();
    auto bitAt = [&](const std::vector<std::uint8_t>& bits, bc::vec2 t) {
        if (t.x < 0 || t.y < 0 || t.x >= width || t.y >= height) return true;
        const auto i = static_cast<std::size_t>(t.y * width + t.x);
        return ((bits[i >> 3] >> (i & 7)) & 1) != 0;
    };
    // Bresenham from a to b.
    int x = a.x, y = a.y;
    const int dx = std::abs(b.x - a.x), dy = -std::abs(b.y - a.y), sx = a.x < b.x ? 1 : -1, sy = a.y < b.y ? 1 : -1;
    int err = dx + dy, run = 0, result = 0;
    bc::vec2 prev = a;
    while (!(x == b.x && y == b.y)) {
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x += sx; }
        if (e2 <= dx) { err += dx; y += sy; }
        const bc::vec2 t{x, y};
        if (bitAt(wallBits_, t) && !(t == b)) return -1;
        if (bitAt(holeBits_, t)) {
            if (++run > 1) return -1;
            if (result == 0) {
                result = 1;
                if (hopFrom) *hopFrom = prev;
            }
        } else {
            if (run == 1 && result == 1 && hopTo && hopTo->x < 0) *hopTo = t;
            run = 0;
            prev = t;
        }
    }
    return result;
}

bool Statics::dashLegal(bc::vec2 from, bc::vec2 to) const {
    const int d = dist(from, to);
    if (d < 1 || d > 2 || to.x < 0 || to.y < 0 || to.x >= width || to.y >= height || terrain.is_set(to)) return false;
    if (d == 1) return true;
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy) {
            const bc::vec2 m{from.x + dx, from.y + dy};
            if (m.x < 0 || m.y < 0 || m.x >= width || m.y >= height || dist(m, to) > 1) continue;
            if (!dashBlock.is_set(m)) return true;
        }
    return false;
}

const GroupInfo* Statics::pocketAt(bc::vec2 tile) const {
    if (reach.is_set(tile)) return nullptr;
    for (const auto& g : groups)
        if (g.dashIn && g.inPocket(tile)) return &g;
    return nullptr;
}
