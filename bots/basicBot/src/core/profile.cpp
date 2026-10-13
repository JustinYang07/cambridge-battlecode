#include "profile.h"
#include <array>

namespace bc = cambc;

// Indexed by cambc::champion_type. farmPower ~ basic damage per turn on camps
// including resets/splash; survivability ~ base health scaled by kit.
static constexpr std::array<Profile, 12> kProfiles{{
    {"Ryth", Role::bruiser, Stat::strength, 56, 1.25f},
    {"Vesper Vale", Role::marksman, Stat::strength, 51, 0.9f},
    {"Vendetta", Role::bruiser, Stat::strength, 80, 0.9f},
    {"Cassi", Role::caster, Stat::strength, 30, 0.7f},
    {"Issac", Role::bruiser, Stat::strength, 45, 0.9f},
    {"Stellar", Role::caster, Stat::strength, 35, 0.75f},
    {"Prism", Role::caster, Stat::strength, 33, 0.7f},
    {"Marcella", Role::bruiser, Stat::strength, 50, 1.2f},
    {"Willow", Role::support, Stat::strength, 50, 0.7f},
    {"Ren", Role::bruiser, Stat::strength, 45, 0.95f},
    {"Varg", Role::bruiser, Stat::strength, 56, 1.1f},
    {"Tavi", Role::marksman, Stat::strength, 40, 0.75f},
}};

const Profile& profile(bc::champion_type type) {
    const auto i = static_cast<std::size_t>(type);
    return kProfiles[i < kProfiles.size() ? i : 0];
}

const char* name(bc::champion_type type) { return profile(type).name; }

const char* name(bc::champion_draft pick) {
    static constexpr std::array<const char*, 11> names{"Ryth", "Vesper Vale", "Vendetta", "Cassi & Issac", "Stellar",
                                                       "Prism", "Marcella", "Willow", "Ren", "Varg", "Tavi"};
    const auto i = static_cast<std::size_t>(pick);
    return i < names.size() ? names[i] : "?";
}
