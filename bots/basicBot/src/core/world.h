#pragma once
// Per-turn snapshot of what this champion can observe, plus memory that
// persists between its turns (camp timers, what we were doing last turn).
//
// Fuel: roughly 1 fuel per WASM instruction and 100k per turn. Bulk bitboard
// queries (champion_tiles, camp_tiles) are cheap; per-object live queries
// (vision::champions, camp health) cost hundreds to thousands each, so they are
// only made when something is close enough to matter this turn.
#include <array>
#include <cambc.h>
#include <optional>
#include <vector>
#include "statics.h"

enum class GroupStatus : std::uint8_t { unknown, alive, reward, respawning };
const char* to_string(GroupStatus s);

struct GroupMemory {
    GroupStatus status = GroupStatus::unknown;
    int respawnAt = 0;       // round the group is expected back (respawning only)
    int seenRound = -1;      // last round any of its tiles was in our vision
    float healthLeft = 1.0f; // fraction of total health remaining when last measured
    int enemyNearRound = -1; // last round an enemy champion was seen near it
    bool visibleNow = false; // any of its tiles is in vision this turn
};

struct EnemySeen {
    cambc::champion handle;
    cambc::vec2 pos;
    float hp = 0, maxHp = 1, strength = 0, focus = 0;
    int gems = 0, range = 1;
    bool channeling = false;  // recalling / tributing: a stationary, interruptible target
};

struct AllySeen {
    cambc::vec2 pos;
    std::optional<cambc::champion> handle;  // loaded only when we can heal
    float hp = 0, maxHp = 1;
};

struct CampSeen {
    cambc::vec2 pos;
    int groupId = -1;
    float hp = 0, maxHp = 1, strength = 0;
    bool hpKnown = false;                   // live health read this turn (otherwise estimated)
    std::optional<cambc::camp> handle;      // loaded for camps near us (needed to attack)
};

// Our damage profile. The ability metadata is fixed; spell damage scales with focus.
struct Kit {
    bool hasReset = false, splash = false, hasHeal = false;
    float spellModifiers = 0;  // sum of damage-spell focus multipliers
    float spellDps = 0;        // average damage per turn from damage spells over their cooldown
};

class World {
public:
    // ---- identity (fixed after the first turn)
    int slot = 0;                  // index among allied champions, used for shared-memory claims
    cambc::champion_type type{};
    bool ranged = false;
    Kit kit;

    // ---- this turn
    int round = 0;
    cambc::vec2 pos;
    float hp = 0, maxHp = 1, strength = 0, focus = 0;
    int gems = 0, gold = 0, range = 1;
    bool alive = false, atSpawn = false;
    int teamGems = 0, enemyGems = 0;
    cambc::bitboard visible, occupied;
    cambc::bitboard danger;        // tiles within reach of live, damaging monsters we can see
    int engagedGroup = -1;         // awake group whose monsters can hit us right now (closest)
    float incoming = 0;            // monster strength able to hit our tile this turn (awake groups)
    bool beingHit = false;         // we lost health since our previous turn
    std::vector<EnemySeen> enemies;
    std::vector<AllySeen> allies;
    std::vector<CampSeen> camps;

    // ---- memory across turns
    std::vector<GroupMemory> groups;
    int lastOption = -1;           // Option::Kind of last turn's choice
    int lastTarget = -1;           // group/altar id of last turn's choice
    int turnsOnOption = 0;
    int waitingTurns = 0;          // consecutive turns spent waiting for a party
    float prevHp = -1;

    void refresh(bool firstTurn);
    float hpFrac() const { return hp / (maxHp > 0 ? maxHp : 1); }
    // Local bit test on this turn's exported vision (bitboard calls cost ~30-300 fuel each).
    bool isVisible(cambc::vec2 t) const;
    bool campsInRangeOf(cambc::vec2 tile, int range) const;
    float incomingCampDamage(cambc::vec2 tile) const;
    const CampSeen* campAt(cambc::vec2 tile) const;
    // Danger zones except the given group's (we accept its hits while fighting it).
    cambc::bitboard dangerExcept(int groupId) const;

    // Abilities, fetched once per bot and readiness once per turn (is_ready costs ~600 fuel).
    const std::array<cambc::ability, 2>& abilities() const { return *abilities_; }
    bool abilityReady(int slot);
    void spentAbility(int slot) { ready_[slot] = 0; }

private:
    void loadNearbyCamps();
    void updateGroups();
    std::vector<std::uint8_t> visibleBits_, campBits_;
    std::optional<std::array<cambc::ability, 2>> abilities_;
    std::array<signed char, 2> ready_{-1, -1};  // -1 unknown this turn, else 0/1
};
