#include "micro.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include "../combat/combat.h"
#include "../core/comms.h"
#include "../core/config.h"
#include "../core/fixed_str.h"
#include "../core/fuel.h"
#include "../core/statics.h"
#include "../core/trace.h"
#include "../nav/fields.h"
#include "../nav/navigator.h"
#include "../strategy/wards.h"

namespace bc = cambc;

namespace micro {

namespace {

constexpr int kFar = 1 << 20;

// Melee champion types (attack range 1), from the designs.
bool meleeDesign(bc::champion_type t) {
    static std::array<std::int8_t, 16> cache{};
    const auto i = static_cast<std::size_t>(t);
    if (i >= cache.size()) return false;
    if (cache[i] == 0) cache[i] = bc::champion_designs[i].base_stats().base_attack_range <= 1 ? 1 : 2;
    return cache[i] == 1;
}

float enemyDps(const EnemySeen& e) { return e.strength + e.focus * 1.5f / tune::spell_cooldown; }

int nearestEnemy(const World& w, bc::vec2 t) {
    int d = kFar;
    for (const auto& e : w.enemies)
        if (e.hp > 0) d = std::min(d, dist(t, e.pos));
    return d;
}

int bushNear(bc::vec2 p) {
    const int d = wards::bushDistance(p);
    return d < 0 ? 3 : std::min(d, 3);
}

// Context shared by every tile this turn.
struct Ctx {
    float D = 1;                 // our damage per turn: the unit every term is measured in
    int front = kFar;            // ranged: how near the enemy our front line stands (kFar: none)
    bool covered = false;        // a melee ally near us (we are not alone with them)
    bc::vec2 refuge{-1, -1};     // no cover: the nearest melee ally (or spawn) to fall back to
    int rangedFront = kFar;      // melee: how near the enemy our nearest ranged ally stands (kFar: none near)
    int meleeFront = kFar;       // melee: how near the enemy our other melee near us stand (kFar: none)
    std::array<bc::vec2, 8> allies{};
    int na = 0;
    bool canAttack = false, canMove = false;
    bc::vec2 lastPos{-100, -100};
    // Bush tiles within 3 of us that we cannot see into (any tile we score is within 2 of them),
    // listed once a turn: a 5x5 scan per scored tile cost up to ~20k on turns that overran.
    std::array<bc::vec2, 49> hidden{};
    int nh = 0;
};

// Distance from t to the nearest bush tile we cannot see into (3: none within 2; 0: t is a bush).
// The old exposure lesson (eight: Stellar and Cassi walked along the central bush wall ahead of
// our melee): anyone may wait in a bush we cannot see.
int hiddenBush(const Ctx& c, bc::vec2 t) {
    const int b = bushNear(t);
    if (b == 0 || b >= 3 || !tune::exposure_unseen_only) return b;
    int best = 3;
    for (int k = 0; k < c.nh; ++k) best = std::min(best, dist(t, c.hidden[static_cast<std::size_t>(k)]));
    return best;
}

struct Tile {
    bc::vec2 t;
    bool stay = false;
    // front: the positional penalty (ranged: ahead of our melee front; melee: behind our ranged, or
    // too far ahead of our other melee).
    float dealt = 0, next = 0, taken = 0, front = 0, bush = 0, approach = 0, refuge = 0, back = 0, edge = 0;
    float score = 0;
    int hitters = 0, friends = 1;  // enemies that can hit the tile next turn; us plus allies within 2 of it
    bool onTarget = false;         // melee: we can hit the chosen (or team) target from here this turn
    int tier = 0;                  // safety tier, compared before the score (higher is safer)
};

// Safety tiers, compared first (XSquare BC26 MicroManager.safe()): a tile that kills us, leaves us
// outnumbered, or lets them hit us while we cannot hit back loses to any safer tile, however much
// damage it scores.
enum Tier { kLethal = 0, kOutnumbered = 1, kFreeHit = 2, kSafe = 3 };

Ctx context(const World& w) {
    Ctx c;
    c.D = std::max(combat::championDamagePerTurn(w), 1.0f);
    auto& me = bc::self();
    c.canAttack = me.can_attack();
    c.canMove = me.can_move();
    const int slack = tune::pair_lives && w.pairChampion() ? tune::pair_front_slack : tune::ranged_front_slack;
    int refugeD = kFar;
    for (int s = 0; s < comms::alert_byte && s < w.allyCount; ++s) {
        if (s == w.slot) continue;
        const auto p = w.allyPosCached(s);
        if (p.x < 0) continue;
        if (c.na < static_cast<int>(c.allies.size())) c.allies[static_cast<std::size_t>(c.na++)] = p;
        const bool melee = meleeDesign(w.allyTypes[static_cast<std::size_t>(s)]);
        const int d = dist(p, w.pos);
        if (!w.ranged) {
            if (!melee && d <= tune::melee_cover_radius) c.rangedFront = std::min(c.rangedFront, nearestEnemy(w, p));
            if (melee && d <= tune::ranged_front_radius + 2) c.meleeFront = std::min(c.meleeFront, nearestEnemy(w, p));
            continue;
        }
        if (!melee) continue;
        if (d <= tune::ranged_front_radius) {
            c.covered = true;
            c.front = std::min(c.front, nearestEnemy(w, p));
        } else if (d < refugeD) {
            refugeD = d;
            c.refuge = p;
        }
    }
    if (c.front < kFar) c.front = std::max(0, c.front - slack);
    const auto& st = Statics::get();
    for (int dy = -3; dy <= 3; ++dy)
        for (int dx = -3; dx <= 3; ++dx) {
            const bc::vec2 p{w.pos.x + dx, w.pos.y + dy};
            if (p.x < 0 || p.y < 0 || p.x >= st.width || p.y >= st.height || bushNear(p) != 0 || w.isVisible(p)) continue;
            c.hidden[static_cast<std::size_t>(c.nh++)] = p;
        }
    if (!c.covered && c.refuge.x < 0) c.refuge = Statics::get().spawnCenter;
    // The tile we stepped from last round: stepping straight back undoes it (bouncing).
    static bc::vec2 prev{-100, -100};
    static int prevRound = -100;
    c.lastPos = prevRound == w.round - 1 && prev != w.pos ? prev : bc::vec2{-100, -100};
    prev = w.pos;
    prevRound = w.round;
    return c;
}

// Damage we can take on t next turn. Enemy melee step and hit in one turn (reach range + 1);
// enemy ranged choose, so only their range counts (rangeBot: a ranged champion kites a melee from
// distance 2 by stepping back). An enemy with an ally of ours nearer it, and in its reach, may hit
// that ally instead: half weight (the front-line lesson, in damage terms).
float takenAt(const World& w, const Ctx& c, bc::vec2 t, int* hitters) {
    float total = 0;
    for (const auto& e : w.enemies) {
        if (e.hp <= 0) continue;
        const int reach = e.range + (e.range <= 1 ? 1 : 0);
        const int d = dist(e.pos, t);
        if (d > reach) continue;
        if (!e.stunned) ++*hitters;
        float dps = enemyDps(e);
        if (e.stunned) dps *= tune::micro_stunned;
        for (int k = 0; k < c.na; ++k) {
            const int ad = dist(c.allies[static_cast<std::size_t>(k)], e.pos);
            if (ad < d && ad <= reach) {
                dps *= tune::micro_shared;
                break;
            }
        }
        total += dps;
    }
    if (w.danger.is_set(t)) total += w.incomingCampDamage(t);
    return total;
}

bool enemyInRangeOf(const World& w, bc::vec2 t, const Request& r, bool* target) {
    bool any = false;
    for (const auto& e : w.enemies) {
        if (e.hp <= 0 || dist(e.pos, t) > w.range) continue;
        any = true;
        if (target && ((r.target && e.handle == *r.target) || e.index == w.teamTarget)) *target = true;
    }
    return any;
}

void tier(const World& w, Tile& x) {
    if (!tune::micro_tiers) x.tier = kSafe;
    else if (x.taken * tune::micro_lethal_turns >= w.hp) x.tier = kLethal;
    else if (x.hitters > x.friends) x.tier = kOutnumbered;
    else if (x.hitters > 0 && x.dealt + x.next <= 0) x.tier = kFreeHit;
    else x.tier = kSafe;
}

// Melee walk and hit in one turn: every tile is "where do I hit from". Positional lessons:
// stand in front of our ranged (combatBotv3 melee_cover), and no more than safe_dash_slack tiles
// ahead of our other melee (combatBotv2 safe dash, eight: Issac dashed two tiles ahead of Ren with
// four of them arriving).
void scoreMelee(const World& w, const Ctx& c, const Request& r, Tile& x) {
    const float D = c.D;
    bool onTarget = false;
    const bool inRange = enemyInRangeOf(w, x.t, r, &onTarget);
    if (inRange && c.canAttack) x.dealt = D * (onTarget ? 1.0f + tune::micro_target_bonus : 1.0f);
    else if (inRange) x.next = D * tune::micro_next_shot;
    x.onTarget = inRange && onTarget && c.canAttack;
    x.taken = takenAt(w, c, x.t, &x.hitters);
    for (int k = 0; k < c.na; ++k) x.friends += dist(c.allies[static_cast<std::size_t>(k)], x.t) <= 2;
    // Their Cassi & Issac die together: Issac's own deaths count double (safe_dash's rule).
    const float pair = w.type == cambc::champion_type::issac ? 2.0f : 1.0f;
    const float hurt = pair * (1.0f + tune::micro_hurt_scale * std::clamp(1.0f - w.hpFrac(), 0.0f, 1.0f));
    const int nd = nearestEnemy(w, x.t);
    if (c.rangedFront < kFar && nd >= c.rangedFront) x.front += D * tune::micro_cover * static_cast<float>(nd - c.rangedFront + 1);
    if (c.meleeFront < kFar && nd < c.meleeFront - tune::safe_dash_slack)
        x.front += D * tune::micro_leash * static_cast<float>(c.meleeFront - tune::safe_dash_slack - nd);
    const int hb = hiddenBush(c, x.t);
    if (hb > 0 && hb < 3) x.bush = D * tune::micro_bush * static_cast<float>(3 - hb);
    if (x.t == c.lastPos) x.back = D * tune::micro_backtrack;
    // Not in reach of the target from here: closing in still counts.
    if (r.at.x >= 0 && dist(x.t, r.at) > w.range)
        x.approach = D * tune::micro_approach * static_cast<float>(dist(w.pos, r.at) - dist(x.t, r.at));
    tier(w, x);
    // Outnumbered is a cost for melee, not a veto: the brain already judged the fight (win chance,
    // team call), and backing out of contact lost a fifth of our melee's swings (xBot v3 A/B).
    const float outnumbered = x.tier == kOutnumbered ? D * tune::micro_melee_outnumbered * static_cast<float>(x.hitters - x.friends) : 0.0f;
    x.score = x.dealt + x.next + x.approach - hurt * tune::micro_taken * x.taken - x.front - x.bush - x.back - outnumbered;
}

void score(const World& w, const Ctx& c, const Request& r, Tile& x) {
    if (!w.ranged) return scoreMelee(w, c, r, x);
    const float D = c.D;
    bool onTarget = false;
    const bool inRange = enemyInRangeOf(w, x.t, r, &onTarget);
    if (x.stay && c.canAttack && inRange) x.dealt = D * (onTarget ? 1.0f + tune::micro_target_bonus : 1.0f);
    else if (inRange) x.next = D * tune::micro_next_shot;
    x.taken = takenAt(w, c, x.t, &x.hitters);
    for (int k = 0; k < c.na; ++k) x.friends += dist(c.allies[static_cast<std::size_t>(k)], x.t) <= 2;
    const float hurt = 1.0f + tune::micro_hurt_scale * std::clamp(1.0f - w.hpFrac(), 0.0f, 1.0f);
    const int nd = nearestEnemy(w, x.t);
    // In range, as far from them as our range allows (XSquare: the kite at the edge of range).
    if (inRange) x.edge = D * tune::micro_edge * static_cast<float>(std::min(nd, w.range));
    // Never nearer the enemy than our front line (rangeBot), per tile ahead of it.
    if (c.front < kFar && nd < c.front) x.front = D * tune::micro_front * static_cast<float>(c.front - nd);
    const int hb = hiddenBush(c, x.t);
    if (hb > 0 && hb < 3) x.bush = D * tune::micro_bush * static_cast<float>(3 - hb);
    if (x.t == c.lastPos) x.back = D * tune::micro_backtrack;
    // Engage: walk toward the target while nothing is in range (and only with a front to walk behind:
    // eight vs Ascendance, Cassi walked alone into their four).
    if (r.intent == Intent::engage && r.at.x >= 0 && (c.covered || !tune::ranged_fight_needs_front) &&
        !enemyInRangeOf(w, w.pos, r, nullptr)) {
        const int now = dist(w.pos, r.at), then = dist(x.t, r.at);
        if (now > w.range) x.approach = D * tune::micro_approach * static_cast<float>(now - then);
    }
    // No melee near (combatBotv2: Stellar stood near enemies with no melee within 6): fall back to one.
    if (!c.covered && c.refuge.x >= 0)
        x.refuge = D * tune::micro_refuge * static_cast<float>(dist(w.pos, c.refuge) - dist(x.t, c.refuge));
    x.score = x.dealt + x.next + x.edge + x.approach + x.refuge - hurt * tune::micro_taken * x.taken - x.front - x.bush - x.back;
    tier(w, x);
}

// Melee: only a lethal tile loses on safety alone; outnumbered and free-hit tiles compete on score.
int rank(bool ranged, const Tile& x) { return ranged ? x.tier : (x.tier == kLethal ? 0 : 1); }

bool better(bool ranged, const Tile& a, const Tile& b) {
    if (rank(ranged, a) != rank(ranged, b)) return rank(ranged, a) > rank(ranged, b);
    if (a.score != b.score) return a.score > b.score;
    if (a.stay != b.stay) return a.stay;  // no move for nothing
    // Toward home on ties (contest: Vesper Vale escaped deeper into their half).
    const int ha = fields::toSpawn(0, a.t), hb = fields::toSpawn(0, b.t);
    return ha >= 0 && hb >= 0 && ha < hb;
}

void traceChoice(const char* what, const Tile& best, const Tile& stay) {
    if constexpr (!kTracing) return;
    FixedStr<160> msg;
    auto i = [](float v) { return static_cast<int>(v); };
    msg << "micro " << what << ": " << best.t.x << ',' << best.t.y << (best.stay ? " stay" : " step") << " T" << best.tier << " s" << i(best.score)
        << " d" << i(best.dealt + best.next) << " t" << i(best.taken) << " f" << i(best.front) << " b" << i(best.bush)
        << " a" << i(best.approach + best.refuge);
    if (!best.stay) msg << " | stay T" << stay.tier << " s" << i(stay.score) << " t" << i(stay.taken);
    trace().note(msg.view());
}

} // namespace

bool applies(const World& w) {
    if (w.ranged ? !tune::micro_ranged : !tune::micro_melee) return false;
    const auto& st = Statics::get();
    for (const auto& e : w.enemies)
        if (e.hp > 0 && dist(e.pos, w.pos) <= w.range + tune::micro_radius &&
            dist(e.pos, st.enemySpawnCenter) > tune::enemy_spawn_radius)
            return true;
    return false;
}

bool run(World& w, const Request& r) {
    // Its own fuel section: the time before it goes to execute, its own work to micro.
    fuel::mark("execute");
    struct Mark { ~Mark() { fuel::mark("micro.act"); } } mark;  // the move and attack after the choice
    const Ctx c = context(w);
    const auto& st = Statics::get();
    std::array<Tile, 9> tiles;
    int n = 0;
    tiles[static_cast<std::size_t>(n)].t = w.pos;
    tiles[static_cast<std::size_t>(n++)].stay = true;
    if (c.canMove) {
        const auto blocked = nav().blocked(w);
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                const bc::vec2 t{w.pos.x + dx, w.pos.y + dy};
                if (t.x < 0 || t.y < 0 || t.x >= st.width || t.y >= st.height || blocked.is_set(t)) continue;
                tiles[static_cast<std::size_t>(n++)].t = t;
            }
    }
    for (int k = 0; k < n; ++k) score(w, c, r, tiles[static_cast<std::size_t>(k)]);
    const Tile& stay = tiles[0];
    // Melee in contact: when a tile that is not lethal reaches the chosen (or team) target, we hit it
    // from one of them (focus fire: hitting whoever was nearest halved our melee's kills); else, when
    // some non-lethal tile reaches anyone, we hit from one of those.
    bool hitOnly = false, targetOnly = false;
    if (!w.ranged)
        for (int k = 0; k < n; ++k) {
            const auto& x = tiles[static_cast<std::size_t>(k)];
            hitOnly = hitOnly || (x.dealt > 0 && x.tier != kLethal);
            targetOnly = targetOnly || (x.onTarget && x.tier != kLethal);
        }
    auto eligible = [&](const Tile& x) {
        if (targetOnly) return x.onTarget && x.tier != kLethal;
        return !hitOnly || (x.dealt > 0 && x.tier != kLethal);
    };
    int bi = -1;
    for (int k = 0; k < n; ++k)
        if (eligible(tiles[static_cast<std::size_t>(k)]) &&
            (bi < 0 || better(w.ranged, tiles[static_cast<std::size_t>(k)], tiles[static_cast<std::size_t>(bi)])))
            bi = k;
    const Tile& best = tiles[static_cast<std::size_t>(bi)];
    fuel::mark("micro");  // scoring the tiles
    // Melee with no enemy in reach from any tile: the approach (path search, safe dash) stays with
    // the fight code; micro only picks the tile we hit from.
    if (!w.ranged) {
        bool contact = false;
        for (int k = 0; k < n; ++k) contact = contact || tiles[static_cast<std::size_t>(k)].dealt + tiles[static_cast<std::size_t>(k)].next > 0;
        if (!contact) return false;
    }

    combat::Focus focus{.enemy = r.target};
    focus.anyEnemy = true;
    if (r.intent == Intent::hold) {
        // Hold: shoot from here unless a neighbour is clearly better; with nothing to shoot and no
        // clearly better tile, the plan's own walk runs.
        const bool step = !best.stay && (best.tier > stay.tier || best.score - stay.score >= c.D * tune::micro_hold_gain);
        if (!step) {
            if (stay.dealt <= 0) return false;
            traceChoice("hold", stay, stay);
            combat::act(w, focus, true);
            return true;
        }
    }
    traceChoice(r.intent == Intent::hold ? "hold" : w.ranged ? "fight" : "melee", best, stay);
    if (best.stay || !nav().moveTo(w, best.t)) {
        combat::act(w, focus, true);
        return true;
    }
    combat::act(w, focus, false);
    return true;
}

} // namespace micro
