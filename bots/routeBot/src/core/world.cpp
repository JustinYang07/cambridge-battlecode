#include "world.h"
#include <algorithm>
#include <variant>
#include "comms.h"
#include "config.h"
#include "fuel.h"

namespace bc = cambc;

const char* to_string(GroupStatus s) {
    switch (s) {
    case GroupStatus::unknown: return "unknown";
    case GroupStatus::alive: return "alive";
    case GroupStatus::reward: return "reward";
    case GroupStatus::respawning: return "respawning";
    }
    return "?";
}

namespace {
bool bit(const std::vector<std::uint8_t>& bits, bc::vec2 t) {
    const auto& s = Statics::get();
    if (t.x < 0 || t.y < 0 || t.x >= s.width || t.y >= s.height || bits.empty()) return false;
    const auto i = static_cast<std::size_t>(t.y * s.width + t.x);
    return (bits[i >> 3] >> (i & 7)) & 1;
}
} // namespace

void World::refresh(bool firstTurn) {
    auto& me = bc::self();
    const auto& statics = Statics::get();
    if (firstTurn) {
        type = me.champion_name();
        ranged = me.attack_range() > 1;
        groups.assign(statics.groups.size(), GroupMemory{});
        const auto allyList = bc::champions(bc::team::ally);
        allyHandles_ = allyList;
        enemyHandles_ = bc::champions(bc::team::enemy);
        allyCount = static_cast<int>(std::min<std::size_t>(allyList.size(), allyTypes.size()));
        for (std::size_t i = 0; i < allyList.size(); ++i) {
            if (allyList[i] == me) slot = static_cast<int>(i);
            if (i < allyTypes.size()) {
                try {
                    allyTypes[i] = allyList[i].champion_name();
                } catch (const bc::error&) {
                }
            }
        }
        abilities_ = me.abilities();
        kit.splash = me.passive() == bc::passive_type::splash_attack;
        kit.goldBonus = me.passive() == bc::passive_type::bonus_gold;
        for (const auto& a : *abilities_) {
            if (a.is_attack_reset()) kit.hasReset = true;
            if (a.is_heal()) kit.hasHeal = true;
            if (a.is_dash() && a.info())
                if (const auto* d = std::get_if<bc::ability_dash>(&*a.info())) kit.dashRange = static_cast<float>(d->range);
            if (!(a.is_damage() || a.is_aoe_damage() || a.is_cc()) || !a.info()) continue;
            std::visit([&](const auto& x) {
                if constexpr (requires { x.modifier; }) kit.spellModifiers += x.modifier;
            }, *a.info());
        }
    }
    ready_ = {-1, -1};

    round = static_cast<int>(bc::round_number());
    alive = me.is_alive();
    gold = static_cast<int>(me.gold_count());
    teamGems = static_cast<int>(bc::global_submitted_gems(bc::team::ally));
    enemyGems = static_cast<int>(bc::global_submitted_gems(bc::team::enemy));
    enemies.clear();
    allies.clear();
    camps.clear();
    if (!alive) {
        hp = 0;
        return;
    }

    pos = me.position();
    prevHp = hp;
    hp = me.health();
    maxHp = me.max_health();
    strength = me.strength();
    focus = me.focus();
    kit.spellDps = kit.spellModifiers * focus / tune::spell_cooldown;
    gems = static_cast<int>(me.gem_count());
    range = me.attack_range();
    atSpawn = statics.allySpawn.is_set(pos);
    fuel::mark("refresh.self");

    visible = bc::vision::visible_tiles();
    visibleBits_ = visible.export_linear_buffer();
    const auto allyTiles = bc::vision::champion_tiles(bc::team::ally);
    const auto enemyTiles = bc::vision::champion_tiles(bc::team::enemy);
    const auto campTiles = bc::vision::camp_tiles();
    campBits_ = campTiles.export_linear_buffer();
    occupied = allyTiles | enemyTiles | campTiles | bc::vision::teleport_tiles();
    fuel::mark("refresh.tiles");

    // Champions: positions from the cheap occupancy board; details only when needed.
    for (auto p : allyTiles.positions())
        if (p != pos) allies.push_back(AllySeen{.pos = p});
    if (kit.hasHeal && !allies.empty()) {
        allies.clear();
        for (const auto& c : bc::vision::champions(bc::team::ally))
            if (!(c == me)) allies.push_back(AllySeen{.pos = c.position(), .handle = c, .hp = c.health(), .maxHp = c.max_health()});
    }
    // Who on their team is dead, and when the first of them respawns (raids need the window).
    // is_alive costs ~300 fuel per enemy, and these turns run close to the budget (an every-round
    // check cost Vesper 50+ lost turns on corners), so each champion checks every 4th round,
    // staggered by slot. A death is then seen up to 3 rounds late: respawns are assumed that early.
    enemyBodies = static_cast<int>(std::min<std::size_t>(enemyHandles_.size(), enemyDeadSince_.size()));
    if ((round + slot) % tune::enemy_alive_every == 0) {
        for (int k = 0; k < enemyBodies; ++k) {
            bool aliveNow = true;
            try {
                aliveNow = enemyHandles_[static_cast<std::size_t>(k)].is_alive();
            } catch (const bc::error&) {
            }
            auto& since = enemyDeadSince_[static_cast<std::size_t>(k)];
            if (aliveNow) since = -1;
            else if (since < 0) since = round - (tune::enemy_alive_every - 1);
        }
    }
    enemiesAlive = 0;
    enemyRespawnAt = -1;
    for (int k = 0; k < enemyBodies; ++k) {
        const int since = enemyDeadSince_[static_cast<std::size_t>(k)];
        if (since < 0) {
            ++enemiesAlive;
            continue;
        }
        const int back = since + bc::constants::respawn_time;
        // Past its respawn: it is back (we just have not checked yet).
        if (back <= round) {
            ++enemiesAlive;
            continue;
        }
        if (enemyRespawnAt < 0 || back < enemyRespawnAt) enemyRespawnAt = back;
    }
    fuel::mark("refresh.enemyAlive");
    if (!enemyTiles.is_empty()) {
        for (const auto& c : bc::vision::champions(bc::team::enemy)) {
            EnemySeen e{.handle = c, .pos = c.position()};
            for (std::size_t k = 0; k < enemyHandles_.size(); ++k)
                if (enemyHandles_[k] == c) e.index = static_cast<int>(k);
            e.hp = c.health();
            e.maxHp = c.max_health();
            e.strength = c.strength();
            e.focus = c.focus();
            e.gems = static_cast<int>(c.gem_count());
            e.range = c.attack_range();
            e.tributing = c.is_submitting_gems();
            e.channeling = c.is_recalling() || e.tributing;
            enemies.push_back(e);
            const float dps = e.strength + e.focus * 1.5f / tune::spell_cooldown;
            auto it = std::find_if(ghosts.begin(), ghosts.end(), [&](const EnemyGhost& g) { return g.handle == c; });
            if (it == ghosts.end()) ghosts.push_back({c, e.pos, round, dps, e.range});
            else *it = {c, e.pos, round, dps, e.range};
        }
    }
    std::erase_if(ghosts, [&](const EnemyGhost& g) { return round - g.round > tune::enemy_memory_rounds; });
    fuel::mark("refresh.champs");

    // Camps: presence from camp_tiles against the static camp table.
    for (const auto& g : statics.groups) {
        auto& mem = groups[static_cast<std::size_t>(g.id)];
        mem.visibleNow = false;
        for (auto t : g.tiles) mem.visibleNow = mem.visibleNow || bit(visibleBits_, t);
        if (!mem.visibleNow) continue;
        for (std::size_t i = 0; i < g.tiles.size(); ++i) {
            const auto t = g.tiles[i];
            if (!bit(campBits_, t)) continue;
            const auto* info = statics.campAt(t);
            CampSeen s{.pos = t, .groupId = g.id};
            s.maxHp = info ? info->health : 1;
            s.strength = info ? info->strength : 0;
            s.hp = s.maxHp * mem.healthLeft;  // estimate until read live
            camps.push_back(s);
        }
    }
    loadNearbyCamps();
    fuel::mark("refresh.camps");

    danger.clear();
    engagedGroup = -1;
    incoming = 0;
    int engagedDist = 1 << 20;
    int lastGroup = -1;
    beingHit = prevHp > 0 && hp < prevHp - 0.5f;
    for (const auto& c : camps) {
        if (c.strength <= 0 || c.hp <= 0) continue;
        const int d = dist(c.pos, pos);
        // Monsters sleep until damaged: only a group that is hurt (or hurting us) is a fight.
        const bool awake = true;  // variant: treat every monster in reach as a fight
        if (d <= 3 && awake) {
            incoming += c.strength;
            if (d < engagedDist) { engagedDist = d; engagedGroup = c.groupId; }
        }
        // One OR of the group's precomputed reach mask instead of a draw per camp.
        if (c.groupId != lastGroup && d <= 14) danger |= statics.groups[static_cast<std::size_t>(c.groupId)].reach;
        lastGroup = c.groupId;
    }
    fuel::mark("refresh.danger");
    updateGroups();
    fuel::mark("refresh.groups");
}

