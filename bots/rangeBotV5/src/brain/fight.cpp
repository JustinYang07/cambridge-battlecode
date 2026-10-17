// Fighting enemy champions when we expect to win: their bag and the kill gold
// are the reward, time-to-kill (plus the walk) is the cost.
#include <algorithm>
#include "../combat/combat.h"
#include "../core/comms.h"
#include "../core/config.h"
#include "../nav/navigator.h"
#include "../strategy/cohesion.h"
#include "../strategy/regroup.h"
#include "scorers.h"

namespace scorers {

void fight(World& w, std::vector<Option>& out) {
    const float myDps = std::max(combat::championDamagePerTurn(w), 1.0f);
    for (const auto& e : w.enemies) {
        // On or by its own spawn it heals every round: scored at x0.05 anyway, so skip the work
        // (rushbait-v2: scoring five enemies standing on their spawn cost ~30k fuel every turn).
        if (tune::fight_skip_at_spawn && !e.channeling && dist(e.pos, Statics::get().enemySpawnCenter) <= tune::enemy_spawn_radius) continue;
        // Fights are team against team: the target plus enemies close enough to join it, against
        // us plus allies near it or already attacking it (shared focus). Each side's time to wipe
        // the other decides who wins; we all hit the target, so it falls in e.hp / our damage.
        float theirHp = 0, theirDps = 0;
        int theirCount = 0;
        for (const auto& o : w.enemies)
            if (o.hp > 0 && dist(o.pos, e.pos) <= tune::fight_join_radius) {
                theirHp += o.hp;
                theirDps += std::max(o.strength + o.focus * 1.5f / tune::spell_cooldown, 1.0f);
                ++theirCount;
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
        o.add("kill gold", tune::kill_gold_value * goldValue(w));
        // Standing on a cleared camp's spawn tiles, it blocks our capture: killing it frees the reward.
        for (const auto& g : Statics::get().groups)
            if (w.groups[static_cast<std::size_t>(g.id)].status == GroupStatus::reward && dist(g.center, e.pos) <= 3 &&
                g.tileMask.is_set(e.pos)) {
                o.add("frees capture", static_cast<float>(g.gems) + static_cast<float>(g.gold) * goldValue(w));
                break;
            }
        o.turns("travel", static_cast<float>(std::max(0, dist(w.pos, e.pos) - w.range)));
        // Do not chase: an enemy this far off is leaving, and the walk is time not farmed. A
        // channelling enemy (tribute, recall) is standing still, so it is still worth the walk.
        if (dist(w.pos, e.pos) > tune::chase_radius && !e.channeling) o.mul("too far to chase", tune::chase_factor);
        o.turns("kill time", turnsToKill);
        // Never exactly zero: a losing fight still shows up (with a tiny score) when you inspect
        // the decision, so it is visibly considered and rejected.
        o.mul("win margin", std::clamp((edge - 1.0f) / (tune::fight_win_margin - 1.0f), 0.02f, 1.5f));
        if (ours.count == 1 && myTurnsToDie < turnsToKill) o.mul("die first", 0.3f);
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
                const auto p = w.allyPos(s);
                if (p.x >= 0 && dist(p, w.pos) <= tune::engage_radius) { ++withMe; ++alliesNear; }
            }
            for (const auto& x : w.enemies)
                if (x.hp > 0 && dist(x.pos, e.pos) <= tune::engage_radius) ++them;
            if (alliesNear > 0 && withMe > them) {
                o.mul("party engage", tune::engage_bonus);
                o.c.note << "engage " << withMe << "v" << them << "; ";
            }
        }
        o.mul("role appetite", w.role().fightAppetite);
        o.c.note << ours.count << "v" << theirCount << ": kill in " << static_cast<int>(turnsToKill) << ", edge x10 "
                 << static_cast<int>(edge * 10.0f);
        o.finish();
        out.push_back(std::move(o));
    }
}

} // namespace scorers
