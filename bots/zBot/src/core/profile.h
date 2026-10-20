#pragma once
// Static knowledge about each champion: stats for the draft and shop, and the
// role mix that turns into per-champion tuning for the brain.
//
// Roles are weights, not labels (Varg is half tank, half farmer), so behaviour
// blends: a 0.8-tank Ryth leads pulls and fights longer before retreating, a
// 1.0-farmer Vesper banks early because its bag is the team's income.
#include <cambc.h>

// Heart of Ruin (new patch) is not in the 2.2.4 SDK enums: assumed appended after Tavi in both.
inline constexpr auto kHeartOfRuinDraft = static_cast<cambc::champion_draft>(11);
inline constexpr auto kHeartOfRuinType = static_cast<cambc::champion_type>(12);

enum class Stat : std::uint8_t { strength, focus, health };

struct RoleMix {
    float farmer = 0;   // kills camps fastest, carries the economy, avoids risk
    float tank = 0;     // goes into camps first and soaks monster focus for the party
    float caster = 0;   // range-4 spells: safe chip damage, stuns on enemy channels
    float support = 0;  // keeps the party alive (heals) and travels with it
    float duelist = 0;  // contests enemy champions
};

struct Profile {
    const char* name;
    RoleMix roles;
    Stat primary;        // stat the shop invests in first
    float farmPower;     // rough camp-clearing strength, used to rank draft picks
    float survivability; // rough durability, used to rank draft picks
};

// Behaviour knobs derived from the role mix (see profile.cpp for the formulas).
struct Tuning {
    float maxFightHpUse;   // share of max health we will plan to spend on a camp fight
    float healThreshold;   // health fraction below which healing becomes critical
    float bankTarget;      // bag size at which banking is fully attractive
    float fightAppetite;   // multiplier on fight-an-enemy scores
    float partyAbsorb;     // share of monster hits we expect to take in a party
    bool leadsPull;        // a tank: starts party fights so others take fewer hits
    bool followsParty;     // a support: prefers groups allies are already farming
};

const Profile& profile(cambc::champion_type type);
const Tuning& tuning(cambc::champion_type type);
bool isTank(cambc::champion_type type);
const char* name(cambc::champion_type type);
const char* name(cambc::champion_draft pick);