void World::loadNearbyCamps() {
    // Live health and handles only matter for camps we could hit or that could hit us soon.
    // Handles come from the static table (vision::camps() costs ~1k per visible camp).
    const auto& statics = Statics::get();
    for (auto& c : camps) {
        if (dist(c.pos, pos) > 5) continue;
        const auto* info = statics.campAt(c.pos);
        if (!info || !info->handle) continue;
        try {
            c.hp = info->handle->health();
            c.hpKnown = true;
            c.handle = info->handle;
        } catch (const bc::error&) {
        }
    }
}

void World::updateGroups() {
    const auto& statics = Statics::get();
    for (const auto& g : statics.groups) {
        auto& mem = groups[static_cast<std::size_t>(g.id)];
        if (!mem.visibleNow) continue;
        mem.seenRound = round;
        bool present = false, measured = false;
        float left = 0;
        for (const auto& c : camps)
            if (c.groupId == g.id) {
                present = true;
                if (c.hpKnown) { measured = true; left += c.hp; }
            }
        for (const auto& e : enemies)
            if (dist(e.pos, g.center) <= 5) mem.enemyNearRound = round;
        if (present) {
            if (mem.status != GroupStatus::alive) mem.healthLeft = 1.0f;  // freshly (re)spawned
            mem.status = GroupStatus::alive;
            if (measured && g.totalHealth > 0) mem.healthLeft = left / g.totalHealth;
            continue;
        }
        // Nothing standing: either waiting for a capture or respawning. Ask the engine only
        // when we do not already know the timer (state() costs ~0.3-3k fuel).
        if (mem.status == GroupStatus::respawning && round < mem.respawnAt) continue;
        try {
            const auto state = g.handle->state();
            if (std::holds_alternative<bc::map::camp_group_reward_available>(state)) {
                mem.status = GroupStatus::reward;
                mem.healthLeft = 0;
            } else if (const auto* r = std::get_if<bc::map::camp_group_respawning>(&state)) {
                mem.status = GroupStatus::respawning;
                mem.respawnAt = round + r->timer;
                mem.healthLeft = 0;
            } else {
                // Alive but its monsters are out of sight (bush / partial vision): assume healthy.
                mem.status = GroupStatus::alive;
                mem.healthLeft = 1.0f;
            }
        } catch (const bc::error&) {
            continue;
        }
    }
    // A respawn timer that has run out means the group is (probably) back.
    for (auto& mem : groups)
        if (mem.status == GroupStatus::respawning && round >= mem.respawnAt) {
            mem.status = GroupStatus::alive;
            mem.healthLeft = 1.0f;
        }
}

