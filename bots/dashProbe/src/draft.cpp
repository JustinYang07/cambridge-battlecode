#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <string_view>
#include <vector>

// Probe: draft the dash champions first (Vesper, Vendetta, Ren), then anything.
void play_draft() {
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    const auto has = [&](cambc::champion_draft c) { return std::find(chosen.begin(), chosen.end(), c) != chosen.end(); };
    cambc::draft_memory = std::string_view("probe");
    for (auto c : {cambc::champion_draft::vesper_vale, cambc::champion_draft::vendetta, cambc::champion_draft::ren,
                   cambc::champion_draft::varg, cambc::champion_draft::tavi, cambc::champion_draft::stellar,
                   cambc::champion_draft::ryth, cambc::champion_draft::marcella})
        if (!has(c)) { cambc::draft::pick(c); return; }
}
