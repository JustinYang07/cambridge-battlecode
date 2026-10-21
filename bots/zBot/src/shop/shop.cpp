#include "shop.h"
#include <algorithm>
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
bool inventoryStale = true;

// Tavi shops anywhere. A champion's passive never changes: ask the engine once (it is a host
// call, and spend()/affordable() asked every turn).
bool remoteShop() {
    static int cached = -1;
    if (cached < 0) cached = bc::self().passive() == bc::passive_type::remote_shop ? 1 : 0;
    return cached == 1;
}
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
// Treasure-first map (loopBot): the treasure emblem's extra gold beats jungle's safer farming where
// gold is everywhere close to spawn and the early camps are weak (self-play, treasure - jungle:
// swirl +774, eight +269, fragment +212; corners -837, interlock -545, stampede -360). Small maps
// put every cluster and wolf within early reach; mostly-elk maps do not count (elk are slow and
// tough early). A map whose only gems are in its Monoliths (contest) is a gold game too.
bool treasureMap() {
    // The map never changes: worked out once (plan() runs every turn; rescanning cost fuel).
    static int cached = -1;
    if (cached >= 0) return cached == 1;
    const auto& s = Statics::get();
    float elkHp = 0, wolfHp = 0;
    int gems = 0;
    for (const auto& g : s.groups) {
        if (g.hasMonolith) continue;
        gems += g.gems;
        for (const auto& m : g.killOrder)
            if (m.strength > 0) (m.strength >= 40.0f ? elkHp : wolfHp) += m.health;  // monsters only
    }
    const bool small = s.width * s.height <= tune::treasure_map_max_area;
    const bool mostlyElk = elkHp > tune::treasure_map_max_elk_share * std::max(elkHp + wolfHp, 1.0f);
    cached = (gems == 0 || (small && !mostlyElk)) ? 1 : 0;
    return cached == 1;
}

// combatBotv3: farming or combat map, from the map editor's size classes (longest side: small 30-49,
// medium 50-74, large 75-100) and, for medium maps, how far the walk between the spawns is against
// the straight line (lanes and rows force detours). Measured on the pool (deaths per 100 rounds):
// large maps and medium maps with walk >= 1.5x straight are the farming maps (expanse, lanes,
// ambush, rushbait, rows, pilgrimage); small and other medium maps are where the fights are.
bool farmingMap() {
    static int cached = -1;
    if (cached >= 0) return cached == 1;
    const auto& s = Statics::get();
    const int side = std::max(s.width, s.height);
    if (side >= tune::large_map_side) { cached = 1; return true; }
    if (side < tune::medium_map_side) { cached = 0; return false; }
    cached = 0;
    try {
        // Walking distance from the tiles beside their spawn to our spawn centre.
        bc::bitboard beside;
        beside.clear();
        for (auto p : s.enemySpawn.positions()) beside.draw_chebyshev(p, 1.0f);
        beside = beside & ~s.terrain;
        bc::distance_field field;
        bc::bitboard visited;
        field.compute(s.terrain, beside, visited);
        const int straight = std::max(1, dist(s.spawnCenter, s.enemySpawnCenter));
        if (visited.is_set(s.spawnCenter) && static_cast<float>(field.get(s.spawnCenter)) >= tune::farming_walk_ratio * static_cast<float>(straight))
            cached = 1;
    } catch (const bc::error&) {
    }
    return cached == 1;
}

