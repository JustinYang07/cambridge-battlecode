#include "shop.h"
#include <array>
#include <optional>
#include <string>
#include <vector>
#include "../core/fixed_str.h"
#include "../core/trace.h"

namespace bc = cambc;

namespace shop {

namespace {

constexpr int kShardsPerEmblem = 5;

std::optional<bc::enum_map<bc::item_type>> inventory;
unsigned passiveMask = 0;  // bit p set when an emblem with item_passive p is owned

bc::item_type shardOf(Step s) {
    return bc::item_type(static_cast<std::uint32_t>(static_cast<int>(s.family) * 6 + static_cast<int>(s.stat) * 2));
}
bc::item_type emblemOf(Step s) {
    return bc::item_type(static_cast<std::uint32_t>(static_cast<int>(s.family) * 6 + static_cast<int>(s.stat) * 2 + 1));
}

const char* familyName(Family f) {
    static constexpr std::array<const char*, 7> names{"monster", "power", "durability", "treasure",
                                                      "jungle", "home", "bruiser"};
    return names[static_cast<std::size_t>(f)];
}

// Build orders: farm safely first (jungle), then faster (monster), then recall
// faster (home) and survive fights (durability), then raw stats.
std::vector<Step> plan(bc::champion_type type) {
    const auto& p = profile(type);
    const Stat main = p.primary;
    std::vector<Step> steps{{Family::jungle, main}, {Family::monster, main}};
    if (type == bc::champion_type::vesper_vale) steps.push_back({Family::treasure, main});
    steps.push_back({Family::home, Stat::health});
    steps.push_back({Family::durability, Stat::health});
    for (int i = 0; i < 6; ++i) steps.push_back({Family::power, main});
    return steps;
}

int owned(Step s) {
    if (!inventory) return 0;
    return static_cast<int>((*inventory)[shardOf(s)]) + kShardsPerEmblem * static_cast<int>((*inventory)[emblemOf(s)]);
}

// Which step are we on? Repeated steps (e.g. several power emblems) need cumulative targets.
std::optional<std::pair<Step, int>> currentStep(bc::champion_type type) {
    const auto steps = plan(type);
    for (std::size_t i = 0; i < steps.size(); ++i) {
        int target = 0;
        for (std::size_t j = 0; j <= i; ++j)
            if (steps[j].family == steps[i].family && steps[j].stat == steps[i].stat) target += kShardsPerEmblem;
        if (owned(steps[i]) < target) return std::make_pair(steps[i], target - owned(steps[i]));
    }
    return std::nullopt;
}

} // namespace

void refresh() {
    inventory = bc::self().item_count();
    passiveMask = 0;
    const std::pair<bc::item_passive, Family> families[] = {
        {bc::item_passive::monster, Family::monster}, {bc::item_passive::durability, Family::durability},
        {bc::item_passive::treasure, Family::treasure}, {bc::item_passive::jungle, Family::jungle},
        {bc::item_passive::home, Family::home}};
    // Index the raw counts directly: enum_map::operator[] validates keys and is slow under fuel metering.
    const auto* counts = inventory->ptr();
    for (const auto& [passive, family] : families)
        for (int stat = 0; stat < 3; ++stat)
            if (counts[static_cast<std::size_t>(emblemOf({family, static_cast<Stat>(stat)}))] > 0)
                passiveMask |= 1u << static_cast<unsigned>(passive);
}

bool hasPassive(bc::item_passive passive) { return passiveMask & (1u << static_cast<unsigned>(passive)); }

void spend(World& w) {
    auto& me = bc::self();
    // Shops are only open on our spawn or while dead (Tavi shops anywhere); skip the host calls otherwise.
    if (w.alive && !w.atSpawn && me.passive() != bc::passive_type::remote_shop) return;
    for (int guard = 0; guard < 4; ++guard) {
        const auto step = currentStep(w.type);
        if (!step) return;
        const auto shard = shardOf(step->first);
        const unsigned afford = static_cast<unsigned>(w.gold) / std::max(1u, shard.cost());
        const unsigned count = std::min<unsigned>(afford, static_cast<unsigned>(step->second));
        if (count == 0) return;
        try {
            if (!me.can_buy(shard, count)) return;
            me.buy(shard, count);
        } catch (const bc::error& e) {
            trace().note(std::string("buy failed: ") + e.message());
            return;
        }
        w.gold = static_cast<int>(me.gold_count());
        FixedStr<24> msg;
        trace().action((msg << 'b' << static_cast<int>(count) << familyName(step->first.family)).view());
        refresh();
    }
}

std::string progress() {
    const auto step = currentStep(bc::self().champion_name());
    if (!step) return "complete";
    static constexpr std::array<const char*, 3> stats{"str", "foc", "hp"};
    return std::string(familyName(step->first.family)) + "-" + stats[static_cast<std::size_t>(step->first.stat)] +
           " needs " + num(step->second);
}

} // namespace shop
