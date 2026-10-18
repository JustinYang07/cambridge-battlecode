// Farming camp groups: kill every monster, then hold a spawn tile for the capture.
#include <algorithm>
#include <array>
#include <cmath>
#include "../combat/combat.h"
#include "../core/comms.h"
#include "../core/config.h"
#include "../core/profile.h"
#include "../core/trace.h"
#include "../core/fuel.h"
#include "../nav/fields.h"
#include "../nav/navigator.h"
#include "../shop/shop.h"
#include "../strategy/cohesion.h"
#include "../strategy/loop.h"
#include "../strategy/mapplan.h"
#include "../strategy/regroup.h"
#include "scorers.h"

namespace bc = cambc;

namespace scorers {


// Early, gold buys items that speed up everything after; late, it only breaks ties.
// Opening: monsters >= gold > gems. Endgame: gems >= monsters > gold.
float goldValue(const World& w) {
    float v;
    if (w.round < tune::opening_rounds) v = tune::gold_value_opening;
    else if (w.round < tune::late_game_round)
        v = tune::gold_value_opening + (tune::gold_value_late - tune::gold_value_opening) *
                static_cast<float>(w.round - tune::opening_rounds) / static_cast<float>(tune::late_game_round - tune::opening_rounds);
    else v = tune::gold_value_late;
    return w.kit.goldBonus ? v * 1.4f : v;  // Vesper Vale earns 20% more from every gold drop
}

// Behind on tributed gems: every gem is worth more, the further behind the more (capped).
float gemValue(const World& w) {
    const float deficit = static_cast<float>(w.enemyGems - w.teamGems);
    if (deficit <= 0) return 1.0f;
    return 1.0f + tune::catchup_max_boost * std::min(deficit / tune::catchup_full_deficit, 1.0f);
}

// ---- the middle of the map
// On maps with a busy centre (see centreIsBusy), camps toward the geometric centre are worth more than their own reward: the middle is where
// rotations are short and the map is contested. The bonus scales with the camp's radius from the
// centre, each axis measured against half the map's size (so wide maps work too): x max at the
// centre, x1 in the corners.
namespace {
// Radius of a tile from the map centre: each axis against half the map's size, 0 centre .. 1 corner.
float centreRadius(bc::vec2 p) {
    const auto& s = Statics::get();
    const float hx = std::max(static_cast<float>(s.width) / 2.0f, 1.0f), hy = std::max(static_cast<float>(s.height) / 2.0f, 1.0f);
    const float dx = (static_cast<float>(p.x) + 0.5f - hx) / hx, dy = (static_cast<float>(p.y) + 0.5f - hy) / hy;
    return std::min(std::sqrt((dx * dx + dy * dy) / 2.0f), 1.0f);
}

// Only on maps whose centre is busier than their outskirts: more camps per area inside
// tune::centre_zone than outside it (monoliths excluded). Ring maps (elkring) and edge-heavy maps
// have no middle worth pulling toward: there the bonus cost up to ~470 gems/game.
bool centreIsBusy() {
    static int verdict = -1;
    if (verdict >= 0) return verdict == 1;
    int inner = 0, outer = 0;
    for (const auto& g : Statics::get().groups) {
        if (g.hasMonolith || (g.gems == 0 && g.gold == 0)) continue;
        (centreRadius(g.center) < tune::centre_zone ? inner : outer) += 1;
    }
    // Areas in the normalised square [-1,1]^2 (area 4): the inner zone is a disc of radius zone * sqrt(2).
    const float innerArea = 3.14159265f * 2.0f * tune::centre_zone * tune::centre_zone;
    const float outerArea = std::max(4.0f - innerArea, 1e-3f);
    verdict = static_cast<float>(inner) / innerArea > static_cast<float>(outer) / outerArea ? 1 : 0;
    trace().note(verdict ? "map centre is busy: middle bonus on" : "map centre is quiet: middle bonus off");
    return verdict == 1;
}
} // namespace

float centreBonus(const World& w, const GroupInfo& g) {
    if (!centreIsBusy()) return 1.0f;
    const float radius = centreRadius(g.center);
    // Healthier means bolder (none at half health), and never where visible enemies outnumber us.
    const float bold = std::clamp((w.hpFrac() - 0.5f) / 0.5f, 0.0f, 1.0f);
    if (bold <= 0) return 1.0f;
    if (w.enemyPressure(g.center, tune::enemy_watch_radius) > w.alliedPower(g.center, 6)) return 1.0f;
    return 1.0f + (tune::centre_bonus_max - 1.0f) * (1.0f - radius) * bold;
}

namespace {

// Capture hand-off: of the allies at this dead camp (claiming it from within 6 tiles), the weakest
// damage dealer holds the capture and takes the reward; the others move on to the next camp. With
// enemies about, everyone holds it together.
// Capture hand-off: one capturer is enough (extra allies do not speed it up, and the capturer takes
// the whole reward), so everyone else should be walking to the next camp, fighting or banking.
// Anchored on who is actually standing on the tiles (ally positions are exact, so every champion
// reaches the same answer): designating a capturer who was not there yet, or who then went home
// to heal, left the camp empty and reset the capture. Returns the factor for our capture score.
float captureShareUncached(const World& w, int groupId);

// Melee champion types (attack range 1), from the designs.
bool meleeChampion(bc::champion_type t) {
    static std::array<std::int8_t, 16> cache{};
    const auto i = static_cast<std::size_t>(t);
    if (i >= cache.size()) return false;
    if (cache[i] == 0) cache[i] = bc::champion_designs[i].base_stats().base_attack_range <= 1 ? 1 : 2;
    return cache[i] == 1;
}

// Champion types with a dash ability (Issac, Ren, Vesper Vale, Vendetta), from the designs.
bool hasDash(bc::champion_type t) {
    static std::array<std::int8_t, 16> cache{};
    const auto i = static_cast<std::size_t>(t);
    if (i >= cache.size()) return false;
    if (cache[i] == 0) {
        cache[i] = 2;
        try {
            for (const auto& a : bc::champion_designs[i].abilities())
                if (const auto inf = a.info(); inf && std::holds_alternative<bc::ability_dash>(*inf)) cache[i] = 1;
        } catch (const bc::error&) {
        }
    }
    return cache[i] == 1;
}

bool rotateOnCapture(const World& w) { return tune::rotate_on_capture && w.round <= tune::rotate_capture_rounds; }

// A teammate stands on this group's capture tiles now (the 5-round capture is under way), cached
// per turn. Narrower than captureShare() < 1, which a nearer ally's claim alone also gives: a camp
// killed and left uncaptured kept the whole party rotating and rescoring for rounds.
bool allyHolding(const World& w, int groupId) {
    static int round = -1, slot = -1;
    static std::vector<std::int8_t> cache;
    if (round != w.round || slot != w.slot) {
        round = w.round;
        slot = w.slot;
        cache.assign(Statics::get().groups.size(), -1);
    }
    if (groupId < 0 || groupId >= static_cast<int>(cache.size())) return false;
    auto& c = cache[static_cast<std::size_t>(groupId)];
    if (c < 0) {
        c = 0;
        const auto& g = Statics::get().groups[static_cast<std::size_t>(groupId)];
        for (int sl = 0; sl < comms::alert_byte && sl < w.allyCount && !c; ++sl) {
            if (sl == w.slot) continue;
            const auto p = w.allyPosCached(sl);
            if (p.x >= 0 && dist(p, g.center) <= 3 && Statics::get().onGroup(g, p)) c = 1;
        }
    }
    return c == 1;
}

// Cached per turn and group: farm scoring asks it once per scored camp and the rotation checks
// ask again (uncached, the rotation rules cost ~245 lost turns a game).
float captureShare(const World& w, int groupId) {
    static int round = -1, slot = -1;
    static std::vector<float> cache;
    if (round != w.round || slot != w.slot) {
        round = w.round;
        slot = w.slot;
        cache.assign(Statics::get().groups.size(), -1.0f);
    }
    if (groupId < 0 || groupId >= static_cast<int>(cache.size())) return captureShareUncached(w, groupId);
    auto& c = cache[static_cast<std::size_t>(groupId)];
    if (c < 0) c = captureShareUncached(w, groupId);
    return c;
}

// We are the designated keeper of this group's capture this turn (set by captureShareUncached).
char& designatedKeeper(const World& w, int groupId) {
    static int round = -1, slot = -1;
    static std::vector<char> flags;
    if (round != w.round || slot != w.slot) {
        round = w.round;
        slot = w.slot;
        flags.assign(Statics::get().groups.size() + 1, 0);
    }
    return groupId >= 0 && groupId < static_cast<int>(flags.size()) - 1 ? flags[static_cast<std::size_t>(groupId)] : flags.back();
}

float captureShareUncached(const World& w, int groupId) {
    if (!tune::capture_handoff) return 1.0f;
    const auto& g = Statics::get().groups[static_cast<std::size_t>(groupId)];
    if (w.enemyPressure(g.center, tune::route_enemy_radius) > 0) return 1.0f;  // enemies about: hold it together
    struct Key {
        bool homeward;
        float hp, power;
        int slot;
        bool tank = false;
        bool mobile = false;  // a dash: walks the reward to an altar sooner
        float missing = 0;    // health lost: a recall home after the capture heals all of it
        bool tankType = false; // combatBotv2: a tank at any health (tune::keeper_tank_first)
    };
    // Only what every teammate can read about every other: health. (A bag test made it one-sided:
    // a teammate's bag may not be readable, so both champions on the tiles decided they kept it.)
    auto homeward = [&](float hp) { return hp < tune::capture_home_hp; };
    auto keeps = [](const Key& a, const Key& b) {  // a keeps the capture rather than b
        if (a.homeward != b.homeward) return a.homeward;
        // The tank walks on with the party (rows: Varg kept the first wolves' capture, the second
        // wolves were too strong without it, and three of us stood on the tiles splitting it).
        // combatBotv2 (as Fish): a tank at the camp keeps it and recalls with it; the rest walk on, a
        // melee champion fronting the next camp (rows: Varg kept 5 of the 6 captures he was at).
        if (tune::keeper_tank_first && a.tankType != b.tankType) return a.tankType;
        if (!tune::keeper_tank_first && tune::capture_keeper_not_tank && a.tank != b.tank) return !a.tank;
        // combatBotv2: whoever took the most damage keeps it, then recalls: the trip home banks the
        // reward and heals it (rows: Ren captured, and Issac, hurt, recalled with an empty bag).
        if (tune::keeper_most_damaged && std::abs(a.missing - b.missing) > tune::keeper_missing_margin) return a.missing > b.missing;
        // Then a champion with a dash keeps it (rows: Issac banks the reward faster).
        if (tune::capture_keeper_mobile && a.mobile != b.mobile) return a.mobile;
        if (!tune::keeper_most_damaged && std::abs(a.hp - b.hp) > 0.02f) return a.hp < b.hp;
        if (a.power != b.power) return a.power < b.power;
        return a.slot > b.slot;
    };
    auto nearestTile = [&](bc::vec2 p) {
        int best = 1 << 20;
        for (auto t : g.tiles) best = std::min(best, dist(p, t));
        return best;
    };
    const bool meOn = Statics::get().onGroup(g, w.pos);
    // A tank only "walks on with the party" while it is fit to: a hurt tank is going home next, so it
    // competes on damage taken like everyone else (rows: Ryth at 58% recalled empty-handed right
    // after the wolves died, and Ren held the capture and recalled too, leaving two to farm on).
    auto walksOn = [&](bc::champion_type t, float hp) { return isTank(t) && hp >= tune::keeper_tank_fit; };
    const Key me{homeward(w.hpFrac()), w.hpFrac(), profile(w.type).farmPower, w.slot, walksOn(w.type, w.hpFrac()), hasDash(w.type), w.maxHp - w.hp,
                 isTank(w.type)};
    const int myDist = nearestTile(w.pos);
    bool allyOnTiles = false, allyKeeps = false, allyNearer = false;
    // combatBotv2: the keeper is chosen among everyone at the camp, not only those already on its
    // tiles (the first to step on used to keep it). Every teammate reads the same positions and
    // health, so all reach the same answer.
    // A champion heading home is the best keeper of all: it captures, then recalls with the reward
    // (rows: Ryth at 42% went home empty-handed while Ren, at 84%, held the capture and recalled too).
    // (Earlier this counted it only once on the tiles; one tile off, the capture went to Ren.)
    bool designated = tune::keeper_most_damaged;  // we are the one to hold it
    bool designatedOther = false;                                 // a teammate here is
    for (int sl = 0; sl < comms::alert_byte && sl < w.allyCount; ++sl) {
        if (sl == w.slot) continue;
        const auto p = w.allyPosCached(sl);
        if (p.x < 0 || dist(p, g.center) > 8) continue;
        const auto type = w.allyTypes[static_cast<std::size_t>(sl)];
        const float allyHp = w.allyHpFrac(sl);
        const Key them{allyHp >= 0 && homeward(allyHp), allyHp >= 0 ? allyHp : 1.0f, profile(type).farmPower, sl, walksOn(type, allyHp >= 0 ? allyHp : 1.0f), hasDash(type),
                       w.allyMissingHp(sl), isTank(type)};
        if (tune::keeper_most_damaged && dist(p, g.center) <= tune::keeper_near_radius && keeps(them, me)) {
            designated = false;
            designatedOther = true;
        }
        if (Statics::get().onGroup(g, p)) {
            allyOnTiles = true;
            if (meOn && keeps(them, me)) allyKeeps = true;
        } else if (!meOn && comms::claimOf(sl) == groupId) {
            const int d = nearestTile(p);
            if (d < myDist || (d == myDist && sl < w.slot)) allyNearer = true;
        }
    }
    // Only from near the camp, and once a teammate is already capturing, only from beside its tiles:
    // arriving late adds nothing and splits the reward (rows: Issac walked back 7 tiles to a capture
    // Ren had started and stepped on as it completed: 33 gems each).
    if (designated && (dist(w.pos, g.center) > tune::keeper_near_radius || (allyOnTiles && !meOn && myDist > 1))) designated = false;
    designatedKeeper(w, groupId) = designated;
    if (designated) return 1.0f;                                      // ours to take, whoever is on it now
    if (meOn) return allyKeeps ? tune::capture_taken_factor : 1.0f;  // two of us on it: the keeper stays (we hold it until it is on)
    if (allyOnTiles) return tune::capture_taken_factor;               // a teammate is already capturing
    if (designatedOther) return tune::handoff_factor;                 // the keeper is coming
    if (allyNearer) return tune::handoff_factor;                      // a teammate will get there first
    return 1.0f;
}

struct Candidate {
    const GroupInfo* group;
    int travel, wait;
    bc::vec2 nearest;
    float bound;  // optimistic score: no option for this group can score higher
    bool dash = false;  // a sealed group we get into (and out of) by dashing
};

// Sealed group a dash can get into, for a champion that has a dash.
bool dashable(const World& w, const GroupInfo& g) {
    return tune::dash_pockets && !g.reachable && g.dashIn && w.kit.dashRange >= 2.0f;
}
// Walking + dashing distance to a sealed group's nearest tile (from outside: to the dash tile,
// one dash, then on from the landing tile; from inside: straight there).
int dashTravel(const World& w, const GroupInfo& g, bc::vec2& nearest) {
    const bc::vec2 from = g.inPocket(w.pos) ? w.pos : g.dashTo;
    int best = 1 << 20;
    for (auto t : g.tiles)
        if (dist(from, t) < best) { best = dist(from, t); nearest = t; }
    if (g.inPocket(w.pos)) return best;
    return static_cast<int>(static_cast<float>(dist(w.pos, g.dashFrom)) * 1.25f) + 1 + best;
}

// The map's gem camp for a team rush: one group holding at least team_rush_share of all the gems
// (stampede: the elk camp holds all of them). Worked out once; -1 when no group qualifies.
int rushGroup() {
    static int cached = -2;
    if (cached != -2) return cached;
    cached = -1;
    const auto& st = Statics::get();
    int total = 0, best = -1, bestGems = 0;
    for (const auto& g : st.groups) {
        if (g.hasMonolith) continue;
        total += g.gems;
        if (g.gems > bestGems && g.reachable) { bestGems = g.gems; best = g.id; }
    }
    if (tune::team_rush && best >= 0 && total > 0 && static_cast<float>(bestGems) >= tune::team_rush_share * static_cast<float>(total))
        cached = best;
    return cached;
}

// combatBotv3: team camp. Each champion scores a camp by its own share of the reward, so a camp only
// a whole team can take never looks better than a solo wolf (eight: the two-elk camps scored 0-12 all
// game, "too strong" for four and 1/6 each for six). When most of the team is together, healthy and
// with no enemy champion near, the team picks the best party camp by its whole reward over the
// team's time, and everyone commits (the team rush's gather and pull). Worked out from what every
// teammate reads alike (ally positions, the camp table, a fixed damage per body), so all agree.
int teamCamp(const World& w) {
    static int round = -1, slot = -1, cached = -1, held = -1, failing = 0;
    if (round == w.round && slot == w.slot) return cached;
    round = w.round;
    slot = w.slot;
    cached = -1;
    if (!tune::team_camp || w.round < tune::team_camp_round) return cached;
    const auto& st = Statics::get();
    // The team's centre and who is near it.
    std::array<bc::vec2, 8> pos{};
    std::array<float, 8> hp{};
    int n = 0;
    for (int s = 0; s < comms::alert_byte && s < w.allyCount && n < 8; ++s) {
        const auto p = s == w.slot ? w.pos : w.allyPosCached(s);
        if (p.x < 0) continue;
        pos[static_cast<std::size_t>(n)] = p;
        hp[static_cast<std::size_t>(n)] = s == w.slot ? w.hpFrac() : w.allyHpFrac(s);
        ++n;
    }
    long cx = 0, cy = 0;
    for (int i = 0; i < n; ++i) { cx += pos[static_cast<std::size_t>(i)].x; cy += pos[static_cast<std::size_t>(i)].y; }
    const bc::vec2 centre = n ? bc::vec2{static_cast<int>(cx / n), static_cast<int>(cy / n)} : w.pos;
    int grouped = 0;
    bool healthy = true;
    for (int i = 0; i < n; ++i)
        if (dist(pos[static_cast<std::size_t>(i)], centre) <= tune::team_camp_radius) {
            ++grouped;
            healthy = healthy && hp[static_cast<std::size_t>(i)] >= tune::team_camp_hp;
        }
    bool enemyNear = false;
    for (const auto& e : w.enemies) enemyNear = enemyNear || (e.hp > 0 && dist(e.pos, centre) <= tune::team_camp_enemy_radius);
    const bool ok = grouped >= tune::team_camp_min && healthy && !enemyNear;
    // Keep a held camp while it is up and we are mostly still fit for it (a few rounds of grace).
    if (held >= 0) {
        const auto& mem = w.groups[static_cast<std::size_t>(held)];
        if (mem.status == GroupStatus::respawning) held = -1;
        else if (!ok && ++failing > tune::team_camp_grace) held = -1;
        else {
            if (ok) failing = 0;
            cached = held;
            return cached;
        }
    }
    if (!ok) return cached;
    const float gv = goldValue(w), gemV = gemValue(w);
    float bestV = 0;
    int best = -1;
    for (const auto& g : st.groups) {
        if (g.harmless || g.hasMonolith || !g.reachable || g.gems <= 0) continue;
        const auto& mem = w.groups[static_cast<std::size_t>(g.id)];
        if (mem.status == GroupStatus::respawning) continue;
        // A party camp: one champion could not take it alone.
        if (g.totalStrength < tune::team_camp_min_strength) continue;
        const int d = dist(centre, g.center);
        if (d > tune::team_camp_max_dist) continue;
        const float kill = g.totalHealth / (tune::team_camp_body_dps * static_cast<float>(grouped));
        const float time = static_cast<float>(d) * tune::travel_weight + kill + tune::capture_turns;
        const float v = (static_cast<float>(g.gems) * gemV + static_cast<float>(g.gold) * gv) / std::max(time, 1.0f);
        if (v > bestV) { bestV = v; best = g.id; }
    }
    if (best >= 0) {
        held = best;
        failing = 0;
        cached = best;
    }
    return cached;
}

// Rush now: it is the rush camp, the opening is over, and enough of us are alive to take it.
bool teamRush(const World& w, const GroupInfo& g) {
    if (g.id == teamCamp(w)) return true;
    // combatBotv3: the team opening's camp: the whole team gathers and takes it together.
    if (mapplan::read().teamOpening && g.id == mapplan::read().openingCamp && mapplan::openingActive(w)) return true;
    if (g.id != rushGroup() || w.round < tune::team_rush_round) return false;
    // alliesAlive works out of sight (ally positions do not: an unseen ally reads as absent).
    return w.alliesAlive >= tune::team_rush_min_alive;
}

Option scoreGroup(World& w, const Candidate& c) {
    const auto& statics = Statics::get();
    const auto& g = *c.group;
    const auto& mem = w.groups[static_cast<std::size_t>(g.id)];
    Option o;
    o.kind = mem.status == GroupStatus::reward ? OptionKind::capture : OptionKind::farm;
    o.target = g.id;
    o.at = c.nearest;
    o.c.target << 'g' << g.id;
    // Endgame: gems still carried at the round limit count for nothing, so a camp whose gems cannot
    // reach an altar in time is worth only its gold (lifetime gold is the first tiebreak).
    bool gemsInTime = true;
    const int maxRounds = static_cast<int>(bc::constants::max_rounds);
    if (g.gems > 0 && w.round > maxRounds - tune::endgame_window) {
        int toAltar = 1 << 20;
        for (int i = 0; i < static_cast<int>(statics.altars.size()); ++i)
            if (const int d = fields::toAltar(i, g.center); d >= 0) toAltar = std::min(toAltar, d);
        const float ticks = static_cast<float>((w.gems + g.gems + 9) / 10);
        const float finish = static_cast<float>(w.round) + w.walkTurns(static_cast<float>(c.travel)) +
                             static_cast<float>(c.wait) + tune::capture_turns + static_cast<float>(toAltar) + ticks;
        gemsInTime = finish < static_cast<float>(maxRounds);
        if (!gemsInTime) o.c.note << "too late to bank; ";
    }
    o.add("gems", gemsInTime ? static_cast<float>(g.gems) * gemValue(w) : 0.0f);
    o.add("gold", static_cast<float>(g.gold) * goldValue(w));
    // Long walks cost more than their turns: a far camp is often contested or respawned by the
    // time we arrive, and walking past nearer camps leaves them to the enemy (collision: walking
    // 33 tiles past two elks to a richer corner camp).
    o.turns("travel", w.walkTurns(static_cast<float>(c.travel)) * tune::travel_weight);
    if (c.wait > 0) o.turns("respawn wait", static_cast<float>(c.wait) * tune::respawn_wait_weight);
    // routeBot (plan R3): the gems still have to reach an altar. One trip serves a whole bag,
    // so each camp pays its share of the walk from it to the nearest altar.
    if (g.gems > 0 && tune::altar_time_weight > 0) {
        int toAltar = 1 << 20;
        for (int i = 0; i < static_cast<int>(statics.altars.size()); ++i)
            if (const int d = fields::toAltar(i, g.center); d >= 0) toAltar = std::min(toAltar, d);
        if (toAltar < Navigator::unreachable)
            o.turns("to altar (share)", tune::altar_time_weight * static_cast<float>(toAltar) *
                                            std::min(1.0f, static_cast<float>(g.gems) / w.role().bankTarget));
    }

    bool awaiting = false;  // holding outside a party camp for allies
    float killTurns = 0;    // estimated fight length (farm options)
    int claims = comms::claimsOn(g.id, w.slot);
    unsigned movingMates = 0;  // party mates walking on with us to this camp (see below)
    // The draft's opening camp: the champions without a route are all coming, so judge it with
    // them (not "too strong" / "awaiting help" before anyone has had a turn to claim it).
    const bool opening = g.id == mapplan::read().openingCamp && mapplan::openingActive(w);
    if (opening) claims = std::max(claims, std::min(tune::max_team_per_camp, mapplan::read().openingParty) - 1);
    // Allies whose claim outranks ours: nearer to the camp, or as near and an earlier slot. Only they
    // keep us off a solo camp; a claim from across the map no longer does (it blocked the nearest
    // champion in 188 of 513 walk-pasts in the v1 A/B).
    unsigned owners = comms::ownersMask(g.id, w.slot, dist(w.pos, g.center));
    // Cohesion: following the leader onto its camp is the point, not a clash over a solo camp.
    if (g.id == cohesion::leaderCamp() && cohesion::leader() >= 0) owners &= ~(1u << cohesion::leader());
    // A teammate holds our party camp's capture and the rest of the party moves on together: judge
    // the next camp with them, not alone (rows: Cassi and Issac judged the second wolves "too strong"
    // solo and stood on the first camp's tiles, splitting its reward three ways).
    if ((rotateOnCapture(w) || tune::move_on_all_game) && tune::party_rotation && w.partyMates != 0 && g.id != w.lastTarget &&
        mem.status != GroupStatus::respawning && mem.status != GroupStatus::reward && w.lastTarget >= 0 &&
        w.lastTarget < static_cast<int>(w.groups.size()) &&
        mapplan::isNeighbour(w.lastTarget, g.id) &&  // only where the party moves on to (every camp: +200 lost turns)
        w.groups[static_cast<std::size_t>(w.lastTarget)].status == GroupStatus::reward && allyHolding(w, w.lastTarget)) {
        // combatBotv2: only the mates actually walking on with us count: near us, off the old camp's
        // tiles (the keeper), and still claiming the old camp or this one (a mate that started a
        // recall has cleared its claim). Rows: Stellar judged the third wolves as a party of four
        // ("lose ~78 hp") while Ren recalled, Ryth held the capture and Issac was at spawn; she and
        // Cassi fought them alone and lost ~900 health between them.
        const auto& from = statics.groups[static_cast<std::size_t>(w.lastTarget)];
        for (int sl = 0; sl < comms::alert_byte && sl < w.allyCount; ++sl) {
            if (sl == w.slot || !((w.partyMates >> sl) & 1u)) continue;
            const auto p = w.allyPosCached(sl);
            const int claim = comms::claimOf(sl);
            if (p.x < 0 || Statics::get().onGroup(from, p)) continue;
            if (tune::moving_on_real && (dist(p, w.pos) > tune::moving_on_radius || (claim != w.lastTarget && claim != g.id))) continue;
            movingMates |= 1u << sl;
        }
        const int movingOn = tune::moving_on_real ? __builtin_popcount(movingMates)
                                                  : std::max(0, __builtin_popcount(w.partyMates) - 1);  // less the keeper
        claims = std::max(claims, std::min(tune::max_team_per_camp - 1, movingOn));
    }
    if (o.kind == OptionKind::farm) {
        const float left = mem.status == GroupStatus::alive && mem.seenRound >= 0 ? mem.healthLeft : 1.0f;
        // Health we must keep to walk out of reach afterwards (monsters do not chase).
        const auto solo = combat::campFight(w, g, left, 1);
        const float reserve = solo.hitting * (tune::escape_margin_turns + 1.0f);
        const auto& role = w.role();
        const float budget = w.maxHp * role.maxFightHpUse - reserve;
        // Monsters hit the nearest champion, ties going to the earliest draft slot (measured from
        // 727 party fights). A tank stands adjacent and was drafted first, so it takes nearly every
        // hit while the rest of the party, ranged allies at full range especially, takes almost none.
        // combatBotv2: only a tank that gets there no later than we do covers us (rows: Ren claimed
        // the third wolves from spawn, 35 tiles off, so Stellar and Cassi fought them "behind the
        // tank" alone, lost ~900 health between them and recalled, and Ren then picked another camp).
        bool tankLeads = false;
        if (!role.leadsPull) {
            const unsigned claimers = comms::claimersMask(g.id, w.slot);
            for (int sl = 0; sl < comms::alert_byte && sl < w.allyCount && !tankLeads; ++sl) {
                if (sl == w.slot || !((claimers >> sl) & 1u) || !isTank(w.allyTypes[static_cast<std::size_t>(sl)])) continue;
                const auto p = w.allyPosCached(sl);
                tankLeads = !tune::tank_cover_near || (p.x >= 0 && dist(p, g.center) <= dist(w.pos, g.center) + tune::tank_cover_slack);
            }
        }
        // Seen, not assumed: an ally is already beside the fight and the monsters are not hitting
        // us, so they are on it. Vendetta used to recall from lanes' elk at half health while Varg
        // took over the hits right behind it.
        const bool allyTanks = !w.beingHit && w.allyTanking(g.id);
        // Ranged with a melee ally on the camp: monsters hit the nearest champion, and the melee
        // stands adjacent while we stand at full range, so we take few hits (rows: Tavi judged
        // the third wolves as needing four of us and the camp sat untouched for 200 rounds).
        bool meleeCover = false;
        if (tune::ranged_cover_share > 0 && w.ranged && !role.leadsPull) {
            const unsigned mates = comms::claimersMask(g.id, w.slot) | movingMates;
            for (int sl = 0; sl < w.allyCount && sl < comms::alert_byte; ++sl)
                if ((mates >> sl) & 1u) meleeCover = meleeCover || meleeChampion(w.allyTypes[static_cast<std::size_t>(sl)]);
        }
        const float share = allyTanks ? 0.05f : role.leadsPull ? 1.0f : tankLeads ? 0.05f :
                            meleeCover ? std::min(role.partyAbsorb, tune::ranged_cover_share) : role.partyAbsorb;
        // With k attackers the fight is k times shorter and we absorb a share of the hits,
        // so per-head damage is solo * share / k (what campFight computes, without re-simulating).
        auto damageFor = [&](int k) {
            return k == 1 ? solo.damage : solo.damage * share / static_cast<float>(k);
        };
        int need = 1;
        while (need < tune::max_team_per_camp && damageFor(need) > budget) ++need;
        // combatBotv3: a camp nearer their spawn than ours is where they find us mid-fight: take it
        // with enough of us to win that fight too, or not at all (elkring vs alfungus: Ren and Varg
        // took an elk 26 from our spawn, 19 from theirs; three of them walked over and killed both,
        // user). Not while their team is down (raids), nor on a team rush / team opening camp.
        if (tune::enemy_side_min_party > 1 && w.enemiesAlive > 1 && !teamRush(w, g)) {
            const auto& st = Statics::get();
            if (dist(g.center, st.spawnCenter) > dist(g.center, st.enemySpawnCenter))
                need = std::max(need, std::min(tune::enemy_side_min_party, std::max(w.alliesAlive, 1)));
        }
        // combatBotv3: down bodies, a ranged champion does not open a monster camp alone (elkring vs
        // alfungus r83: Stellar woke g0's elk alone at 4v6 while four of them walked into our half).
        if (tune::outnumbered_ranged_pair && w.ranged && !g.harmless && w.alliesAlive < w.enemiesAlive) need = std::max(need, 2);
        const int k = std::max(need, 1 + claims);
        const unsigned mates = comms::claimersMask(g.id, w.slot) | movingMates;
        const auto fight = k == 1 ? solo : combat::campFight(w, g, left, k, share, mates);
        if (allyTanks) o.c.note << "ally tanking";
        else if (tankLeads) o.c.note << "tank soaks";
        if (role.followsParty && claims > 0) o.mul("support joins party", 1.5f);
        o.need = need;
        // Joining an ally who is waiting on a camp it cannot take without us unlocks the whole reward
        // for the team, not just our cut (Issac idled 30 turns on a respawn while Ren held beside an
        // elk for help on elkring).
        const bool unlocksAlly = claims > 0 && need > claims && need <= 1 + claims;
        if (k > 1 && !unlocksAlly) o.mul("our share", 1.0f / static_cast<float>(k));
        if (unlocksAlly) o.c.note << "ally needs us";
        o.turns("kill time", fight.turns);
        killTurns = fight.turns;
        o.turns("capture", tune::capture_turns);
        o.expectDamage = fight.damage;

        const bool engaged = w.engagedGroup == g.id;
        // Only charge the walk-out when monsters are already hitting us. Charging it whenever we
        // stood inside a sleeping camp's range made the verdict flip with every step in and out
        // (Varg alternated farm / heal-first on collision for 50+ rounds).
        const float escapeCost = engaged ? static_cast<float>(nav().escapeTurns(w)) * solo.hitting : 0.0f;
        // Gem rally: an ally claims a gem camp that needs a party. Judge it with a full party and
        // pull towards it (stampede's only gems sit in one elk camp: one champion held it alone for
        // 750 turns while the rest farmed gold, each judging it too strong for two).
        const bool gemRally = tune::gem_rally_bonus != 1.0f && g.gems > 0 && claims > 0 && need > 1;
        bool tooStrong = fight.damage > budget;
        if (tooStrong && gemRally) tooStrong = combat::campFight(w, g, left, tune::max_team_per_camp, share, mates).damage > budget;
        if (gemRally && !tooStrong) o.mul("rally: ally holds gems", tune::gem_rally_bonus);
        // combatBotv2: partial tank. Leading the pull we need not survive the whole fight: we take
        // the hits until our retreat point and leave, and the allies with us finish it (Fish on rows:
        // Ren tanked the third wolves from 660 to 260, recalled, and Cassi and Stellar killed them).
        // Doable when what we can spend plus what they can spend after us covers the fight.
        bool partialTank = false;
        // The front is whoever takes the hits: the tank, or a melee champion when no tank leads.
        const bool front = role.leadsPull || (meleeChampion(w.type) && !tankLeads && !allyTanks);
        if (tune::partial_tank && front && k > 1 && mates != 0 && (tooStrong || fight.damage + reserve + escapeCost > w.hp)) {
            // All of the camp's hits land on the front, us first, then them once we leave.
            const float total = combat::campFight(w, g, left, k, 1.0f, mates).damage;
            // What we can spend before leaving: down to a few turns of hits (and the walk out) left.
            const float perTurn = w.hitRate > 1.0f ? w.hitRate : solo.hitting;
            const float mine = tune::late_heal
                                   ? w.hp - perTurn * (static_cast<float>(nav().escapeTurns(w)) + tune::death_margin_turns)
                                   : w.hp - tune::partial_tank_floor * w.maxHp - escapeCost;
            float theirs = 0;
            for (int sl = 0; sl < comms::alert_byte && sl < w.allyCount; ++sl)
                if (sl != w.slot && ((mates >> sl) & 1u)) {
                    const float hp = w.allyHp(sl), frac = w.allyHpFrac(sl);
                    if (hp > 0 && frac > 0) theirs += std::max(0.0f, hp - tune::partial_tank_floor * hp / frac);
                }
            // Joining: worth it for a real share of the fight. Already tanking: stay while we can.
            const bool worth = engaged ? mine > 0 : mine >= tune::partial_tank_min_share * total;
            if (worth && mine + theirs >= total) {
                partialTank = true;
                tooStrong = false;
                // The plan is with the allies coming, not a full party: wait for no one else
                // (rows: Ren held outside the wolves' reach 4 turns, "need 3 more", until Stellar opened).
                need = std::min(need, 1 + __builtin_popcount(mates));
                o.need = need;
                o.expectDamage = std::min(total, mine);
                o.c.note << "tank first, then leave; ";
            }
        }
        // Team rush: on maps where one camp holds most of the gems (stampede's elk camp), the whole
        // team takes it together from the early game. Top teams cleared it at r122-133 with five
        // champions; we judged it too strong for four and farmed gold-only clusters until r800+.
        const bool rush = teamRush(w, g);
        if (rush) {
            tooStrong = false;
            // Gather most of the team before the pull (arriving one at a time, each melee champion
            // died within 4 rounds of stepping in: the elks hit for ~360 a round).
            // A team camp gathers the champions that chose it (team_camp_min), not everyone alive:
            // waiting for a fifth who is elsewhere stalled the team at the ring (eight: captures fell
            // from ~126 to 72 by round 500 over 6 games).
            const bool camp = g.id == teamCamp(w) && g.id != rushGroup();
            need = std::max(need, std::min(w.alliesAlive, camp ? tune::team_camp_min : tune::team_rush_party));
            o.need = need;
            o.mul("team rush", tune::team_rush_bonus);
            o.c.note << "team rush; ";
        }
        if (tooStrong) {
            o.mul("too strong", 0.05f);
            if (g.id == loop::target()) loop::tooStrong(w, g.id);
        } else if (!partialTank && !rush && fight.damage + reserve + escapeCost > w.hp) {
            if (engaged) o.mul("would not survive", 0.02f);  // disengage now; heal will win
            else {
                // Not doable at this health: this is really a reason to go home first.
                o.turns("heal first", 5.0f + static_cast<float>(dist(statics.spawnCenter, g.center)));
                o.mul("needs a trip home", tune::heal_first_factor);
                o.kind = OptionKind::heal;
            }
        }
        // Party rotation: our party camp is down for a while and this one is up. The party's leader
        // (lowest slot) moves on first; mates follow when a mate already claims the new camp. While
        // rotating, the party is on its way, so we are not "awaiting help".
        bool rotating = false;
        if (tune::party_rotation && loop::target() < 0 && need > 1 && g.id != w.lastTarget && w.partyMates != 0) {
            if (comms::claimersMask(g.id, w.slot) & w.partyMates) {
                rotating = true;
                o.mul("follow the party", tune::rotate_bonus);
            } else if (w.lastTarget >= 0 && w.lastTarget < static_cast<int>(w.groups.size())) {
                const auto& from = w.groups[static_cast<std::size_t>(w.lastTarget)];
                // Leader: the lowest slot among mates still at our camp (one off banking cannot lead).
                const unsigned present = w.partyMates & comms::claimersMask(w.lastTarget, w.slot);
                const bool leader = (present & ((1u << w.slot) - 1u)) == 0;
                // Down for long enough, or down and a teammate holds its capture (the rest of us move on
                // now rather than stand on the tiles splitting the reward, then walk back to the camp
                // behind us: rows' second wolves).
                const bool handedOff = rotateOnCapture(w) && from.status == GroupStatus::reward && allyHolding(w, w.lastTarget);
                const bool done = (from.status == GroupStatus::respawning && from.respawnAt - w.round >= tune::rotate_min_wait) || handedOff;
                // A capture hand-off frees everyone but the keeper (the keeper may be the lowest slot).
                if ((leader || handedOff) && done && mem.status != GroupStatus::respawning && mem.status != GroupStatus::reward) {
                    rotating = true;
                    o.mul("lead the party on", tune::rotate_bonus);
                }
            }
        }
        // Once a fight has started, waiting for help is no longer an option: finish or leave.
        awaiting = need > 1 + claims && !engaged && !rotating;
        if (awaiting) {
            // On the loop the party is on its way (members follow each other's claims).
            if (g.id != loop::target()) {
                o.mul("awaiting help", 0.7f);
                o.turns("gather", 8.0f);
            }
            // Help that never arrives: stop camping this group.
            if (w.lastTarget == g.id && w.waitingTurns > (claims > 0 ? tune::max_party_wait_claimed : tune::max_party_wait)) {
                o.mul("help not coming", 0.3f);
                if (g.id == loop::target()) loop::tooStrong(w, g.id);
            }
        }
        // Party full: allies nearer this camp than us already claim it in the numbers it needs. We
        // add little but speed, and another camp stays idle (rows: the whole team walked the row
        // as one group while two wolf camps sat respawned; their team ran two or three parties).
        bool partyFull = false;
        if (tune::party_full_factor != 1.0f && need > 1 && !engaged && g.id != w.lastTarget) {
            const unsigned ahead = comms::ownersMask(g.id, w.slot, dist(w.pos, g.center));
            if (__builtin_popcount(ahead) >= need) {
                partyFull = true;
                o.mul("party full", tune::party_full_factor);
            }
        }
        if (need > 1 && claims > 0 && !partyFull) o.mul("join ally", 1.3f);
        // Under-levelled: build strength on wolves and clusters before taking elks.
        if (need > 1 && shop::shardsOwned() < tune::underpowered_shards && !teamRush(w, g)) o.mul("underpowered", tune::underpowered_factor);
        if (need == 1 && owners) o.mul("nearer ally claimed", tune::ally_claim_factor);
        if (w.gems > 0 && fight.damage > 0)
            o.mul("bag at risk", std::max(0.4f, 1.0f - tune::carry_risk_per_gem * w.gems * fight.damage / std::max(w.hp, 1.0f)));
    } else {
        o.turns("capture", tune::capture_turns);
        if (owners && !(captureShare(w, g.id) >= 1.0f && designatedKeeper(w, g.id))) o.mul("nearer ally capturing", tune::ally_claim_factor);
        // Capture hand-off: a loop mate stays for this one, we walk on to the next camp.
        if (const float share = captureShare(w, g.id); share < 1.0f) {
            o.mul(share <= tune::capture_taken_factor ? "teammate is capturing" : "teammate capturing first", share);
            // A teammate stands on the tiles holding it: we add nothing here. Move on to the next camp
            // now, as their team does (we used to wait beside the tiles for the 5-round capture).
            if (tune::move_on_all_game && allyHolding(w, g.id)) o.mul("teammate holds it: move on", 1e-4f);
        }
        // An enemy on the spawn tiles contests the capture: progress resets every round, so the
        // reward is not 5 turns away (both teams' Vargs sat on packs' cluster trading hits for 14
        // turns, scoring capture 2109 until one died). Winning the fight is the way to the reward.
        for (const auto& e : w.enemies)
            if (e.hp > 0 && Statics::get().onGroup(g, e.pos)) {
                o.mul("contested by enemy", tune::contested_capture_factor);
                o.c.note << "contested";
                break;
            }
        // Standing on the tiles while enemies and monsters hit us: can we last the capture?
        const float incoming = combat::threatPerTurn(w, w.pos) + w.incoming;
        if (incoming > 0 && incoming * tune::capture_turns >= w.hp) o.mul("would not survive", 0.02f);
    }

    if (g.id == mapplan::routeGroup()) {
        o.mul("route", tune::route_bonus);
        o.c.note << "route";
    }
    bool raiding = false;
    // Raid: their team is (nearly) wiped, so their half is open until the first of them respawns.
    // Clusters are free and quick (strong pull); monster camps take longer and they may come back
    // mid-fight (mild pull). Only camps we can finish in time, and bolder the healthier we are.
    if (w.enemyBodies >= 2 && w.enemiesAlive <= tune::raid_max_alive && w.enemyRespawnAt > w.round &&
        dist(g.center, statics.enemySpawnCenter) < dist(g.center, statics.spawnCenter)) {
        const float finish = static_cast<float>(w.round) + w.walkTurns(static_cast<float>(c.travel)) +
                             static_cast<float>(c.wait) + killTurns + tune::capture_turns;
        const float bold = std::clamp((w.hpFrac() - 0.5f) / 0.5f, 0.0f, 1.0f);
        if (finish <= static_cast<float>(w.enemyRespawnAt) && bold > 0) {
            const float full = g.harmless ? tune::raid_cluster_factor : tune::raid_camp_factor;
            o.mul("raid: their team is down", 1.0f + (full - 1.0f) * bold);
            raiding = true;
        }
    }
    // Their whole team is dead with time left to use it: take their nearest camps first. Ours will
    // still be there when the window closes, theirs will not be open again.
    if (w.enemyBodies >= 2 && w.enemiesAlive == 0 && w.enemyRespawnAt - w.round >= tune::raid_min_window &&
        dist(g.center, statics.enemySpawnCenter) >= dist(g.center, statics.spawnCenter)) {
        const float bold = std::clamp((w.hpFrac() - 0.5f) / 0.5f, 0.0f, 1.0f);
        if (bold > 0) {
            o.mul("their team is dead: theirs first", 1.0f - (1.0f - tune::raid_own_half_factor) * bold);
            raiding = true;
        }
    }
    // Regrouping: stay with the spotter. Camps beside it are fine, the rest wait for the fight.
    if (regroup::active() && dist(g.center, regroup::at()) > tune::regroup_leash) o.mul("regrouping", tune::regroup_farm_factor);
    if (w.engagedGroup >= 0 && w.engagedGroup != g.id) {
        // Walking away from monsters that are hitting us costs health on the way out.
        o.turns("leave fight", static_cast<float>(nav().escapeTurns(w)));
        o.mul("abandon fight", tune::abandon_factor);
    }
    // Gem and gold clusters: quick, free and safe, a good use of time between bigger camps.
    // Waiting for a respawn is neither a quick safe kill nor finishing what we started: those
    // bonuses made a respawning gem cluster outscore every wolf (expanse), so they skip waits.
    const bool waiting = c.wait > 0;
    if (g.harmless && c.travel <= tune::safe_camp_radius && !waiting) o.mul("safe camp nearby", tune::safe_camp_bonus);
    if (c.wait > tune::idle_wait_turns) o.mul("idle wait", tune::idle_wait_factor);
    // While a team rush is on (its camp is up), every other camp waits: the team converges on it.
    if (const int rg = rushGroup(); rg >= 0 && g.id != rg && teamRush(w, Statics::get().groups[static_cast<std::size_t>(rg)]) &&
        w.groups[static_cast<std::size_t>(rg)].status != GroupStatus::respawning)
        o.mul("team rushing", tune::team_rush_other_factor);
    else if (const int tc = teamCamp(w); tc >= 0 && g.id != tc) o.mul("team camp elsewhere", tune::team_rush_other_factor);
    if (g.id == w.stuckGroup && w.round < w.stuckUntil) o.mul("stuck here before", 0.05f);
    // Opening gold: items bought early speed up everything after; Vesper earns 20% more of it.
    if (g.gems == 0 && g.gold > 0 && w.round < tune::early_gold_skip_round && c.travel > 4 && mapplan::skipGoldOnly())
        o.mul("early: gems first", 0.05f);
    if (g.gems == 0 && g.gold > 0 && w.round < tune::opening_rounds)
        o.mul("opening gold", w.kit.goldBonus ? tune::opening_gold_bonus * 1.3f : tune::opening_gold_bonus);
    if (mem.enemyNearRound >= 0 && w.round - mem.enemyNearRound <= 10) o.mul("enemy nearby", tune::contested_camp_factor);
    // Enemy champions around the camp: worse the more we carry and the more outnumbered we are.
    const float pressure = w.enemyPressure(g.center, tune::enemy_watch_radius);
    if (pressure > 0 && w.gems >= tune::bag_avoid_gems) {
        // Only a real bag in front of a stronger enemy group is worth giving up a camp for.
        const float ratio = pressure / std::max(w.alliedPower(g.center, 6), 1.0f);
        if (ratio > 1.0f) o.mul("contested", 1.0f / (1.0f + (ratio - 1.0f) * static_cast<float>(w.gems) / 40.0f));
    }
    // Holding for help earns no "committed" bonus: it made a 100-turn wait on corners stick.
    if (!waiting && !awaiting && w.lastTarget == g.id && (w.lastOption == static_cast<int>(OptionKind::farm) ||
                                             w.lastOption == static_cast<int>(OptionKind::capture))) {
        // Finish what we started, all the more when we have already put damage into it.
        const float done = std::clamp(1.0f - mem.healthLeft, 0.0f, 1.0f);
        o.mul("committed", tune::engaged_bonus + tune::focus_bonus_per_damage * done);
    }
    if (const float mid = centreBonus(w, g); mid > 1.001f) o.mul("towards the middle", mid);
    // No invading without a numbers lead.
    if (!w.mayInvade() && w.theirHalf(g.center)) o.mul("their half: no lead", tune::no_invade_factor);
    // combatBotv3: a ranged champion is the exposed one at a camp that is not clearly ours, all the
    // more beside a bush that hides whoever waits in it (swirl: our team farmed the two centre camps,
    // 8-9 tiles from each spawn beside the bush walls, and lost four of five by round 51 while the
    // wolves on our side stood untouched; user). Risky: an enemy seen by it lately, or a bush beside it.
    // A team rush or team opening is the whole team going together on purpose (stampede's gem camp).
    if (tune::ranged_contested_factor < 1.0f && (w.ranged || tune::contested_all) && !teamRush(w, g)) {
        const auto& st = Statics::get();
        const float dOur = static_cast<float>(dist(g.center, st.spawnCenter)), dTheir = static_cast<float>(dist(g.center, st.enemySpawnCenter));
        if (dOur > tune::own_side_ratio * dTheir) {
            const bool seen = mem.enemyNearRound >= 0 && w.round - mem.enemyNearRound <= tune::contested_seen_rounds;
            if (seen || bushBeside(g)) o.mul("contested: ranged exposed", tune::ranged_contested_factor);
        }
    }
    // Cohesion: farm around the leader (soft pull; banking and healing stay individual).
    if (const int lc = cohesion::leaderCamp(); lc >= 0 && !raiding) {
        if (g.id == lc) o.mul("with the leader", tune::cohesion_leader_bonus);
        else if (mapplan::isNeighbour(lc, g.id)) o.mul("next to the leader", tune::cohesion_near_bonus);
        else o.mul("away from the leader", tune::cohesion_away_factor);
    }
    // Chaining: right after taking a camp, its nearest camps (from the draft's walking distances)
    // come first.
    if (w.chainFrom >= 0 && w.chainFrom != g.id && w.round - w.chainRound <= tune::chain_rounds &&
        mapplan::isNeighbour(w.chainFrom, g.id))
        o.mul("next to the last camp", tune::chain_bonus);
    // combatBotv3: the team opening is on: everyone goes there first.
    if (mapplan::read().teamOpening && !opening && mapplan::read().openingCamp >= 0 && mapplan::openingActive(w))
        o.mul("team opening elsewhere", tune::team_opening_other_factor);
    if (opening) {
        o.mul("opening camp", tune::opening_camp_bonus);
        o.c.note << "opening";
    }
    // Farming loop: its current camp first, camps elsewhere only when clearly better (raids aside).
    if (const int lt = loop::target(); lt >= 0) {
        if (g.id == lt) {
            o.mul("loop", tune::loop_bonus);
            o.c.note << "loop";
        } else if (!raiding && !loop::onLoop(g.id)) {
            o.mul("off loop", tune::loop_off_factor);
        }
    }
    // Sealed pocket: only we get in (no party), and leaving costs the walk back to the landing
    // tile plus a dash (the 8-round cooldown is over by the time the camp is down).
    if (c.dash) {
        o.turns("dash out", static_cast<float>(dist(c.nearest, g.dashTo) + tune::dash_exit_turns));
        if (o.need > 1) o.mul("dash: solo only", 0.05f);
        o.c.note << "dash in";
    }
    o.finish();
    return o;
}

} // namespace

float captureShareOf(const World& w, int groupId) { return captureShare(w, groupId); }
bool allyHoldingOf(const World& w, int groupId) { return allyHolding(w, groupId); }

// A bush within 3 tiles of the group's centre (worked out once per group).
bool bushBeside(const GroupInfo& g) {
    static std::vector<std::int8_t> cache;
    const auto& st = Statics::get();
    if (cache.empty()) cache.assign(st.groups.size(), -1);
    if (g.id < 0 || g.id >= static_cast<int>(cache.size())) return false;
    auto& c = cache[static_cast<std::size_t>(g.id)];
    if (c < 0) {
        c = 0;
        try {
            for (auto b : bc::map::bushes().positions())
                if (dist(b, g.center) <= 3) { c = 1; break; }
        } catch (const bc::error&) {
        }
    }
    return c == 1;
}

bool captureHold(const World& w) {
    // Rounds we have stood on the same reward's tiles (once per round; a gap restarts it).
    static int lastRound = -100, lastGroup = -1, onSince = 0;
    if (lastRound != w.round) {
        int on = -1;
        const auto& st = Statics::get();
        // Only someone who was farming or capturing last turn can be standing on a reward's tiles.
        const bool maybe = w.lastOption == static_cast<int>(OptionKind::capture) || w.lastOption == static_cast<int>(OptionKind::farm);
        if (w.alive && maybe)
            for (const auto& g : st.groups)
                if (w.groups[static_cast<std::size_t>(g.id)].status == GroupStatus::reward && dist(g.center, w.pos) <= 6 && st.onGroup(g, w.pos)) {
                    on = g.id;
                    break;
                }
        if (on < 0 || on != lastGroup || lastRound != w.round - 1) onSince = w.round;
        lastGroup = on;
        lastRound = w.round;
    }
    // The answer holds for the round (fight scoring asks once per enemy).
    static int answeredRound = -100;
    static bool answer = false;
    if (answeredRound == w.round) return answer;
    answeredRound = w.round;
    answer = false;
    if (tune::capture_hold_radius <= 0 || lastGroup < 0 || w.round - onSince < tune::capture_hold_after) return false;
    for (const auto& e : w.enemies)
        if (e.hp > 0 && dist(e.pos, w.pos) <= tune::capture_hold_radius) return false;
    answer = true;
    return true;
}

bool teamRushOn(const World& w, int groupId) {
    if (groupId >= 0 && groupId == teamCamp(w)) return true;
    const int rg = rushGroup();
    return rg >= 0 && rg == groupId && teamRush(w, Statics::get().groups[static_cast<std::size_t>(rg)]);
}

void farm(World& w, std::vector<Option>& out) {
    // Branch and bound: an optimistic bound per group (Chebyshev distance never exceeds
    // walking distance, the kill uses the largest party), then fully score groups in
    // bound order until no remaining bound can beat the best real score. The walking
    // distance BFS is only consulted for the few groups that get scored.
    const auto& statics = Statics::get();
    constexpr int kMax = 64;
    std::array<Candidate, kMax> pre;
    int n = 0;
    // Largest product of '*' factors any option can get (committed, join/support party, home region).
    const float maxMultiplier = (tune::engaged_bonus + tune::focus_bonus_per_damage) * 1.5f *
                                tune::safe_camp_bonus * tune::route_bonus * tune::centre_bonus_max * tune::loop_bonus * tune::chain_bonus * tune::nearest_camp_bonus * (tune::cohesion ? tune::cohesion_leader_bonus : 1.0f) *
                                (w.enemyBodies >= 2 && w.enemiesAlive <= tune::raid_max_alive ? tune::raid_cluster_factor : 1.0f);
    const float gv = goldValue(w);
    const float gemV = gemValue(w);
    for (const auto& g : statics.groups) {
        if (n >= kMax) break;
        const auto& mem = w.groups[static_cast<std::size_t>(g.id)];
        if (g.gems == 0 && g.gold == 0) continue;
        const bool dash = dashable(w, g);
        if (!g.reachable && !dash) continue;  // sealed off from our spawn: cannot be walked to
        const int reach = mem.status == GroupStatus::reward ? 0 : w.range;
        // Exact walking distance once its field exists (a tighter bound prunes more groups),
        // otherwise Chebyshev distance, which walking never beats.
        bc::vec2 unused;
        int cheb = dash ? dashTravel(w, g, unused) : fields::toGroup(g.id, w.pos);
        // Its field says no walking path reaches it (an island behind holes: ambush). It cannot be
        // captured on foot, so it is not a farming option (it used to be scored by straight-line
        // distance, and champions stood still for 1,500 rounds trying to path to it).
        if (tune::skip_field_unreachable && cheb >= Navigator::unreachable) continue;
        if (cheb < 0) {
            cheb = 1 << 20;
            for (auto t : g.tiles) cheb = std::min(cheb, dist(w.pos, t));
        }
        cheb = std::max(0, cheb - reach);
        const int wait = mem.status == GroupStatus::respawning ? std::max(0, mem.respawnAt - w.round - cheb) : 0;
        if (wait > 60) continue;  // too far in the future to plan around
        float reward = static_cast<float>(g.gems) * gemV + static_cast<float>(g.gold) * gv;
        // Keep the bound honest for the opening-gold bonus (applied per group, not to all).
        if (g.gems == 0 && g.gold > 0 && w.round < tune::opening_rounds) reward *= tune::opening_gold_bonus * 1.3f;
        float minKill = 0;
        if (mem.status != GroupStatus::reward) {
            const float left = mem.status == GroupStatus::alive && mem.seenRound >= 0 ? mem.healthLeft : 1.0f;
            minKill = g.totalHealth * left /
                      (std::max(combat::campDamagePerTurn(w, g), 1.0f) * static_cast<float>(tune::max_team_per_camp));
        }
        // Include the respawn wait: without it a respawning camp's bound looked huge, it took
        // the few fully-scored slots, and nearby wolves were never scored at all (expanse).
        // max(cheb, respawn) never exceeds the real travel + wait, so the bound stays valid.
        const float time = static_cast<float>(cheb + wait) + tune::capture_turns + minKill;
        pre[static_cast<std::size_t>(n++)] = {&g, cheb, 0, g.center, reward / std::max(time, 1.0f) * 100.0f * maxMultiplier, dash};
    }
    fuel::mark("farm.pre");

    const std::size_t first = out.size();  // this scorer's options start here
    float best = 0;
    int scored = 0;
    // Degrade gracefully when the turn is already expensive (big maps, crowded fights).
    const auto fuelLeft = bc::fuel_remaining();
    // Leave ~50k for executing, tracing and slack; spend the rest on scoring (~6k per group).
    const auto spare = fuelLeft > tune::fuel_for_full_scoring - 10000 ? fuelLeft - (tune::fuel_for_full_scoring - 10000) : 0;
    const int maxScored = std::clamp(static_cast<int>(spare / tune::fuel_per_scored_group), 1, tune::max_scored_groups);
    bool bestNeedsHelp = false;
    // The nearest camp that is up now (no respawn wait): it gets a bonus once everything is scored.
    int nearestOut = -1, nearestTravel = 1 << 20;
    // Fully score one candidate (walking distance, fight estimate) and add it to the options.
    unsigned long long costliest = 0;  // combatBotv3: the dearest camp scored this turn
    auto scoreOne = [&](Candidate c) {
        // combatBotv2: past the first camp, stop before the rest of the turn runs out of fuel.
        // combatBotv3: leave room for one more camp as dear as the dearest so far (party fights read
        // each ally, so a camp can cost well over the 6k the cap assumes).
        const auto before = bc::fuel_remaining();
        if (scored > 0 && tune::farm_reserve > 0 && before < tune::farm_reserve + (tune::farm_reserve_adaptive ? costliest : 0)) return;
        const auto& g = *c.group;
        const auto& mem = w.groups[static_cast<std::size_t>(g.id)];
        const int reach = mem.status == GroupStatus::reward ? 0 : w.range;
        int travel = Navigator::unreachable;
        if (c.dash) {
            travel = std::max(0, dashTravel(w, g, c.nearest) - reach);
        } else if (const int d = fields::toGroup(g.id, w.pos); d >= 0) {
            if (tune::skip_field_unreachable && d >= Navigator::unreachable) return;  // no walking path (see above)
            // Precomputed field: one local read. Aim at the straight-line nearest tile.
            if (d < Navigator::unreachable) travel = std::max(0, d - reach);
            int nearestD = 1 << 20;
            for (auto t : g.tiles)
                if (dist(w.pos, t) < nearestD) { nearestD = dist(w.pos, t); c.nearest = t; }
        } else {
            for (auto t : g.tiles) {
                const int dt = nav().distTo(w, t);
                if (dt < Navigator::unreachable && std::max(0, dt - reach) < travel) {
                    travel = std::max(0, dt - reach);
                    c.nearest = t;
                }
            }
        }
        // Beyond the BFS radius: estimate from straight-line distance (walls add ~25%).
        if (travel >= Navigator::unreachable) travel = static_cast<int>(static_cast<float>(c.travel) * 1.25f) + 4;
        if (c.travel > tune::bfs_radius * 2) return;
        c.travel = travel;
        c.wait = mem.status == GroupStatus::respawning ? std::max(0, mem.respawnAt - w.round - travel) : 0;
        auto o = scoreGroup(w, c);
        if (c.wait == 0 && mem.status != GroupStatus::respawning && c.travel < nearestTravel) {
            nearestTravel = c.travel;
            nearestOut = static_cast<int>(out.size());
        }
        if (o.score > best) {
            // Waiting on help that has not claimed it: a solo option nearby may be better.
            bestNeedsHelp = o.kind == OptionKind::farm && o.need > 1 + comms::claimsOn(g.id, w.slot);
        }
        best = std::max(best, o.score);
        ++scored;
        out.push_back(std::move(o));
        if (const auto now = bc::fuel_remaining(); before > now) costliest = std::max(costliest, before - now);
    };
    // The nearest camp that is up is always scored too (it may earn the nearest-camp bonus).
    int nearestUp = -1;
    for (int i = 0, bestD = 1 << 20; i < n; ++i) {
        const auto& pc = pre[static_cast<std::size_t>(i)];
        if (pc.group && w.groups[static_cast<std::size_t>(pc.group->id)].status != GroupStatus::respawning &&
            !(rotateOnCapture(w) && w.groups[static_cast<std::size_t>(pc.group->id)].status == GroupStatus::reward &&
              allyHolding(w, pc.group->id)) &&
            pc.travel < bestD) {
            bestD = pc.travel;
            nearestUp = pc.group->id;
        }
    }
    // The route's and the loop's camps are always scored, whatever their bounds.
    // A gem camp an ally claims (it may be holding there for a party) is always scored too.
    int rallyGroup = -1;
    if (tune::gem_rally_bonus != 1.0f)
        for (int i = 0; i < n && rallyGroup < 0; ++i)
            if (const auto* pg = pre[static_cast<std::size_t>(i)].group; pg && pg->gems > 0 && comms::claimsOn(pg->id, w.slot) > 0) rallyGroup = pg->id;
    for (const int forced : {mapplan::routeGroup(), loop::target(), nearestUp, rallyGroup})
        if (forced >= 0)
            for (int i = 0; i < n; ++i)
                if (pre[static_cast<std::size_t>(i)].group && pre[static_cast<std::size_t>(i)].group->id == forced) {
                    auto c = pre[static_cast<std::size_t>(i)];
                    pre[static_cast<std::size_t>(i)].group = nullptr;
                    scoreOne(c);
                    break;
                }
    for (int round = 0; round < n; ++round) {
        // Selection instead of a full sort: we usually stop after a handful.
        int pick = -1;
        for (int i = 0; i < n; ++i)
            if (pre[static_cast<std::size_t>(i)].group && (pick < 0 || pre[static_cast<std::size_t>(i)].bound > pre[static_cast<std::size_t>(pick)].bound))
                pick = i;
        if (pick < 0) break;
        auto c = pre[static_cast<std::size_t>(pick)];
        pre[static_cast<std::size_t>(pick)].group = nullptr;
        // Stop when nothing left can win, and never score more than a handful (~6k fuel each).
        // Traced builds score a few more so the trace can show runners-up; submissions stop as soon
        // as no bound can beat the best (a bound is an upper limit on the score).
        const int minScored = kTracing ? tune::trace_candidates
                              : tune::rich_fuel > 0 && fuelLeft >= tune::rich_fuel ? tune::min_scored_rich : 1;
        if ((c.bound < best && scored >= minScored) || scored >= maxScored) break;

        scoreOne(c);
    }
    // The bound ignores whether we can take a camp: when every group it ranked first turned out
    // infeasible (corners' rich elk groups, "too strong" for one champion) the best score sat below
    // idling and Varg stood at spawn for 539 turns with a wolf 7 tiles away never scored. Then
    // also score the nearest groups not yet looked at.
    // Also when the best is a party camp nobody else is coming to (Cassi and Issac held at an elk
    // for 30+ turns on collision while the wolves they could solo were never scored).
    if (best < tune::idle_rescue_score || bestNeedsHelp)
        for (int extra = 0; extra < tune::idle_rescue_groups; ++extra) {
            int pick = -1;
            for (int i = 0; i < n; ++i)
                if (pre[static_cast<std::size_t>(i)].group &&
                    (pick < 0 || pre[static_cast<std::size_t>(i)].travel < pre[static_cast<std::size_t>(pick)].travel))
                    pick = i;
            if (pick < 0) break;
            auto c = pre[static_cast<std::size_t>(pick)];
            pre[static_cast<std::size_t>(pick)].group = nullptr;
            scoreOne(c);
        }
    // Gems-only mode: from the mid game, while the enemy has banked gems_only_lag more than us, the
    // whole team leaves gold-only camps for gem camps. Only when some gem camp is worth going to:
    // otherwise gold scored below idling and all six stood at spawn for 1,700 rounds (lanes), never
    // banking, so the mode never ended.
    if (w.gemsOnly() && !w.hunting()) {
        float bestGem = 0;
        const auto isCamp = [&](const Option& o) { return o.kind == OptionKind::farm || o.kind == OptionKind::capture; };
        for (std::size_t i = first; i < out.size(); ++i)
            if (isCamp(out[i]) && statics.groups[static_cast<std::size_t>(out[i].target)].gems > 0) bestGem = std::max(bestGem, out[i].score);
        if (bestGem >= tune::idle_rescue_score)
            for (std::size_t i = first; i < out.size(); ++i) {
                if (!isCamp(out[i])) continue;
                const auto& g = statics.groups[static_cast<std::size_t>(out[i].target)];
                if (g.gems == 0 && g.gold > 0) {
                    out[i].mul("behind on gems: gems only", tune::gems_only_factor);
                    out[i].finish();
                }
            }
    }
    if (nearestOut >= 0 && tune::nearest_camp_bonus != 1.0f) {
        auto& o = out[static_cast<std::size_t>(nearestOut)];
        o.mul("nearest camp", tune::nearest_camp_bonus);
        o.finish();
    }
}

} // namespace scorers
