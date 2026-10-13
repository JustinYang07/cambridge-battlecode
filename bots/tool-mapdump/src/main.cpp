// Lab helper, not a competitive bot: prints the map as one "@MAP{json}" line on
// team A's first champion turn, then idles. The lab server runs it with --rounds 1.
#include <cambc.h>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>

namespace bc = cambc;

static std::string grid(const bc::bitboard& board) {
    // One bulk export instead of a metered is_set call per tile.
    const auto bytes = board.export_linear_buffer();
    const int w = bc::map::width(), h = bc::map::height();
    std::string out = "[";
    out.reserve(static_cast<std::size_t>((w + 3) * h + 2));
    for (int y = 0; y < h; ++y) {
        out += y ? ",\"" : "\"";
        // Despite its doc comment the linear buffer is bit-packed, LSB first.
        for (int x = 0; x < w; ++x) {
            const auto i = static_cast<std::size_t>(y * w + x);
            out += (bytes[i >> 3] >> (i & 7)) & 1 ? '1' : '0';
        }
        out += '"';
    }
    return out + "]";
}

static std::string points(const bc::bitboard& board) {
    std::string out = "[";
    bool first = true;
    for (auto p : board.positions()) {
        out += (first ? "[" : ",[") + std::to_string(p.x) + "," + std::to_string(p.y) + "]";
        first = false;
    }
    return out + "]";
}

static void dump() {
    std::ostringstream js;
    js << "@MAP{\"w\":" << bc::map::width() << ",\"h\":" << bc::map::height()
       << ",\"symmetry\":\"" << bc::to_string(bc::map::symmetry()) << "\""
       << ",\"teamSize\":" << bc::draft::team_size()
       << ",\"constants\":{\"captureTime\":" << bc::constants::capture_time
       << ",\"campSleepTime\":" << bc::constants::camp_sleep_time
       << ",\"respawnTime\":" << bc::constants::respawn_time << ",\"recallTime\":" << bc::constants::recall_time
       << ",\"passiveGold\":" << bc::constants::passive_gold << ",\"spawnHealing\":" << bc::constants::spawn_healing
       << ",\"killGold\":" << bc::constants::kill_gold << ",\"submitRange\":" << bc::constants::submit_gems_range
       << ",\"wardCooldown\":" << bc::constants::ward_cooldown << ",\"maxRounds\":" << bc::constants::max_rounds
       << ",\"winningGems\":" << bc::constants::winning_gems << "}"
       << ",\"walls\":" << grid(bc::map::walls()) << ",\"holes\":" << grid(bc::map::holes())
       << ",\"bushes\":" << grid(bc::map::bushes()) << ",\"altars\":" << points(bc::map::altars())
       << ",\"spawnA\":" << points(bc::map::spawn_region(bc::team::ally))
       << ",\"spawnB\":" << points(bc::map::spawn_region(bc::team::enemy)) << ",\"groups\":[";
    bool first = true;
    for (const auto& group : bc::map::camp_groups()) {
        js << (first ? "" : ",") << "{\"gems\":" << group.gem_value() << ",\"gold\":" << group.gold_value()
           << ",\"respawn\":" << group.max_respawn_timer() << ",\"camps\":[";
        bool firstCamp = true;
        for (const auto& camp : group.camps()) {
            const auto p = camp.spawn_position();
            const auto stats = camp.base_stats();
            js << (firstCamp ? "" : ",") << "{\"type\":\"" << bc::to_string(camp.kind()) << "\",\"x\":" << p.x
               << ",\"y\":" << p.y << ",\"hp\":" << stats.base_health << ",\"str\":" << stats.base_strength
               << ",\"range\":" << stats.base_attack_range << "}";
            firstCamp = false;
        }
        js << "]}";
        first = false;
    }
    js << "]}";
    std::cout << js.str() << std::endl;
}

int main() {
    bool done = false;
    while (true) {
        try {
            if (bc::is_draft()) {
                for (int i = 0; i < 11; ++i) {
                    auto pick = static_cast<bc::champion_draft>(i);
                    auto mine = bc::draft::current_draft(bc::team::ally);
                    if (std::find(mine.begin(), mine.end(), pick) == mine.end()) {
                        bc::draft::pick(pick);
                        break;
                    }
                }
            } else if (!done) {
                done = true;
                // Team A owns the lowest spawn tile ordering only by convention; print from both and
                // let the lab keep team A's copy, which uses team A's perspective for spawnA.
                dump();
            }
        } catch (const std::exception& e) {
            std::cout << "mapdump error: " << e.what() << std::endl;
        }
        bc::end_turn();
    }
}
