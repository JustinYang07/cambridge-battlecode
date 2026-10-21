#include "fields.h"
#include <algorithm>
#include <cstdint>
#include <vector>
#include "../core/config.h"
#include "../core/rawbuf.h"
#include "../core/statics.h"
#include "navigator.h"

namespace bc = cambc;

namespace fields {

namespace {

struct Field {
    bool ready = false;
    RawBuf<std::uint16_t> dist;        // row-major; meaningless where not reached (not zeroed: the export fills it)
    std::vector<std::uint8_t> reached; // bit-packed, LSB first
};

// xBot: mirrored fields. Maps are symmetric, so the field to a group on their half is the mirror
// image of the field to its twin on ours: only our half's fields are computed, and a read for a
// group on their half reads its twin's field at the mirrored tile. Measured on all 17 maps
// (snap-mirrorcheck): every group has a twin, and the mirrored field differs from the real one on
// under 1% of tiles, by 1-4 steps, where a path hugs a spawn (our terrain blocks only their spawn).
// Our spawn mirrors onto theirs, which the field never reaches: reads from inside our spawn go via
// the tiles just outside it (the spawn is open ground, so the walk inside it is the Chebyshev distance).
struct Slot {
    int source = -1;        // field index this entry reads (itself, or its twin)
    bool mirrored = false;  // read at the mirrored tile
    // Mirrored: distance from each tile of our spawn (by its index in spawnTiles_), filled on the
    // first read from inside the spawn (bank scoring reads every altar from the spawn centre each
    // turn: going round the ring on every read cost 20k+ a turn on lanes).
    std::vector<std::uint16_t> fromSpawn;
};

std::vector<Field> groups_, altars_;
std::vector<Slot> groupSlot_, altarSlot_;
Field spawn_;
std::vector<bc::vec2> spawnRing_;  // walkable tiles just outside our spawn
std::vector<std::uint8_t> inSpawn_; // our spawn: index into spawnTiles_ + 1 per tile, 0 outside (bitboard reads cost ~30-300 fuel)
std::vector<bc::vec2> spawnTiles_;
std::size_t next_ = 0;  // groups first, then altars
std::vector<Field> points_;
std::vector<bc::vec2> pointTiles_;
std::size_t nextPoint_ = 0;

// Local arithmetic: the SDK's reflect_* / rotate_180 are host calls, and farm scoring reads
// thousands of mirrored tiles a turn (with them, overruns rose from 89 to 156 a game).
bc::vec2 mirror(bc::vec2 p) {
    static const auto sym = bc::map::symmetry();
    const auto& s = Statics::get();
    switch (sym) {
    case bc::symmetry::horizontal: return {s.width - 1 - p.x, p.y};
    case bc::symmetry::vertical: return {p.x, s.height - 1 - p.y};
    default: return {s.width - 1 - p.x, s.height - 1 - p.y};
    }
}

void compute(Field& f, const bc::bitboard& roots) {
    const auto& s = Statics::get();
    bc::distance_field field;
    bc::bitboard visited;
    field.compute(s.terrain, roots, visited);
    f.dist.alloc(static_cast<std::size_t>(s.width * s.height));
    field.export_linear_buffer(f.dist.span());
    f.reached = visited.export_linear_buffer();
    f.ready = true;
}

int read(const Field& f, bc::vec2 from) {
    if (!f.ready) return -1;
    const auto& s = Statics::get();
    if (from.x < 0 || from.y < 0 || from.x >= s.width || from.y >= s.height) return Navigator::unreachable;
    const auto i = static_cast<std::size_t>(from.y * s.width + from.x);
    if (!((f.reached[i >> 3] >> (i & 7)) & 1)) return Navigator::unreachable;
    return f.dist[i];
}

// Pair every group and altar with its mirror twin; the twin nearer our spawn gets the field.
void sized() {
    const auto& s = Statics::get();
    if (!groups_.empty() || !altars_.empty() || (s.groups.empty() && s.altars.empty())) return;
    groups_.resize(s.groups.size());
    altars_.resize(s.altars.size());
    groupSlot_.assign(s.groups.size(), Slot{});
    altarSlot_.assign(s.altars.size(), Slot{});
    for (std::size_t i = 0; i < s.groups.size(); ++i) groupSlot_[i].source = static_cast<int>(i);
    for (std::size_t i = 0; i < s.altars.size(); ++i) altarSlot_[i].source = static_cast<int>(i);
    if (!tune::mirror_fields) return;
    auto ours = [&](bc::vec2 a, bc::vec2 b) {  // a is on our side of the pair (ties: the lower index keeps it)
        return dist(a, s.spawnCenter) < dist(b, s.spawnCenter);
    };
    for (const auto& g : s.groups) {
        const auto* m = s.groupAt(mirror(g.tiles.front()));
        if (!m || m->id == g.id || m->tiles.size() != g.tiles.size()) continue;
        bool twin = true;
        for (auto t : g.tiles) twin = twin && s.groupAt(mirror(t)) == m;
        if (!twin) continue;
        if (ours(m->center, g.center) || (dist(m->center, s.spawnCenter) == dist(g.center, s.spawnCenter) && m->id < g.id))
            groupSlot_[static_cast<std::size_t>(g.id)] = Slot{m->id, true};
    }
    for (std::size_t i = 0; i < s.altars.size(); ++i) {
        const auto ma = mirror(s.altars[i]);
        for (std::size_t j = 0; j < s.altars.size(); ++j) {
            if (j == i || s.altars[j] != ma) continue;
            if (ours(s.altars[j], s.altars[i]) || (dist(s.altars[j], s.spawnCenter) == dist(s.altars[i], s.spawnCenter) && j < i))
                altarSlot_[i] = Slot{static_cast<int>(j), true};
        }
    }
    bc::bitboard ring;
    ring.clear();
    for (auto p : s.allySpawn.positions()) ring.draw_chebyshev(p, 1.0f);
    for (auto p : (ring & ~s.allySpawn & ~s.terrain).positions()) spawnRing_.push_back(p);
    inSpawn_.assign(static_cast<std::size_t>(s.width * s.height), 0);
    spawnTiles_ = s.allySpawn.positions();
    for (std::size_t k = 0; k < spawnTiles_.size() && k < 255; ++k)
        inSpawn_[static_cast<std::size_t>(spawnTiles_[k].y * s.width + spawnTiles_[k].x)] = static_cast<std::uint8_t>(k + 1);
}

// A read through a slot: the twin's field at the mirrored tile, or our own field.
int readSlot(const std::vector<Field>& fields, Slot& slot, bc::vec2 from) {
    const auto& f = fields[static_cast<std::size_t>(slot.source)];
    if (!slot.mirrored) return read(f, from);
    if (!f.ready) return -1;
    const auto& s = Statics::get();
    if (from.x < 0 || from.y < 0 || from.x >= s.width || from.y >= s.height) return Navigator::unreachable;
    const int k = inSpawn_[static_cast<std::size_t>(from.y * s.width + from.x)];
    if (k == 0) return read(f, mirror(from));
    if (slot.fromSpawn.empty()) {
        slot.fromSpawn.assign(spawnTiles_.size(), static_cast<std::uint16_t>(Navigator::unreachable));
        for (auto q : spawnRing_) {
            const int d = read(f, mirror(q));
            if (d >= Navigator::unreachable) continue;
            for (std::size_t i = 0; i < spawnTiles_.size(); ++i)
                slot.fromSpawn[i] = static_cast<std::uint16_t>(std::min<int>(slot.fromSpawn[i], d + dist(spawnTiles_[i], q)));
        }
    }
    return slot.fromSpawn[static_cast<std::size_t>(k - 1)];
}

void ensureAltarField(std::size_t i) {
    auto& f = altars_[i];
    if (f.ready) return;
    const auto& s = Statics::get();
    bc::bitboard ring;
    ring.clear();
    ring.draw_chebyshev(s.altars[i], 1.0f);
    compute(f, ring & ~s.terrain);
}

} // namespace

void step(unsigned long long reserve) {
    sized();
    const std::size_t total = groups_.size() + altars_.size();
    // One field costs ~3-12k on small maps and more on big ones: only start one when the most a
    // field has cost so far still fits above the reserve (an overrun loses the next turn).
    static unsigned long long maxCost = 15000;
    if (!spawn_.ready && bc::fuel_remaining() > reserve + maxCost) ensureSpawn();
    while (next_ < total && bc::fuel_remaining() > reserve + maxCost) {
        const bool group = next_ < groups_.size();
        const std::size_t i = group ? next_ : next_ - groups_.size();
        const auto& slot = group ? groupSlot_[i] : altarSlot_[i];
        const bool ready = group ? groups_[i].ready : altars_[i].ready;
        if (ready || slot.source != static_cast<int>(i)) {  // done, or read from its twin's field
            ++next_;
            continue;
        }
        const auto before = bc::fuel_remaining();
        if (group) compute(groups_[i], Statics::get().groups[i].tileMask);
        else ensureAltarField(i);
        ++next_;
        maxCost = std::max(maxCost, before - bc::fuel_remaining());
    }
    while (nextPoint_ < points_.size() && bc::fuel_remaining() > reserve + maxCost) {
        if (!points_[nextPoint_].ready) {
            const auto before = bc::fuel_remaining();
            ensurePoint(static_cast<int>(nextPoint_));
            maxCost = std::max(maxCost, before - bc::fuel_remaining());
        }
        ++nextPoint_;
    }
}

void ensureGroup(int groupId) {
    sized();
    if (groupId < 0 || groupId >= static_cast<int>(groups_.size())) return;
    const int src = groupSlot_[static_cast<std::size_t>(groupId)].source;
    auto& f = groups_[static_cast<std::size_t>(src)];
    if (!f.ready) compute(f, Statics::get().groups[static_cast<std::size_t>(src)].tileMask);
}

void ensureAltar(int altarIndex) {
    sized();
    if (altarIndex < 0 || altarIndex >= static_cast<int>(altars_.size())) return;
    ensureAltarField(static_cast<std::size_t>(altarSlot_[static_cast<std::size_t>(altarIndex)].source));
}

void ensureSpawn(int) {
    if (!spawn_.ready) compute(spawn_, Statics::get().allySpawn);
}

int toSpawn(int, bc::vec2 from) { return read(spawn_, from); }

int toGroup(int groupId, bc::vec2 from) {
    if (groupId < 0 || groupId >= static_cast<int>(groups_.size())) return -1;
    return readSlot(groups_, groupSlot_[static_cast<std::size_t>(groupId)], from);
}

int toAltar(int altarIndex, bc::vec2 from) {
    if (altarIndex < 0 || altarIndex >= static_cast<int>(altars_.size())) return -1;
    return readSlot(altars_, altarSlot_[static_cast<std::size_t>(altarIndex)], from);
}

int registerPoint(bc::vec2 tile) {
    for (std::size_t i = 0; i < pointTiles_.size(); ++i)
        if (pointTiles_[i] == tile) return static_cast<int>(i);
    pointTiles_.push_back(tile);
    points_.emplace_back();
    return static_cast<int>(points_.size() - 1);
}

int toPoint(int id, bc::vec2 from) {
    if (id < 0 || id >= static_cast<int>(points_.size())) return -1;
    return read(points_[static_cast<std::size_t>(id)], from);
}

void ensurePoint(int id) {
    if (id < 0 || id >= static_cast<int>(points_.size()) || points_[static_cast<std::size_t>(id)].ready) return;
    bc::bitboard root;
    root.clear();
    root.set(pointTiles_[static_cast<std::size_t>(id)]);
    compute(points_[static_cast<std::size_t>(id)], root);
}

} // namespace fields
