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
#include "profile.h"
#include "config.h"
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
    int index = -1;           // position in the enemy roster (shared focus-fire id)
    cambc::vec2 pos;
    float hp = 0, maxHp = 1, strength = 0, focus = 0;
    int gems = 0, range = 1;
    bool channeling = false;  // recalling / tributing: a stationary, interruptible target
    bool tributing = false;
    bool stun = false;        // has a crowd-control ability (Tavi, Ren, Cassi, ...)
    bool stunned = false;     // stunned right now (read within tune::stun_check_radius)
};

// Where an enemy was last seen; vision is not shared, so remembering recent sightings
// is the only way to avoid walking back into a group that just left our view.
struct EnemyGhost {
    cambc::champion handle;
    cambc::vec2 pos;
    int round = 0;
    float dps = 0;
    int range = 1;
    bool stun = false;        // has a crowd-control ability: can cancel our tribute
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
    bool hasReset = false, splash = false, hasHeal = false, goldBonus = false;
    float spellModifiers = 0;  // sum of damage-spell focus multipliers
    float spellDps = 0;        // average damage per turn from damage spells over their cooldown
    float dashRange = 0;       // tiles a dash ability covers (0 = no dash)
};

class World {
public:
    // ---- identity (fixed after the first turn)
    int slot = 0;                  // index among allied champions, used for shared-memory claims
    cambc::champion_type type{};
    bool ranged = false;
    Kit kit;
    std::array<cambc::champion_type, 8> allyTypes{};  // champion in each allied slot
    int allyCount = 0;
    const Tuning& role() const { return tuning(type); }
    // Another allied slot in `mask` (bit per slot) is a tank that will lead the pull.
    bool tankAmong(unsigned mask) const;
    // An ally is the monsters' target in a group already in a fight, and stays the target once
    // we stand at our attack range: it is nearer, or as near and drafted earlier (monsters hit
    // the nearest champion each round, ties to the earliest draft slot). Then we can (re)join
    // without taking the hits.
    bool allyTanking(int groupId) const;

    // Our side of a fight around `at`: us plus visible allies within `radius` of it or already
    // attacking enemy `enemyIndex` (shared focus), with their live health and damage.
    struct Side { int count = 0; float hp = 0, dps = 0; };
    Side ourSide(cambc::vec2 at, int radius, int enemyIndex, float myDps) const;

    // ---- this turn
    int round = 0;
    cambc::vec2 pos;
    float hp = 0, maxHp = 1, strength = 0, focus = 0;
    int gems = 0, gold = 0, range = 1;
    bool alive = false, atSpawn = false;
    int teamGems = 0, enemyGems = 0;
    // Enemy deaths are visible out of vision (champion::is_alive). Bodies: Cassi & Issac are two.
    int enemyBodies = 0, enemiesAlive = 0;
    int alliesAlive = 0;           // our bodies alive (us included), checked on the same cadence
    // Invading (farming, fighting or banking on their half) only with a numbers lead: more of us
    // alive than of them (tune::invade_needs_lead).
    bool mayInvade() const { return !tune::invade_needs_lead || alliesAlive > enemiesAlive; }
    // Enemy (roster index) seen last round further from us than now... reversed: it is moving away
    // from us (its distance to our tile grew since we last saw it, within 2 rounds).
    bool enemyMovingAway(int index, cambc::vec2 now) const;
    // The enemy got nearer to us since we last saw it (within 2 rounds).
    bool enemyApproaching(int index, cambc::vec2 now) const;
    // Seen within the last two rounds and now nearer `target` than it was (combatBotv2).
    bool enemyClosingOn(int index, cambc::vec2 now, cambc::vec2 target) const;
    // Gems-only mode: from tune::gems_only_round, while the enemy has banked tune::gems_only_lag more gems.
    bool gemsOnly() const {
        return round >= tune::gems_only_round &&
               (enemyGems - teamGems >= tune::gems_only_lag || (tune::gems_only_when_none && teamGems == 0));
    }
    // The tile is on their half (nearer their spawn than ours).
    bool theirHalf(cambc::vec2 p) const;
    int enemyRespawnAt = -1;       // round the first dead enemy comes back (-1: none dead)
    cambc::bitboard visible, occupied;
    cambc::bitboard danger;        // tiles within reach of live, damaging monsters we can see
    bool reserveDash = false;
    // combatBotv3: which allies are in our vision this round (bit per slot). Reading an ally out of
    // sight throws "not in vision", and a thrown error costs ~2-3k fuel: every per-ally read asks this first.
    mutable unsigned allyVisMask_ = 0;
    mutable int allyVisRound_ = -1;
    mutable std::array<cambc::vec2, 8> allyPosCache_{};
    mutable int allyPosRound_ = -1;     // combatBotv2: outnumbered here, so no dash on ordinary moves this turn
    int engagedGroup = -1;         // awake group whose monsters can hit us right now (closest)
    float incoming = 0;            // monster strength able to hit our tile this turn (awake groups)
    bool beingHit = false;         // we lost health since our previous turn
    std::vector<EnemySeen> enemies;
    std::vector<AllySeen> allies;
    std::vector<CampSeen> camps;
    std::vector<EnemyGhost> ghosts;  // enemies seen recently (including this turn)
    // Team target: the first enemy we saw losing health (an ally or we hit it). Everyone focuses
    // it while it stays alive and in sight (enemy roster index, -1 none).
    int teamTarget = -1, teamTargetSeen = -1;

