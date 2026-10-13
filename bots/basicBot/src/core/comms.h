#pragma once
// Team coordination through the 8-byte shared memory.
//
// Layout: byte i belongs to allied champion slot i (0..7) and holds the camp
// group it has claimed, plus one, or 0 for no claim. Claims are rewritten every
// turn, so a dead or retargeted champion releases its claim on its next turn.
#include <cstdint>

namespace comms {

inline constexpr int max_slots = 8;

// Read the shared bytes once per turn; queries below use the cached copy.
void refresh();
void claimGroup(int slot, int groupId);  // groupId < 0 clears the claim
void clear(int slot);
// Number of other allies currently claiming this group.
int claimsOn(int groupId, int exceptSlot);

} // namespace comms
