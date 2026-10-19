#include "budget.h"
#include <algorithm>
#include <cstdint>
#include "config.h"

namespace budget {

namespace {
unsigned long long estimate[2] = {tune::exec_reserve_start, tune::exec_reserve_start_contact};
unsigned long long farmPre = tune::farm_pre_start;
unsigned long long tail[2] = {tune::farm_tail_start, tune::farm_tail_start_contact};
unsigned long long farmDoneAt = 0;
std::uint32_t farmDoneRound = 0;
}

void noteExecute(unsigned long long cost, bool contact) {
    auto& e = estimate[contact ? 1 : 0];
    // Jump up to a dear turn at once; ease back down by 1/64 a turn (a fight's worst case stays
    // remembered for ~40 turns).
    e = std::max(cost, e - e / 64);
    e = std::clamp<unsigned long long>(e, contact ? tune::exec_reserve_min_contact : tune::exec_reserve_min, tune::exec_reserve_max);
}

unsigned long long reserve(bool contact) { return estimate[contact ? 1 : 0] + tune::turn_tail_fuel; }

unsigned long long spare(bool contact) {
    const auto left = cambc::fuel_remaining();
    const auto keep = reserve(contact);
    return left > keep ? left - keep : 0;
}

void noteFarmPre(unsigned long long cost) { farmPre = std::max(cost, farmPre - farmPre / 16); }

unsigned long long farmNeed() { return farmPre + tune::farm_camp_fuel; }

void markFarmDone() {
    farmDoneAt = cambc::fuel_remaining();
    farmDoneRound = cambc::round_number();
}

void noteTurnEnd(bool contact) {
    if (farmDoneAt == 0) return;
    const auto now = cambc::fuel_remaining();
    // Overran since: the tail took everything that was left and more.
    const bool overran = cambc::round_number() != farmDoneRound || now > farmDoneAt;
    const auto cost = overran ? farmDoneAt + tune::farm_tail_overrun_extra : farmDoneAt - now;
    auto& t = tail[contact ? 1 : 0];
    t = std::clamp<unsigned long long>(std::max(cost, t - t / 64), tune::farm_tail_min, tune::farm_tail_max);
    farmDoneAt = 0;
}

unsigned long long farmTail(bool contact) { return tail[contact ? 1 : 0]; }

} // namespace budget
