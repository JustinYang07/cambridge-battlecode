#include "fuel.h"
#include <array>
#include <cambc.h>
#include <cstring>
#include "json.h"
#include "trace.h"
#include <algorithm>
#include <iostream>
#include <string>

namespace fuel {

namespace {

struct Section {
    const char* name = nullptr;
    unsigned long long used = 0;
};

std::array<Section, 32> sections, keptSections;
std::size_t count = 0, keptCount = 0;
bool keptSplit = false;
unsigned long long start = 0, last = 0;
std::uint32_t startRound = 0;
bool overran = false;
const char* overranIn = nullptr;      // the section running when the budget ran out
unsigned long long quantum = 100000;  // a turn's budget (largest turn start seen)

} // namespace

void beginTurn() {
    count = 0;
    overran = false;
    overranIn = nullptr;
    startRound = cambc::round_number();
    start = last = cambc::fuel_remaining();
    // First turns get 2x (draft 10x): only learn the budget from ordinary-looking starts.
    if (start > quantum && start < 150000) quantum = start;
}

void mark(const char* section) {
#ifndef BB_FUEL_LOG
    // xBot: the sections only feed traces and the fuel log. Untraced, a mark (a host read plus a
    // name search over up to 22 sections, ~22 marks a turn) cost a few thousand fuel every turn.
    if constexpr (!kTracing) return;
#endif
    const auto now = cambc::fuel_remaining();
    // Fuel only goes down within a turn; going up means we were suspended and resumed: this
    // section spent what was left before the suspension, then (budget - now) after it.
    // A section can spend more after the resume than it had left before it, hiding the jump in
    // fuel, so the round number is the reliable sign (one host read per mark, ~12 fuel).
    unsigned long long delta = now > last ? 0ULL : last - now;
#ifdef BB_FUEL_LOG
    const bool roundChanged = !overranIn && cambc::round_number() != startRound;
#else
    const bool roundChanged = false;  // a host call per mark is only worth it when logging
#endif
    if (roundChanged) {
        overran = true;
        overranIn = section;
        delta = last + (quantum > now ? quantum - now : 0ULL);
    } else if (now > last) {
        overran = true;
        delta = last + (quantum > now ? quantum - now : 0ULL);
    }
    last = now;
    for (std::size_t i = 0; i < count; ++i)
        if (std::strcmp(sections[i].name, section) == 0) {
            sections[i].used += delta;
            return;
        }
    if (count < sections.size()) sections[count++] = {section, delta};
}

bool split() { return overran || cambc::round_number() != startRound; }

unsigned long long used() {
    const auto now = cambc::fuel_remaining();
    return now > start ? 0ULL : start - now;
}

static std::string format(const std::array<Section, 32>& list, std::size_t n, bool wasSplit) {
    std::string out = wasSplit ? "SPLIT" : "";
    for (std::size_t i = 0; i < n; ++i) {
        if (list[i].used < 2500) continue;  // keep the note short: only sections that matter
        if (!out.empty()) out += ' ';
        out += list[i].name;
        out += ' ';
        Json::integer(out, static_cast<long long>((list[i].used + 500) / 1000));
        out += 'k';
    }
    return out;
}

std::string report() { return format(sections, count, split()); }

void keep() {
    keptSections = sections;
    keptCount = count;
    keptSplit = split();
}

std::string keptReport() { return format(keptSections, keptCount, keptSplit); }

#ifdef BB_FUEL_LOG
namespace {
constexpr std::size_t kNames = 32;
struct Agg {
    std::array<const char*, kNames> name{};
    std::array<unsigned long long, kNames> sum{};
    std::array<unsigned long long, kNames> max{};
    std::array<int, kNames> ranOut{};  // overruns that happened in this section
    std::size_t n = 0;
    long long turns = 0, overruns = 0;
    std::array<long long, 4> startBins{};  // turn start fuel: <50k, 50-90k, 90-99k, full
    unsigned long long unmarked = 0;       // fuel spent outside any section (whole turns only)
    long long unmarkedTurns = 0;
    std::size_t slot(const char* s) {  // by pointer: string compares every turn cost more than the log is worth
        for (std::size_t i = 0; i < n; ++i)
            if (name[i] == s) return i;
        if (n < kNames) name[n++] = s;
        return n - 1;
    }
};
std::array<Agg, 2> agg;  // [0] quiet turns, [1] turns with an enemy champion within 8 tiles
}  // namespace

void logTurn(int round, int slot, const char* who, const char* option, int enemiesNear) {
    auto& a = agg[enemiesNear > 0 ? 1 : 0];
    // Totals from every 4th round only (a sample, kept cheap); overruns are all counted.
    unsigned long long sum = 0;
    for (std::size_t i = 0; i < count; ++i) sum += sections[i].used;
    if (round % 4 == 0) {
        ++a.turns;
        ++a.startBins[start < 50000 ? 0 : start < 90000 ? 1 : start < 99000 ? 2 : 3];
        if (!split()) {
            const auto spent = start - std::min(start, static_cast<unsigned long long>(cambc::fuel_remaining()));
            a.unmarked += spent > sum ? spent - sum : 0;
            ++a.unmarkedTurns;
        }
        for (std::size_t i = 0; i < count; ++i) {
            const auto k = a.slot(sections[i].name);
            a.sum[k] += sections[i].used;
            a.max[k] = std::max(a.max[k], sections[i].used);
        }
    }
    if (split()) {
        ++a.overruns;
        if (overranIn) ++a.ranOut[a.slot(overranIn)];
        std::string line = "@O r" + std::to_string(round) + " u" + std::to_string(slot) + " " + who + " " + option + " e" +
                           std::to_string(enemiesNear) + " in=" + (overranIn ? overranIn : "?") + " start=" + std::to_string(start / 1000) +
                           "k sum=" + std::to_string(sum / 1000) + "k |";
        for (std::size_t i = 0; i < count; ++i)
            if (sections[i].used >= 1000) line += " " + std::string(sections[i].name) + " " + std::to_string(sections[i].used / 1000) + "k";
        std::cout << line << std::endl;
    }
    if (round % 250 == 0) {
        for (int f = 0; f < 2; ++f) {
            const auto& b = agg[static_cast<std::size_t>(f)];
            std::string line = "@A r" + std::to_string(round) + " u" + std::to_string(slot) + " " + (f ? "fight" : "quiet") + " sampled " +
                               std::to_string(b.turns) + " overruns " + std::to_string(b.overruns) + " starts " +
                               std::to_string(b.startBins[0]) + "/" + std::to_string(b.startBins[1]) + "/" + std::to_string(b.startBins[2]) + "/" +
                               std::to_string(b.startBins[3]) + " unmarked " +
                               std::to_string(b.unmarkedTurns ? b.unmarked / static_cast<unsigned long long>(b.unmarkedTurns) : 0) + " |";
            for (std::size_t i = 0; i < b.n; ++i)
                line += " " + std::string(b.name[i]) + " " + std::to_string(b.turns ? b.sum[i] / static_cast<unsigned long long>(b.turns) : 0) + "/" +
                        std::to_string(b.max[i] / 1000) + "k/" + std::to_string(b.ranOut[i]);
            std::cout << line << std::endl;
        }
    }
}
#else
void logTurn(int, int, const char*, const char*, int) {}
#endif

} // namespace fuel
