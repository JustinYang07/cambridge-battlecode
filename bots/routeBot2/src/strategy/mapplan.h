#pragma once
// Whole-map planning done once in the draft, handed to champions through draft memory.
//
// Strategy: clear our own half on a fixed, looping path until items are bought, then contest.
// The draft sees the whole map and gets 10x fuel on its first turn, so it does the expensive
// work there:
//   1. analyse(): distance fields from both spawns (and, fuel permitting, every camp group),
//      giving walking distances and the map features the draft and the centre decision use.
//      Nothing is keyed to a map name.
//   2. prepare(): split our half into a deep lane (the far camps plus every camp that needs a
//      party) and a base lane (the near ones), each ordered into a clear path by walking
//      distance. The deep lane's share of the work is the duo's share of our bodies.
//   3. planFor(): the duo (a tank plus a damage dealer) takes the deep lane together; the
//      remaining champions (weaker early fighters, Cassi & Issac as one unit) split the base
//      lane into contiguous stretches of equal work.
//   4. write(): lanes (camp group ids), each group's depth toward the enemy and the
//      centre-or-farm flag go to draft memory. Distance fields do not fit in the 1,024 bytes,
//      so champions recompute their own (nav/fields.cpp).
//
// In the game, follow() walks this champion's loop: the next camp that is up (or back by the
// time we arrive) gets a bonus in farm scoring. When every camp on the loop is down
// ("downtime"), the champion banks if it carries gems, or heads for the middle to farm and
// pick fights instead of standing at a respawning camp. The loop holds until the champion has
// bought tune::contest_shards shards; then it ends and the brain roams and contests. The
// option brain stays in charge throughout: enemies close by are fought or fled, low health heals.
#include <array>
#include <cambc.h>
#include <cstdint>
#include <string>
#include <vector>

class World;

namespace mapplan {

inline constexpr int kMaxSteps = 24;    // camps per lane
inline constexpr int kMaxGroups = 254;  // group ids must fit a step byte

struct Features {
    float spawnToSpawn = 0;     // walking distance between the spawns
    float incomeRate = 0;       // sum over farmable groups of (gems + lambda gold) / respawn
    float centreShare = 0;      // share of incomeRate in centre groups
    float centreOurDist = 0, centreEnemyDist = 0;  // mean walking distance to centre groups
    bool centreAltar = false;   // an altar sits in the centre band
    int centreGroups = 0;
    float harmlessShare = 0;    // value share in gem/gold clusters
    float goldShare = 0;        // share of value that is gold (lambda-weighted)
    float splashShare = 0;      // value share in groups with adjacent members (Varg's splash)
    float partyShare = 0;       // value share in groups one champion cannot solo safely
};

struct Route {
    cambc::champion_type type{};
    int lane = 0;               // 0: the deep duo; 1+: base lanes. Same lane = clear together.
    int len = 0;
    std::array<std::uint8_t, kMaxSteps> steps{};
};

struct Plan {
    bool valid = false;          // analysis finished; otherwise champions use the brain alone
    bool contestCentre = false;
    std::vector<std::uint8_t> centre;  // per group: 1 if in the centre band
    std::vector<std::uint8_t> depth;   // per group: 0 at our spawn .. 255 at theirs (128 = halfway)
    std::vector<Route> routes;
};

// ---- draft side
// Map features from the spawn fields: runs once (fits the first draft turn's 10x fuel).
// Returns false if it failed; picks then fall back to the tested roster.
bool analyse();
// Build more per-group distance fields while a whole field fits above `reserve` fuel.
// Lanes estimate the distances still missing, so this can stop at any point.
void buildFields(unsigned long long reserve);
bool analysed();
// Split our half into lanes and order each into a clear path. Call once fields are done, on
// the first draft turn; ordering falls back to distance from spawn below `floor` fuel.
void prepare(unsigned long long floor);
const Features& features();
// Feature bonus for drafting `pick` (0 when the analysis is missing), in roster-rank units.
float draftBonus(cambc::champion_draft pick, int teamSize);
// Champion bodies a roster of picks gives (Cassi & Issac are two).
std::vector<cambc::champion_type> bodies(const std::vector<cambc::champion_draft>& picks);
// Assign the lanes to this roster (cheap: reads what prepare() built).
Plan planFor(const std::vector<cambc::champion_type>& roster, int teamSize);
void write(const Plan& p);
// Summary for the draft trace: features, decision, lanes and fuel.
std::string describe(const Plan& p);

// ---- gameplay side
const Plan& read();                    // decoded from the draft memory snapshot (cached)
// Pick this turn's camp on our loop, or end the loop once we have scaled. Call once per turn while alive.
void follow(World& w);
// Camp group our loop wants now: the loop's next camp that is up, else (loop down) the nearest
// unclaimed camp up on our half; -1 when there is no loop, it is over, our half is down, or suspended.
int routeGroup();
// Still on the clear loop (not yet scaled): region splitting is off, the loop decides where we farm.
bool clearing();
// Every camp on our half is down: bank or head for the middle (healthy) instead of waiting.
bool downtime();
// Farm score multiplier for a camp past halfway to the enemy: strong while clearing, mild once
// scaled or in downtime.
float enemyHalfFactor(int groupId);
// Walkable tile in the middle of the map (between the spawns), for downtime roaming.
cambc::vec2 middle();
bool isCentre(int groupId);
std::string routeText();

} // namespace mapplan
