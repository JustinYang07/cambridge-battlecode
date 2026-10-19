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
    // rangeBot (camp_focus_fire): no stickiness, and ties broken by tile, so every champion on a
    // camp picks the same monster from the same readings: the first one hurt becomes everyone's
    // target (each champion kept its own wolf and the party split its damage).
    if (!tune::camp_focus_fire)
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
        const bool tieWins = tune::camp_focus_fire && best && std::abs(key - bestKey) <= 1e-7f * std::max(key, bestKey) &&
                             (c.pos.x < best->pos.x || (c.pos.x == best->pos.x && c.pos.y < best->pos.y));
        if (key > bestKey * (tune::camp_focus_fire ? 1.0f + 1e-7f : 1.0f) || tieWins) { bestKey = std::max(key, bestKey); best = &c; }
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
        // Allies attacking it (claims), or the first enemy we saw take damage (the team target).
        const bool allyFocus = comms::focusMask(e.index, w.slot) != 0 || (e.index >= 0 && e.index == w.teamTarget);
        const bool fighting = focus.enemy || focus.anyEnemy || allyFocus;
        // combatBotv3: one we can finish now (target_mode 1), or the weakest (2), comes first.
        const bool finish = tune::target_mode == 1 && fighting && e.hp <= tune::finish_hits * std::max(w.strength, 1.0f);
        const bool wanted = chosen || allyFocus || focus.anyEnemy || finish || (focus.defend && dist(e.pos, w.pos) <= e.range) || e.channeling;
        if (!wanted) continue;
        const int rank = tune::target_mode == 2 && fighting ? 0 : finish ? 3 : chosen ? 2 : allyFocus ? 1 : 0;
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
            // Lab probe: ranged champions hit attacking monsters with spells only (aggro test).
            if (tune::probe_spell_only_camps && w.ranged && c->strength > 0) return false;
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

// The ability stuns: a crowd-control spell, or a ground spell that lingers (Ryth's area stun).
bool stuns(const bc::ability& a) {
    if (a.is_cc()) return true;
    const auto& info = a.info();
    if (!info) return false;
    const auto* aoe = std::get_if<bc::ability_aoe_damage>(&*info);
    return aoe && aoe->duration > 0;
}

