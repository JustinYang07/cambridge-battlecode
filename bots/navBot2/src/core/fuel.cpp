#include "fuel.h"
#include <array>
#include <cambc.h>
#include <cstring>
#include "json.h"

namespace fuel {

namespace {

struct Section {
    const char* name = nullptr;
    unsigned long long used = 0;
};

std::array<Section, 16> sections;
std::size_t count = 0;
unsigned long long start = 0, last = 0;
std::uint32_t startRound = 0;
bool overran = false;

} // namespace

void beginTurn() {
    count = 0;
    overran = false;
    startRound = cambc::round_number();
    start = last = cambc::fuel_remaining();
}

void mark(const char* section) {
    const auto now = cambc::fuel_remaining();
    // Fuel only goes down within a turn; going up means we were suspended and resumed.
    if (now > last) overran = true;
    const auto delta = now > last ? 0ULL : last - now;
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

std::string report() {
    std::string out = split() ? "SPLIT" : "";
    for (std::size_t i = 0; i < count; ++i) {
        if (!out.empty()) out += ' ';
        out += sections[i].name;
        out += ' ';
        Json::integer(out, static_cast<long long>((sections[i].used + 500) / 1000));
        out += 'k';
    }
    return out;
}

} // namespace fuel
