#include "regroup.h"
#include "../core/comms.h"
#include "../core/config.h"
#include "../core/profile.h"
#include "../core/fixed_str.h"
#include "../core/statics.h"
#include "../core/trace.h"
#include "../core/world.h"
#include "../brain/option.h"

namespace bc = cambc;

namespace regroup {

namespace {
bool responding = false, activeNow = false;
bc::vec2 spot{-1, -1};
int lastFresh = -1000, reported = 0;
bool reportedSquishy = false;
bc::vec2 alertPos{-1, -1};
int alertRound = -1;
int rallying = 0;  // combatBotv3: enemies an ally is rallying the team against (0: a plain alert)
} // namespace

void update(World& w) {
    activeNow = false;
    alertRound = -1;
    if (w.allyCount > comms::alert_byte) return;  // no free bytes for the alert
    const auto& s = Statics::get();
    auto alert = comms::readAlert(w.round);

    // Spotter: an enemy we can see on our half (nearer our spawn than theirs) and close to us. Once
    // it leaves that radius the alert lapses and responders go back to farming and banking.
    int seen = 0;
    if (w.alive)
        for (const auto& e : w.enemies)
            if (e.hp > 0 && dist(e.pos, w.pos) <= tune::regroup_enemy_radius &&
                dist(e.pos, s.spawnCenter) < dist(e.pos, s.enemySpawnCenter))
                ++seen;
    // Combat call: we fought an enemy champion last turn, or one close by is hitting us. Anywhere
    // on the map: allies nearby come to us. Sent as a squishy call (the strong pull).
    bool fighting = false;
    if (tune::combat_call && w.alive && seen == 0) {
        int close = 0, visible = 0;
        for (const auto& e : w.enemies)
            if (e.hp > 0) {
                ++visible;
                if (dist(e.pos, w.pos) <= tune::regroup_enemy_radius) ++close;
            }
        const bool hitByThem = w.beingHit && w.incoming <= 0 && close > 0;  // hurt, and not by monsters
        if (close > 0 && (w.lastOption == static_cast<int>(OptionKind::fight) || hitByThem)) {
            seen = close;
            fighting = true;
        } else if (tune::call_on_several && visible >= 2) {
            // Several enemies in sight, anywhere: a fight may be coming, get the team together.
            seen = visible;
            fighting = true;
        }
    }
    // A fragile spotter (not a tank, or hurt) is the one that needs help: it calls harder.
    const bool squishy = fighting || profile(w.type).survivability < 1.0f || w.hpFrac() < tune::squishy_hp;
    const bool mineAlert = alert.fresh && dist(w.pos, bc::vec2{alert.x, alert.y}) <= tune::regroup_arrive;
    // No support needed: we have been calling for support_check_rounds and are still above
    // support_hp. Let the call lapse (responders go back to farming); call again if we drop below.
    static int callStart = -1;
    if (seen == 0) callStart = -1;
    else if (callStart < 0) callStart = w.round;
    const bool holdingFine = tune::support_check_rounds > 0 && seen > 0 && w.round - callStart >= tune::support_check_rounds &&
                             w.hpFrac() >= tune::support_hp;
    if (holdingFine && w.round - callStart == tune::support_check_rounds) trace().note("holding fine: no support needed");
    bool spotter = false;
    if (seen > 0 && !holdingFine && (!alert.fresh || mineAlert)) {
        comms::raiseAlert(w.pos.x, w.pos.y, seen, squishy, w.round);
        if (!alert.fresh) trace().note(fighting ? "alert: fighting, calling allies" : "alert: enemy on our half");
        alert = comms::readAlert(w.round);
        spotter = true;
    }

    if (alert.fresh) {
        alertPos = {alert.x, alert.y};
        alertRound = w.round;
    }

    // Responders: allies near the spotter. Once responding we follow the alert wherever it moves.
    // combatBotv3: an ally falling back from a group calls the team to its rally point.
    const int rally = tune::rally_alert && alert.fresh ? comms::rallyThreat(w.slot) : 0;
    if (alert.fresh && !spotter) {
        const bc::vec2 a{alert.x, alert.y};
        const int d = dist(w.pos, a);
        // A squishy spotter needs the help: call allies from further away. A tank can hold its own.
        const int radius = rally > 0 ? tune::rally_radius : alert.squishy ? tune::regroup_radius_squishy : tune::regroup_radius;
        if (responding || d <= radius) {
            if (!responding) {
                FixedStr<48> msg;
                if (rally > 0) trace().note((msg << "rally: " << rally << " enemies, gather at " << a.x << ',' << a.y).view());
                else trace().note((msg << "regroup: " << alert.enemies << " enemy at " << a.x << ',' << a.y).view());
            }
            rallying = rally;
            responding = true;
            spot = a;
            reported = alert.enemies;
            reportedSquishy = alert.squishy;
            lastFresh = w.round;
        }
    }
    if (responding && w.round - lastFresh > tune::regroup_end_rounds) {
        responding = false;
        rallying = 0;
        trace().note("regroup over: enemy left");
    }
    activeNow = responding && !spotter && w.alive && w.hpFrac() >= tune::regroup_min_hp;
}

bool active() { return activeNow; }
bc::vec2 at() { return spot; }
int enemies() { return reported; }
bool squishySpotter() { return reportedSquishy; }
int rallyEnemies() { return activeNow ? rallying : 0; }
bool alertNear(bc::vec2 p, int radius) { return alertRound >= 0 && dist(p, alertPos) <= radius; }

} // namespace regroup
