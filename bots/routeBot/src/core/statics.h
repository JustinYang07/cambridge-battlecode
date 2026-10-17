#pragma once
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
    bool reachable = true;            // some tile is connected to our spawn on foot (false: sealed off, e.g. behind holes)
    // Sealed groups a dash can get into: the pocket around them (walkable inside), and one legal dash
    // from dashFrom (reachable on foot) to dashTo (inside the pocket). The way out is the reverse.
    bool dashIn = false;
    cambc::bitboard pocket;
    cambc::vec2 dashFrom{-1, -1}, dashTo{-1, -1};
    int campCount = 0;
    int adjacentPairs = 0;            // members within 1 tile of each other (splash value)
    std::string label;                // e.g. "wolf+wolf@(8,2)"
};

struct Statics {
    int width = 0, height = 0;
    cambc::bitboard terrain;          // walls | holes | altars | enemy spawn: never walkable
    cambc::bitboard reach;            // tiles walkable from our spawn (all set if the fill failed)
    cambc::bitboard allySpawn, enemySpawn;
    cambc::vec2 spawnCenter;
    cambc::vec2 enemySpawnCenter;
    std::vector<cambc::vec2> altars;
    std::vector<GroupInfo> groups;
    int teamSize = 0;

    static Statics& get();            // lazily initialised singleton
    const GroupInfo* groupAt(cambc::vec2 tile) const;
    // A dash of up to 2 tiles may pass over holes but not walls (probed on ambush: the engine
    // refused every dash across a wall row, allowed one over a hole).
    bool dashLegal(cambc::vec2 from, cambc::vec2 to) const;
    // The dash-in group whose pocket holds this tile, if any.
    const GroupInfo* pocketAt(cambc::vec2 tile) const;
    cambc::bitboard dashBlock;        // tiles a dash cannot pass over: terrain except holes
    // Per camp spawn tile: owning group and base stats (camps never leave their spawn).
    struct TileCamp {
        int groupId = -1;
        float health = 0, strength = 0;
        std::optional<cambc::camp> handle;  // from map::camp_groups(); live reads cost ~50 fuel
    };
    const TileCamp* campAt(cambc::vec2 tile) const;

private:
    void init();
    std::vector<TileCamp> tileCamps_;  // width*height, groupId -1 where there is no camp
    bool ready_ = false;
};

inline int dist(cambc::vec2 a, cambc::vec2 b) { return static_cast<int>(a.chebyshev_distance(b)); }
std::string fmt(cambc::vec2 p);
// Integer to string without std::to_string, which goes through snprintf (~3k fuel per call).
std::string num(long long v);
