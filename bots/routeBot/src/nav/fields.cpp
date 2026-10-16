#include "fields.h"
#include <algorithm>
#include <cstdint>
#include <vector>
#include "../core/statics.h"
#include "navigator.h"

namespace bc = cambc;

namespace fields {

namespace {

struct Field {
    bool ready = false;
    std::vector<std::uint16_t> dist;   // row-major; meaningless where not reached
    std::vector<std::uint8_t> reached; // bit-packed, LSB first
};

std::vector<Field> groups_, altars_;
std::size_t next_ = 0;  // groups first, then altars

void compute(Field& f, const bc::bitboard& roots) {
    const auto& s = Statics::get();
    bc::distance_field field;
    bc::bitboard visited;
    field.compute(s.terrain, roots, visited);
    f.dist.resize(static_cast<std::size_t>(s.width * s.height));
    field.export_linear_buffer(f.dist);
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

} // namespace

void step(unsigned long long reserve) {
    const auto& s = Statics::get();
    if (groups_.empty() && altars_.empty()) {
        groups_.resize(s.groups.size());
        altars_.resize(s.altars.size());
    }
    const std::size_t total = groups_.size() + altars_.size();
    // One field costs ~3-12k on small maps and more on big ones: only start one when the most a
    // field has cost so far still fits above the reserve (an overrun loses the next turn).
    static unsigned long long maxCost = 15000;
    while (next_ < total && bc::fuel_remaining() > reserve + maxCost) {
        const auto before = bc::fuel_remaining();
        if (next_ < groups_.size()) {
            compute(groups_[next_], s.groups[next_].tileMask);
        } else {
            const auto altar = s.altars[next_ - groups_.size()];
            bc::bitboard ring;
            ring.clear();
            ring.draw_chebyshev(altar, 1.0f);
            compute(altars_[next_ - groups_.size()], ring & ~s.terrain);
        }
        ++next_;
        maxCost = std::max(maxCost, before - bc::fuel_remaining());
    }
}

int toGroup(int groupId, bc::vec2 from) {
    if (groupId < 0 || groupId >= static_cast<int>(groups_.size())) return -1;
    return read(groups_[static_cast<std::size_t>(groupId)], from);
}

int toAltar(int altarIndex, bc::vec2 from) {
    if (altarIndex < 0 || altarIndex >= static_cast<int>(altars_.size())) return -1;
    return read(altars_[static_cast<std::size_t>(altarIndex)], from);
}

} // namespace fields
