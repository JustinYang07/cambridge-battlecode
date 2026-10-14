#include "comms.h"
#include <array>
#include <cambc.h>
#include <span>

namespace comms {

static std::array<std::uint8_t, max_slots> cache{};

void refresh() { cambc::shared_memory.read(0, std::span{cache}); }

namespace {
void write(int slot, std::uint8_t value) {
    if (slot < 0 || slot >= max_slots) return;
    if (cache[static_cast<std::size_t>(slot)] == value) return;
    cambc::shared_memory[static_cast<std::size_t>(slot)] = value;
    cache[static_cast<std::size_t>(slot)] = value;
}
} // namespace

void claimGroup(int slot, int groupId) {
    write(slot, groupId >= 0 && groupId + 1 < enemy_code ? static_cast<std::uint8_t>(groupId + 1) : 0);
}

void claimEnemy(int slot, int enemyIndex) {
    write(slot, enemyIndex >= 0 && enemyIndex < 255 - enemy_code ? static_cast<std::uint8_t>(enemy_code + enemyIndex) : 0);
}

unsigned focusMask(int enemyIndex, int exceptSlot) {
    if (enemyIndex < 0) return 0;
    unsigned mask = 0;
    for (int i = 0; i < max_slots; ++i)
        if (i != exceptSlot && cache[static_cast<std::size_t>(i)] == enemy_code + enemyIndex) mask |= 1u << i;
    return mask;
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
