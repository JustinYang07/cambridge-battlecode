#include "shop.h"
#include <array>
#include <optional>
#include <string>
#include <vector>
#include "../core/config.h"
#include "../core/fixed_str.h"
#include "../core/trace.h"

namespace bc = cambc;

namespace shop {

namespace {

// Shards per emblem: 5, except Power which takes 6 (it grants a sixth shard's worth of stats).
int shardsPer(Family f) { return f == Family::power ? 6 : 5; }

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

// Build orders. tune::build_variant selects one (see config.h); 0 is the default:
// farm safely first (jungle), then faster (monster), then recall faster (home) and
// survive fights (durability), then raw stats.
std::vector<Step> plan(bc::champion_type type) {
    const auto& p = profile(type);
    Stat main = p.primary;
    const bool vesper = type == bc::champion_type::vesper_vale;
    // Stat matching: Ryth turns 100 bonus health into 1 strength; casters scale with focus.
    if (tune::stat_matching) {
        if (type == bc::champion_type::ryth) main = Stat::health;
        if (p.roles.caster >= 0.5f) main = Stat::focus;
    }
    // Variant 11 (the ladder's top teams): casters build focus, their spell stat.
    if ((tune::build_variant == 11 || tune::build_variant == 12) && p.roles.caster >= 0.5f) main = Stat::focus;
    using F = Family;
    std::vector<Step> steps;
    auto add = [&](F f, Stat s) { steps.push_back({f, s}); };
    switch (tune::build_variant) {
    case 1:  // treasure rush for everyone
        add(F::treasure, main); add(F::jungle, main); add(F::monster, main); add(F::home, Stat::health);
        add(F::durability, Stat::health);
        break;
    case 2:  // treasure rush only on Vesper (20% + 20% gold)
        if (vesper) add(F::treasure, main);
        add(F::jungle, main); add(F::monster, main); add(F::home, Stat::health); add(F::durability, Stat::health);
        break;
    case 3:  // damage to camps first
        add(F::monster, main); add(F::jungle, main); add(F::home, Stat::health); add(F::durability, Stat::health);
        break;
    case 4:  // raw stats early
        add(F::jungle, main); add(F::power, main); add(F::monster, main); add(F::home, Stat::health);
        break;
    case 5:  // fast rotations early
        add(F::jungle, main); add(F::home, Stat::health); add(F::monster, main); add(F::durability, Stat::health);
        break;
    case 6:  // carries protect their bags early
        add(F::jungle, main);
        if (p.roles.farmer >= 0.5f) add(F::durability, Stat::health);
        add(F::monster, main); add(F::home, Stat::health);
        break;
    case 7:  // treasure rush, then damage to camps
        add(F::treasure, main); add(F::monster, main); add(F::jungle, main); add(F::home, Stat::health);
        add(F::durability, Stat::health);
        break;
    case 8:  // treasure rush, then fast rotations
        add(F::treasure, main); add(F::home, Stat::health); add(F::jungle, main); add(F::monster, main);
        add(F::durability, Stat::health);
        break;
    case 11:  // like Mite: safe farming, raw stats, then survival; no treasure
        add(F::jungle, main); add(F::power, main); add(F::durability, main);
        break;
    case 12:  // Mite's order from its games: jungle, durability, a bruiser emblem of health, then power
        add(F::jungle, main); add(F::durability, main); add(F::bruiser, Stat::health);
        break;
    case 10:  // camp damage first, then treasure
        add(F::monster, main); add(F::treasure, main); add(F::jungle, main); add(F::home, Stat::health);
        add(F::durability, Stat::health);
        break;
    case 9:  // treasure rush, then raw stats
        add(F::treasure, main); add(F::power, main); add(F::jungle, main); add(F::monster, main);
        break;
    default:
        add(F::jungle, main); add(F::monster, main);
        if (vesper) add(F::treasure, main);
        add(F::home, Stat::health); add(F::durability, Stat::health);
        break;
    }
    for (int i = 0; i < 6; ++i) add(F::power, main);
    return steps;
}

int owned(Step s) {
    if (!inventory) return 0;
    return static_cast<int>((*inventory)[shardOf(s)]) + shardsPer(s.family) * static_cast<int>((*inventory)[emblemOf(s)]);
}

// Which step are we on? Repeated steps (e.g. several power emblems) need cumulative targets.
std::optional<std::pair<Step, int>> currentStep(bc::champion_type type) {
    const auto steps = plan(type);
    for (std::size_t i = 0; i < steps.size(); ++i) {
        int target = 0;
        for (std::size_t j = 0; j <= i; ++j)
            if (steps[j].family == steps[i].family && steps[j].stat == steps[i].stat) target += shardsPer(steps[j].family);
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

Affordable affordable(const World& w) {
    Affordable a;
    if (w.atSpawn || bc::self().passive() == bc::passive_type::remote_shop) return a;
    // The build order is rebuilt on every call: cache it for the round.
    static int cachedRound = -1;
    static Affordable cached;
    if (cachedRound == w.round) return cached;
    cachedRound = w.round;
    const auto step = currentStep(w.type);
    if (step) {
        const unsigned cost = std::max(1u, shardOf(step->first).cost());
        const int afford = static_cast<int>(static_cast<unsigned>(w.gold) / cost);
        a.shards = std::min(afford, step->second);
        a.completes = afford >= step->second;
    }
    cached = a;
    return a;
}

int shardsOwned() {
    if (!inventory) return 0;
    const auto* counts = inventory->ptr();
    int total = 0;
    for (int i = 0; i < 42; ++i) total += static_cast<int>(counts[i]) * (i % 2 ? (i / 6 == 1 ? 6 : 5) : 1);
    return total;
}

void buyConsumables(World& w) {
    if (!tune::buy_altar_locks && !tune::buy_tp_crystals) return;
    auto& me = bc::self();
    if (w.alive && !w.atSpawn) return;
    for (auto type : {bc::consumable_type::altar_lock, bc::consumable_type::tp_crystal}) {
        if (type == bc::consumable_type::altar_lock ? !tune::buy_altar_locks : !tune::buy_tp_crystals) continue;
        try {
            if (me.can_buy(type)) {
                me.buy(type);
                trace().action(type == bc::consumable_type::altar_lock ? "b1lock" : "b1crystal");
            }
        } catch (const bc::error&) {
        }
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
