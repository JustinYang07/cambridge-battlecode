#pragma once
// Static knowledge about each champion, shared by the draft (roster) and shop modules.
#include <cambc.h>

enum class Stat : std::uint8_t { strength, focus, health };
enum class Role : std::uint8_t { bruiser, marksman, caster, support };

struct Profile {
    const char* name;
    Role role;
    Stat primary;        // stat the shop invests in first
    float farmPower;     // rough camp-clearing strength, used to rank draft picks
    float survivability; // rough durability, used to rank draft picks
};

const Profile& profile(cambc::champion_type type);
const char* name(cambc::champion_type type);
const char* name(cambc::champion_draft pick);
