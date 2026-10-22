#include "world.h"
#include "fixed_str.h"
#include <algorithm>
#include <array>
#include <variant>
#include "comms.h"
#include "config.h"
#include "fuel.h"
#include "trace.h"

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
    {
        const int before = enemyGems;
        enemyGems = static_cast<int>(bc::global_submitted_gems(bc::team::enemy));
        if (enemyGems > before) enemyBankRound = round;
    }
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
        // Our own team too (same cadence): the invasion rule compares the two. Only when it is on:
        // is_alive costs ~300 fuel per champion.
        for (std::size_t k = 0; (tune::invade_needs_lead || tune::team_rush) && k < allyHandles_.size() && k < allyAlive_.size(); ++k) {
            if (static_cast<int>(k) == slot) continue;
            try {
                allyAlive_[k] = allyHandles_[k].is_alive() ? 1 : 0;
            } catch (const bc::error&) {
            }
        }
    }
    if (slot >= 0 && slot < static_cast<int>(allyAlive_.size())) allyAlive_[static_cast<std::size_t>(slot)] = alive ? 1 : 0;
    alliesAlive = 0;
    for (std::size_t k = 0; k < allyHandles_.size() && k < allyAlive_.size(); ++k) alliesAlive += allyAlive_[k];
    auto countEnemies = [&] {
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
    };
    countEnemies();
    if (enemyRespawnAt >= 0 || !alive) anyDeathSeen = true;  // an enemy is down, or we are
    fuel::mark("refresh.enemyAlive");
    if (!enemyTiles.is_empty()) {
        for (const auto& c : bc::vision::champions(bc::team::enemy)) {
            EnemySeen e{.handle = c, .pos = c.position()};
            // Enemies seen only far away (e.g. through a ward) do not drive our decisions.
            if (tune::enemy_react_radius > 0 && dist(e.pos, pos) > tune::enemy_react_radius) continue;
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
            if (e.index >= 0 && e.index < static_cast<int>(enemyStun_.size())) {
                auto& known = enemyStun_[static_cast<std::size_t>(e.index)];
                if (known < 0) {
                    known = 0;
                    try {
                        for (const auto& a : c.abilities()) known = known || a.is_cc();
                    } catch (const bc::error&) {
                    }
                }
                e.stun = known > 0;
            }
            if ((tune::stun_target != 0 || tune::stunned_team_target) && dist(e.pos, pos) <= tune::stun_check_radius &&
                bc::fuel_remaining() > tune::stun_check_fuel) {
                try {
                    e.stunned = c.buffs().has_stun();
                } catch (const bc::error&) {
                }
            }
            enemies.push_back(e);
            const float dps = e.strength + e.focus * 1.5f / tune::spell_cooldown;
            auto it = std::find_if(ghosts.begin(), ghosts.end(), [&](const EnemyGhost& g) { return g.handle == c; });
            if (it == ghosts.end()) ghosts.push_back({c, e.pos, round, dps, e.range, e.stun, e.index});
            else *it = {c, e.pos, round, dps, e.range, e.stun, e.index};
        }
    }
    // yBot: dead enemies leave no ghost. An enemy seen last round that is gone from a tile we still
    // see has died, recalled or teleported: one is_alive read tells which (eight vs Crimson: after a
    // full wipe the five ghosts kept "enemies about" for 10 rounds, and three healthy champions
    // recalled out of a 31-round free window).
    if (tune::purge_dead_ghosts) {
        bool died = false;
        for (const auto& g : ghosts) {
            if (g.round != round - 1 || g.index < 0 || g.index >= enemyBodies || !isVisible(g.pos)) continue;
            auto& since = enemyDeadSince_[static_cast<std::size_t>(g.index)];
            if (since >= 0) continue;
            try {
                if (!g.handle.is_alive()) {
                    since = round;
                    died = true;
                }
            } catch (const bc::error&) {
            }
        }
        if (died) countEnemies();
        std::erase_if(ghosts, [&](const EnemyGhost& g) {
            if (g.index < 0 || g.index >= enemyBodies) return false;
            const int since = enemyDeadSince_[static_cast<std::size_t>(g.index)];
            return since >= 0 && since + static_cast<int>(bc::constants::respawn_time) > round;
        });
    }
    std::erase_if(ghosts, [&](const EnemyGhost& g) { return round - g.round > tune::enemy_memory_rounds; });

    // Where each enemy stood last turn (for "moving away"): roll the current sightings in after use.
    for (const auto& e : enemies)
        if (e.index >= 0 && e.index < static_cast<int>(enemyPrevPos_.size())) {
            const auto k = static_cast<std::size_t>(e.index);
            enemyPrevPos_[k] = lastSeenPos_[k];
            enemyPrevRound_[k] = lastSeenRound_[k];
            lastSeenPos_[k] = e.pos;
            lastSeenRound_[k] = round;
        }
    for (const auto& e : enemies)
        if (e.hp > 0 && dist(e.pos, pos) <= tune::quiet_enemy_radius) lastEnemyNearRound = round;
    // Team target: keep it while it is alive and seen recently; otherwise the first enemy seen
    // losing health since the last round we saw it (the most hurt one if several at once).
    if (teamTarget >= 0 && round - teamTargetSeen > tune::team_target_memory) teamTarget = -1;
    int hurt = -1;
    float hurtFrac = 2.0f;
    for (const auto& e : enemies) {
        if (e.index < 0 || e.index >= static_cast<int>(enemyHpSeen_.size())) continue;
        const auto k = static_cast<std::size_t>(e.index);
        if (e.index == teamTarget) {
            if (e.hp <= 0) teamTarget = -1;
            else teamTargetSeen = round;
        }
        const bool justSeen = enemyHpRound_[k] >= round - 2;
        if (e.hp > 0 && justSeen && e.hp < enemyHpSeen_[k] - 1.0f && e.hp / std::max(e.maxHp, 1.0f) < hurtFrac) {
            hurt = e.index;
            hurtFrac = e.hp / std::max(e.maxHp, 1.0f);
        }
        enemyHpSeen_[k] = e.hp;
        enemyHpRound_[k] = round;
    }
    if (teamTarget < 0 && hurt >= 0) {
        teamTarget = hurt;
        teamTargetSeen = round;
    }
    // A stunned enemy cannot hit back or walk away: everyone switches to it (the weakest if several).
    if (tune::stunned_team_target) {
        const EnemySeen* st = nullptr;
        for (const auto& e : enemies)
            if (e.stunned && e.hp > 0 && e.index >= 0 && (!st || e.hp < st->hp)) st = &e;
        if (st) {
            teamTarget = st->index;
            teamTargetSeen = round;
        }
    }
    fuel::mark("refresh.champs");

    // Camps: presence from camp_tiles against the static camp table.
    // A group whose nearest tile is beyond our sight (vision 8; our ward is within 2 and sees 6)
    // cannot be visible: skip its per-tile tests (big maps scanned every tile of every group).
    static std::vector<int> extent;  // per group: farthest tile from its centre
    if (extent.size() != statics.groups.size()) {
        extent.assign(statics.groups.size(), 0);
        for (const auto& g : statics.groups)
            for (auto t : g.tiles) extent[static_cast<std::size_t>(g.id)] = std::max(extent[static_cast<std::size_t>(g.id)], dist(t, g.center));
    }
    // Each group's tiles as a sparse byte mask over the visibility / presence bit rows: a group is
    // visible if any of its bytes meets the vision bits, and its present members come out of a few
    // byte ANDs (yellow brick road: two per-tile tests over 28 groups with 30-tile cluster camps cost
    // 9-15k a turn, more than thinking; this is a few hundred).
    struct MaskByte { std::uint32_t index; std::uint8_t bits; };
    static std::vector<std::vector<MaskByte>> masks;
    if (masks.size() != statics.groups.size()) {
        masks.assign(statics.groups.size(), {});
        for (const auto& g : statics.groups) {
            auto& m = masks[static_cast<std::size_t>(g.id)];
            for (auto t : g.tiles) {
                const auto i = static_cast<std::uint32_t>(t.y * statics.width + t.x);
                const auto bi = i >> 3;
                const auto b = static_cast<std::uint8_t>(1u << (i & 7));
                bool found = false;
                for (auto& mb : m)
                    if (mb.index == bi) { mb.bits |= b; found = true; break; }
                if (!found) m.push_back({bi, b});
            }
        }
    }
    const bool haveBits = !visibleBits_.empty() && !campBits_.empty();
    for (const auto& g : statics.groups) {
        auto& mem = groups[static_cast<std::size_t>(g.id)];
        mem.visibleNow = false;
        if (tune::scan_skip_far && dist(g.center, pos) > tune::scan_sight + extent[static_cast<std::size_t>(g.id)]) continue;
        if (!haveBits) continue;
        const auto& m = masks[static_cast<std::size_t>(g.id)];
        for (const auto& mb : m)
            if (mb.index < visibleBits_.size() && (visibleBits_[mb.index] & mb.bits)) { mem.visibleNow = true; break; }
        if (!mem.visibleNow) continue;
        for (const auto& mb : m) {
            if (mb.index >= campBits_.size()) continue;
            std::uint8_t present = static_cast<std::uint8_t>(campBits_[mb.index] & mb.bits);
            while (present) {
                const int bitIdx = __builtin_ctz(present);
                present = static_cast<std::uint8_t>(present & (present - 1));
                const auto i = static_cast<int>(mb.index * 8u) + bitIdx;
                const bc::vec2 t{i % statics.width, i / statics.width};
                const auto* info = statics.campAt(t);
                CampSeen s{.pos = t, .groupId = g.id};
                s.maxHp = info ? info->health : 1;
                s.strength = info ? info->strength : 0;
                s.range = info ? info->range : tune::monster_range;
                s.hp = s.maxHp * mem.healthLeft;  // estimate until read live
                camps.push_back(s);
            }
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
    {
        const float loss = beingHit ? (prevHp - hp) / static_cast<float>(std::max(1, round - prevHpRound)) : 0.0f;
        hitRate = beingHit ? (hitRate > 0 ? 0.5f * hitRate + 0.5f * loss : loss) : hitRate * 0.5f;
        prevHpRound = round;
    }
    // loopBot: only awake camps act (rules: "Awake camps then perform their actions"; damage wakes
    // the group, two rounds without a champion in range put it back to sleep). A sleeping camp is
    // safe to walk past, so it is neither danger nor incoming damage: paths stop detouring around
    // every camp within 14 tiles. One state read per group per turn (the group wakes together).
    std::array<std::pair<int, bool>, 16> awakeCache{};
    int awakeCached = 0;
    auto groupAwake = [&](const CampSeen& c) {
        if (!tune::sleeping_camps_safe) return true;
        for (int i = 0; i < awakeCached; ++i)
            if (awakeCache[static_cast<std::size_t>(i)].first == c.groupId) return awakeCache[static_cast<std::size_t>(i)].second;
        bool awake = true;  // unknown: assume it can hit us
        const auto* info = statics.campAt(c.pos);
        if (info && info->handle) {
            try {
                awake = !info->handle->state().is_asleep();
            } catch (const bc::error&) {
            }
        }
        if (awakeCached < static_cast<int>(awakeCache.size())) awakeCache[static_cast<std::size_t>(awakeCached++)] = {c.groupId, awake};
        return awake;
    };
    for (const auto& c : camps) {
        if (c.strength <= 0 || c.hp <= 0) continue;
        const int d = dist(c.pos, pos);
        if (d > 14) continue;
        const bool awake = groupAwake(c);
        if (awake) groups[static_cast<std::size_t>(c.groupId)].awakeRound = round;
        if (!awake) continue;
        if (d <= c.range && awake) {  // patch: elk and golem reach 4, monoliths 5
            incoming += c.strength;
            if (d < engagedDist) { engagedDist = d; engagedGroup = c.groupId; }
        }
        // One OR of the group's precomputed reach mask instead of a draw per camp.
        if (c.groupId != lastGroup && d <= 14) danger |= statics.groups[static_cast<std::size_t>(c.groupId)].reach;
        lastGroup = c.groupId;
    }
    // Stun-aware positioning: alone (no ally within stun_danger_ally_radius) against two or more, or
    // outnumbered by two by the enemies within 6 (alone against one it cost tempo: collision and
    // pilgrimage -400/-540 a game vs the live bot, +455/+380 without it), the tiles a recently seen enemy reaches next round are danger too: its stun
    // range plus a step if it has one, else its attack range plus a step. Against Fish and Nyx 27%
    // of our deaths to champions came with no ally within 4, mostly from near-full health in ~3
    // rounds (stun chains). Every walk that avoids danger now walks round them.
    if (tune::stun_danger && alive) {
        const int allies = alliesWithin(pos, tune::stun_danger_ally_radius);
        int foes = 0;
        for (const auto& e : enemies) foes += e.hp > 0 && dist(e.pos, pos) <= 6;
        if ((allies == 0 && foes >= 2) || foes > allies + 1) {
            for (const auto& g : ghosts) {
                if (round - g.round > 1) continue;
                const int reach = (g.stun ? tune::stun_range : g.range) + 1;
                if (dist(g.pos, pos) > reach + 6) continue;
                danger.draw_chebyshev(g.pos, static_cast<float>(reach));
            }
        }
    }
    fuel::mark("refresh.danger");
    updateGroups();
    fuel::mark("refresh.groups");
}

void World::loadNearbyCamps() {
    // Live health and handles only matter for camps we could hit or that could hit us soon.
    // Handles come from the static table (vision::camps() costs ~1k per visible camp).
    const auto& statics = Statics::get();
    // Regeneration guard: a monster's health going up between two of our readings, too soon for a
    // respawn (groups respawn 40+ rounds after capture), is logged. None seen so far: 342 re-reads
    // of damaged monsters after 3-39 rounds were all the same or lower.
    // Trace-only: the two per-tile tables cost ~20k (lanes) to ~40k (expanse) on the first turn.
    static std::vector<float> lastHp;
    static std::vector<int> lastRound;
    if (kTracing && lastHp.empty()) {
        lastHp.assign(static_cast<std::size_t>(statics.width * statics.height), -1.0f);
        lastRound.assign(lastHp.size(), -1000);
    }
    // Only members we can hit this turn or that can hit us, nearest first, at most camp_reads_per_turn
    // (yellow brick road: a 30-cluster camp beside spawn cost ~17k a turn in health reads, 777 lost
    // turns a game; a cluster out of our reach keeps its estimate until we get there).
    std::array<int, 64> order{};
    int n = 0;
    for (int i = 0; i < static_cast<int>(camps.size()) && n < 64; ++i) {
        const auto& c = camps[static_cast<std::size_t>(i)];
        const int d = dist(c.pos, pos);
        if (d > 5) continue;
        if (tune::camp_reads_per_turn > 0 && d > range + 1 && !(c.strength > 0 && d <= c.range + 1)) continue;
        order[static_cast<std::size_t>(n++)] = i;
    }
    std::sort(order.begin(), order.begin() + n, [&](int a, int b) { return dist(camps[static_cast<std::size_t>(a)].pos, pos) < dist(camps[static_cast<std::size_t>(b)].pos, pos); });
    if (tune::camp_reads_per_turn > 0) n = std::min(n, tune::camp_reads_per_turn);
    for (int k = 0; k < n; ++k) {
        auto& c = camps[static_cast<std::size_t>(order[static_cast<std::size_t>(k)])];
        const auto* info = statics.campAt(c.pos);
        if (!info || !info->handle) continue;
        try {
            c.hp = info->handle->health();
            c.hpKnown = true;
            c.handle = info->handle;
            if constexpr (!kTracing) continue;
            const auto k = static_cast<std::size_t>(c.pos.y * statics.width + c.pos.x);
            if (lastHp[k] > 0 && c.hp > lastHp[k] + 0.5f && round - lastRound[k] < 30) {
                FixedStr<64> msg;
                msg << "camp regen (" << c.pos.x << ',' << c.pos.y << ") " << static_cast<int>(lastHp[k]) << "->"
                    << static_cast<int>(c.hp) << " in " << (round - lastRound[k]);
                pendingNotes.push_back(std::string(msg.view()));
            }
            lastHp[k] = c.hp;
            lastRound[k] = round;
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
        // Waiting for capture: it stays so until someone captures it. Ask every few rounds unless we
        // are near it, where the capture matters now (lanes: ~2.6 state reads a turn, ~11k fuel).
        if (tune::reward_recheck_every > 1 && mem.status == GroupStatus::reward && dist(g.center, pos) > 4 &&
            (round + g.id) % tune::reward_recheck_every != 0)
            continue;
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

bool World::fightNearCamp(int groupId) const {
    if (groupId < 0 || groupId >= static_cast<int>(groups.size())) return false;
    const auto c = Statics::get().groups[static_cast<std::size_t>(groupId)].center;
    const int r = tune::fight_near_camp_radius;
    for (const auto& e : enemies)
        if (e.hp > 0 && (dist(e.pos, c) <= r || dist(e.pos, pos) <= r)) return true;
    for (int s = 0; s < comms::alert_byte && s < allyCount; ++s) {
        if (s == slot || !(comms::fighting(s) || comms::retreating(s))) continue;
        const auto p = allyPosCached(s);
        if (p.x >= 0 && dist(p, c) <= r) return true;
    }
    return false;
}

bool World::campAsleep(int groupId) const {
    if (groupId < 0 || groupId >= static_cast<int>(groups.size())) return false;
    const auto& g = Statics::get().groups[static_cast<std::size_t>(groupId)];
    return !g.harmless && groups[static_cast<std::size_t>(groupId)].awakeRound != round;
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
            for (const auto& a : allies) anyNear |= dist(a.pos, c.pos) <= c.range;
    if (!anyNear) return false;
    for (int i = 0; i < static_cast<int>(allyHandles_.size()); ++i) {
        if (i == slot) continue;
        const bc::vec2 at = allyPosCached(i);  // read once per turn
        if (at.x < 0) continue;
        for (const auto& c : camps) {
            if (c.groupId != groupId || c.hp <= 0 || c.strength <= 0) continue;
            const int d = dist(at, c.pos);
            if (d <= c.range && (d < range || (d == range && i < slot))) return true;
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
    for (int i = 0; i < static_cast<int>(allyHandles_.size()) && i < 8; ++i) {
        if (i == slot) continue;
        // Position, health and strength read once per turn (fight scoring asks per enemy).
        const auto p = allyPosCached(i);
        if (p.x < 0) continue;
        // In the fight: near the target, attacking it, or right beside us (we fight together).
        if (!((focusing >> i) & 1u) && dist(p, at) > radius && dist(p, pos) > tune::fight_beside_radius) continue;
        // combatBotv3: an ally announcing a retreat is leaving, not fighting with us (eight: Stellar
        // counted Ren as 5v4 while he walked away, and stayed).
        if (tune::ranged_follow_front && comms::retreating(i)) continue;
        readAllyHp(i);
        const auto& c = allyHpCache_[static_cast<std::size_t>(i)];
        if (!c.alive) continue;
        ++side.count;
        side.hp += c.hp;
        // Allies' spells are unknown here: their basic attacks, plus the share spells add for us.
        side.dps += c.strength * (myDps / std::max(strength, 1.0f));
    }
    return side;
}

bool World::allyVisible(int s) const {
    if (!tune::ally_vision_gate) return true;
    if (s < 0 || s >= static_cast<int>(allyHandles_.size()) || s >= 8) return false;
    if (allyVisRound_ != round) {
        allyVisRound_ = round;
        allyVisMask_ = 0;
        // The occupancy board already says whether any ally is in sight: none, no host call.
        if (!allies.empty()) {
            try {
                for (const auto& c : bc::vision::champions(bc::team::ally))
                    for (int k = 0; k < static_cast<int>(allyHandles_.size()) && k < 8; ++k)
                        if (k != slot && c == allyHandles_[static_cast<std::size_t>(k)]) allyVisMask_ |= 1u << k;
            } catch (const bc::error&) {
                allyVisMask_ = ~0u;  // fall back to reading (and catching) as before
            }
        }
    }
    return (allyVisMask_ >> s) & 1u;
}

int World::alliesStunReadyNear(bc::vec2 at, int radius) const {
    int n = 0;
    for (int i = 0; i < static_cast<int>(allyHandles_.size()) && i < 8; ++i) {
        if (i == slot) continue;
        const auto p = allyPosCached(i);
        if (p.x < 0 || dist(p, at) > radius) continue;
        try {
            const auto abilities = allyHandles_[static_cast<std::size_t>(i)].abilities();
            for (const auto& a : abilities) {
                const auto& info = a.info();
                const auto* aoe = info ? std::get_if<bc::ability_aoe_damage>(&*info) : nullptr;
                if ((a.is_cc() || (aoe && aoe->duration > 0)) && a.cooldown() == 0) {
                    ++n;
                    break;
                }
            }
        } catch (const bc::error&) {
        }
    }
    return n;
}

int World::allyGems(int s) const {
    if (s < 0 || s >= static_cast<int>(allyHandles_.size())) return -1;
    if (!allyVisible(s)) return -1;
    try {
        const auto& a = allyHandles_[static_cast<std::size_t>(s)];
        if (!a.is_alive()) return -1;
        return static_cast<int>(a.gem_count());
    } catch (const bc::error&) {
        return -1;
    }
}

cambc::vec2 World::allyPos(int s) const {
    if (s < 0 || s >= static_cast<int>(allyHandles_.size())) return {-1, -1};
    if (!allyVisible(s)) return {-1, -1};
    try {
        const auto& a = allyHandles_[static_cast<std::size_t>(s)];
        if (!a.is_alive()) return {-1, -1};
        return a.position();
    } catch (const bc::error&) {
        return {-1, -1};
    }
}

cambc::vec2 World::allyPosCached(int s) const {
    if (s < 0 || s >= static_cast<int>(allyPosCache_.size())) return {-1, -1};
    if (allyPosRound_ != round) {
        allyPosRound_ = round;
        for (int k = 0; k < static_cast<int>(allyPosCache_.size()); ++k)
            allyPosCache_[static_cast<std::size_t>(k)] = k < allyCount && k != slot ? allyPos(k) : bc::vec2{-1, -1};
    }
    return allyPosCache_[static_cast<std::size_t>(s)];
}

// Ally health, read once per turn per ally (two or three host calls each; farm scoring asks per camp).
void World::readAllyHp(int s) const {
    auto& c = allyHpCache_[static_cast<std::size_t>(s)];
    if (c.round == round) return;
    c = {round, 0.0f, 0.0f, 0.0f, false};
    if (!allyVisible(s)) return;
    try {
        const auto& a = allyHandles_[static_cast<std::size_t>(s)];
        if (a.is_alive()) c = {round, a.health(), a.max_health(), a.strength(), true};
    } catch (const bc::error&) {
    }
}

float World::allyHpFrac(int s) const {
    if (s < 0 || s >= static_cast<int>(allyHandles_.size()) || s >= 8) return -1.0f;
    readAllyHp(s);
    const auto& c = allyHpCache_[static_cast<std::size_t>(s)];
    return c.alive ? c.hp / std::max(c.maxHp, 1.0f) : -1.0f;
}

float World::allyHp(int s) const {
    if (s < 0 || s >= static_cast<int>(allyHandles_.size()) || s >= 8) return 0.0f;
    readAllyHp(s);
    const auto& c = allyHpCache_[static_cast<std::size_t>(s)];
    return c.alive ? c.hp : 0.0f;
}

float World::allyMissingHp(int s) const {
    if (s < 0 || s >= static_cast<int>(allyHandles_.size()) || s >= 8) return 0.0f;
    readAllyHp(s);
    const auto& c = allyHpCache_[static_cast<std::size_t>(s)];
    return c.alive ? std::max(0.0f, c.maxHp - c.hp) : 0.0f;
}

float World::allyCampDps(int s, const GroupInfo& g) const {
    struct Read { int round = -1; float strength = 0, focus = 0, spellMod = -1; bool splash = false, reset = false; };
    // combatBotv3: one cache for the whole team. The champions share this process and an ally's
    // stats read the same from any of us; wiping it whenever another champion's turn ran made every
    // call re-read the engine (~3k per party mate per fight estimate, 13k for stampede's elk party).
    static std::array<Read, 8> cache;
    static int forSlot = -1;
    if (s < 0 || s >= static_cast<int>(allyHandles_.size()) || s >= static_cast<int>(cache.size())) return 0.0f;
    if (!tune::ally_cache_shared && forSlot != slot) {
        cache = {};
        forSlot = slot;
    }
    auto& r = cache[static_cast<std::size_t>(s)];
    // combatBotv3: strength and focus change only with items, and passive and spells never: re-read
    // them every ally_read_period rounds, not each round (three reads per ally per round tripled the
    // fight estimate's fuel and doubled lost turns). A dead ally reads 0 until it is seen alive again.
    const bool stale = r.round < 0 || round - r.round >= tune::ally_read_period || r.strength <= 0;
    if (r.round != round && (tune::ally_read_period <= 1 || stale) && !allyVisible(s)) {
        // Out of sight: the last values we read stand. Never seen: 0, and campFight counts a copy of
        // us in its place (the design's base stats ignore items: stampede's elk party read far too weak).
    } else if (r.round != round && (tune::ally_read_period <= 1 || stale)) {
        const float keepMod = r.spellMod;
        const bool keepReset = r.reset, keepSplash = r.splash;
        r = {round, 0, 0, keepMod, keepSplash, keepReset};
        try {
            const auto& a = allyHandles_[static_cast<std::size_t>(s)];
            if (a.is_alive()) {
                r.strength = a.strength();
                r.focus = a.focus();
                if (r.spellMod < 0 || tune::ally_read_period <= 1) r.splash = a.passive() == bc::passive_type::splash_attack;
                // combatBotv3: the ally's spells from its design, as we read our own kit (damage
                // modifiers summed, times focus, per cooldown), once: a flat focus x 1.5 made casters
                // like Stellar look weak and parties "too strong".
                if (r.spellMod < 0) {
                    r.spellMod = 0;
                    for (const auto& ab : a.abilities()) {
                        if (ab.is_attack_reset()) r.reset = true;
                        if (!(ab.is_damage() || ab.is_aoe_damage() || ab.is_cc()) || !ab.info()) continue;
                        std::visit([&](const auto& x) {
                            if constexpr (requires { x.modifier; }) r.spellMod += x.modifier;
                        }, *ab.info());
                    }
                }
            }
        } catch (const bc::error&) {
        }
    }
    // Between re-reads, a death shows through the every-4th-round alive check.
    if (tune::ally_read_period > 1 && s < static_cast<int>(allyAlive_.size()) && allyAlive_[static_cast<std::size_t>(s)] == 0) return 0.0f;
    float basic = r.strength;
    if (r.splash && g.campCount > 1)
        basic *= std::min<float>(static_cast<float>(g.campCount), 1.0f + 2.0f * g.adjacentPairs / g.campCount);
    if (r.reset) basic *= 1.0f + tune::reset_attack_bonus;
    const float spells = tune::ally_design_spells && r.spellMod >= 0 ? r.spellMod * r.focus / tune::spell_cooldown : r.focus * 1.5f / tune::spell_cooldown;
    return basic + spells;
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
    if (!tune::power_fighting_only) return mine * static_cast<float>(1 + alliesWithin(at, radius));
    // combatBotv2: only allies actually fighting an enemy champion count (71% of our deaths in the
    // Fish games had no ally hit its attackers, while "allies near" said we were ahead).
    int fighters = 0;
    for (int s = 0; s < comms::alert_byte && s < allyCount; ++s) {
        if (s == slot || !comms::fighting(s)) continue;
        const auto p = allyPosCached(s);
        if (p.x >= 0 && dist(p, at) <= radius) ++fighters;
    }
    return mine * static_cast<float>(1 + fighters);
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
        if (c.hp > 0 && dist(c.pos, tile) <= c.range) total += c.strength;
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

bool World::tributeAtRisk(bc::vec2 altar, int bag) const {
    // ceil(bag / 10) ticks; the turn we start is the first, so enemies have that many turns to act.
    const int ticks = std::max(1, (bag + 9) / 10);
    float burst = 0;
    for (const auto& g : ghosts) {
        if (round - g.round > 1) continue;  // only enemies seen this round or last
        const int walk = std::max(0, dist(g.pos, altar) - 1);  // we stand beside the altar
        if (g.stun && walk <= tune::stun_range + ticks) return true;
        if (walk <= g.range + ticks) burst += g.dps * static_cast<float>(ticks);
    }
    return burst >= hp;
}

bool World::theirHalf(cambc::vec2 p) const {
    const auto& s = Statics::get();
    return dist(p, s.enemySpawnCenter) < dist(p, s.spawnCenter);
}

bool World::invadeWithoutMelee(bc::vec2 at) const {
    if (!tune::invade_needs_melee || !ranged || !theirHalf(at)) return false;
    for (int s = 0; s < allyCount && s < 8; ++s) {
        if (s == slot) continue;
        const auto t = static_cast<std::size_t>(allyTypes[static_cast<std::size_t>(s)]);
        if (t >= bc::champion_designs.size() || bc::champion_designs[t].base_stats().base_attack_range > 1) continue;
        const auto p = allyPosCached(s);
        if (p.x < 0 || dist(p, at) > tune::invade_melee_radius) continue;
        // invade_melee_ahead: it must also be in front of us (no farther from their spawn than we are).
        if (tune::invade_melee_ahead && dist(p, Statics::get().enemySpawnCenter) > dist(pos, Statics::get().enemySpawnCenter)) continue;
        return false;
    }
    return true;
}

bool World::enemyClosingOn(int index, cambc::vec2 now, cambc::vec2 target) const {
    if (index < 0 || index >= static_cast<int>(enemyPrevPos_.size())) return false;
    const auto k = static_cast<std::size_t>(index);
    if (enemyPrevRound_[k] < round - 2 || enemyPrevRound_[k] >= round) return false;
    return dist(now, target) < dist(enemyPrevPos_[k], target);
}

bool World::enemyApproaching(int index, cambc::vec2 now) const {
    if (index < 0 || index >= static_cast<int>(enemyPrevPos_.size())) return false;
    const auto k = static_cast<std::size_t>(index);
    if (enemyPrevRound_[k] < round - 2 || enemyPrevRound_[k] >= round) return false;
    return dist(now, pos) < dist(enemyPrevPos_[k], pos);
}

bool World::enemyMovingAway(int index, cambc::vec2 now) const {
    if (index < 0 || index >= static_cast<int>(enemyPrevPos_.size())) return false;
    const auto k = static_cast<std::size_t>(index);
    if (enemyPrevRound_[k] < round - 2 || enemyPrevRound_[k] >= round) return false;
    return dist(now, pos) > dist(enemyPrevPos_[k], pos);
}
