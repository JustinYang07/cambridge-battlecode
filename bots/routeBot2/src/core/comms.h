#pragma once
// Team coordination through the 8-byte shared memory.
//
// Layout: byte i belongs to allied champion slot i (0..7) and holds what it is working on:
// 0 nothing, 1..199 camp group + 1, 200+k fighting enemy champion k (index in the enemy
// roster). Claims are rewritten every turn, so a dead or retargeted champion releases its
// claim on its next turn.
#include <cstdint>

namespace comms {

inline constexpr int max_slots = 8;
inline constexpr int enemy_code = 200;

// Read the shared bytes once per turn; queries below use the cached copy.
void refresh();
void claimGroup(int slot, int groupId);  // groupId < 0 clears the claim
void claimEnemy(int slot, int enemyIndex);  // focus fire: the enemy champion we are attacking
void clear(int slot);
// Number of other allies currently claiming this group.
int claimsOn(int groupId, int exceptSlot);
// Bit per allied slot (excluding exceptSlot) currently claiming this group.
unsigned claimersMask(int groupId, int exceptSlot);
// Bit per allied slot (excluding exceptSlot) attacking this enemy champion.
unsigned focusMask(int enemyIndex, int exceptSlot);

} // namespace comms
