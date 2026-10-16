#include "regroup.h"
#include "../core/comms.h"
#include "../core/config.h"
#include "../core/profile.h"
#include "../core/fixed_str.h"
#include "../core/statics.h"
#include "../core/trace.h"
#include "../core/world.h"

namespace bc = cambc;

namespace regroup {

namespace {
bool responding = false, activeNow = false;
bc::vec2 spot{-1, -1};
int lastFresh = -1000, reported = 0;
bool reportedSquishy = false;
bc::vec2 alertPos{-1, -1};
int alertRound = -1;
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
    // A fragile spotter (not a tank, or hurt) is the one that needs help: it calls harder.
    const bool squishy = profile(w.type).survivability < 1.0f || w.hpFrac() < tune::squishy_hp;
    const bool mineAlert = alert.fresh && dist(w.pos, bc::vec2{alert.x, alert.y}) <= tune::regroup_arrive;
    bool spotter = false;
    if (seen > 0 && (!alert.fresh || mineAlert)) {
        comms::raiseAlert(w.pos.x, w.pos.y, seen, squishy, w.round);
        if (!alert.fresh) trace().note("alert: enemy on our half");
        alert = comms::readAlert(w.round);
        spotter = true;
    }

    if (alert.fresh) {
        alertPos = {alert.x, alert.y};
        alertRound = w.round;
    }

    // Responders: allies near the spotter. Once responding we follow the alert wherever it moves.
    if (alert.fresh && !spotter) {
        const bc::vec2 a{alert.x, alert.y};
        const int d = dist(w.pos, a);
        // A squishy spotter needs the help: call allies from further away. A tank can hold its own.
        const int radius = alert.squishy ? tune::regroup_radius_squishy : tune::regroup_radius;
        if (responding || d <= radius) {
            if (!responding) {
                FixedStr<40> msg;
                trace().note((msg << "regroup: " << alert.enemies << " enemy at " << a.x << ',' << a.y).view());
            }
            responding = true;
            spot = a;
            reported = alert.enemies;
            reportedSquishy = alert.squishy;
            lastFresh = w.round;
        }
    }
    if (responding && w.round - lastFresh > tune::regroup_end_rounds) {
        responding = false;
        trace().note("regroup over: enemy left");
    }
    activeNow = responding && !spotter && w.alive && w.hpFrac() >= tune::regroup_min_hp;
}

bool active() { return activeNow; }
bc::vec2 at() { return spot; }
int enemies() { return reported; }
bool squishySpotter() { return reportedSquishy; }
bool alertNear(bc::vec2 p, int radius) { return alertRound >= 0 && dist(p, alertPos) <= radius; }

} // namespace regroup