std::vector<Step> plan(bc::champion_type type) {
    // combatBotv3: per-champion item orders (user, from the top teams' builds), then Power emblems
    // of the last stat for good: duplicate emblems add no second passive, but Power has none and
    // its stats stack. Champions without an entry keep build_variant's plan below.
    if (tune::champion_builds) {
        using F = Family;
        using T = bc::champion_type;
        std::vector<Step> s;
        auto add = [&](F f, Stat st) { s.push_back({f, st}); };
        Stat last = Stat::focus;
        // yBot: Fish's orders (their ladder replays, first three emblems per champion), Power after,
        // and no farming-map Jungle/Monster swaps (Fish never buys Jungle).
        if (tune::fish_builds) {
            switch (type) {
            case T::stellar: add(F::power, Stat::focus); add(F::home, Stat::health); add(F::durability, Stat::health); last = Stat::focus; break;
            case T::ren:     add(F::bruiser, Stat::health); add(F::durability, Stat::health); add(F::power, Stat::focus); last = Stat::focus; break;
            case T::cassi:   add(F::power, Stat::focus); add(F::durability, Stat::focus); add(F::bruiser, Stat::health); last = Stat::focus; break;
            case T::issac:   add(F::power, Stat::strength); add(F::home, Stat::health); add(F::durability, Stat::health); last = Stat::strength; break;
            case T::ryth:    add(F::bruiser, Stat::health); add(F::durability, Stat::health); add(F::power, Stat::strength); last = Stat::strength; break;
            case T::varg:    add(F::power, Stat::strength); add(F::home, Stat::health); add(F::durability, Stat::health); last = Stat::strength; break;
            case T::tavi:    add(F::treasure, Stat::focus); add(F::durability, Stat::focus); add(F::power, Stat::focus); last = Stat::focus; break;
            case T::prism:   add(F::power, Stat::focus); last = Stat::focus; break;
            default: break;
            }
            if (!s.empty()) {
                for (int i = 0; i < 12; ++i) add(F::power, last);
                return s;
            }
        }
        switch (type) {
        case T::cassi:   add(F::bruiser, Stat::health); add(F::durability, Stat::focus); last = Stat::focus; break;
        case T::issac:   add(F::durability, Stat::health); last = Stat::strength; break;
        case T::ryth:    add(F::bruiser, Stat::health); add(F::durability, Stat::focus); last = Stat::focus; break;
        case T::stellar: add(F::durability, Stat::health); last = Stat::focus; break;
        case T::tavi:    add(F::treasure, Stat::focus); add(F::durability, Stat::focus); last = Stat::focus; break;
        case T::prism:   add(F::durability, Stat::health); last = Stat::focus; break;
        case T::ren:     add(F::bruiser, Stat::health); add(F::durability, Stat::health); last = Stat::focus; break;
        case T::varg:    add(F::durability, Stat::health); last = Stat::strength; break;
        default: break;
        }
        if (!s.empty()) {
            // Farming maps: a Jungle emblem of our damage stat first (30% less camp damage: the
            // orders above lost every game on rows and rushbait without it).
            if (tune::farm_map_jungle && farmingMap()) s.insert(s.begin(), Step{F::jungle, last});
            // Farming maps: Monster (10% more damage to camps) in place of Durability (10% less
            // damage taken), which pays off in fights (user).
            if (tune::farm_map_monster && farmingMap())
                for (auto& st : s)
                    if (st.family == F::durability) st.family = F::monster;
            for (int i = 0; i < 12; ++i) add(F::power, last);
            return s;
        }
    }
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
        add(tune::treasure_maps && treasureMap() ? F::treasure : F::jungle, main); add(F::durability, main); add(F::bruiser, Stat::health);
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

void refresh(const World& w) {
    // Items change only when we buy, and we can buy only on spawn, while dead, or anywhere as Tavi:
    // elsewhere the inventory read last time still holds (it is a host call every turn otherwise).
    if (inventory && !inventoryStale && w.alive && !w.atSpawn && !remoteShop()) return;
    inventoryStale = false;
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

bool isFarmingMap() { return farmingMap(); }

bool hasPassive(bc::item_passive passive) { return passiveMask & (1u << static_cast<unsigned>(passive)); }

void spend(World& w) {
    auto& me = bc::self();
    // Shops are only open on our spawn or while dead (Tavi shops anywhere); skip the host calls otherwise.
    if (w.alive && !w.atSpawn && !remoteShop()) return;
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
        inventoryStale = true;
        refresh(w);
    }
}

Affordable affordable(const World& w) {
    Affordable a;
    if (w.atSpawn || remoteShop()) return a;
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

namespace {
bool crystals = false;
}
void wantCrystals() { crystals = true; }
bool crystalsWanted() { return crystals; }

void buyConsumables(World& w) {
    const bool buyCrystals = tune::buy_tp_crystals || crystals;
    if (!tune::buy_altar_locks && !buyCrystals) return;
    auto& me = bc::self();
    if (w.alive && !w.atSpawn) return;
    for (auto type : {bc::consumable_type::altar_lock, bc::consumable_type::tp_crystal}) {
        if (type == bc::consumable_type::altar_lock ? !tune::buy_altar_locks : !buyCrystals) continue;
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