    // ---- memory across turns
    std::vector<GroupMemory> groups;
    int lastOption = -1;           // Option::Kind of last turn's choice
    int lastTarget = -1;           // group/altar id of last turn's choice
    int turnsOnOption = 0;
    int waitingTurns = 0;          // consecutive turns spent waiting for a party
    // Stuck watchdog: turns standing on one tile while idle or holding for a party; when it trips,
    // the camp we held is dropped until stuckUntil and we hunt enemies until huntUntil.
    cambc::vec2 stillPos{-1, -1};
    int stillTurns = 0;
    int stuckGroup = -1, stuckUntil = -1, huntUntil = -1;
    bool hunting() const { return round < huntUntil; }
    int captureTurns = 0;          // consecutive turns standing on the current capture
    bool hasTributed = false;      // we have banked at least once this game (opening shop rule)
    int prevGems = 0;
    cambc::vec2 lastAttacked{-1, -1};  // camp we hit last turn: keep hitting it until it dies
    // Recent decisions (kind, target; newest first) for hysteresis and flip detection.
    std::array<std::pair<int, std::string>, 4> history{};
    std::pair<int, std::string> lock{-1, ""};
    int lockTurns = 0;
    int partyGroup = -1, partyReadyUntil = -1;  // once a party is judged ready, engage for a while
    std::array<cambc::vec2, 6> posHistory{};     // our tile after each turn, newest first
    std::pair<int, std::string> stuck{-1, ""};   // decision that left us bouncing between two tiles
    int stuckTurns = 0;
    bool forceReplan = false;
    // Party rotation: allies (bit per slot) who worked the party camp we are on, and that camp.
    unsigned partyMates = 0;
    int partyCamp = -1;
    // Chaining: the last camp we worked that died, and when (camps near it get a bonus for a while).
    int chainFrom = -1, chainRound = -1000;
    float prevHp = -1;
    int prevHpRound = -1;
    // combatBotv2: health lost per round while monsters are on us (smoothed; 0 when not engaged).
    float hitRate = 0;
    std::vector<std::string> pendingNotes;  // notes from refresh(), traced once the turn's trace starts

