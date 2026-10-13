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

Scored score(bc::champion_draft pick, int teamSize) {
    const auto& p = profile(representative(pick));
    Scored s{pick, 0, p.farmPower, p.survivability, 1.0f};
    if (pick == bc::champion_draft::cassi_and_issac) {
        // A second body farms in parallel; worth more on small rosters, but the linked death hurts.
        s.bodies = 1.0f + 0.55f * (3.0f / static_cast<float>(std::max(teamSize, 3)));
    }
    s.score = s.farm * s.survive * s.bodies;
    return s;
}

} // namespace

void play_draft() {
    const auto mine = bc::draft::current_draft(bc::team::ally);
    const int teamSize = static_cast<int>(bc::draft::team_size());

    std::vector<Scored> options;
    for (int i = 0; i < kChampionDrafts; ++i) {
        const auto pick = static_cast<bc::champion_draft>(i);
        if (std::find(mine.begin(), mine.end(), pick) == mine.end()) options.push_back(score(pick, teamSize));
    }
    std::sort(options.begin(), options.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
    if (options.empty()) return;

    Json j;
    j.begin().field("pick", static_cast<int>(mine.size())).field("teamSize", teamSize).array("opts");
    for (const auto& o : options)
        j.begin().field("k", name(o.pick)).field("s", o.score).field("farm", o.farm).field("survive", o.survive)
            .field("bodies", o.bodies).end();
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
