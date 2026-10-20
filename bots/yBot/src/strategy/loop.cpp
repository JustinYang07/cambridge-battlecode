#include "loop.h"
#include <vector>
#include "mapplan.h"
#include "../core/comms.h"
#include "../core/config.h"
#include "../core/profile.h"
#include "../core/statics.h"
#include "../core/trace.h"
#include "../core/world.h"

namespace loop {

namespace {
bool init = false;
const mapplan::Loop* mine = nullptr;
unsigned mates = 0;           // allied slots on our loop
int ptr = -1, tgt = -1;       // position on the loop, and this turn's camp
std::vector<int> skipUntil;   // per loop position: skipped until this round

int indexOf(int groupId) {
    if (!mine || groupId < 0) return -1;
    for (std::size_t i = 0; i < mine->camps.size(); ++i)
        if (mine->camps[i] == groupId) return static_cast<int>(i);
    return -1;
}
} // namespace

namespace {
void pickTarget(World& w);
} // namespace

void update(World& w) {
    const int before = tgt;
    pickTarget(w);
    if (tgt != before) trace().note(tgt >= 0 ? "loop -> g" + num(tgt) : std::string("loop off"));
}

namespace {
void pickTarget(World& w) {
    if (!init) {
        init = true;
        const auto& p = mapplan::read();
        int idx = -1;
        for (const auto& [t, l] : p.loopOf)
            if (t == w.type && l < p.loops.size()) idx = l;
        if (idx >= 0 && !p.loops[static_cast<std::size_t>(idx)].camps.empty()) {
            mine = &p.loops[static_cast<std::size_t>(idx)];
            for (int s = 0; s < w.allyCount && s < comms::alert_byte; ++s)
                if (s != w.slot)
                    for (const auto& [t, l] : p.loopOf)
                        if (t == w.allyTypes[static_cast<std::size_t>(s)] && l == idx) mates |= 1u << s;
            skipUntil.assign(mine->camps.size(), -1);
        }
        trace().note("loop: " + text());
    }
    tgt = -1;
    if (!tune::loops || !mine || mapplan::routeActive()) return;
    const auto& s = Statics::get();
    const int n = static_cast<int>(mine->camps.size());
    auto ready = [&](int i) {
        const int g = mine->camps[static_cast<std::size_t>(i)];
        if (g >= static_cast<int>(w.groups.size()) || skipUntil[static_cast<std::size_t>(i)] > w.round) return false;
        const auto& mem = w.groups[static_cast<std::size_t>(g)];
        if (mem.status == GroupStatus::respawning) {
            const int walk = dist(w.pos, s.groups[static_cast<std::size_t>(g)].center) * 5 / 4;
            return mem.respawnAt - w.round <= walk + tune::loop_max_wait;
        }
        if (mem.status == GroupStatus::reward) return capturer(w, g);
        return true;
    };
    if (ptr < 0) {
        ptr = 0;
        for (int i = 1; i < n; ++i)
            if (dist(w.pos, s.groups[mine->camps[static_cast<std::size_t>(i)]].center) <
                dist(w.pos, s.groups[mine->camps[static_cast<std::size_t>(ptr)]].center))
                ptr = i;
    }
    // Follow the party's leader: the lowest slot on our loop working a camp on it that is ready.
    for (int sl = 0; sl < w.slot; ++sl)
        if ((mates >> sl) & 1u) {
            const int i = indexOf(comms::claimOf(sl));
            if (i >= 0 && ready(i)) {
                ptr = i;
                tgt = mine->camps[static_cast<std::size_t>(i)];
                return;
            }
        }
    for (int k = 0; k < n; ++k) {
        const int i = (ptr + k) % n;
        if (ready(i)) {
            ptr = i;
            tgt = mine->camps[static_cast<std::size_t>(i)];
            return;
        }
    }
    // Everything is down: wait at the camp back soonest.
    int soonest = -1;
    for (int i = 0; i < n; ++i) {
        const int g = mine->camps[static_cast<std::size_t>(i)];
        if (g >= static_cast<int>(w.groups.size()) || skipUntil[static_cast<std::size_t>(i)] > w.round) continue;
        if (soonest < 0 || w.groups[static_cast<std::size_t>(g)].respawnAt <
                               w.groups[static_cast<std::size_t>(mine->camps[static_cast<std::size_t>(soonest)])].respawnAt)
            soonest = i;
    }
    if (soonest >= 0) {
        ptr = soonest;
        tgt = mine->camps[static_cast<std::size_t>(soonest)];
    }
}

} // namespace

int target() { return tgt; }

bool onLoop(int groupId) { return indexOf(groupId) >= 0; }

bool capturer(const World& w, int groupId) {
    if (!mine || groupId < 0) return true;
    // Enemies about: the party holds the capture together.
    if (w.enemyPressure(Statics::get().groups[static_cast<std::size_t>(groupId)].center, tune::route_enemy_radius) > 0) return true;
    const float myPower = profile(w.type).farmPower;
    const float myMissing = w.maxHp - w.hp;
    for (int sl = 0; sl < comms::alert_byte; ++sl) {
        if (!((mates >> sl) & 1u) || comms::claimOf(sl) != groupId || comms::claimBracket(sl) != 0) continue;
        if (tune::keeper_stellar_first) {  // yBot: Stellar keeps it
            const bool meS = w.type == cambc::champion_type::stellar, themS = w.allyTypes[static_cast<std::size_t>(sl)] == cambc::champion_type::stellar;
            if (meS && !themS) continue;
            if (themS && !meS) return false;
        }
        if (tune::keeper_tank_first && isTank(w.type) != isTank(w.allyTypes[static_cast<std::size_t>(sl)])) {
            if (isTank(w.type)) continue;  // we are the tank: we keep it
            return false;                  // the tank keeps it
        }
        // combatBotv2: the most damaged mate stays (its recall home banks the reward and heals it).
        if (tune::keeper_most_damaged) {
            const float theirs = w.allyMissingHp(sl);
            if (theirs > myMissing + tune::keeper_missing_margin) return false;
            if (myMissing > theirs + tune::keeper_missing_margin) continue;
        }
        const float theirs = profile(w.allyTypes[static_cast<std::size_t>(sl)]).farmPower;
        // The weakest damage dealer stays (ties: the later slot).
        if (theirs < myPower || (theirs == myPower && sl > w.slot)) return false;
    }
    return true;
}

void tooStrong(const World& w, int groupId) {
    if (const int i = indexOf(groupId); i >= 0 && skipUntil[static_cast<std::size_t>(i)] <= w.round) {
        skipUntil[static_cast<std::size_t>(i)] = w.round + tune::loop_skip_rounds;
        trace().note("loop: skipping g" + num(groupId) + " for now");
    }
}

std::string text() {
    if (!mine) return mapplan::read().valid ? "none" : "no plan";
    std::string out = "x" + num(mine->bodies);
    for (std::size_t i = 0; i < mine->camps.size(); ++i) out += (i ? " > g" : ": g") + num(mine->camps[i]);
    return out;
}

} // namespace loop
