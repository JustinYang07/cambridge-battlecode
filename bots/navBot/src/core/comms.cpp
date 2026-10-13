#include "comms.h"
#include <array>
#include <cambc.h>
#include <span>

namespace comms {

static std::array<std::uint8_t, max_slots> cache{};

void refresh() { cambc::shared_memory.read(0, std::span{cache}); }

void claimGroup(int slot, int groupId) {
    if (slot < 0 || slot >= max_slots) return;
    const std::uint8_t value = groupId >= 0 && groupId < 255 ? static_cast<std::uint8_t>(groupId + 1) : 0;
    if (cache[static_cast<std::size_t>(slot)] == value) return;
    cambc::shared_memory[static_cast<std::size_t>(slot)] = value;
    cache[static_cast<std::size_t>(slot)] = value;
}

void clear(int slot) { claimGroup(slot, -1); }

int claimsOn(int groupId, int exceptSlot) {
    int count = 0;
    for (int i = 0; i < max_slots; ++i)
        if (i != exceptSlot && cache[static_cast<std::size_t>(i)] == groupId + 1) ++count;
    return count;
}

unsigned claimersMask(int groupId, int exceptSlot) {
    unsigned mask = 0;
    for (int i = 0; i < max_slots; ++i)
        if (i != exceptSlot && cache[static_cast<std::size_t>(i)] == groupId + 1) mask |= 1u << i;
    return mask;
}

} // namespace comms
