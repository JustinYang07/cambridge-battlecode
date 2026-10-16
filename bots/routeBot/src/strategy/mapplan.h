#pragma once
// Whole-map planning done once in the draft, handed to champions through draft memory.
//
// The draft sees the whole map and gets 10x fuel on its first turn, so it does the
// expensive work there (routing plan, layers R1/R3/R4):
//   1. analyse(): distance fields from both spawns and every camp group, giving walking
//      distances spawn -> group, group -> group and group -> nearest altar, plus the map
//      features the draft and the centre decision use. Nothing is keyed to a map name.
//   2. planRoutes(): score every champion-camp pair by value per turn minus risk, assign
//      greedily (best pair first, so no two champions split one capture), then extend each
//      route 2 more steps by lookahead, inserting an altar visit when the bag gets full.
//   3. write(): routes (as camp group ids) + the centre-or-farm flag go to draft memory.
// Distance fields do not fit in the 1,024 bytes, so champions recompute their own
// (nav/fields.cpp); only routes and flags cross over.
//
// In the game, follow() walks this champion's route: the current step's camp gets a bonus
// in farm scoring, and the route ends when an enemy shows up near it, health runs low or
// the opening is over. The option brain stays in charge throughout (fights, heals, banking).
#include <array>
#include <cambc.h>
#include <cstdint>
#include <string>
#include <vector>

class World;

namespace mapplan {

inline constexpr int kMaxSteps = 4;                // up to 3 camps plus one altar visit
inline constexpr std::uint8_t kAltarStep = 254;    // route step: bank at the nearest altar
inline constexpr int kMaxGroups = 254;             // group ids must fit a step byte

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
    int len = 0;
    std::array<std::uint8_t, kMaxSteps> steps{};
};

// A farming loop (after the opening): a cycle over camp groups that a party of `bodies` champions
// repeats all game, solved in the draft as cyclic orienteering (gems per round, where a lap shorter
// than a camp's respawn gains nothing).
inline constexpr int kMaxLoops = 3;
inline constexpr int kMaxLoopLen = 16;
struct Loop {
    int bodies = 0;
    std::vector<std::uint8_t> camps;  // group ids in visiting order
};

struct Plan {
    bool valid = false;          // analysis finished; otherwise champions use the brain alone
    bool contestCentre = false;
    std::vector<std::uint8_t> centre;  // per group: 1 if in the centre band
    std::vector<Route> routes;
    std::vector<Loop> loops;
    std::vector<std::pair<cambc::champion_type, std::uint8_t>> loopOf;  // champion -> loop index
    // Per group: its nearest farmable groups by walking distance (kNeighbours, 255 = none).
    std::vector<std::array<std::uint8_t, 3>> neighbours;
};
inline constexpr int kNeighbours = 3;

// ---- draft side
// Map features from the spawn fields: runs once (fits the first draft turn's 10x fuel).
// Returns false if it failed; picks then fall back to the tested roster.
bool analyse();
// Build more per-group distance fields while a whole field fits above `reserve` fuel.
// Routes estimate the distances still missing, so this can stop at any point.
void buildFields(unsigned long long reserve);
bool analysed();
// Precompute altar walks and each group's nearest groups once the fields are done, while more
// than `floor` fuel is left (planFor calls it too, with no floor).
void prepare(unsigned long long floor);
const Features& features();
// Feature bonus for drafting `pick` (0 when the analysis is missing), in roster-rank units.
float draftBonus(cambc::champion_draft pick, int teamSize);
// Champion bodies a roster of picks gives (Cassi & Issac are two).
std::vector<cambc::champion_type> bodies(const std::vector<cambc::champion_draft>& picks);
// Solve (or continue solving) the farming loops for a team of `predicted` bodies while more than
// `floor` fuel is left. The first call sets the team; later calls (empty roster) resume.
void solveLoops(const std::vector<cambc::champion_type>& predicted, unsigned long long floor);
// Nearest-camp lists (Plan::neighbours) while more than `floor` fuel is left; resumes next pick.
void buildNeighbours(unsigned long long floor);
// Below `fuelFloor` fuel, remaining champions get only their first camp (no lookahead).
Plan planFor(const std::vector<cambc::champion_type>& roster, int teamSize, unsigned long long fuelFloor);
void write(const Plan& p);
// Summary for the draft trace: features, decision, routes and fuel.
std::string describe(const Plan& p);

// ---- gameplay side
const Plan& read();                    // decoded from the draft memory snapshot (cached)
// Advance or end this champion's route. Call once per turn while alive.
void follow(World& w);
// Camp group the route wants next (-1: no route or the route is over / at an altar step).
int routeGroup();
// The opening route is still running (it has priority over the loop).
bool routeActive();
// The route's current step is an altar visit and we carry gems.
bool routeWantsBank(const World& w);
bool isCentre(int groupId);
// `other` is one of `groupId`'s nearest farmable groups (by walking distance, from the draft; by
// straight-line distance if the draft plan is missing). Scales with the map, unlike a radius.
bool isNeighbour(int groupId, int other);
std::string routeText();

} // namespace mapplan