    void refresh(bool firstTurn);
    float hpFrac() const { return hp / (maxHp > 0 ? maxHp : 1); }
    // Turns to walk `tiles`: one step per turn, plus a dash every cooldown for champions with one
    // (about 10 tiles per 8 turns instead of 8).
    float walkTurns(float tiles) const {
        return tiles / (1.0f + kit.dashRange / static_cast<float>(tune::spell_cooldown));
    }
    // Local bit test on this turn's exported vision (bitboard calls cost ~30-300 fuel each).
    bool isVisible(cambc::vec2 t) const;
    bool campsInRangeOf(cambc::vec2 tile, int range) const;
    // Enemy champion damage per turn that could be on `at` soon (visible + recently seen, decayed).
    float enemyPressure(cambc::vec2 at, int radius) const;
    // Our side's damage per turn around `at`: us plus visible allies within `radius`.
    float alliedPower(cambc::vec2 at, int radius) const;
    int alliesWithin(cambc::vec2 at, int radius) const;
    // A tribute of `gems` started now at `altar` would be cancelled: an enemy stunner can get within
    // stun range before it finishes, or the enemies near enough can kill us first. Damage alone
    // does not interrupt a tribute.
    bool tributeAtRisk(cambc::vec2 altar, int gems) const;
    // Tiles enemies can hit soon (their reach + 2), for pathing while carrying gems.
    cambc::bitboard enemyReach() const;
    float incomingCampDamage(cambc::vec2 tile) const;
    const CampSeen* campAt(cambc::vec2 tile) const;
    // Danger zones except the given group's (we accept its hits while fighting it).
    cambc::bitboard dangerExcept(int groupId) const;

    // Abilities, fetched once per bot and readiness once per turn (is_ready costs ~600 fuel).
    const std::array<cambc::ability, 2>& abilities() const { return *abilities_; }
    bool abilityReady(int slot);
    // An ally's health fraction by draft slot (a host read; -1 when unknown or dead).
    float allyHpFrac(int slot) const;
    // An ally's health lost (max - current) by draft slot (0 when unknown or dead).
    float allyMissingHp(int slot) const;
    // An ally's current health by draft slot (0 when unknown or dead).
    float allyHp(int slot) const;
    // combatBotv2: an ally's damage per turn on a camp group (strength with splash, plus its spells
    // from focus), read once per turn; 0 when unknown or dead.
    float allyCampDps(int slot, const GroupInfo& g) const;
    bool allyVisible(int slot) const;  // combatBotv3: in our vision this round (reads would not throw)
    // combatBotv3: Cassi or Issac (their deaths take the other one too).
    bool pairChampion() const { return type == cambc::champion_type::cassi || type == cambc::champion_type::issac; }
    // An ally's tile by draft slot ({-1,-1} when unknown or dead).
    cambc::vec2 allyPos(int slot) const;
    // Same, read once per turn (two host calls per ally otherwise; combatBotv2's checks ask often).
    cambc::vec2 allyPosCached(int slot) const;
    // An ally's carried gems by draft slot (-1 when unknown or dead).
    int allyGems(int slot) const;
    void spentAbility(int slot) { ready_[slot] = 0; }

private:
    void loadNearbyCamps();
    void updateGroups();
    std::vector<std::uint8_t> visibleBits_, campBits_;
    std::vector<cambc::champion> allyHandles_;  // by draft slot
    std::vector<cambc::champion> enemyHandles_; // enemy roster order: the shared focus-fire id
    struct AllyHp { int round = -1; float hp = 0, maxHp = 1, strength = 0; bool alive = false; };
    mutable std::array<AllyHp, 8> allyHpCache_{};
    void readAllyHp(int s) const;
    std::array<signed char, 8> enemyStun_{-1, -1, -1, -1, -1, -1, -1, -1};  // per roster index: has cc (-1 unknown)
    std::array<int, 8> enemyDeadSince_{-1, -1, -1, -1, -1, -1, -1, -1};  // round we first saw each one dead
    std::array<std::uint8_t, 8> allyAlive_{1, 1, 1, 1, 1, 1, 1, 1};
    std::array<cambc::vec2, 8> enemyPrevPos_{};          // where each enemy stood when last seen
    std::array<int, 8> enemyPrevRound_{-1, -1, -1, -1, -1, -1, -1, -1};
    std::array<cambc::vec2, 8> lastSeenPos_{};
    std::array<int, 8> lastSeenRound_{-1, -1, -1, -1, -1, -1, -1, -1};
    std::array<float, 8> enemyHpSeen_{-1, -1, -1, -1, -1, -1, -1, -1};   // health when last seen
    std::array<int, 8> enemyHpRound_{-1, -1, -1, -1, -1, -1, -1, -1};    // round of that sighting
    std::optional<std::array<cambc::ability, 2>> abilities_;
    std::array<signed char, 2> ready_{-1, -1};  // -1 unknown this turn, else 0/1
};