// Stun target (tune::stun_target): 1 the squishiest enemy in reach (low health, half weight on max
// health), 2 the one doing the most damage to us (its damage per turn, doubled when it can hit us
// or an ally now); 0 our usual pick. Already-stunned enemies are skipped.
EnemySeen* stunTarget(World& w, int range, const Focus& focus) {
    if (tune::stun_target == 0) return enemyInRange(w, range, focus);
    EnemySeen* best = nullptr;
    float bestKey = 0;
    for (auto& e : w.enemies) {
        if (e.hp <= 0 || e.stunned || dist(e.pos, w.pos) > range) continue;
        float key;
        if (tune::stun_target == 1) {
            key = -(e.hp + 0.5f * e.maxHp);
        } else {
            bool hitting = dist(e.pos, w.pos) <= e.range;
            for (const auto& al : w.allies) hitting = hitting || dist(e.pos, al.pos) <= e.range;
            key = (e.strength + e.focus * 1.5f / tune::spell_cooldown) * (hitting ? 2.0f : 1.0f);
        }
        if (!best || key > bestKey) { best = &e; bestKey = key; }
    }
    return best ? best : enemyInRange(w, range, focus);
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
        const bool stunning = stuns(a);
        FixedStr<40> msg;
        try {
            if (a.is_aoe_damage()) {
                // Ground spell: centre it where it hits the most enemy champions, then monsters.
                const int radius = static_cast<int>(s.radius);
                bc::vec2 best{};
                int bestScore = 0, bestChamps = 0;
                auto consider = [&](bc::vec2 centre) {
                    if (dist(centre, w.pos) > range) return;
                    int camps = 0, champs = 0;
                    for (const auto& c : w.camps)
                        camps += c.hp > 0 && (focus.groupId < 0 || c.groupId == focus.groupId || !tune::aoe_champions_first) &&
                                 dist(c.pos, centre) <= radius;
                    for (const auto& e : w.enemies)
                        champs += e.hp > 0 && dist(e.pos, centre) <= radius;
                    const int score = tune::aoe_champions_first ? 100 * champs + camps : camps + 2 * champs;
                    if (score > bestScore) { bestScore = score; best = centre; bestChamps = champs; }
                };
                bool enemyInReach = false;
                for (const auto& e : w.enemies) enemyInReach = enemyInReach || (e.hp > 0 && dist(e.pos, w.pos) <= range + radius);
                if (tune::aoe_champions_first && enemyInReach) {
                    // Every tile in reach that touches an enemy (a spot between two of them hits both).
                    for (int dx = -range; dx <= range; ++dx)
                        for (int dy = -range; dy <= range; ++dy) {
                            const bc::vec2 t{w.pos.x + dx, w.pos.y + dy};
                            if (t.x < 0 || t.y < 0 || t.x >= Statics::get().width || t.y >= Statics::get().height) continue;
                            bool near = false;
                            for (const auto& e : w.enemies) near = near || (e.hp > 0 && dist(e.pos, t) <= radius);
                            if (near) consider(t);
                        }
                } else {
                    for (const auto& e : w.enemies) consider(e.pos);
                }
                for (const auto& c : w.camps) if (c.handle && (focus.groupId < 0 || c.groupId == focus.groupId)) consider(c.pos);
                if (bestScore > 0 && a.use(best)) {
                    w.spentAbility(slot);
                    trace().action((msg << "aoe" << best.x << ',' << best.y << 'x' << bestChamps).view());
                    return true;
                }
                continue;
            }
            if (auto* e = stunning ? stunTarget(w, range, focus) : enemyInRange(w, range, focus)) {
                if (a.use(e->handle)) {
                    w.spentAbility(slot);
                    if (stunning) e->stunned = true;
                    refresh(*e);
                    trace().action((msg << (a.is_cc() ? "st" : "se") << e->pos.x << ',' << e->pos.y).view());
                    return true;
                }
            }
            // A pure stun (no damage) on a camp only pays when its hits are landing: stun the
            // hardest hitter in reach while we are being hit and hurt (one round of no damage).
            if (s.modifier <= 0) {
                if (!(w.beingHit && w.incoming > 0 && w.hpFrac() < tune::defensive_stun_hp)) continue;
                CampSeen* worst = nullptr;
                for (auto& c : w.camps)
                    if (c.handle && c.hp > 0 && c.strength > 0 && dist(c.pos, w.pos) <= std::min(range, tune::monster_range) &&
                        (!worst || c.strength > worst->strength))
                        worst = &c;
                if (worst && a.use(*worst->handle)) {
                    w.spentAbility(slot);
                    trace().action((msg << "sc" << worst->pos.x << ',' << worst->pos.y).view());
                    trace().note("stunned the monster hitting us");
                    return true;
                }
                continue;
            }
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

FightEstimate campFight(const World& w, const GroupInfo& g, float healthLeft, int party, float share, unsigned mates) {
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
    // combatBotv2: a party's damage is its members' damage, not ours times its size (rows: Ren, weak
    // early, judged the third wolves too strong beside Stellar, whose splash does most of the work).
    const float mine = std::max(campDamagePerTurn(w, g), 1.0f);
    float dps = mine;
    int counted = 1;
    if (tune::party_real_dps)
        for (int sl = 0; sl < w.allyCount && sl < 8 && counted < party; ++sl)
            if (sl != w.slot && ((mates >> sl) & 1u))
                if (const float d = w.allyCampDps(sl, g); d > 0) {
                    dps += d;
                    ++counted;
                }
    dps += mine * static_cast<float>(std::max(party - counted, 0));
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

int damageSpellRange(const World& w) {
    int best = 0;
    for (const auto& a : w.abilities()) {
        if (!(a.is_damage() || a.is_aoe_damage() || a.is_cc())) continue;
        const auto s = shape(a);
        if (s.modifier > 0) best = std::max(best, static_cast<int>(s.range));
    }
    return best;
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
    // Our own altars too: a lock blocks enemy tributes but not ours, so enemies raiding our side
    // cannot bank there.
    if (!enemyTribute && !enemySide && !tune::lock_own_altars) return false;
    try {
        if (bc::vision::has_altar_lock(at)) return false;
        auto& me = bc::self();
        const auto& lock = me.consumables()[bc::consumable_type::altar_lock];
        if (lock.count() == 0) return false;
        if (lock.use(at)) {
            FixedStr<24> msg;
            trace().action((msg << "lock" << at.x << ',' << at.y).view());
            if (enemyTribute) trace().note("locked altar under an enemy tribute");
            else if (!enemySide) trace().note("locked our own altar");
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
