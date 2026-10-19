#include "cohesion.h"
#include "../core/comms.h"
#include "../core/config.h"
#include "../core/profile.h"
#include "../core/world.h"
#include "../brain/option.h"

namespace cohesion {

namespace {
int lead = -1, camp = -1, active = 1;
} // namespace

void update(const World& w) {
    lead = -1;
    camp = -1;
    active = 0;
    if (!tune::cohesion) return;
    float best = -1;
    for (int sl = 0; sl < w.allyCount && sl < comms::alert_byte; ++sl) {
        // Ourselves: alive and last out farming, capturing or fighting (as our claim would show).
        const bool on = sl == w.slot ? w.alive && (w.lastOption == static_cast<int>(OptionKind::farm) ||
                                                   w.lastOption == static_cast<int>(OptionKind::capture) ||
                                                   w.lastOption == static_cast<int>(OptionKind::fight) ||
                                                   w.lastOption < 0)
                                     : comms::active(sl);
        if (!on) continue;
        ++active;
        const float power = profile(w.allyTypes[static_cast<std::size_t>(sl)]).farmPower;
        if (power > best) {
            best = power;
            lead = sl;
        }
    }
    if (lead >= 0 && lead != w.slot) camp = comms::claimOf(lead);
}

int leader() { return lead; }
bool isLeader(const World& w) { return lead == w.slot; }
int leaderCamp() { return camp; }
int activeCount() { return active < 1 ? 1 : active; }

} // namespace cohesion
