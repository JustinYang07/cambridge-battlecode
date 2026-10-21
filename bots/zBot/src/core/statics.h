#pragma once
#include <cstdint>
// Map facts that never change during a match, computed once on the first turn
// (which has a doubled fuel budget) and reused by every module.
#include <cambc.h>
#include "rawbuf.h"
#include <array>
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
    int maxRange = 0;                 // longest attack range of a damaging member (0: harmless)
    float totalHealth = 0;            // sum of members' base health
    float totalStrength = 0;          // sum of members' base strength (damage per round when awake)
    bool harmless = false;            // only gem/gold clusters
    bool hasMonolith = false;
    bool blocksAltars = false;        // stands on the last free tiles beside the altars (see Statics::altarBlockers)
    bool reachable = true;            // some tile is connected to our spawn on foot (false: sealed off, e.g. behind holes)
    // Sealed groups a dash can get into: the pocket around them (walkable inside), and one legal dash
    // from dashFrom (reachable on foot) to dashTo (inside the pocket). The way out is the reverse.
    bool dashIn = false;
    std::optional<cambc::bitboard> pocket;  // dash-in groups only (a bitboard per group cost ~150 fuel to build)
    bool inPocket(cambc::vec2 p) const { return pocket && pocket->is_set(p); }
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
    // Camp groups standing on every free tile within tribute range of every usable altar (boss-battle:
    // the only altar is walled in on seven sides and a Monolith stands on the eighth). Until one of
    // them dies nobody can bank. Empty when some altar has a free tile.
    std::vector<int> altarBlockers;
    std::vector<GroupInfo> groups;
    int teamSize = 0;

    static Statics& get();            // lazily initialised singleton
    const GroupInfo* groupAt(cambc::vec2 tile) const;
    // A dash of up to 2 tiles may pass over holes but not walls (probed on ambush: the engine
    // refused every dash across a wall row, allowed one over a hole).
    bool dashLegal(cambc::vec2 from, cambc::vec2 to) const;
    // combatBotv3: the straight line from a to b for a dasher. -1: a wall (or a hole run wider than
    // one tile) is on it; 0: plain walk; 1: it crosses holes a 2-tile dash hops, the first hop from
    // *hopFrom to *hopTo. Reads byte tables built once (bitboard reads cost ~30-300 fuel each).
    int dashLine(cambc::vec2 a, cambc::vec2 b, cambc::vec2* hopFrom = nullptr, cambc::vec2* hopTo = nullptr) const;
    // The dash-in group whose pocket holds this tile, if any.
    const GroupInfo* pocketAt(cambc::vec2 tile) const;
    cambc::bitboard dashBlock;        // tiles a dash cannot pass over: terrain except holes
    // Per camp spawn tile: owning group and base stats (camps never leave their spawn).
    struct TileCamp {
        int groupId = -1;
        float health = 0, strength = 0;
        int range = 3;                      // attack range (patch: elk 4, golem 4, monolith 5)
        std::optional<cambc::camp> handle;  // from map::camp_groups(); live reads cost ~50 fuel
    };
    const TileCamp* campAt(cambc::vec2 tile) const;
    // A camp spawn tile of group g: a byte read (bitboard reads cost ~30-300 fuel each), or the
    // group's mask on maps with more camps than the byte index holds.
    bool onGroup(const GroupInfo& g, cambc::vec2 tile) const {
        if (campsCapped_) return g.tileMask.is_set(tile);
        const auto* c = campAt(tile);
        return c && c->groupId == g.id;
    }
    // Draft: append the packed camp facts (and their offset in the last two bytes) to the plan
    // bytes, padding to the full 1024. Champions read it back in init().
    void appendPacked(std::vector<std::uint8_t>& bytes) const;
    bool usedDraftBlock = false;      // champion: camp facts came from draft memory

    static constexpr std::uint8_t kPackMagic = 0x5D, kPackVersion = 3;

private:
    std::vector<std::uint8_t> packed_;               // draft: the block encode() appends
    void init();
    std::vector<TileCamp> tileCamps_;            // one per camp
    RawBuf<std::uint8_t> tileCampIndex_;    // width*height: index into tileCamps_ + 1, 0 where there is no camp
                                                 // (a dense TileCamp table cost ~60k fuel to fill on the first turn)
    bool ready_ = false;
    bool campsCapped_ = false;
    mutable std::vector<std::uint8_t> wallBits_, holeBits_;  // dashLine tables (packed bits)                   // more than 255 camps: some are missing from tileCampIndex_
};

inline int dist(cambc::vec2 a, cambc::vec2 b) { return static_cast<int>(a.chebyshev_distance(b)); }
std::string fmt(cambc::vec2 p);
// Integer to string without std::to_string, which goes through snprintf (~3k fuel per call).
std::string num(long long v);
