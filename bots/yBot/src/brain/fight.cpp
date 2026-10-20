// Fighting enemy champions when we expect to win: their bag and the kill gold
// are the reward, time-to-kill (plus the walk) is the cost.
#include <algorithm>
#include <array>
#include <cmath>
#include "../combat/combat.h"
#include "../core/comms.h"
#include "../core/config.h"
#include "../core/trace.h"
#include "../nav/navigator.h"
#include "../strategy/cohesion.h"
#include "../strategy/regroup.h"
#include "nnfight.h"
#include "nnpolicy.h"
#include "scorers.h"

namespace scorers {

namespace {
// yBot: the fight network's inputs (lab/nn/fight_engage_dataset.py, --bot set) around the spot
// between us and the target. Shards owned = (stat - base) / growth for the champion's type.
struct Shards { float str = 0, foc = 0, hp = 0; };
Shards shardsOf(int type, float strength, float focus, float maxHp) {
    static std::array<cambc::champion_stats, 16> base{};
    static std::array<bool, 16> loaded{};
    if (type < 0 || type >= 16 || static_cast<std::size_t>(type) >= cambc::champion_designs.size()) return {};
    const auto t = static_cast<std::size_t>(type);
    if (!loaded[t]) {
        loaded[t] = true;
        try {
            base[t] = cambc::champion_designs[t].base_stats();
        } catch (const cambc::error&) {
        }
    }
    const auto& b = base[t];
    auto n = [](float v, float b0, float g) { return g > 0 ? std::max(0.0f, (v - b0) / g) : 0.0f; };
    return {n(strength, b.base_strength, b.strength_growth), n(focus, b.base_focus, b.focus_growth), n(maxHp, b.base_health, b.health_growth)};
}

// The local picture around our own tile shared by the fight and policy networks: the first 46
// inputs of each (fight_engage_dataset.side_feats without the fog-hidden totals).
constexpr int kSide = 46;
void sideFeatures(const World& w, std::array<float, kSide>& x) {
    const cambc::vec2 m = w.pos;
    // Indices into x, in the trainer's column order.
    enum { us8, them8, us14, them14, hpUs8, hpThem8, maxUs8, maxThem8, hpUs14, hpThem14, fracUs8, fracThem8, bagUs8, bagThem8,
           strUs8, focUs8, hpShUs8, strThem8, focThem8, hpShThem8, dOur, dTheir, types, end = types + 24 };
    static_assert(end == kSide);
    auto add = [&](bool ours, cambc::vec2 p, float hp, float maxHp, float str, float foc, int gems, int type) {
        const int d = dist(p, m);
        if (d > 14) return;
        const int o = ours ? 0 : 1;
        x[static_cast<std::size_t>(us14 + o)] += 1;
        x[static_cast<std::size_t>(hpUs14 + o)] += hp / 1000.0f;
        if (d > 8) return;
        x[static_cast<std::size_t>(us8 + o)] += 1;
        x[static_cast<std::size_t>(hpUs8 + o)] += hp / 1000.0f;
        x[static_cast<std::size_t>(maxUs8 + o)] += maxHp / 1000.0f;
        x[static_cast<std::size_t>(fracUs8 + o)] += hp / std::max(maxHp, 1.0f);
        x[static_cast<std::size_t>(bagUs8 + o)] += static_cast<float>(gems) / 50.0f;
        if (type >= 0) {
            const auto sh = shardsOf(type, str, foc, maxHp);
            x[static_cast<std::size_t>((ours ? strUs8 : strThem8) + 0)] += sh.str / 50.0f;
            x[static_cast<std::size_t>((ours ? strUs8 : strThem8) + 1)] += sh.foc / 50.0f;
            x[static_cast<std::size_t>((ours ? strUs8 : strThem8) + 2)] += sh.hp / 50.0f;
            if (type < 12) x[static_cast<std::size_t>(types + 2 * type + o)] += 1;
        }
    };
    add(true, w.pos, w.hp, w.maxHp, w.strength, w.focus, w.gems, static_cast<int>(w.type));
    for (int i = 0; i < w.allyCount && i < 8; ++i) {
        if (i == w.slot) continue;
        const auto p = w.allyPosCached(i);
        if (p.x < 0 || dist(p, m) > 14) continue;
        const auto a = w.allyStats(i);
        if (a.alive) add(true, p, a.hp, a.maxHp, a.strength, a.focus, a.gems, static_cast<int>(w.allyTypes[static_cast<std::size_t>(i)]));
        else add(true, p, 1000.0f, 1000.0f, 0, 0, 0, -1);  // out of vision: counted, stats unknown
    }
    for (const auto& e : w.enemies)
        if (e.hp > 0) add(false, e.pos, e.hp, e.maxHp, e.strength, e.focus, e.gems, e.type);
    // Enemies seen in the last two rounds and gone from view still count.
    for (const auto& g : w.ghosts) {
        const int age = w.round - g.round;
        if (age <= 0 || age > 2) continue;
        add(false, g.pos, 1000.0f, 1000.0f, 0, 0, 0, -1);
    }
    for (int o = 0; o < 2; ++o)
        if (x[static_cast<std::size_t>(us8 + o)] > 0) x[static_cast<std::size_t>(fracUs8 + o)] /= x[static_cast<std::size_t>(us8 + o)];
    const auto& st = Statics::get();
    const float D = static_cast<float>(std::max(dist(st.spawnCenter, st.enemySpawnCenter), 1));
    x[dOur] = static_cast<float>(dist(m, st.spawnCenter)) / D;
    x[dTheir] = static_cast<float>(dist(m, st.enemySpawnCenter)) / D;
}

// Around our own tile, once per turn (per enemy cost up to ~23k fuel on fight turns).
float nnFightBadLogit(const World& w) {
    static_assert(nnfight::kIn == kSide + 1);
    std::array<float, kSide> side{};
    sideFeatures(w, side);
    std::array<float, nnfight::kIn> x{};
    std::copy(side.begin(), side.end(), x.begin());
    x[kSide] = static_cast<float>(w.round) / 1000.0f;
    return nnfight::logit(x);
}
} // namespace

std::array<float, 4> nnPolicy(const World& w) {
    // Top teams' fight policy (lab/nn/train_policy.py): hold / engage / retreat / recall here
    // (all -1: no enemy within 6). Once per turn.
    static int round = -1;
    static std::array<float, 4> p{-1, -1, -1, -1};
    if (round == w.round) return p;
    round = w.round;
    p = {-1, -1, -1, -1};
    const EnemySeen* near = nullptr;
    for (const auto& e : w.enemies)
        if (e.hp > 0 && dist(e.pos, w.pos) <= 6 && (!near || dist(e.pos, w.pos) < dist(near->pos, w.pos))) near = &e;
    if (!near) return p;
    static_assert(nnpolicy::kIn == kSide + 9 + 1 + 12);
    std::array<float, kSide> side{};
    sideFeatures(w, side);
    std::array<float, nnpolicy::kIn> x{};
    std::copy(side.begin(), side.end(), x.begin());
    const auto sh = shardsOf(static_cast<int>(w.type), w.strength, w.focus, w.maxHp);
    float* q = x.data() + kSide;
    q[0] = w.hp / std::max(w.maxHp, 1.0f);
    q[1] = w.maxHp / 1000.0f;
    q[2] = static_cast<float>(w.gems) / 50.0f;
    q[3] = sh.str / 50.0f;
    q[4] = sh.foc / 50.0f;
    q[5] = sh.hp / 50.0f;
    q[6] = static_cast<float>(dist(near->pos, w.pos)) / 6.0f;
    q[7] = near->hp / std::max(near->maxHp, 1.0f);
    q[8] = near->maxHp / 1000.0f;
    q[9] = static_cast<float>(w.round) / 1000.0f;
    if (const int t = static_cast<int>(w.type); t >= 0 && t < 12) q[10 + t] = 1.0f;
    p = nnpolicy::probs(x);
    return p;
}

float nnPolicyLeave(const World& w) {
    const auto p = nnPolicy(w);
    return p[0] < 0 ? -1.0f : p[2] + p[3];
}

float nnFightBad(const World& w) {
    // Once per turn (fight scoring and the retreat check share it); -1 with no enemy within 8 tiles.
    static int round = -1;
    static float p = -1.0f;
    if (round == w.round) return p;
    round = w.round;
    p = -1.0f;
    for (const auto& e : w.enemies)
        if (e.hp > 0 && dist(e.pos, w.pos) <= 8) {
            p = 1.0f / (1.0f + std::exp(-nnFightBadLogit(w)));
            break;
        }
    return p;
}

void fight(World& w, std::vector<Option>& out) {
    const float myDps = std::max(combat::championDamagePerTurn(w), 1.0f);
    // xBot: fight scoring cost ~25k on turns that overran. Score only the nearest few enemies (plus
    // the team's target and anyone channelling, which always matter), and find cleared camps once.
    std::array<bool, 16> scored{};
    {
        std::array<std::pair<int, int>, 16> byDist{};
        int n = 0;
        for (int i = 0; i < static_cast<int>(w.enemies.size()) && n < 16; ++i)
            byDist[static_cast<std::size_t>(n++)] = {dist(w.pos, w.enemies[static_cast<std::size_t>(i)].pos), i};
        std::sort(byDist.begin(), byDist.begin() + n);
        for (int k = 0; k < n; ++k) {
            const auto& e = w.enemies[static_cast<std::size_t>(byDist[static_cast<std::size_t>(k)].second)];
            scored[static_cast<std::size_t>(byDist[static_cast<std::size_t>(k)].second)] =
                tune::fight_score_max <= 0 || k < tune::fight_score_max || e.channeling || (e.index >= 0 && e.index == w.teamTarget);
        }
    }
    // yBot: the fight network, once this turn, only with an enemy within 8 tiles (-1: not computed).
    const float pBadNow = tune::nn_fight ? nnFightBad(w) : -1.0f;
    std::array<const GroupInfo*, 16> cleared{};
    int nCleared = 0;
    for (const auto& g : Statics::get().groups)
        if (nCleared < 16 && w.groups[static_cast<std::size_t>(g.id)].status == GroupStatus::reward) cleared[static_cast<std::size_t>(nCleared++)] = &g;
    for (std::size_t ei = 0; ei < w.enemies.size(); ++ei) {
        if (ei < scored.size() && !scored[ei]) continue;
        const auto& e = w.enemies[ei];
        // On or by its own spawn it heals every round: scored at x0.05 anyway, so skip the work
        // (rushbait-v2: scoring five enemies standing on their spawn cost ~30k fuel every turn).
        if (tune::fight_skip_at_spawn && !e.channeling && dist(e.pos, Statics::get().enemySpawnCenter) <= tune::enemy_spawn_radius) continue;
        // Fights are team against team: the target plus enemies close enough to join it, against
        // us plus allies near it or already attacking it (shared focus). Each side's time to wipe
        // the other decides who wins; we all hit the target, so it falls in e.hp / our damage.
        float theirHp = 0, theirDps = 0, theirMaxHp = 0;
        int theirCount = 0, theirBags = 0, theirStuns = 0;
        for (const auto& o : w.enemies)
            if (o.hp > 0 && dist(o.pos, e.pos) <= tune::fight_join_radius) {
                theirHp += o.hp;
                theirMaxHp += o.maxHp;
                theirBags += o.gems;
                theirStuns += o.stun ? 1 : 0;
                theirDps += std::max(o.strength + o.focus * 1.5f / tune::spell_cooldown, 1.0f);
                ++theirCount;
            }
        // Enemies seen in the last few rounds but not now can still join (out of sight, in bushes,
        // walking in): in 16 of 23 traced centre deaths after a fight we judged 3v1 to 5v2, four or
        // five enemies took part in the kill. Count those that could have reached the target since.
        int ghostsJoining = 0;
        if (tune::fight_count_ghosts)
            for (const auto& g : w.ghosts) {
                const int age = w.round - g.round;
                if (age <= 0 || age > tune::ghost_join_age || dist(g.pos, e.pos) > tune::fight_join_radius + age) continue;
                theirHp += tune::ghost_hp;
                theirDps += std::max(g.dps, 1.0f);
                ++theirCount;
                ++ghostsJoining;
            }
        // Monsters fight for them too: the camp hits we are already taking, or the camps around the
        // target that whoever fights there may pull (35% of our deaths to champions came after
        // monster hits in the 15 rounds before, 23% of theirs).
        float monsters = 0;
        if (tune::fight_monster_damage) {
            monsters = std::max(w.incoming, tune::fight_spot_camp_weight * w.incomingCampDamage(e.pos));
            theirDps += monsters;
        }
        auto ours = w.ourSide(e.pos, tune::fight_join_radius + 1, e.index, myDps);
        // Measured on traced fights: the target loses ~0.2x the damage per round our side's
        // stats promise (allies not all on it, targets stepping out of range, lost turns).
        ours.dps *= tune::fight_our_dps_scale;
        const float turnsToKill = e.hp / ours.dps;
        const float edge = (ours.hp / theirDps) / std::max(theirHp / ours.dps, 0.1f);
        // We are the one they can reach first: alone in front, we may die before the target does.
        const float myTurnsToDie = w.hp / theirDps;

        Option o;
        o.kind = OptionKind::fight;
        o.enemy = e.handle;
        o.target = e.index;
        o.at = e.pos;
        o.c.target << 'e' << static_cast<int>(static_cast<cambc::handle_champion_t>(e.handle));
        o.add("their bag", static_cast<float>(e.gems));
        o.add("kill gold", static_cast<float>(cambc::constants::kill_gold) * goldValue(w));
        // Standing on a cleared camp's spawn tiles, it blocks our capture: killing it frees the reward.
        for (int k = 0; k < nCleared; ++k) {
            const auto& g = *cleared[static_cast<std::size_t>(k)];
            if (dist(g.center, e.pos) <= 3 && Statics::get().onGroup(g, e.pos)) {
                o.add("frees capture", static_cast<float>(g.gems) + static_cast<float>(g.gold) * goldValue(w));
                break;
            }
        }
        o.turns("travel", static_cast<float>(std::max(0, dist(w.pos, e.pos) - w.range)));
        // Do not chase: an enemy this far off is leaving, and the walk is time not farmed. A
        // channelling enemy (tribute, recall) is standing still, so it is still worth the walk.
        if (dist(w.pos, e.pos) > tune::chase_radius && !e.channeling) o.mul("too far to chase", tune::chase_factor);
        // Commit only to a fight we can start soon: the target within a couple of steps of our reach,
        // hitting us, or coming at us. Otherwise keep farming (in traced fights two thirds of our
        // fight turns were spent 4+ tiles from the target, and 77-87% of fight turns hit nothing).
        if (tune::fight_commit_gap >= 0 && !e.channeling) {
            const int gap = dist(w.pos, e.pos) - w.range;
            const bool hittingUs = dist(e.pos, w.pos) <= e.range + 1;
            if (gap > tune::fight_commit_gap && !hittingUs && !w.enemyApproaching(e.index, e.pos))
                o.mul("out of reach: keep farming", tune::fight_far_factor);
        }
        o.turns("kill time", turnsToKill);
        // Never exactly zero: a losing fight still shows up (with a tiny score) when you inspect
        // the decision, so it is visibly considered and rejected.
        o.mul("win margin", std::clamp((edge - 1.0f) / (tune::fight_win_margin - 1.0f), 0.02f, 1.5f));
        if (ours.count == 1 && myTurnsToDie < turnsToKill) o.mul("die first", 0.3f);
        // Mid to late game, a lost fight costs the bag we have built up: only take fights we are
        // likely to win (mode 0), or whose expected gain beats the expected loss (mode 1). Win chance from the edge, fitted on traced fights from round 300 (a win:
        // an enemy dies near the target before this champion does, within 15 rounds); it levels off
        // near 70-75%, so a 70% cutoff keeps only lopsided fights (edge ~7+).
        // Win chance (first death near the fight is theirs). Model 0: the first edge curve; 1: the edge
        // curve refit on 1,800 self-play games; 2: eight features fitted on the same fights (held-out
        // AUC 0.705 vs 0.662 for edge alone).
        const float lnEdge = std::log(std::max(edge, 0.05f));
        float z;
        if (tune::fight_win_model == 2) {
            z = tune::fwm_bias + tune::fwm_ln_edge * lnEdge +
                tune::fwm_ln_hp * std::log(std::max(ours.hp, 1.0f) / std::max(theirHp, 1.0f)) +
                tune::fwm_ln_dps * std::log(std::max(ours.dps, 1.0f) / std::max(theirDps, 1.0f)) +
                tune::fwm_lead * static_cast<float>(ours.count - theirCount) + tune::fwm_them * static_cast<float>(theirCount) +
                tune::fwm_their_hp * (theirHp / std::max(theirMaxHp, 1.0f)) + tune::fwm_target_hp * (e.hp / std::max(e.maxHp, 1.0f)) +
                tune::fwm_dist * static_cast<float>(dist(w.pos, e.pos));
        } else if (tune::fight_win_model == 1) {
            z = tune::fight_win_refit_a + tune::fight_win_refit_b * lnEdge;
        } else {
            z = tune::fight_win_fit_a + tune::fight_win_fit_b * lnEdge;
        }
        const float winChance = 1.0f / (1.0f + std::exp(-z));
        if (w.round >= tune::fight_mid_round && !e.channeling && tune::fight_mid_factor != 1.0f) {
            if (tune::fight_mid_mode == 1) {
                // Pot odds: what a win gains (their bag, the kill gold, them out for 60 rounds)
                // against what a loss costs (our bag, us out for 60 rounds). Carrying a big bag we
                // walk away from even fights; against a full bag a long shot can still pay.
                const float gain = static_cast<float>(e.gems) + static_cast<float>(cambc::constants::kill_gold) * goldValue(w) + tune::kill_denial_value;
                const float stake = static_cast<float>(w.gems) + tune::death_time_value;
                if (winChance * gain <= (1.0f - winChance) * stake) o.mul("mid game: bad odds", tune::fight_mid_factor);
                o.c.note << "odds " << static_cast<int>(winChance * gain) << " vs " << static_cast<int>((1.0f - winChance) * stake) << "; ";
            } else if (winChance < tune::fight_mid_win_chance) {
                o.mul("mid game: under win cutoff", tune::fight_mid_factor);
            }
            o.c.note << "win " << static_cast<int>(winChance * 100.0f) << "%; ";
        }
        // yBot: the fight network (learned on 490 ladder games, every team): the chance we lose more
        // champions than they do near here within 15 rounds. Risky fights score down.
        if (tune::nn_fight && !e.channeling && pBadNow >= 0) {
            const float pBad = pBadNow;
            o.c.note << "nn bad " << static_cast<int>(pBad * 100.0f) << "%; ";
            if (pBad > tune::nn_fight_bad) o.mul("nn: risky fight", tune::nn_fight_factor);
        }
        // yBot: where the top teams would press (distilled policy), press: 32% of our contact rows
        // held or backed off where they engage, and those spots were safe (5-7% deaths in 10 rounds).
        if (tune::nn_policy_engage > 0 && !e.channeling) {
            const auto pol = nnPolicy(w);
            if (pol[1] >= tune::nn_policy_engage) o.mul("nn: top teams press", tune::nn_policy_engage_bonus);
        }
        if (comms::focusMask(e.index, w.slot) != 0) o.mul("focus fire", tune::focus_fire_bonus);
        else if (e.index == w.teamTarget) o.mul("team target", tune::team_target_bonus);
        // A hurt enemy is worth chasing beyond the shorter kill time: it is about to recall (and
        // take its bag home), and a kill now removes a body from the next fight.
        const float missing = std::clamp(1.0f - e.hp / std::max(e.maxHp, 1.0f), 0.0f, 1.0f);
        if (missing > 0.05f) o.mul("enemy hurt", 1.0f + tune::fight_hurt_bonus * missing);
        if (e.channeling) o.mul("interrupt channel", 1.5f);
        // Gems-only mode (we are behind): fight harder, but only as a group: 2+ of us near the target
        // and at least as many of us as of them there. A lone champion's fights are unchanged.
        if (w.gemsOnly() && tune::gems_only_group_fight != 1.0f) {
            int us = 1, them = 0;
            for (const auto& a : w.allies)
                if (dist(a.pos, e.pos) <= tune::favour_radius) ++us;
            for (const auto& x : w.enemies)
                if (x.hp > 0 && dist(x.pos, e.pos) <= tune::favour_radius) ++them;
            if (us >= 2 && us >= them) o.mul("behind: group fight", tune::gems_only_group_fight);
        }
        // No chasing: an enemy out of our reach and moving away is leaving; let it go.
        if (tune::no_chase_factor != 1.0f && !e.channeling && dist(w.pos, e.pos) > w.range && w.enemyMovingAway(e.index, e.pos))
            o.mul("it is leaving", tune::no_chase_factor);
        if (!w.mayInvade() && !e.channeling && w.theirHalf(e.pos)) o.mul("their half: no lead", tune::no_invade_factor);
        // An enemy already in our range is self-defence, wherever we stand.
        if (dist(e.pos, w.pos) > w.range && w.invadeWithoutMelee(e.pos)) o.mul("their half: no melee with us", tune::invade_no_melee_factor);
        // Fight as a group: with less than half the active team around us, leave it unless the
        // target is channelling or is the one hitting us.
        if (tune::cohesion && !e.channeling && cohesion::activeCount() >= 3) {
            const int withUs = 1 + w.alliesWithin(w.pos, tune::favour_radius);
            const bool hittingUs = w.beingHit && w.incoming <= 0 && dist(e.pos, w.pos) <= e.range;
            if (withUs * 2 < cohesion::activeCount() && !hittingUs) o.mul("group not here", tune::cohesion_alone_fight);
        }
        // After the early game, fights on their half are where we die (lanes: nearly half our
        // mid-game deaths, after chasing or fighting there). Farm instead. A channelling enemy and a
        // raid (their team down) are still worth it.
        const bool raiding = w.enemyBodies >= 2 && w.enemiesAlive <= tune::raid_max_alive && w.enemyRespawnAt > w.round;
        if (w.round >= tune::their_half_fight_round && !e.channeling && !raiding &&
            dist(e.pos, Statics::get().enemySpawnCenter) < dist(e.pos, Statics::get().spawnCenter))
            o.mul("fight on their half", tune::their_half_fight_factor);
        // Favourable fight: more of us than them around the target (allies walking to an alert
        // there count too, before we can see them). Then a kill is worth more than its gold: the
        // enemy is out for 60 rounds. Healthier means bolder: x1 at half health up to x2 at full.
        int near = 0, enemiesNear = 0;
        for (const auto& a : w.allies)
            if (dist(a.pos, e.pos) <= tune::favour_radius) ++near;
        for (const auto& x : w.enemies)
            if (x.hp > 0 && dist(x.pos, e.pos) <= tune::favour_radius) ++enemiesNear;
        enemiesNear += ghostsJoining;
        const int onTheWay = regroup::alertNear(e.pos, tune::regroup_leash + tune::favour_radius) ? comms::regroupers(w.slot) : 0;
        const int bodies = 1 + std::max(near, onTheWay);  // visible ones may be among those on the way
        const bool atTheirSpawn = dist(e.pos, Statics::get().enemySpawnCenter) <= tune::enemy_spawn_radius;
        if (bodies > enemiesNear && !atTheirSpawn) {
            o.add("kill denies them", tune::kill_denial_value);
            o.mul("favourable fight", 1.0f + (tune::favourable_max - 1.0f) * std::clamp((w.hpFrac() - 0.5f) / 0.5f, 0.0f, 1.0f));
            o.c.note << "favourable " << bodies << "v" << enemiesNear << "; ";
        }
        // An enemy on or near its own spawn heals fully every round: chasing it there only feeds
        // their team (casters chased retreating enemies into interlock's corner and died).
        if (dist(e.pos, Statics::get().enemySpawnCenter) <= tune::enemy_spawn_radius) o.mul("at their spawn", 0.05f);
        // Party engage: allies with us and more of us here than of them around the target. Everyone
        // there sees the same numbers, so the party commits together (focus fire keeps one target).
        if (tune::party_engage && !atTheirSpawn) {
            int withMe = 1, alliesNear = 0, them = 0;
            for (int s = 0; s < w.allyCount; ++s) {
                if (s == w.slot) continue;
                const auto p = w.allyPosCached(s);
                if (p.x >= 0 && dist(p, w.pos) <= tune::engage_radius) { ++withMe; ++alliesNear; }
            }
            for (const auto& x : w.enemies)
                if (x.hp > 0 && dist(x.pos, e.pos) <= tune::engage_radius) ++them;
            if (alliesNear > 0 && withMe > them) {
                o.mul("party engage", tune::engage_bonus);
                o.c.note << "engage " << withMe << "v" << them << "; ";
            }
        }
        // combatBotv3: fight as a group. Fewer than group_fight_min of us around the target (allies
        // near it or beside us), and it is neither channelling nor able to hit us: leave it.
        if (tune::group_fight_alone_factor != 1.0f && !e.channeling && dist(e.pos, w.pos) > e.range) {
            int bodies = 1;
            for (int sl = 0; sl < w.allyCount && sl < 8; ++sl) {
                if (sl == w.slot) continue;
                const auto p = w.allyPosCached(sl);
                if (p.x >= 0 && (dist(p, e.pos) <= tune::group_fight_radius || dist(p, w.pos) <= 3)) ++bodies;
            }
            if (bodies < tune::group_fight_min) o.mul("no group with us", tune::group_fight_alone_factor);
        }
        o.mul("role appetite", w.role().fightAppetite);
        // combatBotv3: mid-capture, an enemy we cannot already hit does not pull us off the tiles.
        if (dist(e.pos, w.pos) > w.range && captureHold(w)) o.mul("finishing a capture", tune::capture_hold_fight_factor);
        o.c.note << ours.count << "v" << theirCount << ": kill in " << static_cast<int>(turnsToKill) << ", edge x10 "
                 << static_cast<int>(edge * 10.0f);
        o.finish();
        if constexpr (kTracing) {
            // Lab fight data: keep the features of the best-scored fight this turn.
            auto& f = w.bestFight;
            if (f.round != w.round || o.score > f.score) {
                int ghosts = 0;
                for (const auto& g : w.ghosts) {
                    const int age = w.round - g.round;
                    if (age > 0 && age <= 5 && dist(g.pos, e.pos) <= tune::fight_join_radius + age) ++ghosts;
                }
                f = {w.round, e.index, ours.count, theirCount, ghosts, e.gems, theirBags, theirStuns, e.pos.x, e.pos.y, dist(w.pos, e.pos),
                     o.score, edge, ours.hp, theirHp, ours.dps, theirDps, e.hp / std::max(e.maxHp, 1.0f),
                     theirHp / std::max(theirMaxHp, 1.0f), monsters, winChance};
            }
        }
        out.push_back(std::move(o));
    }
}

} // namespace scorers
