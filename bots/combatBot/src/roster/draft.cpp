#include "draft.h"
#include <algorithm>
#include <array>
#include <cambc.h>
#include <cstdio>
#include <string>
#include <vector>
#include "../core/json.h"
#include "../core/profile.h"
#include "../core/statics.h"
#include "../core/config.h"
#include "../strategy/mapplan.h"

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

namespace {

// attackBot2's tested roster, in priority order: the fallback and the baseline the map features adjust.
// Five picks: long-range, high-damage. Three picks: Cassi & Issac's extra body behind a melee front.
const std::vector<bc::champion_draft>& baseRoster(int teamSize) {
    using D = bc::champion_draft;
    static const std::vector<D> ranged{D::cassi_and_issac, D::stellar, D::tavi, D::varg, D::ren};
    static const std::vector<D> small{D::cassi_and_issac, D::varg, D::ren, D::stellar, D::tavi};
    // Mite's 3-pick roster on open maps: Ren, Stellar, Cassi & Issac (Varg last).
    static const std::vector<D> mite{D::cassi_and_issac, D::ren, D::stellar, D::varg, D::tavi};
    if (teamSize <= 3) return tune::draft_mite_small ? mite : small;
    return ranged;
}

// Picks outside the tested roster that strong map features can promote (a gold-heavy map for
// Vesper, a pack-heavy map for the tanks). They start below the roster and need the bonus.
const std::vector<bc::champion_draft>& extras() {
    using D = bc::champion_draft;
    static const std::vector<D> e{D::vesper_vale, D::vendetta, D::ryth, D::marcella};
    return e;
}

} // namespace

