#pragma once
// Pathfinding and movement. Distances for scoring come from one terrain-only BFS
// rooted at our tile (cheap: one host call answers "how far is every camp and
// altar"); actual steps use a goal-rooted field that also avoids visible units.
#include <cambc.h>
#include <vector>
#include "../core/world.h"

// loopBot: a precomputed terrain field to the walk's goal (fields::toGroup / toAltar / toSpawn).
// Far from the goal, the step comes from this field instead of a fresh search: the same tiles
// every lap (set paths), a local read instead of a BFS, and dashes that hop holes the walking
// path has to go round. The search still handles the last few tiles and anything blocked.
struct FarField {
    int (*read)(int, cambc::vec2) = nullptr;
    void (*ensure)(int) = nullptr;
    int id = 0;
};

class Navigator {
public:
    static constexpr int unreachable = 1 << 14;

    void beginTurn() { fromMeValid_ = false; }

    // Terrain walking distance from our current tile (cached per position).
    int distTo(const World& w, cambc::vec2 tile);
    // Distance to the closest tile from which `target` is within `range` (e.g. altar ring, attack range).
    int distToRange(const World& w, cambc::vec2 target, int range);

    // Tiles that cannot be entered this turn: terrain plus visible units (our own tile excluded).
    cambc::bitboard blocked(const World& w) const;
    // Free tiles within `range` of `target`.
    cambc::bitboard standTiles(const World& w, cambc::vec2 target, int range) const;

    // Walk (and optionally dash) toward the nearest free goal tile. Returns true if we moved.
    // `avoid` tiles (e.g. other monsters' reach) are treated as walls when a route around them exists.
    // `aim` breaks ties between equally short steps: diagonal moves cost the same as straight
    // ones, so without it we drifted along whichever row came first instead of heading at the goal.
    bool stepToward(World& w, const cambc::bitboard& goals, bool allowDash, const cambc::bitboard* avoid = nullptr,
                    cambc::vec2 aim = {-1, -1}, FarField far = {});
    bool stepToward(World& w, cambc::vec2 target, int range, bool allowDash, const cambc::bitboard* avoid = nullptr);
    // Sealed pockets (dash-in groups): walk to the group's dash tile and dash in, or, from inside,
    // walk to the landing tile and dash back out. Returns true if we moved (or are waiting to dash).
    bool enterPocket(World& w, const GroupInfo& g);
    bool leavePocket(World& w, const GroupInfo& g);
    // A dash ability ready now: its slot, else -1.
    int readyDash(World& w) const;
    // Step to the neighbouring tile that maximises distance from the given threats.
    bool stepAway(World& w, const std::vector<cambc::vec2>& threats, int keepRange);
    // combatBotv3: step to an adjacent tile in keepIn that is clearly less exposed (exposure map); false if none.
    bool saferStep(World& w, const cambc::bitboard& keepIn);
    // combatBotv3: dashers heading for `target` across dashable holes (Statics::dashLine): walk to
    // the tile before the first hole and dash over it. False when it does not apply.
    bool dashAcross(World& w, cambc::vec2 target);

    // Turns of walking needed to leave every visible monster's and enemy's reach (0 if safe now).
    int escapeTurns(const World& w);
    // Walk (and dash) along the shortest route out of reach. Returns true if we moved.
    bool stepEscape(World& w);
    // Flee home: step (or dash) to the tile nearest our spawn along the spawn field, each visible
    // enemy whose reach covers a tile (and awake monsters' reach) costing tune::flee_reach_cost.
    // False when the spawn field is not ready or no tile improves on standing still.
    bool stepHome(World& w);

private:
    bool tryMove(World& w, cambc::vec2 dest);
    // One step (or hop) down a precomputed field; false when it found nothing safe and useful.
    bool stepByField(World& w, const FarField& far, const cambc::bitboard& blockedNow, const cambc::bitboard* avoid,
                     bool allowDash, cambc::vec2 aim);
    // `visited` marks tiles the search actually reached: unreached tiles read as distance 0.
    bool tryDash(World& w, const cambc::distance_field& field, const cambc::bitboard& visited,
                 const cambc::bitboard& blockedNow);

    void computeEscape(const World& w);

    cambc::distance_field escape_;
    cambc::bitboard escapeVisited_;
    cambc::vec2 escapePos_{-1, -1};
    int escapeRound_ = -1;
    bool escapeNeeded_ = false;

    cambc::distance_field fromMe_;
    cambc::bitboard fromMeVisited_;
    cambc::vec2 fromMePos_{-1, -1};
    bool fromMeValid_ = false;
};

Navigator& nav();
