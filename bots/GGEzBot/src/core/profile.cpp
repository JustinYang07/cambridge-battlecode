#include "profile.h"
#include <algorithm>
#include <array>
#include "config.h"

namespace bc = cambc;

namespace {

// Indexed by cambc::champion_type. Role mixes sum to 1.
// farmPower ~ basic damage per turn on camps including resets/splash; survivability ~ durability.
// Patch: base strength changes applied to farmPower one for one (Ryth -5, Vesper +5, Stellar -5,
// Willow +10, Ren -5, Varg -10, Tavi +10); Cassi health 700 -> 600 and Issac/Cassi scaling nerfs.
// Index 12 is Heart of Ruin (not in the 2.2.4 SDK enums yet: recognised, never drafted).
constexpr std::array<Profile, 13> kProfiles{{
    //               farmer tank caster support duelist
    {"Ryth",        {0.2f, 0.8f, 0.0f, 0.0f, 0.0f}, Stat::strength, 51, 1.25f},
    {"Vesper Vale", {1.0f, 0.0f, 0.0f, 0.0f, 0.0f}, Stat::strength, 56, 0.9f},
    {"Vendetta",    {0.8f, 0.0f, 0.0f, 0.0f, 0.2f}, Stat::strength, 80, 0.9f},
    {"Cassi",       {0.0f, 0.0f, 1.0f, 0.0f, 0.0f}, Stat::strength, 28, 0.62f},
    {"Issac",       {0.4f, 0.0f, 0.0f, 0.0f, 0.6f}, Stat::strength, 42, 0.85f},
    {"Stellar",     {0.0f, 0.0f, 1.0f, 0.0f, 0.0f}, Stat::strength, 30, 0.75f},
    {"Prism",       {0.0f, 0.0f, 1.0f, 0.0f, 0.0f}, Stat::strength, 33, 0.65f},
    {"Marcella",    {0.0f, 0.6f, 0.0f, 0.0f, 0.4f}, Stat::strength, 50, 1.2f},
    {"Willow",      {0.0f, 0.0f, 0.0f, 1.0f, 0.0f}, Stat::strength, 60, 0.7f},
    {"Ren",         {0.0f, 0.3f, 0.0f, 0.0f, 0.7f}, Stat::strength, 40, 0.95f},
    {"Varg",        {0.5f, 0.5f, 0.0f, 0.0f, 0.0f}, Stat::strength, 46, 1.1f},
    {"Tavi",        {0.0f, 0.0f, 0.7f, 0.0f, 0.3f}, Stat::strength, 50, 0.75f},
    {"Heart of Ruin", {0.2f, 0.5f, 0.0f, 0.0f, 0.3f}, Stat::focus, 45, 1.2f},  // melee, 1000 health, shield + dash stun
}};

Tuning derive(const RoleMix& r) {
    Tuning t{};
    t.maxFightHpUse = 0.70f + 0.20f * r.tank;                 // tank 0.90, carry 0.70
    t.healThreshold = 0.22f - 0.07f * r.tank + 0.05f * r.farmer;  // critical health: ~15% tank, ~27% carry
    t.bankTarget = (60.0f - 20.0f * r.farmer + 20.0f * r.tank - 15.0f * r.caster) * tune::gem_scale;  // attackBot2: fragile casters bank sooner // carry banks at ~40, tank at ~76
    t.fightAppetite = 0.6f + 0.6f * r.duelist + 1.6f * r.caster;  // attackBot2: casters burst enemies down
    t.partyAbsorb = std::clamp(0.70f + 0.25f * r.tank - 0.30f * (r.farmer + r.caster + r.support), 0.3f, 0.95f);
    t.leadsPull = r.tank >= 0.5f;
    t.followsParty = r.support >= 0.5f;
    return t;
}

const std::array<Tuning, 13>& tunings() {
    static const std::array<Tuning, 13> table = [] {
        std::array<Tuning, 13> out{};
        for (std::size_t i = 0; i < kProfiles.size(); ++i) out[i] = derive(kProfiles[i].roles);
        return out;
    }();
    return table;
}

std::size_t index(bc::champion_type type) {
    const auto i = static_cast<std::size_t>(type);
    return i < kProfiles.size() ? i : 0;
}

} // namespace

const Profile& profile(bc::champion_type type) { return kProfiles[index(type)]; }
const Tuning& tuning(bc::champion_type type) { return tunings()[index(type)]; }
bool isTank(bc::champion_type type) { return tuning(type).leadsPull; }
bool stellarLike(bc::champion_type type) { return type == bc::champion_type::stellar || (tune::prism_for_stellar && type == bc::champion_type::prism); }
const char* name(bc::champion_type type) { return profile(type).name; }

const char* name(bc::champion_draft pick) {
    static constexpr std::array<const char*, 12> names{"Ryth", "Vesper Vale", "Vendetta", "Cassi & Issac", "Stellar",
                                                       "Prism", "Marcella", "Willow", "Ren", "Varg", "Tavi", "Heart of Ruin"};
    const auto i = static_cast<std::size_t>(pick);
    return i < names.size() ? names[i] : "?";
}
