#include "combat.h"
#include <algorithm>
#include <array>
#include <variant>
#include "../core/comms.h"
#include "../core/config.h"
#include "../core/fixed_str.h"
#include "../core/trace.h"
#include "../shop/shop.h"

namespace bc = cambc;

namespace combat {

namespace {

struct SpellShape {
    float range = 0, modifier = 0, radius = 0;
    bool exclusive = false;
};

SpellShape shape(const bc::ability& a) {
    SpellShape s;
    const auto& info = a.info();
    if (!info) return s;
    std::visit([&](const auto& x) {
        s.exclusive = x.exclusive_with_movement;
        if constexpr (requires { x.range; }) s.range = x.range;
        if constexpr (requires { x.modifier; }) s.modifier = x.modifier;
        if constexpr (requires { x.radius; }) s.radius = x.radius;
    }, *info);
    return s;
}

CampSeen* campInRange(World& w, int groupId, int range) {
    // Keep hitting last turn's target until it dies; otherwise dangerous monsters first
    // (strength per remaining health), then the weakest.
    for (auto& c : w.camps)
        if (c.pos == w.lastAttacked && c.handle && c.hp > 0 && dist(c.pos, w.pos) <= range &&
            (groupId < 0 || c.groupId == groupId))
            return &c;
    CampSeen* best = nullptr;
    float bestKey = -1;
    for (auto& c : w.camps) {
        if (!c.handle || c.hp <= 0 || dist(c.pos, w.pos) > range) continue;
        if (groupId >= 0 && c.groupId != groupId) continue;
        const float key = (c.strength + 1.0f) / std::max(c.hp, 1.0f);
        if (key > bestKey) { bestKey = key; best = &c; }
    }
    return best;
}

EnemySeen* enemyInRange(World& w, int range, const Focus& focus) {
    // Our chosen target first, then the one allies are attacking (focus fire finishes kills),
    // then the weakest enemy that can hit us or is channelling.
    EnemySeen* best = nullptr;
    int bestRank = -1;
    for (auto& e : w.enemies) {
        if (e.hp <= 0 || dist(e.pos, w.pos) > range) continue;
        const bool chosen = focus.enemy && e.handle == *focus.enemy;
        const bool allyFocus = comms::focusMask(e.index, w.slot) != 0;
        const bool wanted = chosen || allyFocus || (focus.defend && dist(e.pos, w.pos) <= e.range) || e.channeling;
        if (!wanted) continue;
        const int rank = chosen ? 2 : allyFocus ? 1 : 0;
        if (rank > bestRank || (rank == bestRank && e.hp < best->hp)) {
            best = &e;
            bestRank = rank;
        }
    }
    return best;
}

// Re-read just the unit we hit (live queries are expensive).
void refresh(CampSeen& c) { c.hp = c.handle && c.handle->is_visible() ? c.handle->health() : 0; }
void refresh(EnemySeen& e) { e.hp = e.handle.is_visible() && e.handle.is_alive() ? e.handle.health() : 0; }

bool basicAttack(World& w, const Focus& focus) {
    auto& me = bc::self();
    if (!me.can_attack()) return false;
    FixedStr<40> msg;
    try {
        if (auto* e = enemyInRange(w, w.range, focus)) {
            me.attack(e->handle);
            refresh(*e);
            trace().action((msg << "ae" << e->pos.x << ',' << e->pos.y).view());
            return true;
        }
        if (auto* c = campInRange(w, focus.groupId, w.range)) {
            me.attack(*c->handle);
            w.lastAttacked = c->pos;
            refresh(*c);
            trace().action((msg << "ac" << c->pos.x << ',' << c->pos.y).view());
            return true;
        }
    } catch (const bc::error& e) {
        trace().note((msg << "attack failed: " << e.message()).view());
    }
    return false;
}

bool castOnTargets(World& w, const Focus& focus, bool beforeMove) {
    auto& me = bc::self();
    if (!me.can_cast()) return false;
    for (int slot = 0; slot < 2; ++slot) {
        const auto& a = w.abilities()[static_cast<std::size_t>(slot)];
        if (!(a.is_damage() || a.is_cc() || a.is_aoe_damage())) continue;
        const auto s = shape(a);
        if (s.exclusive && !beforeMove) continue;
        if (!w.abilityReady(slot)) continue;
        const int range = static_cast<int>(s.range);
        FixedStr<40> msg;
        try {
            if (a.is_aoe_damage()) {
                // Ground spell: centre it where it hits the most monsters/enemies.
                bc::vec2 best{};
                int bestHits = 0;
                auto consider = [&](bc::vec2 centre) {
                    if (dist(centre, w.pos) > range) return;
                    int hits = 0;
                    for (const auto& c : w.camps) hits += c.hp > 0 && dist(c.pos, centre) <= static_cast<int>(s.radius);
                    for (const auto& e : w.enemies) hits += 2 * (dist(e.pos, centre) <= static_cast<int>(s.radius));
                    if (hits > bestHits) { bestHits = hits; best = centre; }
                };
                for (const auto& c : w.camps) if (c.handle && (focus.groupId < 0 || c.groupId == focus.groupId)) consider(c.pos);
                for (const auto& e : w.enemies) consider(e.pos);
                if (bestHits > 0 && a.use(best)) {
                    w.spentAbility(slot);
                    trace().action((msg << "aoe" << best.x << ',' << best.y).view());
                    return true;
                }
                continue;
            }
            if (auto* e = enemyInRange(w, range, focus)) {
                if (a.use(e->handle)) {
                    w.spentAbility(slot);
                    refresh(*e);
                    trace().action((msg << (a.is_cc() ? "st" : "se") << e->pos.x << ',' << e->pos.y).view());
                    return true;
                }
            }
            // Pure stuns (no damage) are wasted on camps.
            if (s.modifier <= 0) continue;
            if (auto* c = campInRange(w, focus.groupId, range)) {
                if (a.use(*c->handle)) {
                    w.spentAbility(slot);
                    refresh(*c);
                    trace().action((msg << "sc" << c->pos.x << ',' << c->pos.y).view());
                    return true;
                }
            }
        } catch (const bc::error& e) {
            trace().note((msg << "cast failed: " << e.message()).view());
        }
    }
    return false;
}

bool resetAndAttack(World& w, const Focus& focus) {
    auto& me = bc::self();
    if (!w.kit.hasReset || me.can_attack() || !me.can_cast()) return false;
    const bool targetLeft = enemyInRange(w, w.range, focus) || campInRange(w, focus.groupId, w.range);
    if (!targetLeft) return false;
    for (int slot = 0; slot < 2; ++slot) {
        const auto& a = w.abilities()[static_cast<std::size_t>(slot)];
        if (!a.is_attack_reset() || !w.abilityReady(slot)) continue;
        try {
            if (a.use()) {
                w.spentAbility(slot);
                trace().action("r");
                return basicAttack(w, focus);
            }
        } catch (const bc::error& e) {
            FixedStr<40> msg;
            trace().note((msg << "reset failed: " << e.message()).view());
        }
    }
    return false;
}

} // namespace

float campDamagePerTurn(const World& w, const GroupInfo& g) {
    float basic = w.strength * (1.0f + (w.kit.hasReset ? tune::reset_attack_bonus : 0.0f));
    if (w.kit.splash && g.campCount > 1)
        basic *= std::min<float>(static_cast<float>(g.campCount), 1.0f + 2.0f * g.adjacentPairs / g.campCount);
    const float total = basic + w.kit.spellDps;
    return total * (shop::hasPassive(bc::item_passive::monster) ? 1.10f : 1.0f);
}

FightEstimate campFight(const World& w, const GroupInfo& g, float healthLeft, int party, float share) {
    // Remaining members in kill order, from live vision when we can see the group.
    // Fixed buffer (groups have a handful of members): heap allocation is expensive under fuel metering.
    std::array<CampMember, 8> left{};
    int n = 0;
    const bool visibleNow = w.groups[static_cast<std::size_t>(g.id)].visibleNow;
    if (visibleNow) {
        for (const auto& c : w.camps)
            if (c.groupId == g.id && c.hp > 0 && n < 8) left[static_cast<std::size_t>(n++)] = {c.hp, c.strength};
        // Insertion sort into kill order: hardest hitters first, then weakest.
        for (int i = 1; i < n; ++i)
            for (int j = i; j > 0; --j) {
                auto& a = left[static_cast<std::size_t>(j - 1)];
                auto& b = left[static_cast<std::size_t>(j)];
                if (b.strength > a.strength || (b.strength == a.strength && b.health < a.health)) std::swap(a, b);
                else break;
            }
    } else {
        float consumed = (1.0f - std::clamp(healthLeft, 0.0f, 1.0f)) * g.totalHealth;
        for (const auto& m : g.killOrder) {
            const float eaten = std::min(consumed, m.health);
            consumed -= eaten;
            if (m.health - eaten > 0 && n < 8) left[static_cast<std::size_t>(n++)] = {m.health - eaten, m.strength};
        }
    }

    FightEstimate e;
    const float dps = std::max(campDamagePerTurn(w, g), 1.0f) * static_cast<float>(std::max(party, 1));
    float strength = 0;
    for (int i = 0; i < n; ++i) strength += left[static_cast<std::size_t>(i)].strength;
    float mitigation = party > 1 ? (share >= 0 ? share : w.role().partyAbsorb) : 1.0f;
    if (shop::hasPassive(bc::item_passive::jungle)) mitigation *= 0.7f;
    if (shop::hasPassive(bc::item_passive::durability)) mitigation *= 0.9f;
    e.hitting = strength;
    for (int i = 0; i < n; ++i) {
        const auto& m = left[static_cast<std::size_t>(i)];
        const float t = m.health / dps;
        e.turns += t;
        e.damage += t * strength * mitigation;
        strength -= m.strength;
    }
    return e;
}

float championDamagePerTurn(const World& w) {
    return w.strength * (1.0f + (w.kit.hasReset ? tune::reset_attack_bonus : 0.0f)) + w.kit.spellDps;
}

float threatPerTurn(const World& w, bc::vec2 at) {
    float total = 0;
    for (const auto& e : w.enemies)
        if (dist(e.pos, at) <= e.range + 2) total += e.strength + e.focus * 1.5f / tune::spell_cooldown;
    return total;
}

void act(World& w, const Focus& focus, bool beforeMove) {
    bool attacked = false;
    if (beforeMove) {
        castOnTargets(w, focus, true);
        attacked = basicAttack(w, focus);
    } else {
        attacked = basicAttack(w, focus);
        castOnTargets(w, focus, false);
    }
    if (attacked) resetAndAttack(w, focus);
}

bool castFromRange(World& w, const Focus& focus) {
    if (w.kit.spellModifiers <= 0) return false;
    return castOnTargets(w, focus, true);
}

bool interruptChannels(World& w) {
    bool anyChanneling = false;
    for (const auto& e : w.enemies) anyChanneling = anyChanneling || e.channeling;
    if (!anyChanneling) return false;
    auto& me = bc::self();
    if (!me.can_cast()) return false;
    for (int slot = 0; slot < 2; ++slot) {
        const auto& a = w.abilities()[static_cast<std::size_t>(slot)];
        const bool stuns = a.is_cc() || (a.is_aoe_damage() && shape(a).radius > 0 && a.info() &&
                                         std::holds_alternative<bc::ability_aoe_damage>(*a.info()) &&
                                         std::get<bc::ability_aoe_damage>(*a.info()).duration > 0);
        if (!stuns || !w.abilityReady(slot)) continue;
        const int range = static_cast<int>(shape(a).range);
        for (auto& e : w.enemies) {
            if (!e.channeling || e.hp <= 0 || dist(e.pos, w.pos) > range) continue;
            try {
                const bool ok = a.is_aoe_damage() ? a.use(e.pos) : a.use(e.handle);
                if (ok) {
                    w.spentAbility(slot);
                    FixedStr<24> msg;
                    trace().action((msg << "st" << e.pos.x << ',' << e.pos.y).view());
                    trace().note("interrupted enemy channel");
                    return true;
                }
            } catch (const bc::error&) {
            }
        }
    }
    return false;
}

bool useAltarLock(World& w) {
    if (!tune::buy_altar_locks) return false;
    const auto& s = Statics::get();
    // Cheap pre-check before asking the engine about our inventory.
    int altar = -1;
    for (std::size_t i = 0; i < s.altars.size(); ++i)
        if (dist(s.altars[i], w.pos) <= 2) altar = static_cast<int>(i);
    if (altar < 0) return false;
    const auto at = s.altars[static_cast<std::size_t>(altar)];
    bool enemyTribute = false;
    for (const auto& e : w.enemies) enemyTribute = enemyTribute || (e.tributing && dist(e.pos, at) <= 1);
    bc::vec2 enemySpawn{};
    {
        long x = 0, y = 0, n = 0;
        for (auto p : s.enemySpawn.positions()) { x += p.x; y += p.y; ++n; }
        if (n) enemySpawn = {static_cast<int>(x / n), static_cast<int>(y / n)};
    }
    const bool enemySide = dist(at, enemySpawn) < dist(at, s.spawnCenter);
    if (!enemyTribute && !enemySide) return false;
    try {
        if (bc::vision::has_altar_lock(at)) return false;
        auto& me = bc::self();
        const auto& lock = me.consumables()[bc::consumable_type::altar_lock];
        if (lock.count() == 0) return false;
        if (lock.use(at)) {
            FixedStr<24> msg;
            trace().action((msg << "lock" << at.x << ',' << at.y).view());
            if (enemyTribute) trace().note("locked altar under an enemy tribute");
            return true;
        }
    } catch (const bc::error&) {
    }
    return false;
}

void support(World& w) {
    auto& me = bc::self();
    if (!(w.kit.hasHeal || w.incoming > 0 || !w.enemies.empty())) return;
    if (!me.can_cast()) return;
    for (int slot = 0; slot < 2; ++slot) {
        const auto& a = w.abilities()[static_cast<std::size_t>(slot)];
        if (!(a.is_heal() || a.is_shield())) continue;
        if (!w.abilityReady(slot)) continue;
        FixedStr<40> msg;
        try {
            if (a.is_heal()) {
                const int range = static_cast<int>(shape(a).range);
                const float amount = a.heal_amount();
                const AllySeen* best = nullptr;
                for (const auto& ally : w.allies) {
                    if (!ally.handle || dist(ally.pos, w.pos) > range || ally.maxHp - ally.hp < amount * 0.6f) continue;
                    if (!best || ally.hp / ally.maxHp < best->hp / best->maxHp) best = &ally;
                }
                if (best && a.use(*best->handle)) {
                    w.spentAbility(slot);
                    trace().action((msg << "h" << best->pos.x << ',' << best->pos.y).view());
                    return;
                }
            } else if (a.is_shield()) {
                const bool underFire = w.incoming > 0 || threatPerTurn(w, w.pos) > 0;
                if (underFire && w.hpFrac() < 0.9f && a.use()) {
                    w.spentAbility(slot);
                    trace().action("sh");
                    return;
                }
            }
        } catch (const bc::error& e) {
            trace().note((msg << "support cast failed: " << e.message()).view());
        }
    }
}

} // namespace combat