bool World::isVisible(bc::vec2 t) const { return bit(visibleBits_, t); }

bool World::tankAmong(unsigned mask) const {
    for (int i = 0; i < allyCount; ++i)
        if (i != slot && (mask >> i) & 1u && isTank(allyTypes[static_cast<std::size_t>(i)])) return true;
    return false;
}

bool World::allyTanking(int groupId) const {
    if (groupId < 0 || groups[static_cast<std::size_t>(groupId)].healthLeft >= 0.99f) return false;
    // Cheap test first: any ally (from the occupancy board) beside a live member at all.
    bool anyNear = false;
    for (const auto& c : camps)
        if (c.groupId == groupId && c.hp > 0 && c.strength > 0)
            for (const auto& a : allies) anyNear |= dist(a.pos, c.pos) <= tune::monster_range;
    if (!anyNear) return false;
    for (int i = 0; i < static_cast<int>(allyHandles_.size()); ++i) {
        if (i == slot) continue;
        bc::vec2 at;
        try {
            const auto& h = allyHandles_[static_cast<std::size_t>(i)];
            if (!h.is_visible() || !h.is_alive()) continue;
            at = h.position();
        } catch (const bc::error&) {
            continue;
        }
        for (const auto& c : camps) {
            if (c.groupId != groupId || c.hp <= 0 || c.strength <= 0) continue;
            const int d = dist(at, c.pos);
            if (d <= tune::monster_range && (d < range || (d == range && i < slot))) return true;
        }
    }
    return false;
}

