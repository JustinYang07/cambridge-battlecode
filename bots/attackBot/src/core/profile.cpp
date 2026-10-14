#include "profile.h"
#include <algorithm>
#include <array>

namespace bc = cambc;

namespace {

// Indexed by cambc::champion_type. Role mixes sum to 1.
// farmPower ~ basic damage per turn on camps including resets/splash; survivability ~ durability.
constexpr std::array<Profile, 12> kProfiles{{
    //               farmer tank caster support duelist
    {"Ryth",        {0.2f, 0.8f, 0.0f, 0.0f, 0.0f}, Stat::strength, 56, 1.25f},
    {"Vesper Vale", {1.0f, 0.0f, 0.0f, 0.0f, 0.0f}, Stat::strength, 51, 0.9f},
    {"Vendetta",    {0.8f, 0.0f, 0.0f, 0.0f, 0.2f}, Stat::strength, 80, 0.9f},
    {"Cassi",       {0.0f, 0.0f, 1.0f, 0.0f, 0.0f}, Stat::strength, 30, 0.7f},
    {"Issac",       {0.4f, 0.0f, 0.0f, 0.0f, 0.6f}, Stat::strength, 45, 0.9f},
    {"Stellar",     {0.0f, 0.0f, 1.0f, 0.0f, 0.0f}, Stat::strength, 35, 0.75f},
    {"Prism",       {0.0f, 0.0f, 1.0f, 0.0f, 0.0f}, Stat::strength, 33, 0.7f},
    {"Marcella",    {0.0f, 0.6f, 0.0f, 0.0f, 0.4f}, Stat::strength, 50, 1.2f},
    {"Willow",      {0.0f, 0.0f, 0.0f, 1.0f, 0.0f}, Stat::strength, 50, 0.7f},
    {"Ren",         {0.0f, 0.3f, 0.0f, 0.0f, 0.7f}, Stat::strength, 45, 0.95f},
    {"Varg",        {0.5f, 0.5f, 0.0f, 0.0f, 0.0f}, Stat::strength, 56, 1.1f},
    {"Tavi",        {0.0f, 0.0f, 0.7f, 0.0f, 0.3f}, Stat::strength, 40, 0.75f},
}};

Tuning derive(const RoleMix& r) {
    Tuning t{};
    t.maxFightHpUse = 0.70f + 0.20f * r.tank;                 // tank 0.90, carry 0.70
    t.healThreshold = 0.22f - 0.07f * r.tank + 0.05f * r.farmer;  // critical health: ~15% tank, ~27% carry
    t.bankTarget = 60.0f - 20.0f * r.farmer + 20.0f * r.tank; // carry banks at ~40, tank at ~76
    t.fightAppetite = 0.6f + 0.6f * r.duelist + 0.4f * r.caster;
    t.partyAbsorb = std::clamp(0.70f + 0.25f * r.tank - 0.30f * (r.farmer + r.caster + r.support), 0.3f, 0.95f);
    t.leadsPull = r.tank >= 0.5f;
    t.followsParty = r.support >= 0.5f;
    return t;
}

const std::array<Tuning, 12>& tunings() {
    static const std::array<Tuning, 12> table = [] {
        std::array<Tuning, 12> out{};
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
const char* name(bc::champion_type type) { return profile(type).name; }

const char* name(bc::champion_draft pick) {
    static constexpr std::array<const char*, 11> names{"Ryth", "Vesper Vale", "Vendetta", "Cassi & Issac", "Stellar",
                                                       "Prism", "Marcella", "Willow", "Ren", "Varg", "Tavi"};
    const auto i = static_cast<std::size_t>(pick);
    return i < names.size() ? names[i] : "?";
}
