#pragma once
// Team coordination through the 8-byte shared memory.
//
// Layout: byte i belongs to allied champion slot i (0..7) and holds what it is working on:
//   0          nothing
//   1..219     camp group g with distance bracket b: 1 + 3g + b (groups 0..72)
//              b = 0: within 6 tiles, 1: within 15, 2: further (Chebyshev to the group's centre)
//   220..249   fighting enemy champion k = value - 220 (index in the enemy roster)
//   250        walking to an ally's alert (regrouping): counted as on its way to the fight
//   251        retreating from a fight (rangeBot): allies can follow or hold
// Claims are rewritten every turn, so a dead or retargeted champion releases its claim on its
// next turn, and the distance bracket follows the claimer as it walks.
//
// Bytes 6 and 7 carry one team alert instead (a team has at most 6 bodies, so they are free):
//   byte 6: spotter x / 2 + 1 (6 bits) | several enemies (bit 6) | squishy spotter (bit 7)
//   byte 7: spotter y / 2     (6 bits) | (round & 3) << 6
// Both zero means no alert. The spotter rewrites it every turn it still sees the enemy; a reader
// that finds it older than last round clears it, so a stale alert never looks fresh again.
//
// The bracket lets the nearer champion own a contested camp: a claim only keeps us off a camp
// when the claimer is in a nearer bracket, or the same bracket and an earlier slot. Vision is not
// shared, so this is the only way to know how far a teammate is from its target.
#include <cstdint>

namespace comms {

inline constexpr int max_slots = 8;
inline constexpr int max_claim_group = 72;
inline constexpr int enemy_code = 220;
inline constexpr int regroup_code = 250;
inline constexpr int retreat_code = 251;

// Distance bracket for a claim from this far away (tiles).
int bracket(int tiles);

// Read the shared bytes once per turn; queries below use the cached copy.
void refresh();
void claimGroup(int slot, int groupId, int tiles);  // groupId < 0 clears the claim
void claimEnemy(int slot, int enemyIndex);  // focus fire: the enemy champion we are attacking
void claimRegroup(int slot);  // on our way to an ally's alert
void claimRetreat(int slot);  // retreating from a fight
// The ally in `slot` is retreating / fighting an enemy champion (from its claim byte).
bool retreating(int slot);
bool fighting(int slot);
void clear(int slot);
// Allies (excluding exceptSlot) currently walking to an alert.
int regroupers(int exceptSlot);
// Number of other allies currently claiming this group (any distance): party size estimates.
int claimsOn(int groupId, int exceptSlot);
// Bit per allied slot (excluding exceptSlot) currently claiming this group (any distance).
unsigned claimersMask(int groupId, int exceptSlot);
// Bit per ally whose claim on this group outranks ours from `tiles` away: nearer bracket, or the
// same bracket and an earlier slot. These are the claims that should keep us off a solo camp.
unsigned ownersMask(int groupId, int mySlot, int tiles);
// The ally in `slot` is out working (claims a camp, an enemy or an alert): not banking, healing,
// channelling or dead (those clear the claim).
bool active(int slot);
// Camp group the ally in `slot` claims (-1: none, or not a camp claim) and its distance bracket.
int claimOf(int slot);
int claimBracket(int slot);
// Bit per allied slot (excluding exceptSlot) attacking this enemy champion.
unsigned focusMask(int enemyIndex, int exceptSlot);

// ---- capture board (byte 5, only for teams of at most 5 bodies, where it is not a claim byte)
// The champion that completes a capture writes the group there (7 bits, +1) with a bit that flips
// on every write, so a repeat capture of the same group is still news. Teammates far away learn
// that the group now respawns, which vision cannot tell them.
inline constexpr int capture_byte = 5;
void announceCapture(int groupId);
// The group captured since our last call (-1: nothing new). Call once per turn after refresh().
int newCapture();

// ---- team alert (bytes 6-7): an enemy seen on our half, and who saw it
inline constexpr int alert_byte = 6;
struct Alert {
    bool fresh = false;   // written this round or last
    int x = 0, y = 0;     // spotter's tile (to 2 tiles)
    int enemies = 0;      // 1, or 2 meaning two or more
    bool squishy = false; // the spotter is fragile (not a tank, or hurt): call harder
};
// Read the alert; clears it when stale. Only valid when the team has at most 6 bodies.
Alert readAlert(int round);
void raiseAlert(int x, int y, int enemies, bool squishy, int round);

} // namespace comms
