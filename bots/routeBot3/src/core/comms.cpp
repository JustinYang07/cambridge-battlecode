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

// Group and bracket in a claim byte; group -1 when the byte is not a camp claim.
int groupOf(std::uint8_t v) { return v >= 1 && v < enemy_code ? (v - 1) / 3 : -1; }
int bracketOf(std::uint8_t v) { return (v - 1) % 3; }
} // namespace

int bracket(int tiles) { return tiles <= 6 ? 0 : tiles <= 15 ? 1 : 2; }

void claimGroup(int slot, int groupId, int tiles) {
    if (slot >= alert_byte) return;  // bytes 6-7 are the team alert
    write(slot, groupId >= 0 && groupId <= max_claim_group ? static_cast<std::uint8_t>(1 + 3 * groupId + bracket(tiles)) : 0);
}

void claimEnemy(int slot, int enemyIndex) {
    if (slot >= alert_byte) return;
    write(slot, enemyIndex >= 0 && enemyIndex < regroup_code - enemy_code ? static_cast<std::uint8_t>(enemy_code + enemyIndex) : 0);
}

unsigned focusMask(int enemyIndex, int exceptSlot) {
    if (enemyIndex < 0) return 0;
    unsigned mask = 0;
    for (int i = 0; i < alert_byte; ++i)
        if (i != exceptSlot && cache[static_cast<std::size_t>(i)] == enemy_code + enemyIndex) mask |= 1u << i;
    return mask;
}

void claimRegroup(int slot) {
    if (slot < alert_byte) write(slot, regroup_code);
}

int regroupers(int exceptSlot) {
    int count = 0;
    for (int i = 0; i < alert_byte; ++i)
        if (i != exceptSlot && cache[static_cast<std::size_t>(i)] == regroup_code) ++count;
    return count;
}

void clear(int slot) {
    if (slot < alert_byte) write(slot, 0);
}

int claimsOn(int groupId, int exceptSlot) {
    int count = 0;
    for (int i = 0; i < alert_byte; ++i)
        if (i != exceptSlot && groupOf(cache[static_cast<std::size_t>(i)]) == groupId) ++count;
    return count;
}

unsigned claimersMask(int groupId, int exceptSlot) {
    unsigned mask = 0;
    for (int i = 0; i < alert_byte; ++i)
        if (i != exceptSlot && groupOf(cache[static_cast<std::size_t>(i)]) == groupId) mask |= 1u << i;
    return mask;
}

unsigned ownersMask(int groupId, int mySlot, int tiles) {
    const int mine = bracket(tiles);
    unsigned mask = 0;
    for (int i = 0; i < alert_byte; ++i) {
        const auto v = cache[static_cast<std::size_t>(i)];
        if (i == mySlot || groupOf(v) != groupId) continue;
        const int theirs = bracketOf(v);
        if (theirs < mine || (theirs == mine && i < mySlot)) mask |= 1u << i;
    }
    return mask;
}

Alert readAlert(int round) {
    Alert a;
    const auto lo = cache[alert_byte], hi = cache[alert_byte + 1];
    if (lo == 0 && hi == 0) return a;
    const int age = (round - (hi >> 6)) & 3;
    if (age > 1) {
        write(alert_byte, 0);
        write(alert_byte + 1, 0);
        return a;
    }
    a.fresh = true;
    a.x = ((lo & 0x3F) - 1) * 2;
    a.y = (hi & 0x3F) * 2;
    a.enemies = (lo & 0x40) ? 2 : 1;
    a.squishy = (lo & 0x80) != 0;
    return a;
}

void raiseAlert(int x, int y, int enemies, bool squishy, int round) {
    write(alert_byte, static_cast<std::uint8_t>(((x / 2 + 1) & 0x3F) | (enemies > 1 ? 0x40 : 0) | (squishy ? 0x80 : 0)));
    write(alert_byte + 1, static_cast<std::uint8_t>(((y / 2) & 0x3F) | ((round & 3) << 6)));
}

} // namespace comms
