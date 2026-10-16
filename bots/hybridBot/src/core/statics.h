#pragma once
#include <cstdint>
// Map facts that never change during a match, computed once on the first turn
// (which has a doubled fuel budget) and reused by every module.
#include <cambc.h>
#include <optional>
#include <string>
#include <vector>

struct CampMember {
    float health = 0, strength = 0;
};

struct GroupInfo {
    int id = 0;                       // index into cambc::map::camp_groups(); identical for all allies
    std::optional<cambc::map::camp_group> handle; // camp_group has no default constructor
    std::vector<cambc::vec2> tiles;   // original camp spawn tiles
    std::vector<CampMember> killOrder; // members sorted the way we fight them: hardest hitters first
    cambc::bitboard tileMask;
    cambc::bitboard reach;            // tiles within monster attack range of any damaging member
    cambc::vec2 center;
    int gems = 0, gold = 0, respawn = 0;
    float totalHealth = 0;            // sum of members' base health
    float totalStrength = 0;          // sum of members' base strength (damage per round when awake)
    bool harmless = false;            // only gem/gold clusters
    bool hasMonolith = false;
    int campCount = 0;
    int adjacentPairs = 0;            // members within 1 tile of each other (splash value)
    std::string label;                // e.g. "wolf+wolf@(8,2)"
};

struct Statics {
    int width = 0, height = 0;
    cambc::bitboard terrain;          // walls | holes | altars | enemy spawn: never walkable
    cambc::bitboard allySpawn, enemySpawn;
    cambc::vec2 spawnCenter;
    cambc::vec2 enemySpawnCenter;
    std::vector<cambc::vec2> altars;
    std::vector<GroupInfo> groups;
    int teamSize = 0;

    static Statics& get();            // lazily initialised singleton
    const GroupInfo* groupAt(cambc::vec2 tile) const;
    // Per camp spawn tile: owning group and base stats (camps never leave their spawn).
    struct TileCamp {
        int groupId = -1;
        float health = 0, strength = 0;
        std::optional<cambc::camp> handle;  // from map::camp_groups(); live reads cost ~50 fuel
    };
    const TileCamp* campAt(cambc::vec2 tile) const;

private:
    void init();
    // Per tile, an index into camps_ (-1: no camp). A full TileCamp per tile cost ~66k fuel to
    // build on the first turn (it holds a camp handle); only the ~30 camp tiles need one.
    std::vector<std::int16_t> tileIndex_;
    std::vector<TileCamp> camps_;
    bool ready_ = false;
};

inline int dist(cambc::vec2 a, cambc::vec2 b) { return static_cast<int>(a.chebyshev_distance(b)); }
std::string fmt(cambc::vec2 p);
// Integer to string without std::to_string, which goes through snprintf (~3k fuel per call).
std::string num(long long v);