void play_draft() {
    const auto fuelStart = bc::fuel_remaining();
    std::vector<int> cp;  // fuel checkpoints (k) for the draft fuel stress test
    auto mark = [&] { cp.push_back(static_cast<int>(bc::fuel_remaining() / 1000)); };
    const auto mine = bc::draft::current_draft(bc::team::ally);
    const int teamSize = static_cast<int>(bc::draft::team_size());
    const auto& plan = templateFor(teamSize);
    const auto pickIndex = std::min(mine.size(), plan.size() - 1);
    const Slot want = plan[pickIndex];
    auto taken = [&](bc::champion_draft d) { return std::find(mine.begin(), mine.end(), d) != mine.end(); };

    // A crashed or timed-out draft gets random picks: settle the fallback pick first, then
    // let the map analysis (10x fuel on the first draft turn) improve on it.
    bc::champion_draft fallback = bc::champion_draft::varg;
    for (auto d : baseRoster(teamSize))
        if (!taken(d)) { fallback = d; break; }
    if (taken(fallback))
        for (auto d : priorities(want))
            if (!taken(d)) { fallback = d; break; }

    mark();
    std::vector<Scored> options;
    std::string planText;
    try {
        mapplan::analyse();  // map features; runs once, on the first draft turn (10x fuel)
        // First turn only (10x fuel): per-group distance fields while a whole one fits, then the
        // per-champion camp tables. Later picks (100k) only assign routes; an overrun draft turn
        // gets a random pick, so they build nothing expensive.
        mark();
        if (mine.empty()) mapplan::buildFields(tune::draft_fuel_reserve);
        mapplan::buildNeighbours(mine.empty() ? tune::draft_neighbour_floor : tune::draft_neighbour_floor_late);
        mark();
        const auto& base = baseRoster(teamSize);
        for (std::size_t i = 0; i < base.size(); ++i)
            if (!taken(base[i]))
                options.push_back({base[i], 10.0f - static_cast<float>(i) + mapplan::draftBonus(base[i], teamSize), 0, 0, 1});
        const auto& more = extras();
        for (std::size_t i = 0; i < more.size(); ++i)
            if (!taken(more[i]))
                options.push_back({more[i], 5.5f - 0.25f * static_cast<float>(i) + mapplan::draftBonus(more[i], teamSize), 0, 0, 1});
        if (options.empty())
            for (int i = 0; i < kChampionDrafts; ++i) {
                const auto d = static_cast<bc::champion_draft>(i);
                if (!taken(d)) options.push_back(score(d, teamSize, false));
            }
        std::stable_sort(options.begin(), options.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
        if (!options.empty()) fallback = options.front().pick;
        // Fixed comps, taken from the top for however many picks the map has (3 picks: the first
        // three; 4 and 5 add the next). Any champion can be taken at any time. Gold-heavy maps get
        // the caster comp; every other map Ren, Stellar, Cassi & Issac, Ryth, Varg.
        using D = bc::champion_draft;
        static const std::vector<D> standardOrder{D::ren, D::stellar, D::cassi_and_issac, D::ryth, D::varg};
        static const std::vector<D> goldOrder{D::stellar, D::tavi, D::prism, D::cassi_and_issac, D::ren};
        // Gold-heavy: most farmable camps hold no gems at all (contest, stampede).
        int goldOnly = 0, farmable = 0;
        for (const auto& g : Statics::get().groups) {
            if (g.hasMonolith || (g.gems == 0 && g.gold == 0)) continue;
            ++farmable;
            if (g.gems == 0) ++goldOnly;
        }
        const float goldOnlyShare = farmable > 0 ? static_cast<float>(goldOnly) / static_cast<float>(farmable) : 0.0f;
        const bool goldMap = goldOnlyShare >= tune::gold_draft_share;
        planText += "gold-only camps " + std::to_string(static_cast<int>(goldOnlyShare * 100.0f)) + "%; ";
        const auto& fixedOrder = goldMap ? goldOrder : standardOrder;
        if (tune::fixed_draft) {
            for (auto d : fixedOrder)
                if (!taken(d)) { fallback = d; break; }
            planText += goldMap ? "gold map comp; " : "standard comp; ";
        }
        // Farming loops: solved for the roster we expect (the best options now), continued on the
        // later picks while fuel is spare; the last pick assigns the real roster to them.
        if (mine.empty()) {
            std::vector<bc::champion_draft> guess;
            if (tune::fixed_draft) {
                for (auto d : fixedOrder)
                    if (static_cast<int>(guess.size()) < teamSize) guess.push_back(d);
            } else {
                for (const auto& o : options)
                    if (static_cast<int>(guess.size()) < teamSize) guess.push_back(o.pick);
            }
            mapplan::solveLoops(mapplan::bodies(guess), tune::draft_loop_floor);
            mark();
            mapplan::prepare(tune::draft_prepare_floor);
        } else {
            mapplan::solveLoops({}, tune::draft_loop_floor_late);
        }
        mark();
    } catch (const std::exception& e) {
        planText = std::string("draft error: ") + e.what();
    }

    // Engine calls after the pick fail (error 18), so memory and trace go first.
    try {
        // Routes for the roster so far plus this pick; the last pick's write is the one that counts.
        auto roster = mine;
        roster.push_back(fallback);
        mark();
        const auto routes = mapplan::planFor(mapplan::bodies(roster), teamSize, tune::draft_plan_floor);
        mark();
        mapplan::write(routes);
        mark();
        // The full description is for the lab only: skip it when the turn is short of fuel.
        planText += bc::fuel_remaining() > tune::draft_describe_floor ? mapplan::describe(routes) : std::string("routes written");
    } catch (const std::exception& e) {
        planText += std::string("route error: ") + e.what();
    }

    mark();
    Json j;
    j.begin().field("pick", static_cast<int>(mine.size())).field("teamSize", teamSize).field("role", slotName(want)).array("opts");
    for (const auto& o : options) j.begin().field("k", name(o.pick)).field("s", o.score).end();
    j.endArray().field("chosen", name(fallback)).field("plan", planText)
        .field("fuelStart", static_cast<int>(fuelStart / 1000)).field("fuelLeft", static_cast<int>(bc::fuel_remaining() / 1000)).array("cp");
    for (int k : cp) j.value(static_cast<long long>(k));
    j.endArray().end();
    std::printf("@D%.*s\n", static_cast<int>(j.view().size()), j.view().data());
    std::fflush(stdout);

    bc::draft::pick(fallback);
}