World::Side World::ourSide(bc::vec2 at, int radius, int enemyIndex, float myDps) const {
    Side side{1, hp, myDps};
    const unsigned focusing = comms::focusMask(enemyIndex, slot);
    // Cheap pre-check on the occupancy board before asking the engine about each ally.
    bool anyNear = focusing != 0;
    for (const auto& a : allies) anyNear |= dist(a.pos, at) <= radius || dist(a.pos, pos) <= tune::fight_beside_radius;
    if (!anyNear) return side;
    for (int i = 0; i < static_cast<int>(allyHandles_.size()); ++i) {
        if (i == slot) continue;
        try {
            const auto& h = allyHandles_[static_cast<std::size_t>(i)];
            if (!h.is_visible() || !h.is_alive()) continue;
            // In the fight: near the target, attacking it, or right beside us (we fight together).
            const auto p = h.position();
            if (!((focusing >> i) & 1u) && dist(p, at) > radius && dist(p, pos) > tune::fight_beside_radius) continue;
            ++side.count;
            side.hp += h.health();
            // Allies' spells are unknown here: their basic attacks, plus the share spells add for us.
            side.dps += h.strength() * (myDps / std::max(strength, 1.0f));
        } catch (const bc::error&) {
        }
    }
    return side;
}

bool World::abilityReady(int slot) {
    auto& r = ready_[static_cast<std::size_t>(slot)];
    if (r < 0) r = (*abilities_)[static_cast<std::size_t>(slot)].is_ready() ? 1 : 0;
    return r == 1;
}

float World::enemyPressure(bc::vec2 at, int radius) const {
    float total = 0;
    for (const auto& g : ghosts) {
        const int age = round - g.round;
        // An unseen enemy may have walked toward us since: widen the radius with its age.
        if (dist(g.pos, at) > radius + age) continue;
        total += g.dps * (age == 0 ? 1.0f : 0.6f);
    }
    return total;
}

float World::alliedPower(bc::vec2 at, int radius) const {
    const float mine = strength * (1.0f + (kit.hasReset ? tune::reset_attack_bonus : 0.0f)) + kit.spellDps;
    return mine * static_cast<float>(1 + alliesWithin(at, radius));
}

int World::alliesWithin(bc::vec2 at, int radius) const {
    int n = 0;
    for (const auto& a : allies) n += dist(a.pos, at) <= radius;
    return n;
}

bc::bitboard World::enemyReach() const {
    bc::bitboard zone;
    zone.clear();
    for (const auto& g : ghosts)
        if (round - g.round <= 3) zone.draw_chebyshev(g.pos, static_cast<float>(g.range + 2 + (round - g.round)));
    return zone;
}

bool World::campsInRangeOf(bc::vec2 tile, int r) const {
    for (const auto& c : camps)
        if (c.strength > 0 && dist(c.pos, tile) <= r) return true;
    return false;
}

float World::incomingCampDamage(bc::vec2 tile) const {
    float total = 0;
    for (const auto& c : camps)
        if (c.hp > 0 && dist(c.pos, tile) <= 3) total += c.strength;
    return total;
}

bc::bitboard World::dangerExcept(int groupId) const {
    if (groupId < 0) return danger;
    bc::bitboard zone;
    zone.clear();
    int lastGroup = -1;
    for (const auto& c : camps) {
        if (c.strength <= 0 || c.hp <= 0 || c.groupId == groupId || c.groupId == lastGroup) continue;
        if (dist(c.pos, pos) <= 14) zone |= Statics::get().groups[static_cast<std::size_t>(c.groupId)].reach;
        lastGroup = c.groupId;
    }
    return zone;
}

const CampSeen* World::campAt(bc::vec2 tile) const {
    for (const auto& c : camps)
        if (c.pos == tile) return &c;
    return nullptr;
}
