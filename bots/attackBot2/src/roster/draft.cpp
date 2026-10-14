#include "draft.h"
#include <algorithm>
#include <array>
#include <cambc.h>
#include <cstdio>
#include <vector>
#include "../core/json.h"
#include "../core/profile.h"

namespace bc = cambc;

namespace {

constexpr int kChampionDrafts = 11;

struct Scored {
    bc::champion_draft pick;
    float score;
    float farm, survive, bodies;
};

bc::champion_type representative(bc::champion_draft pick) {
    // Cassi & Issac's farming is judged on Issac (the melee half); its body bonus covers Cassi.
    switch (pick) {
    case bc::champion_draft::ryth: return bc::champion_type::ryth;
    case bc::champion_draft::vesper_vale: return bc::champion_type::vesper_vale;
    case bc::champion_draft::vendetta: return bc::champion_type::vendetta;
    case bc::champion_draft::cassi_and_issac: return bc::champion_type::issac;
    case bc::champion_draft::stellar: return bc::champion_type::stellar;
    case bc::champion_draft::prism: return bc::champion_type::prism;
    case bc::champion_draft::marcella: return bc::champion_type::marcella;
    case bc::champion_draft::willow: return bc::champion_type::willow;
    case bc::champion_draft::ren: return bc::champion_type::ren;
    case bc::champion_draft::varg: return bc::champion_type::varg;
    case bc::champion_draft::tavi: return bc::champion_type::tavi;
    }
    return bc::champion_type::ryth;
}

Scored score(bc::champion_draft pick, int teamSize, bool firstPick) {
    const auto& p = profile(representative(pick));
    Scored s{pick, 0, p.farmPower, p.survivability, 1.0f};
    if (pick == bc::champion_draft::cassi_and_issac) {
        // A second body farms in parallel; worth more on small rosters, but the linked death hurts.
        s.bodies = 1.0f + 0.55f * (3.0f / static_cast<float>(std::max(teamSize, 3)));
    }
    s.score = s.farm * s.survive * s.bodies;
    // Monsters hit the nearest champion and break ties by draft order, so our first pick
    // is the party's damage sponge: it must be the toughest tank.
    if (firstPick) s.score = p.roles.tank * p.survivability * 100.0f;
    return s;
}

// Role template by roster size, filled from per-category priority lists. The first pick is
// always a tank: monsters hit the nearest champion and break ties by draft order, so slot 0
// soaks camp damage for every party it is in. Priorities follow the ladder's winning drafts.
// Casters are left out until navBot uses range-4 spells well (A/B showed no gain yet).
enum class Slot : std::uint8_t { tank, farmer, hybrid, caster, support };

const std::vector<Slot>& templateFor(int teamSize) {
    static const std::vector<Slot> t3{Slot::tank, Slot::farmer, Slot::hybrid};
    static const std::vector<Slot> t4{Slot::tank, Slot::farmer, Slot::hybrid, Slot::tank};
    static const std::vector<Slot> t5{Slot::tank, Slot::farmer, Slot::hybrid, Slot::tank, Slot::farmer};
    return teamSize <= 3 ? t3 : teamSize == 4 ? t4 : t5;
}

const std::vector<bc::champion_draft>& priorities(Slot slot) {
    using D = bc::champion_draft;
    static const std::vector<D> tank{D::ryth, D::marcella, D::varg, D::ren};
    static const std::vector<D> farmer{D::vendetta, D::vesper_vale, D::varg, D::cassi_and_issac};
    static const std::vector<D> hybrid{D::varg, D::marcella, D::ryth, D::vendetta};
    static const std::vector<D> caster{D::stellar, D::tavi, D::cassi_and_issac, D::prism};
    static const std::vector<D> support{D::willow, D::stellar};
    switch (slot) {
    case Slot::tank: return tank;
    case Slot::farmer: return farmer;
    case Slot::hybrid: return hybrid;
    case Slot::caster: return caster;
    case Slot::support: return support;
    }
    return tank;
}

const char* slotName(Slot s) {
    static constexpr std::array<const char*, 5> names{"tank", "farmer", "hybrid", "caster", "support"};
    return names[static_cast<std::size_t>(s)];
}

} // namespace

void play_draft() {
    const auto mine = bc::draft::current_draft(bc::team::ally);
    const int teamSize = static_cast<int>(bc::draft::team_size());
    const auto& plan = templateFor(teamSize);
    const auto pickIndex = std::min(mine.size(), plan.size() - 1);
    const Slot want = plan[pickIndex];
    auto taken = [&](bc::champion_draft d) { return std::find(mine.begin(), mine.end(), d) != mine.end(); };

    // attackBot2: a fixed long-range, high-damage roster in priority order (rosters are
    // independent, so only our own picks block). Falls back to the role template if all are taken.
    std::vector<Scored> options;
    static const std::array<bc::champion_draft, 5> ranged{bc::champion_draft::cassi_and_issac, bc::champion_draft::stellar,
                                                        bc::champion_draft::tavi, bc::champion_draft::varg,
                                                        bc::champion_draft::ren};
    // Three picks: the long-range roster had no tank or splash farmer and lost 3v3 farming races
    // (collision, packs); a melee front of Varg and Ren behind Cassi & Issac tested best.
    static const std::array<bc::champion_draft, 5> small{bc::champion_draft::cassi_and_issac, bc::champion_draft::varg,
                                                       bc::champion_draft::ren, bc::champion_draft::stellar,
                                                       bc::champion_draft::tavi};
    for (auto d : teamSize <= 3 ? small : ranged)
        if (!taken(d)) options.push_back({d, static_cast<float>(10 - options.size()), 0, 0, 1});
    if (options.empty())
        for (auto d : priorities(want))
            if (!taken(d)) options.push_back({d, static_cast<float>(10 - options.size()), 0, 0, 1});
    if (options.empty()) {
        for (int i = 0; i < kChampionDrafts; ++i) {
            const auto d = static_cast<bc::champion_draft>(i);
            if (!taken(d)) options.push_back(score(d, teamSize, false));
        }
        std::sort(options.begin(), options.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
    }
    if (options.empty()) return;

    Json j;
    j.begin().field("pick", static_cast<int>(mine.size())).field("teamSize", teamSize).field("role", slotName(want)).array("opts");
    for (const auto& o : options) j.begin().field("k", name(o.pick)).field("s", o.score).end();
    j.endArray().field("chosen", name(options.front().pick)).end();
    std::printf("@D%.*s\n", static_cast<int>(j.view().size()), j.view().data());
    std::fflush(stdout);

    // Draft memory: [version, picks so far...]. Gameplay reads it only for diagnostics today.
    std::array<std::uint8_t, 16> memory{};
    memory[0] = 1;
    for (std::size_t i = 0; i < mine.size() && i + 2 < memory.size(); ++i)
        memory[i + 1] = static_cast<std::uint8_t>(mine[i]);
    memory[std::min(mine.size() + 1, memory.size() - 1)] = static_cast<std::uint8_t>(options.front().pick);
    bc::draft_memory = memory;

    bc::draft::pick(options.front().pick);
}
