// Lab helper: measures the fuel cost of common SDK calls and prints "@FUEL{...}".
// Run: cambc run tool-fuelprobe tool-fuelprobe --map <map> --rounds 30
#include <cambc.h>
#include <wasi/api.h>
#include <cmath>
#include <cstring>
#include "json.h"
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>
#include <string>

namespace bc = cambc;

static std::string out;

template <class F>
static void measure(const char* name, int reps, F&& f) {
    const auto before = bc::fuel_remaining();
    for (int i = 0; i < reps; ++i) f();
    const auto used = before - bc::fuel_remaining();
    out += (out.empty() ? "" : ",") + std::string("\"") + name + "\":" + std::to_string(used / static_cast<unsigned long long>(reps));
}

int main() {
    int turn = 0;
    volatile long sink = 0;
    while (true) {
        try {
            if (bc::is_draft()) {
                auto mine = bc::draft::current_draft(bc::team::ally);
                for (int i = 0; i < 11; ++i) {
                    auto pick = static_cast<bc::champion_draft>(i);
                    if (std::find(mine.begin(), mine.end(), pick) == mine.end()) { bc::draft::pick(pick); break; }
                }
            } else if (turn < 12 && bc::self() == bc::champions(bc::team::ally)[0]) {
                ++turn;
                std::printf("@R turn %d round %u fuel_at_start %llu\n", turn, bc::round_number(),
                            static_cast<unsigned long long>(bc::fuel_remaining()));
                std::fflush(stdout);
            } else if (++turn == 20 && bc::self() == bc::champions(bc::team::ally)[0]) {
                auto& me = bc::self();
                measure("empty", 10, [] {});
                measure("round_number", 10, [&] { sink += bc::round_number(); });
                measure("self.position", 10, [&] { sink += me.position().x; });
                measure("self.health", 10, [&] { sink += static_cast<long>(me.health()); });
                measure("self.can_move", 10, [&] { sink += me.can_move(); });
                measure("self.abilities", 5, [&] { sink += me.abilities()[0].slot(); });
                measure("ability.is_ready", 10, [&] { sink += me.abilities()[0].is_ready(); });
                measure("item_count", 5, [&] { sink += me.item_count()[bc::item_type::jungle_shard_of_strength]; });
                measure("vision.visible_tiles", 5, [&] { sink += bc::vision::visible_tiles().count(); });
                measure("vision.champions", 5, [&] { sink += static_cast<long>(bc::vision::champions().size()); });
                measure("vision.camps", 5, [&] { sink += static_cast<long>(bc::vision::camps().size()); });
                measure("vision.champion_tiles", 5, [&] { sink += bc::vision::champion_tiles().is_empty(); });
                measure("champions(ally)", 5, [&] { sink += static_cast<long>(bc::champions(bc::team::ally).size()); });
                measure("global_submitted_gems", 10, [&] { sink += bc::global_submitted_gems(bc::team::ally); });
                measure("map.walls_ref", 10, [&] { sink += bc::map::walls().width(); });
                bc::bitboard a = bc::map::walls(), b = bc::map::holes();
                measure("bitboard_or", 10, [&] { auto c = a | b; sink += c.width(); });
                measure("bitboard_and_is_empty", 10, [&] { sink += (a & b).is_empty(); });
                measure("bitboard_is_set", 20, [&] { sink += a.is_set({3, 3}); });
                measure("bitboard_ctor_clear", 10, [&] { bc::bitboard c; c.clear(); sink += c.width(); });
                measure("bitboard_positions", 5, [&] { sink += static_cast<long>(a.positions().size()); });
                measure("draw_chebyshev3", 10, [&] { bc::bitboard c; c.clear(); c.draw_chebyshev(me.position(), 3); });
                measure("reachable_tiles1", 10, [&] { sink += a.reachable_tiles(me.position(), 1).count(); });
                bc::distance_field field;
                bc::bitboard roots, visited, dest;
                roots.clear();
                roots.set(me.position());
                measure("distance_field.compute_full", 3, [&] { field.compute(a, roots, visited); });
                dest.clear();
                dest.set({me.position().x + 5, me.position().y + 5});
                measure("distance_field.compute_to_5", 3, [&] { field.compute_to(a, roots, dest, visited); });
                measure("distance_field.get", 20, [&] { sink += field.get(me.position()); });
                auto groups = bc::map::camp_groups();
                measure("map.camp_groups", 2, [&] { sink += static_cast<long>(bc::map::camp_groups().size()); });
                measure("group.state", 5, [&] { try { (void)groups[0].state(); } catch (...) {} });
                measure("group.spawn_tiles", 5, [&] { sink += groups[0].spawn_tiles().count(); });
                auto camps = bc::vision::camps();
                if (!camps.empty()) {
                    measure("camp.health", 10, [&] { sink += static_cast<long>(camps[0].health()); });
                    measure("camp.base_stats", 5, [&] { sink += static_cast<long>(camps[0].base_stats().base_health); });
                }
                {
                    auto list = bc::vision::camps();
                    const auto before = bc::fuel_remaining();
                    for (const auto& c : list) sink += static_cast<long>(c.health());
                    const auto used = before - bc::fuel_remaining();
                    out += ",\"camps_seen\":" + std::to_string(list.size()) + ",\"first_health_each\":" +
                           std::to_string(list.empty() ? 0 : used / list.size());
                    const auto b2 = bc::fuel_remaining();
                    for (const auto& c : list) sink += c.spawn_position().x;
                    out += ",\"spawn_position_each\":" + std::to_string(list.empty() ? 0 : (b2 - bc::fuel_remaining()) / list.size());
                    if (!list.empty()) {
                        const auto p = list[0].spawn_position();
                        measure("vision.camp_at", 3, [&] { sink += static_cast<long>(bc::vision::camp_at(p).health()); });
                        measure("camp_tiles", 3, [&] { sink += bc::vision::camp_tiles().count(); });
                    }
                    auto champs = bc::vision::champions();
                    const auto b3 = bc::fuel_remaining();
                    for (const auto& c : champs) sink += static_cast<long>(c.health());
                    out += ",\"champs_seen\":" + std::to_string(champs.size()) + ",\"champ_first_health_each\":" +
                           std::to_string(champs.empty() ? 0 : (b3 - bc::fuel_remaining()) / champs.size());
                    const auto b4 = bc::fuel_remaining();
                    for (const auto& c : champs) sink += static_cast<long>(c.max_health()) + c.position().x;
                    out += ",\"champ_two_more_each\":" + std::to_string(champs.empty() ? 0 : (b4 - bc::fuel_remaining()) / champs.size());
                    std::vector<int> v;
                    measure("vector_push_100", 3, [&] { std::vector<int> x; for (int i = 0; i < 100; ++i) x.push_back(i); sink += x[5]; });
                    measure("string_concat_10", 3, [&] { std::string x; for (int i = 0; i < 10; ++i) x += "abcdefghij"; sink += x[3]; });
                    measure("float_loop_100", 3, [&] { float f = 0; for (int i = 0; i < 100; ++i) f += static_cast<float>(i) * 1.5f; sink += static_cast<long>(f); });
                }
                {
                    bc::distance_field f2;
                    bc::bitboard r2, v2;
                    r2.clear();
                    r2.set(me.position());
                    f2.compute(bc::map::walls(), r2, v2);
                    measure("distfield_export", 2, [&] { sink += f2.export_linear_buffer()[0]; });
                    measure("bitboard_export", 3, [&] { sink += v2.export_linear_buffer()[0]; });
                    measure("bitboard_raw_export", 3, [&] { sink += v2.export_raw_buffer()[0]; });
                    auto groups2 = bc::map::camp_groups();
                    const bc::camp* stored = nullptr;
                    for (const auto& g : groups2)
                        for (const auto& c : g.camps())
                            if (!stored && bc::vision::visible_tiles().is_set(c.spawn_position())) stored = &c;
                    if (stored) {
                        measure("stored_camp.health", 3, [&] { sink += static_cast<long>(stored->health()); });
                        measure("stored_camp.is_visible", 3, [&] { sink += stored->is_visible(); });
                        const auto p = stored->spawn_position();
                        measure("map.camp(pos).health", 3, [&] { sink += static_cast<long>(bc::map::camp(p).health()); });
                    }
                }
                {
                    static char big[2048];
                    for (int i = 0; i < 2047; ++i) big[i] = 'x';
                    for (int n : {10, 100, 1000, 2000}) {
                        const auto b0 = bc::fuel_remaining();
                        std::fwrite(big, 1, static_cast<std::size_t>(n), stdout);
                        std::fputc('\n', stdout);
                        std::fflush(stdout);
                        out += ",\"stdout_" + std::to_string(n) + "\":" + std::to_string(b0 - bc::fuel_remaining());
                    }
                    for (int n : {100, 1000}) {
                        const auto b0 = bc::fuel_remaining();
                        __wasi_ciovec_t iov{reinterpret_cast<const uint8_t*>(big), static_cast<std::size_t>(n)};
                        __wasi_size_t written = 0;
                        __wasi_fd_write(1, &iov, 1, &written);
                        out += ",\"wasi_write_" + std::to_string(n) + "\":" + std::to_string(b0 - bc::fuel_remaining());
                    }
                }
                {
                    static Json j;
                    measure("json_100_ints", 1, [&] { j.reset(); for (int i = 0; i < 100; ++i) j.value(static_cast<long long>(i * 37)); sink += static_cast<long>(j.view().size()); });
                    measure("json_100_doubles", 1, [&] { j.reset(); for (int i = 0; i < 100; ++i) j.value(i * 1.37); sink += static_cast<long>(j.view().size()); });
                    measure("json_100_strings", 1, [&] { j.reset(); for (int i = 0; i < 100; ++i) j.value("travel"); sink += static_cast<long>(j.view().size()); });
                    measure("llround_100", 1, [&] { long long a = 0; for (int i = 0; i < 100; ++i) a += std::llround(i * 1.37); sink += static_cast<long>(a); });
                    measure("memcpy_100x8", 1, [&] { char d[16]; for (int i = 0; i < 100; ++i) std::memcpy(d, "abcdefgh", static_cast<std::size_t>(8 - (i & 1))); sink += d[0]; });
                }
                measure("fuel_remaining", 10, [&] { sink += static_cast<long>(bc::fuel_remaining()); });
                measure("snprintf", 10, [&] { char buf[64]; std::snprintf(buf, 64, "%d,%d,%.2f", 1, 2, 3.0); sink += buf[0]; });
                std::printf("@FUEL{%s}\n", out.c_str());
                std::fflush(stdout);
            }
        } catch (const std::exception& e) {
            std::printf("probe error: %s\n", e.what());
        }
        bc::end_turn();
    }
}
