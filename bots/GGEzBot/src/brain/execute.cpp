#include <array>
// Turning the chosen option into this turn's move / attack / cast actions.
#include <algorithm>
#include "../combat/combat.h"
#include "../core/comms.h"
#include "../core/config.h"
#include "../core/fixed_str.h"
#include "../core/trace.h"
#include "../nav/fields.h"
#include "../nav/navigator.h"
#include "../micro/micro.h"
#include "scorers.h"
#include "brain.h"

namespace bc = cambc;

namespace {
// Set paths (loopBot): the precomputed fields a walk to a camp group / our spawn follows.
FarField groupPath(int groupId) { return {&fields::toGroup, &fields::ensureGroup, groupId}; }
FarField spawnPath() { return {&fields::toSpawn, &fields::ensureSpawn, 0}; }
}  // namespace

namespace brain {

namespace {

bool groupMemberInRange(const World& w, int groupId) {
    for (const auto& c : w.camps)
        if (c.groupId == groupId && c.hp > 0 && dist(c.pos, w.pos) <= w.range) return true;
    return false;
}

bool groupVisiblyCleared(const World& w, const GroupInfo& g) {
    if (!w.groups[static_cast<std::size_t>(g.id)].visibleNow) return false;
    for (const auto& c : w.camps)
        if (c.groupId == g.id && c.hp > 0) return false;
    return true;
}

int alliesNear(const World& w, const GroupInfo& g) {
    // Allies claiming this camp from within 6 tiles of it (claims are rewritten every turn; a recall,
    // tribute, flight or fight replaces them). yBot: only these count. Counting every visible ally
    // within 6 let Stellar at 39% open the enemy's elk pair on eight because Ryth was walking past
    // and Varg was recalling beside it; neither joined, and the elk mauled Ryth.
    int claimed = 0;
    for (int s = 0; s < comms::alert_byte && s < w.allyCount; ++s)
        if (s != w.slot && comms::claimOf(s) == g.id && comms::claimBracket(s) == 0) ++claimed;
    return claimed;
}

// The tank opens (loopBot): monsters hit the nearest champion, so whoever wakes a sleeping camp
// takes its hits until the tank closes in. A ranged champion holds fire while a tank claiming the
// camp is on its way but not yet beside it (at most tune::tank_open_wait turns).
bool holdForTank(World& w, const GroupInfo& g) {
    static int heldGroup = -1, heldSince = 0;
    if (!tune::tank_opens || !w.ranged || isTank(w.type)) return false;
    const auto& mem = w.groups[static_cast<std::size_t>(g.id)];
    if (mem.healthLeft < 0.99f || w.beingHit) return false;  // already open
    const auto* info = Statics::get().campAt(g.tiles.empty() ? g.center : g.tiles[0]);
    bool asleep = false;
    for (auto t : g.tiles)
        if (const auto* ci = Statics::get().campAt(t); ci && ci->handle) {
            try {
                const auto st = ci->handle->state();
                if (!st.is_alive() && !st.is_asleep()) continue;  // dead member
                asleep = st.is_asleep();
            } catch (const bc::error&) {
            }
            break;
        }
    (void)info;
    if (!asleep) return false;
    bool tankComing = false;
    for (int s = 0; s < comms::alert_byte && s < w.allyCount; ++s) {
        if (s == w.slot || comms::claimOf(s) != g.id || !isTank(w.allyTypes[static_cast<std::size_t>(s)])) continue;
        const auto p = w.allyPosCached(s);
        if (p.x < 0) continue;
        int nearest = 1 << 20;
        for (auto t : g.tiles) nearest = std::min(nearest, dist(p, t));
        if (nearest > 1 && dist(p, g.center) <= tune::tank_open_radius) tankComing = true;
    }
    if (!tankComing) return false;
    if (heldGroup != g.id) { heldGroup = g.id; heldSince = w.round; }
    if (w.round - heldSince > tune::tank_open_wait) return false;
    trace().note("tank opens: holding fire");
    return true;
}

void farmGroup(World& w, const GroupInfo& g, int need);
bool recallClear(const World& w);  // GGEzBot: no recall within reach of any live camp (defined below)
void capture(World& w, const GroupInfo& g);

// While waiting for the party, work a gem/gold cluster beside the camp instead of standing idle
// (Marcella held 40 turns beside lanes' north elk next to three gem clusters). Our claim stays on
// the big camp so the allies keep coming. Sleeping monsters do not attack, so the camp's own reach
// is fine; other monsters' reach is not.
bool farmWhileWaiting(World& w, const GroupInfo& g) {
    const auto& statics = Statics::get();
    const GroupInfo* best = nullptr;
    int bestDist = 1 << 20;
    for (const auto& other : statics.groups) {
        if (!other.harmless || !other.reachable || dist(other.center, g.center) > tune::wait_farm_radius) continue;
        const auto& mem = w.groups[static_cast<std::size_t>(other.id)];
        if (mem.status == GroupStatus::respawning) continue;
        if ((other.tileMask & w.dangerExcept(g.id)).is_empty() == false) continue;
        if (comms::ownersMask(other.id, w.slot, dist(other.center, w.pos))) continue;  // a nearer ally is on it
        const int d = dist(other.center, w.pos);
        if (d < bestDist) { bestDist = d; best = &other; }
    }
    if (!best) return false;
    trace().note("waiting: farming a cluster nearby");
    if (w.groups[static_cast<std::size_t>(best->id)].status == GroupStatus::reward) capture(w, *best);
    else farmGroup(w, *best, 1);
    return true;
}

// Wait just outside monster range until enough allies have arrived to pull the group.
bool waitForParty(World& w, const GroupInfo& g, int need) {
    const auto& mem = w.groups[static_cast<std::size_t>(g.id)];
    // Readiness has hysteresis: allies hovering at the edge of the count radius used to flip
    // it every turn, so two champions stepped in and out waiting on each other.
    const bool committed = w.partyGroup == g.id && w.round <= w.partyReadyUntil;
    const bool partyReady = need <= 1 || committed || alliesNear(w, g) >= need - 1;
    if (partyReady && !committed && need > 1) {
        w.partyGroup = g.id;
        // Already waiting: the others see us ready too. Just arrived: let them read our claim first.
        w.partyGoAt = w.round + (w.waitingTurns > 0 ? 0 : tune::party_sync_turns);
        w.partyReadyUntil = w.partyGoAt + tune::party_commit_turns;
    }
    const bool syncing = need > 1 && w.partyGroup == g.id && w.round < w.partyGoAt;
    // A fight under way does not wait for help: we are taking its hits, or an ally is beside the
    // damaged camp. Damage alone is not enough, since camps do not heal: Vendetta found lanes'
    // elk at 55% from its own earlier attempt and went back in alone three times.
    const bool underWay = (w.engagedGroup == g.id && w.beingHit) || (mem.healthLeft < 0.99f && alliesNear(w, g) > 0);
    // Engage check (user: no hopeless camps): the plan counted a party; before the first hit on an
    // untouched camp, the champions actually here must outlast it (traced games: 1.3 a game passed
    // with a planned party of 2-3 and one champion went in alone, then left or died).
    bool hereIsEnough = true;
    if (tune::engage_check && !underWay && partyReady && !syncing && !g.harmless && mem.healthLeft >= 0.99f) {
        unsigned here = 0;
        int count = 1;
        for (int sl = 0; sl < w.allyCount && sl < 8; ++sl)
            if (sl != w.slot) {
                const auto p = w.allyPosCached(sl);
                if (p.x >= 0 && dist(p, g.center) <= g.maxRange + 3) { here |= 1u << sl; ++count; }
            }
        hereIsEnough = combat::tankSurvives(w, g, mem.healthLeft, count, here);
        if (!hereIsEnough) trace().note("engage check: the champions here would lose it");
    }
    if (underWay || (partyReady && !syncing && hereIsEnough)) {
        w.waitingTurns = 0;
        w.partyTurns = 0;
        return false;
    }
    // Nobody is coming: drop this camp for a while and take something else rather than stand at it.
    const auto dropCamp = [&](const char* why) {
        w.stuckGroup = g.id;
        w.stuckUntil = w.round + tune::party_drop_rounds;
        w.partyHoldBlockUntil = w.round + tune::party_drop_rounds;  // fix 5: no chain of holds on other camps
        if (scorers::teamRushOn(w, g.id)) w.rushGiveUpUntil = w.round + tune::rush_retry_rounds;
        w.waitingTurns = 0;
        w.partyTurns = 0;
        w.forceReplan = true;
        trace().note(why);
    };
    // Travelling counts too (ping-pong fix 2): walking to a teammate reset the hold count, then the
    // ring walk took us back, so the cap never tripped (ambush r1092-1100: 43,29 <-> 44,30, "need 3 more").
    if (w.lastOption != static_cast<int>(OptionKind::farm) || w.lastTarget != g.id) w.partyTurns = 0;  // a new camp or a break
    if (++w.partyTurns > tune::party_form_cap && tune::party_form_cap > 0 && !g.blocksAltars) {
        dropCamp("party not forming after travel: dropping the camp");
        return false;
    }
    // Travel together (user): a teammate claiming this camp is still on its way: walk with them so
    // we arrive as a party, instead of going ahead to stand at the camp's edge. Only while they are
    // clearly behind us and near enough to meet; this is travel, not waiting.
    if (tune::party_travel && !syncing) {
        bc::vec2 mate{-1, -1};
        int mateGap = 1 << 20;
        const int ours = dist(w.pos, g.center);
        for (int s = 0; s < comms::alert_byte && s < w.allyCount; ++s) {
            if (s == w.slot || comms::claimOf(s) != g.id) continue;
            const auto p = w.allyPosCached(s);
            if (p.x < 0) continue;
            const int behind = dist(p, g.center) - ours;
            const int gap = dist(p, w.pos);
            if (behind >= tune::party_travel_gap && gap <= tune::party_travel_radius && gap < mateGap) { mateGap = gap; mate = p; }
        }
        if (mate.x >= 0 && nav().stepToward(w, mate, 2, true, &w.danger)) {
            w.waitingTurns = 0;
            trace().note("travelling with a teammate to the camp");
            return true;
        }
    }
    ++w.waitingTurns;
    // Waits are short (user: at most party_wait_cap rounds): nobody is coming, so drop this camp for a
    // while and take something else rather than stand at it (monolith vs patisserie: all five held the
    // Monolith's edge ~110 rounds; the 80-turn stuck valve was the only way out).
    if (tune::party_wait_cap > 0 && w.waitingTurns > tune::party_wait_cap && !g.blocksAltars) dropCamp("party not forming: dropping the camp");
    if (tune::rush_form_turns > 0 && w.waitingTurns > tune::rush_form_turns && scorers::teamRushOn(w, g.id) && !g.blocksAltars) {
        w.rushGiveUpUntil = w.round + tune::rush_retry_rounds;
        w.waitingTurns = 0;
        w.forceReplan = true;
        trace().note("rush: party never formed, farming elsewhere");
    }
    if (!syncing && farmWhileWaiting(w, g)) return true;
    bc::bitboard ring, inner;
    ring.clear();
    inner.clear();
    for (auto t : g.tiles) {
        ring.draw_chebyshev(t, static_cast<float>(g.maxRange + 2));
        inner.draw_chebyshev(t, static_cast<float>(g.maxRange));
    }
    auto goals = ring & ~inner & ~w.danger;  // never wait inside another monster's reach
    FixedStr<24> msg;
    trace().note((msg << "need " << (need - 1 - alliesNear(w, g)) << " more").view());
    if (goals.is_set(w.pos)) trace().action("hold");
    // No way round the danger (boss-battle: an elk camp's reach covers one approach to the Monoliths
    // and Varg stood 7 tiles off for 150 rounds, uncounted, while the party never formed): go anyway.
    else if (!nav().stepToward(w, goals, false, &w.danger, g.center, groupPath(g.id)))
        nav().stepToward(w, ring & ~inner, false, nullptr, g.center, groupPath(g.id));
    return true;
}

// Team rush pull: gather first, then the ranged hit from just outside the monsters' reach (spells
// reach the camp's range + 1; monsters do not chase) while the melee wait at that ring until the camp
// is low. Returns true if it handled the turn; false lets the normal farm execution run.
bool rushPull(World& w, const GroupInfo& g, int need) {
    if (waitForParty(w, g, need)) return true;
    // No spell outranges this camp: the whole gathered party fights it at once.
    if (g.maxRange >= tune::rush_spell_reach) return false;
    const auto& mem = w.groups[static_cast<std::size_t>(g.id)];
    if (!w.ranged && mem.healthLeft < tune::team_rush_melee_join) return false;  // finish it together
    // A pull that deals no damage is a stall, not a pull: everyone fights the camp the usual way.
    static int stallGroup = -1, stallSince = 0;
    static float stallHealth = 2.0f;
    if (stallGroup != g.id || mem.healthLeft < stallHealth - 0.005f) {
        stallGroup = g.id;
        stallSince = w.round;
        stallHealth = mem.healthLeft;
    }
    if (w.round - stallSince > tune::team_rush_stall_rounds) {
        trace().note("rush: no damage, fighting it");
        return false;
    }
    // Only members that attack have a reach (clusters do not hit back): a ring drawn around the
    // clusters as well stood the ranged a tile beyond spell range of the elks.
    bc::bitboard inner, reach;
    inner.clear();
    reach.clear();
    bool live = false;
    for (const auto& c : w.camps)
        if (c.groupId == g.id && c.hp > 0) {
            live = true;
            if (c.strength <= 0) continue;
            inner.draw_chebyshev(c.pos, static_cast<float>(c.range));
            reach.draw_chebyshev(c.pos, static_cast<float>(c.range + 1));
        }
    if (!live)
        for (auto t : g.tiles) {
            if (const auto* tc = Statics::get().campAt(t); tc && tc->strength <= 0) continue;
            inner.draw_chebyshev(t, static_cast<float>(g.maxRange));
            reach.draw_chebyshev(t, static_cast<float>(g.maxRange + 1));
        }
    const combat::Focus focus{.groupId = g.id};
    if (w.ranged) combat::castFromRange(w, focus);
    const auto goals = reach & ~inner;
    if (!goals.is_empty() && !goals.is_set(w.pos)) nav().stepToward(w, goals, false, nullptr, g.center);
    else trace().action("hold");
    combat::act(w, focus, false);  // a basic attack only lands if a member is in range from here
    trace().note(w.ranged ? "rush: outranging" : "rush: melee waits");
    return true;
}

// combatBotv3 probe (camp_corner_stand): tiles from which a ranged champion reaches a live member of
// the group (Chebyshev range) while more than camp_reach_euclid_sq (squared straight-line distance)
// from every member that attacks: the diagonal corners, if monsters reach in a circle.
bc::bitboard cornerTiles(const World& w, const GroupInfo& g) {
    bc::bitboard out;
    out.clear();
    const auto& st = Statics::get();
    const int r = w.range;
    for (const auto& c : w.camps) {
        if (c.groupId != g.id || c.hp <= 0) continue;
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sy = -1; sy <= 1; sy += 2)
                for (int k = 0; k <= 1; ++k) {
                    const bc::vec2 t{c.pos.x + sx * r, c.pos.y + sy * (r - k)};
                    const bc::vec2 t2{c.pos.x + sx * (r - k), c.pos.y + sy * r};
                    for (const auto q : {t, t2}) {
                        if (q.x < 0 || q.y < 0 || q.x >= st.width || q.y >= st.height || st.terrain.is_set(q)) continue;
                        bool safe = true;
                        for (const auto& m : w.camps) {
                            if (m.groupId != g.id || m.hp <= 0 || m.strength <= 0) continue;
                            const int dx = q.x - m.pos.x, dy = q.y - m.pos.y;
                            if (dx * dx + dy * dy <= tune::camp_reach_euclid_sq) { safe = false; break; }
                        }
                        if (safe) out.set(q);
                    }
                }
    }
    return out & ~g.tileMask;
}

void farmGroup(World& w, const GroupInfo& g, int need) {
    combat::Focus focus{.groupId = g.id};
    const auto status = w.groups[static_cast<std::size_t>(g.id)].status;
    if (status == GroupStatus::respawning && Statics::get().onGroup(g, w.pos)) {
        trace().note("off spawn tile");
        auto goals = nav().standTiles(w, g.center, 2) & ~g.tileMask & ~w.danger;
        nav().stepToward(w, goals, false);
        return;
    }
    if (scorers::teamRushOn(w, g.id) && rushPull(w, g, need)) return;
    if (waitForParty(w, g, need)) return;
    // yBot: never open a sleeping camp next to a fight with enemy champions (eight: our Ren's
    // stray hit woke our elk pair beside a 2v3 fight, and the elk mauled us as we chased through).
    // Not a team rush (stampede's elk, the opening camp while the opening lasts): there the whole team
    // opens it together, enemies or not.
    const bool teamCamp = scorers::teamRushOn(w, g.id);
    const bool fightHold = tune::no_open_near_fight && !teamCamp && w.campAsleep(g.id) && w.groups[static_cast<std::size_t>(g.id)].healthLeft >= 0.99f &&
                           w.fightNearCamp(g.id);
    if (fightHold) trace().note("fight near: not opening the camp");
    const bool holdFire = fightHold || holdForTank(w, g);
    // combatBotv3 spell clear: a ranged champion whose damage spell outranges the monsters, alone on
    // this camp, clears its attacking monsters with spells only from outside their reach (camps only
    // hit whoever attacks them, and only within their attack range): no damage taken.
    if ((tune::spell_clear_camps || w.spellClearGroup == g.id) && w.ranged && !holdFire) {
        const int sr = combat::damageSpellRange(w);
        bool attackers = false, allyHere = false;
        for (const auto& c : w.camps) attackers = attackers || (c.groupId == g.id && c.hp > 0 && c.strength > 0);
        for (int sl = 0; sl < w.allyCount && sl < 8; ++sl) {
            if (sl == w.slot) continue;
            const auto p = w.allyPosCached(sl);
            allyHere = allyHere || (p.x >= 0 && dist(p, g.center) <= tune::spell_clear_ally_radius);
        }
        // Stealing and enemy champions show up near us: run back toward our side and leave the
        // steal for a while (user).
        if (w.spellClearGroup == g.id && tune::steal_back_off_radius > 0) {
            bool threat = false;
            for (const auto& e : w.enemies) threat = threat || (e.hp > 0 && dist(e.pos, w.pos) <= tune::steal_back_off_radius);
            if (threat) {
                w.stealBackoffUntil = w.round + tune::steal_back_off_rounds;
                w.forceReplan = true;
                if (!nav().stepHome(w)) nav().stepToward(w, Statics::get().allySpawn, true, nullptr, Statics::get().spawnCenter);
                trace().note("steal: enemies seen, back off");
                return;
            }
        }
        if (sr > g.maxRange && attackers && (!allyHere || w.spellClearGroup == g.id)) {
            bc::bitboard inReach, inSpell;
            inReach.clear();
            inSpell.clear();
            for (const auto& c : w.camps) {
                if (c.hp <= 0 || c.strength <= 0) continue;
                // Out of reach of every attacking monster we can see, not just this group's: the
                // neighbouring groups sit a few tiles away (expanse: Stellar died to them 1.5 a game).
                inReach.draw_chebyshev(c.pos, static_cast<float>(c.range));
                if (c.groupId == g.id) inSpell.draw_chebyshev(c.pos, static_cast<float>(sr));
            }
            auto goals = inSpell & ~inReach & ~w.danger & ~Statics::get().terrain;
            // Between spells, basic-attack the group's clusters (they do not hit back) from a tile
            // still outside every monster's reach, when there is one.
            if (tune::spell_clear_hit_clusters) {
                bc::bitboard inBasic;
                inBasic.clear();
                for (const auto& c : w.camps)
                    if (c.groupId == g.id && c.hp > 0 && c.strength <= 0) inBasic.draw_chebyshev(c.pos, static_cast<float>(w.range));
                if (const auto both = goals & inBasic; !both.is_empty()) goals = both;
            }
            // Cast from the tiles nearest our side, not from deep in their half (user).
            if (tune::spell_clear_near_side && !goals.is_empty()) {
                const auto home = Statics::get().spawnCenter;
                int best = 1 << 20;
                for (auto t : goals.positions()) best = std::min(best, dist(t, home));
                bc::bitboard near;
                near.clear();
                for (auto t : goals.positions())
                    if (dist(t, home) <= best + 1) near.set(t);
                goals = near;
            }
            if (!goals.is_empty()) {
                combat::Focus spellFocus{.groupId = g.id};
                w.spellClearRound = w.round;
                if (goals.is_set(w.pos)) {
                    bool acted = combat::castFromRange(w, spellFocus);
                    if (!acted && tune::spell_clear_hit_clusters && bc::self().can_attack())
                        for (auto& c : w.camps) {
                            if (c.groupId != g.id || c.hp <= 0 || c.strength > 0 || !c.handle || dist(c.pos, w.pos) > w.range) continue;
                            try {
                                bc::self().attack(*c.handle);
                                FixedStr<24> msg;
                                trace().action((msg << "ac" << c.pos.x << ',' << c.pos.y).view());
                                acted = true;
                            } catch (const bc::error&) {
                            }
                            break;
                        }
                    if (!acted) trace().action("hold");
                } else {
                    combat::castFromRange(w, spellFocus);
                    nav().stepToward(w, goals, false, &w.danger, g.center);
                }
                trace().note("spell clear");
                return;
            }
        }
    }
    if (groupMemberInRange(w, g.id)) {
        // combatBotv3 probe: ranged in range but not on a corner: step to a corner within 2 tiles.
        if (tune::camp_corner_stand && w.ranged && !holdFire && bc::self().can_move()) {
            const auto corners = cornerTiles(w, g);
            if (!corners.is_empty() && !corners.is_set(w.pos)) {
                bc::bitboard nearCorner;
                nearCorner.clear();
                nearCorner.draw_chebyshev(w.pos, 2.0f);
                if (!(corners & nearCorner).is_empty()) {
                    combat::act(w, focus, true);
                    nav().stepToward(w, corners & nearCorner, false);
                    trace().note("camp corner");
                    return;
                }
            }
        }
        // combatBotv3: ranged with enemies about: shoot from the safest tile still in range of a member.
        bool stepped = false;
        if (tune::exposure_stand && w.ranged && !w.enemies.empty()) {
            bc::bitboard keep;
            keep.clear();
            for (const auto& c : w.camps)
                if (c.groupId == g.id && c.hp > 0) keep |= nav().standTiles(w, c.pos, w.range);
            keep = keep & ~g.tileMask & ~w.dangerExcept(g.id);
            stepped = !keep.is_empty() && nav().saferStep(w, keep);
        }
        if (holdFire) trace().action("hold");
        else combat::act(w, focus, !stepped);
    } else {
        // Aim at members that are still standing; dead members' tiles are empty and standing
        // next to one leaves us out of reach of the rest (a melee champion would idle forever).
        bc::bitboard goals;
        goals.clear();
        bool anyLive = false;
        for (const auto& c : w.camps)
            if (c.groupId == g.id && c.hp > 0) {
                goals |= nav().standTiles(w, c.pos, w.range);
                anyLive = true;
            }
        // Nothing in sight (bushes hide camps from range: Stellar stood 3 tiles from a cluster
        // behind a bush row for 333 turns, "in range" of tiles it could not see): walk up beside it.
        if (!anyLive)
            for (auto t : g.tiles) goals |= nav().standTiles(w, t, 1);
        // Never stand on a spawn tile unless capturing: an occupied tile stops the whole group respawning.
        goals = goals & ~g.tileMask;
        // Only tiles we can walk to: a camp sealed in a pocket made every goal unreachable and the
        // whole team stood still for 800 turns (ambush).
        if (const auto open = goals & Statics::get().reach; !open.is_empty()) goals = open;
        // Monsters hit the nearest champion: ranged allies stand at full range so whoever is
        // closer (ideally the tank) soaks the hits.
        if (tune::camp_corner_stand && w.ranged) {
            if (const auto corners = cornerTiles(w, g) & goals; !corners.is_empty()) goals = corners;
        }
        if (w.ranged) {
            bc::bitboard closeIn;
            closeIn.clear();
            for (auto t : g.tiles) closeIn.draw_chebyshev(t, static_cast<float>(w.range - 1));
            const auto far = goals & ~closeIn;
            if (!far.is_empty()) goals = far;
        }
        auto avoid = w.dangerExcept(g.id);
        if (w.gems >= tune::bag_avoid_gems) avoid |= w.enemyReach();
        // Casters hit from 4 tiles: spend the turn casting rather than walking into range.
        if (!holdFire) combat::castFromRange(w, focus);
        nav().stepToward(w, goals, dist(w.pos, g.center) > 4, &avoid, g.center, groupPath(g.id));
        if (!holdFire) combat::act(w, focus, false);
    }
    // Last monster just died: start standing on the capture tiles straight away.
    // Not while it is respawning: then the "step off the spawn tile" rule above fires next turn
    // and the two bounced champions between tiles for 100+ rounds.
    if (status != GroupStatus::respawning && groupVisiblyCleared(w, g) && !Statics::get().onGroup(g, w.pos) &&
        bc::self().can_move()) {
        trace().note("cleared");
        nav().stepToward(w, g.tileMask, false);
    }
}

void capture(World& w, const GroupInfo& g) {
    // A teammate keeps this capture: never stand on its tiles too (the reward is split between
    // everyone on them; rows split its wolves 33/33 all game once the opening rotation ended).
    if (tune::capture_step_off && scorers::allyHoldingOf(w, g.id) && scorers::captureShareOf(w, g.id) < 1.0f) {
        if (Statics::get().onGroup(g, w.pos)) {
            const auto blocked = nav().blocked(w);
            bc::bitboard off;
            off.clear();
            off.draw_chebyshev(w.pos, 1.0f);
            off = off & ~g.tileMask & ~blocked & Statics::get().reach;
            if (!off.is_empty()) nav().stepToward(w, off, false);
            trace().note("teammate keeps the capture: off the tiles");
        }
        combat::act(w, combat::Focus{.groupId = g.id}, false);
        return;
    }
    if (!Statics::get().onGroup(g, w.pos)) {
        nav().stepToward(w, g.tileMask, dist(w.pos, g.center) > 3, &w.danger, g.center, groupPath(g.id));
        w.captureTurns = 0;
    } else {
        trace().action("cap");
        // A capture tile within tribute range of an altar: captures resolve before channels, and a
        // tribute banks the whole bag when it completes, including gems captured mid-channel. Start
        // a tribute timed to finish just as the capture does, and the reward is banked with it.
        if (w.lastTarget == g.id && w.lastOption == static_cast<int>(OptionKind::capture)) ++w.captureTurns;
        else w.captureTurns = 1;
        // Fix 1 (stalls): a capture that never completes means the group is not really cleared (a
        // 30-tile gold row on yellow brick road: members out of sight still alive, the whole team
        // stood on its tiles 178 rounds). Back to alive, and leave it alone for a while.
        if (tune::capture_timeout > 0 && w.captureTurns > static_cast<int>(tune::capture_turns) + tune::capture_timeout) {
            // The engine says the reward is there (the row is cleared); what stops the capture is the
            // region being contested - the enemy on the far end of a row that spans the map. Its own
            // avoid entry, so the stuck valve (one stuckGroup slot) cannot overwrite it.
            if (w.groupAvoidUntil.size() < w.groups.size()) w.groupAvoidUntil.resize(w.groups.size(), -1);
            w.groupAvoidUntil[static_cast<std::size_t>(g.id)] = w.round + tune::capture_drop_rounds;
            w.captureTurns = 0;
            w.forceReplan = true;
            trace().note("capture not completing: group not cleared, leaving it");
        }
        // combatBotv2 (as Fish): the keeper recalls from the tile a round before the capture
        // completes. The reward lands during the 5-turn channel, so it is on its way home (spawn,
        // shop, the altar by spawn) instead of walking the bag over. Only when banking via spawn is
        // about as quick as walking to an altar, or we want the heal anyway, and nobody is about.
        if (tune::recall_on_capture && w.captureTurns >= static_cast<int>(tune::capture_turns) - 1 &&
            w.enemyPressure(w.pos, tune::enemy_watch_radius) <= 0 && !w.beingHit && w.incoming <= 0 &&
            scorers::captureShareOf(w, g.id) >= 1.0f && recallClear(w)) {
            float viaHome = 1e9f, walk = 1e9f;
            for (int i = 0; i < static_cast<int>(Statics::get().altars.size()); ++i) {
                viaHome = std::min(viaHome, scorers::recallBankTurns(w, i));
                if (const int d = fields::toAltar(i, w.pos); d >= 0 && d < Navigator::unreachable) walk = std::min(walk, w.walkTurns(static_cast<float>(d)));
            }
            if (viaHome <= walk + tune::capture_recall_margin || w.hpFrac() < tune::capture_recall_hp) {
                try {
                    bc::self().recall();
                    trace().action("rc");
                    trace().note("recall timed with the capture");
                    return;
                } catch (const bc::error& e) {
                    trace().note(std::string("recall failed: ") + e.message());
                }
            }
        }
        const int ticks = std::max(1, (w.gems + 9) / 10);
        if (w.gems > 0 && w.captureTurns + ticks >= static_cast<int>(tune::capture_turns)) {
            for (auto a : Statics::get().altars)
                if (dist(a, w.pos) <= bc::constants::submit_gems_range) {
                    try {
                        bc::self().submit_gems(a);
                        trace().note("tribute timed with the capture beside an altar");
                        return;
                    } catch (const bc::error&) {
                    }
                    break;
                }
        }
    }
    combat::act(w, combat::Focus{.awakeCampsOnly = true}, false);  // never wake a sleeping camp (patch: monoliths reach 5)
}

void offTeammateCapture(World& w);

// GGEzBot: a recall is cancelled by any hit, and the "out of the engaged camp's reach" test kept
// leaving us inside another camp's reach (ranges differ per map: wolf 3, elk 3-4, monolith 3-5;
// recalls were interrupted again and again in the ladder replays). Hard rule instead: no recall
// within its reach (+ tune::recall_clearance_margin) of any tile of a group that may have live monsters, from the
// static layout (a group counts until it is dead or respawning), not from vision. The clearance
// is each group's own longest range plus a margin: wolves 3, elks 3-4, monoliths 3-5 (user).
// A group can hit us: live, and (awake-only mode) seen awake lately or damaged (a camp we or a
// teammate fought stays awake; sleeping camps never swing at a recall beside them).
bool campCanHit(const World& w, const GroupInfo& g) {
    if (g.harmless) return false;
    const auto& mem = w.groups[static_cast<std::size_t>(g.id)];
    if (mem.status == GroupStatus::respawning || mem.status == GroupStatus::reward) return false;
    if (!tune::recall_clearance_awake_only) return true;
    return (mem.awakeRound >= 0 && w.round - mem.awakeRound <= tune::recall_awake_rounds) || mem.healthLeft < 0.999f ||
           w.engagedGroup == g.id;
}

bool clearToRecall(World& w, bool checkOnly);
bool recallClear(const World& w) { return clearToRecall(const_cast<World&>(w), true); }
bool recallClearTheirs(const World& w) {
    for (const auto& g : Statics::get().groups) {
        if (!campCanHit(w, g)) continue;
        for (auto t : g.tiles)
            if (dist(w.pos, t) <= g.maxRange + tune::recall_clearance_margin) return false;
    }
    return true;
}

// One step away from every such camp tile nearby (toward spawn on ties); false if none is near.
bool stepClearOfCamps(World& w) {
    std::vector<bc::vec2> threats;
    int keep = 0;
    for (const auto& g : Statics::get().groups) {
        if (!campCanHit(w, g)) continue;
        for (auto t : g.tiles)
            if (dist(w.pos, t) <= g.maxRange + tune::recall_clearance_margin + 2) threats.push_back(t);
        keep = std::max(keep, g.maxRange + tune::recall_clearance_margin);
    }
    return nav().stepAway(w, threats, keep);
}

// Recall spacing (user): a recall is a 5-turn channel that damage cancels, and champions kept
// recalling inside a monster camp's reach and dying to it. The camps come from the map table, not
// from vision: monsters see through bushes and hit round corners, so a camp out of sight is still
// a camp (a check over visible camps only let 57 of 77 recall deaths start inside the killer's range).
struct CampThreat {
    bc::vec2 pos;
    int range;
};

// Every monster near us that can hit us: its group not known dead or respawning, and active (seen
// awake this turn, the group we are engaged with, its live state awake, or its state unreadable).
// hitAnyway: we were just hit and nothing visible explains it: count every live group as active.
std::vector<CampThreat> activeCampsNear(const World& w, int radius, bool hitAnyway) {
    const auto& st = Statics::get();
    std::vector<CampThreat> out;
    for (const auto& g : st.groups) {
        if (g.harmless || g.tiles.empty() || dist(g.center, w.pos) > radius + 6) continue;
        const auto& mem = w.groups[static_cast<std::size_t>(g.id)];
        if (mem.status == GroupStatus::respawning || mem.status == GroupStatus::reward) continue;
        // Flat rule (recall_clearance_awake_only off): every live group counts, asleep or not. A camp
        // asleep when the channel starts can be woken during it (version 2's rule: 0.01 camp-hit
        // recall deaths a game vs 0.19 trusting the asleep reading).
        bool active = hitAnyway || !tune::recall_clearance_awake_only || mem.awakeRound == w.round || g.id == w.engagedGroup;
        if (!active) {
            active = true;  // unknown: assume it can hit us
            if (const auto* info = st.campAt(g.tiles.front()); info && info->handle) {
                try {
                    active = !info->handle->state().is_asleep();
                } catch (const bc::error&) {
                }
            }
        }
        if (!active) continue;
        for (auto t : g.tiles) {
            const auto* c = st.campAt(t);
            if (!c || c->strength <= 0) continue;
            if (w.isVisible(t) && !w.campAt(t)) continue;  // we can see it is dead
            out.push_back({t, tune::recall_group_range ? g.maxRange : c->range});
        }
    }
    return out;
}

// Before any recall: every active monster at least its range + recall_camp_margin away (map
// distances), and no enemy champion that just hit us still in reach. Otherwise walk to the nearest
// tile within recall_search_radius that is that far from all of them, and recall there on a later
// turn. Returns true when the recall may go ahead here (also when no tile helps: cornered, recall
// anyway as before). checkOnly: never move, just say whether it is clear (optional recalls - bank,
// capture - skip the recall and carry on instead).
bool clearToRecall(World& w, bool checkOnly) {
    if (!tune::recall_spacing) return true;
    const int margin = tune::recall_camp_margin;
    const int radius = tune::recall_search_radius;
    std::vector<bc::vec2> hitters;  // enemy champions that can have hit us
    if (w.beingHit)
        for (const auto& e : w.enemies)
            if (e.hp > 0 && dist(e.pos, w.pos) <= e.range + 1) hitters.push_back(e.pos);
    const auto camps = activeCampsNear(w, radius + margin + 6, w.beingHit && hitters.empty());
    auto campSafe = [&](bc::vec2 t) {
        for (const auto& c : camps)
            if (dist(c.pos, t) < c.range + margin) return false;
        return true;
    };
    if (campSafe(w.pos) && hitters.empty()) return true;
    if (checkOnly) return false;
    const auto& st = Statics::get();
    bc::bitboard bad, goals;
    bad.clear();
    goals.clear();
    for (const auto& c : camps) bad.draw_chebyshev(c.pos, static_cast<float>(c.range + margin - 1));
    for (const auto& e : w.enemies)
        if (e.hp > 0 && dist(e.pos, w.pos) <= e.range + radius) bad.draw_chebyshev(e.pos, static_cast<float>(e.range + 1));
    goals.draw_chebyshev(w.pos, static_cast<float>(radius));
    goals = goals & ~bad & ~st.terrain & st.reach;
    // No avoid set: the way out may cross the unsafe tiles we are standing among.
    bool moved = !goals.is_empty() && nav().stepToward(w, goals, true, nullptr, st.spawnCenter);
    if (!moved) {
        std::vector<bc::vec2> threats = hitters;
        for (const auto& c : camps)
            if (dist(c.pos, w.pos) < c.range + margin) threats.push_back(c.pos);
        moved = !threats.empty() && nav().stepAway(w, threats, tune::monster_range + margin);
    }
    // Cornered (no clear tile, no step away): never channel in a camp's reach (version 2's rule; with
    // "recall anyway" here 6 of 19 deaths after a recall still started inside the killer's range).
    // Walk home instead.
    if (!moved && tune::recall_never_in_reach) moved = nav().stepToward(w, st.allySpawn, true, nullptr, st.spawnCenter);
    if (moved) {
        trace().note(hitters.empty() ? "recall: to a tile clear of the camps" : "recall: away from what hit us first");
        combat::act(w, combat::Focus{.noCamps = true}, false);
        return false;
    }
    return !tune::recall_never_in_reach;
}

void bank(World& w, int altarIdx) {
    auto& me = bc::self();
    const auto altar = Statics::get().altars[static_cast<std::size_t>(altarIdx)];
    // A tribute is a stationary channel only a stun or a kill cancels (damage does not): never start
    // one an enemy stunner can reach in time, or that the enemies about could kill us during.
    // Judged at the altar (not at our tile) so the verdict does not flip as we step toward it.
    const bool unsafe = w.tributeAtRisk(altar, w.gems);
    // GGEzBot: an awake camp covers the altar and we are hurt: wait outside its reach for the camp
    // to sleep or die (or us to be healthier) instead of tributing under fire.
    if (dist(w.pos, altar) > 1 && w.hpFrac() < tune::altar_fire_wait_hp && scorers::campFireAt(w, altar) > 0) {
        if (!recallClear(w) && stepClearOfCamps(w)) {}
        combat::act(w, combat::Focus{.awakeCampsOnly = true}, false);
        trace().note("altar under a camp: waiting outside its reach");
        return;
    }
    // Fix 4 (stalls): holding at an unsafe altar is short (the_farm: Vesper held 155 rounds with 22
    // gems). After bank_hold_cap rounds the altar is avoided for a while: another altar, or recall.
    w.bankHoldTurns = unsafe ? w.bankHoldTurns + 1 : 0;
    if (unsafe && tune::bank_hold_cap > 0 && w.bankHoldTurns > tune::bank_hold_cap && altarIdx >= 0 && altarIdx < 16) {
        w.altarAvoidUntil[static_cast<std::size_t>(altarIdx)] = w.round + tune::altar_avoid_rounds;
        w.bankHoldTurns = 0;
        w.forceReplan = true;
        trace().note("unsafe altar too long: trying elsewhere");
    }
    if (unsafe) {
        // Keep our distance instead of walking up and backing off every other turn.
        trace().note("unsafe to tribute: holding");
        if (dist(w.pos, altar) <= 1 || w.enemyPressure(w.pos, 3) > 0) {
            if (!nav().stepEscape(w)) nav().stepToward(w, Statics::get().allySpawn, true);
        }
        combat::act(w, combat::Focus{.awakeCampsOnly = true}, false);  // never wake a sleeping camp (patch: monoliths reach 5)
        return;
    }
    // Recall first when it is clearly quicker than walking, and nothing can cancel it (damage does).
    if (tune::bank_by_recall && !w.atSpawn && dist(w.pos, altar) > 1 && w.incoming <= 0 && !w.beingHit &&
        w.enemyPressure(w.pos, tune::enemy_watch_radius) <= 0 && recallClear(w)) {
        const int walk = fields::toAltar(altarIdx, w.pos);
        if (walk > 0 && scorers::recallBankTurns(w, altarIdx) + tune::bank_recall_margin < w.walkTurns(static_cast<float>(walk))) {
            offTeammateCapture(w);
            try {
                me.recall();
                trace().action("rc");
                trace().note("recall to bank");
                return;
            } catch (const bc::error& e) {
                trace().note(std::string("recall failed: ") + e.message());
            }
        }
    }
    auto avoid = w.danger;
    if (w.gems >= tune::bag_avoid_gems) avoid |= w.enemyReach();
    if (dist(w.pos, altar) > 1)
        nav().stepToward(w, nav().standTiles(w, altar, 1), dist(w.pos, altar) > 3, &avoid, altar,
                         FarField{&fields::toAltar, &fields::ensureAltar, altarIdx});
    if (dist(w.pos, altar) <= 1) {
        try {
            me.submit_gems(altar);
            FixedStr<24> msg;
            trace().action((msg << 't' << w.gems).view());
            return;
        } catch (const bc::error& e) {
            trace().note(std::string("tribute failed: ") + e.message());
        }
    }
    combat::act(w, combat::Focus{.awakeCampsOnly = true}, false);  // never wake a sleeping camp (patch: monoliths reach 5)
}

std::vector<bc::vec2> nearbyThreats(const World& w) {
    std::vector<bc::vec2> threats;
    for (const auto& c : w.camps)
        if (c.strength > 0 && dist(c.pos, w.pos) <= c.range + 1) threats.push_back(c.pos);
    for (const auto& e : w.enemies)
        if (dist(e.pos, w.pos) <= e.range + 3) threats.push_back(e.pos);
    return threats;
}

// Longest attack range among the monsters nearbyThreats() counts (patch: elk 4, monolith 5).
int nearbyCampRange(const World& w) {
    int r = tune::monster_range;
    for (const auto& c : w.camps)
        if (c.strength > 0 && dist(c.pos, w.pos) <= c.range + 1) r = std::max(r, c.range);
    return r;
}

// Where to retreat: the nearest ally (seen, or the camp a teammate claims) that is further from
// the threat than we are and within tune::retreat_ally_radius, so the chaser meets two of us.
// Spawn (-1,-1) otherwise.
bc::vec2 retreatTarget(const World& w) {
    const auto& s = Statics::get();
    bc::vec2 threat{-1, -1};
    int threatD = 1 << 20;
    for (const auto& e : w.enemies)
        if (e.hp > 0 && dist(e.pos, w.pos) < threatD) { threatD = dist(e.pos, w.pos); threat = e.pos; }
    if (threat.x < 0) return {-1, -1};
    bc::vec2 best{-1, -1};
    int bestD = tune::retreat_ally_radius + 1;
    auto consider = [&](bc::vec2 p) {
        const int d = dist(p, w.pos);
        if (d >= 2 && d < bestD && dist(p, threat) > threatD) { bestD = d; best = p; }
    };
    for (const auto& a : w.allies) consider(a.pos);
    for (int sl = 0; sl < w.allyCount && sl < comms::alert_byte; ++sl) {
        const int g = sl == w.slot ? -1 : comms::claimOf(sl);
        if (g >= 0 && g < static_cast<int>(s.groups.size())) consider(s.groups[static_cast<std::size_t>(g)].center);
    }
    return best;
}

// combatBotv2: before a recall, step off the tiles of a dead camp a teammate is capturing. The
// recall's 5 turns match the capture's, so a champion recalling on the tiles is still there when it
// completes and takes half the reward (rows: Ryth, the tank, recalled on the wolves' tiles and
// split the capture with Cassi, the keeper). Moves one tile; the recall can follow this turn.
void offTeammateCapture(World& w) {
    if (!tune::recall_off_capture) return;
    const auto* g = Statics::get().groupAt(w.pos);
    if (!g || w.groups[static_cast<std::size_t>(g->id)].status != GroupStatus::reward || !scorers::allyHoldingOf(w, g->id)) return;
    bc::bitboard ring;
    ring.clear();
    ring.draw_chebyshev(w.pos, 1.0f);
    const auto goals = ring & ~g->tileMask & ~nav().blocked(w);
    if (goals.is_empty()) return;
    if (nav().stepToward(w, goals, false, nullptr, Statics::get().spawnCenter)) trace().note("off a teammate's capture before recalling");
}

void heal(World& w) {
    auto& me = bc::self();
    if (w.atSpawn) {
        trace().action("rest");
        return;
    }
    const int walk = nav().distTo(w, Statics::get().spawnCenter);
    const auto avoid = w.danger | w.enemyReach();
    if (walk <= tune::recall_turns) {  // walking home is no slower than the recall channel
        nav().stepToward(w, Statics::get().allySpawn, true, &avoid, Statics::get().spawnCenter, spawnPath());
        return;
    }
    // Enemy champions do chase: a 5-turn recall in their sight just gets interrupted, so walk
    // home along a route around their reach. One consistent move: previously "escape their
    // reach" and "walk home" alternated whenever an enemy stood between us and home, and the
    // champion bounced in place next to it (Varg on packs).
    bool enemyClose = false;
    for (const auto& g : w.ghosts)
        if (w.round - g.round <= 1 && dist(g.pos, w.pos) <= g.range + 5) enemyClose = true;
    // yBot: monsters hitting us and no enemy champion in reach of us: out of the monsters' reach
    // first (Ren on eight kept walking along a woken elk camp toward an ally and died to it).
    bool champHitting = false;
    for (const auto& e : w.enemies) champHitting = champHitting || (e.hp > 0 && dist(e.pos, w.pos) <= e.range + 1);
    if (enemyClose && w.incoming > 0 && !champHitting && nav().escapeTurns(w) > 0) {
        if (!nav().stepEscape(w)) nav().stepAway(w, nearbyThreats(w), nearbyCampRange(w));
        combat::act(w, combat::Focus{.noCamps = true}, false);
        trace().note("monsters hitting: out of their reach first");
        return;
    }
    // GGEzBot: an awake camp's reach beside us is a wall while hurt, not a soft avoid: the ally and
    // home walks fell through to a plain route whenever the safe route failed, and the champion
    // stepped back under the camp every other turn (concentrated wealth: Ryth, 20 rounds, dead).
    if (enemyClose && !(w.stuckTurns > 0 && !w.beingHit) && tune::hurt_walk_camp_wall) {
        bc::bitboard near;
        near.clear();
        near.draw_chebyshev(w.pos, 2.0f);
        if (!(w.danger & near).is_empty()) {
            if (nav().stepHome(w)) {
                combat::act(w, combat::Focus{.noCamps = true}, false);
                trace().note("enemy close, camp beside us: home around both");
                return;
            }
        }
    }
    if (enemyClose && !(w.stuckTurns > 0 && !w.beingHit)) {
        // Toward an ally first when one is near and away from the threat: the chaser meets two of us.
        if (const auto ally = retreatTarget(w); ally.x >= 0 && nav().stepToward(w, ally, 1, true, &avoid)) {
            trace().note("enemy close: retreating to an ally");
            combat::act(w, combat::Focus{.noCamps = true}, false);
            return;
        }
        trace().note("enemy close: walking home around it");
        nav().stepToward(w, Statics::get().allySpawn, true, &avoid, Statics::get().spawnCenter, spawnPath());
        combat::act(w, combat::Focus{.noCamps = true}, false);
        return;
    }
    // Monsters do not chase: get out of their reach first, then recall (damage would cancel it).
    const int leftGroup = w.engagedGroup;
    if (w.incoming > 0 && nav().escapeTurns(w) > 0) {
        if (!nav().stepEscape(w)) nav().stepAway(w, nearbyThreats(w), nearbyCampRange(w));
        if (nav().escapeTurns(w) > 0) return;
        // An ally still beside the camp takes over the hits once we are out of reach: look again
        // next turn (rejoining may beat the trip home) instead of committing to a 5-turn recall.
        if (w.allyTanking(leftGroup)) {
            trace().note("ally tanking: rethinking before recall");
            combat::act(w, combat::Focus{.noCamps = true}, false);
            return;
        }
    }
    // Clear of every camp's reach before the channel starts, whatever escapeTurns() said.
    if (!clearToRecall(w, false)) return;
    offTeammateCapture(w);
    try {
        me.recall();
        trace().action("rc");
    } catch (const bc::error& e) {
        trace().note(std::string("recall failed: ") + e.message());
        nav().stepToward(w, Statics::get().allySpawn, true);
    }
}

void flee(World& w) {
    std::vector<bc::vec2> threats;
    for (const auto& e : w.enemies) threats.push_back(e.pos);
    // Toward an ally away from the threat, around enemy reach; else out of reach, else home.
    if (const auto ally = w.round < w.fleeHomeUntil ? bc::vec2{-1, -1} : retreatTarget(w); ally.x >= 0) {
        const auto avoid = w.enemyReach();
        if (nav().stepToward(w, ally, 1, true, &avoid)) {
            trace().note("fleeing to an ally");
            if (!tune::flee_no_attack) combat::act(w, combat::Focus{.noCamps = true}, false);
            return;
        }
    }
    // Run home around their reach (43% of our deaths came while fleeing, a third of those flights
    // drifting away from home: the nearest tile out of reach is often deeper in their half).
    if (tune::flee_home_weighted && nav().stepHome(w)) {
        if (!tune::flee_no_attack) combat::act(w, combat::Focus{.noCamps = true}, false);
        return;
    }
    // Ping-pong fix 5: home weighting judged our bush the best tile. The escape below then stepped
    // out of it and the next turn's home step went back in (eight r1562: 20,22 <-> 19,21). Stay hidden.
    if (tune::flee_bush_hold && tune::flee_home_weighted && nav().homeHeld()) {
        trace().note("fleeing: staying hidden in the bush");
        if (!tune::flee_no_attack) combat::act(w, combat::Focus{.noCamps = true}, false);
        return;
    }
    // Nothing to escape out of: head home along the spawn path rather than just away from them.
    if (!nav().stepEscape(w) && !(tune::flee_homeward ? false : nav().stepAway(w, threats, 6)))
        nav().stepToward(w, Statics::get().allySpawn, true, nullptr, Statics::get().spawnCenter, spawnPath());
    if (!tune::flee_no_attack) combat::act(w, combat::Focus{.noCamps = true}, false);
}

// Melee champion types (attack range 1), from the designs: they form the front line.
bool meleeType(bc::champion_type t) {
    static std::array<std::int8_t, 16> cache{};
    const auto i = static_cast<std::size_t>(t);
    if (i >= cache.size() || i >= bc::champion_designs.size()) return false;  // types newer than the SDK (Heart of Ruin)
    if (cache[i] == 0) cache[i] = bc::champion_designs[i].base_stats().base_attack_range <= 1 ? 1 : 2;
    return cache[i] == 1;
}

constexpr int kNoFront = 1 << 20;

int nearestEnemy(const World& w, bc::vec2 t) {
    int d = kNoFront;
    for (const auto& e : w.enemies)
        if (e.hp > 0) d = std::min(d, dist(t, e.pos));
    return d;
}

// Our front line around `at`: how near the enemy our nearest melee ally there stands, less the
// slack we may stand ahead of it (kNoFront: no melee ally there, or the rule is off).
int frontLine(const World& w, bc::vec2 at) {
    if (!tune::ranged_front_line) return kNoFront;
    int front = kNoFront;
    for (int s = 0; s < comms::alert_byte && s < w.allyCount; ++s) {
        if (s == w.slot || !meleeType(w.allyTypes[static_cast<std::size_t>(s)])) continue;
        const auto p = w.allyPosCached(s);
        if (p.x < 0 || dist(p, at) > tune::ranged_front_radius) continue;
        front = std::min(front, nearestEnemy(w, p));
    }
    const int slack = tune::pair_lives && w.pairChampion() ? tune::pair_front_slack : tune::ranged_front_slack;
    return front < kNoFront ? std::max(0, front - slack) : kNoFront;
}

// rangeBot: the kite step. Melee champions walk and attack in one turn, ranged ones choose, so a
// ranged champion trading alone with a melee loses: with a melee ally beside us we keep shooting
// (the team punishes it), alone we step away from it toward our nearest melee ally, or home. From
// distance 2 a step back keeps us out of its reach; adjacent, it hits us either way, so we drag it
// to our team. Returns true if we stepped.
bool rangedKite(World& w) {
    if (!w.ranged || !tune::ranged_kite) return false;
    std::vector<bc::vec2> melee;
    int nearestMelee = kNoFront;
    for (const auto& e : w.enemies)
        if (e.hp > 0 && e.range <= 1) {
            melee.push_back(e.pos);
            nearestMelee = std::min(nearestMelee, dist(e.pos, w.pos));
        }
    if (nearestMelee > tune::ranged_kite_from) return false;
    // Not alone: a melee ally beside us.
    bc::vec2 refuge = Statics::get().spawnCenter;
    int refugeD = kNoFront;
    for (int s = 0; s < comms::alert_byte && s < w.allyCount; ++s) {
        if (s == w.slot || !meleeType(w.allyTypes[static_cast<std::size_t>(s)])) continue;
        const auto p = w.allyPosCached(s);
        if (p.x < 0) continue;
        if (dist(p, w.pos) <= tune::ranged_kite_ally_radius) return false;
        if (dist(p, w.pos) < refugeD) { refugeD = dist(p, w.pos); refuge = p; }
    }
    // The neighbour farthest from their melee (up to 3), then nearest the refuge.
    const auto blocked = nav().blocked(w);
    bc::vec2 best{-1, -1};
    int bestSafe = -1, bestRefuge = kNoFront;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) continue;
            const bc::vec2 t{w.pos.x + dx, w.pos.y + dy};
            const auto& s = Statics::get();
            if (t.x < 0 || t.y < 0 || t.x >= s.width || t.y >= s.height || blocked.is_set(t)) continue;
            int safe = 3;
            for (auto m : melee) safe = std::min(safe, dist(m, t));
            const int r = dist(t, refuge);
            if (safe > bestSafe || (safe == bestSafe && r < bestRefuge)) { bestSafe = safe; bestRefuge = r; best = t; }
        }
    // Adjacent and nowhere better than adjacent: still step toward the refuge (drag it to us).
    if (best.x < 0 || (bestSafe <= 1 && nearestMelee > 1)) return false;
    bc::bitboard goal;
    goal.clear();
    goal.set(best, true);
    if (!nav().stepToward(w, goal, false)) return false;
    combat::act(w, combat::Focus{.anyEnemy = true}, false);
    trace().note(nearestMelee <= 1 ? "ranged: kite to team" : "ranged: kite back");
    return true;
}

// combatBotv2: bodyguard. A melee champion steps between a squishy (ranged) ally and an enemy
// champion closing on it: onto the tile beside the ally on the enemy's side, so the enemy meets the
// tank first. Melee champions move and attack in one turn, so it still hits whatever it reaches.
bool bodyguard(World& w, const Option& o) {
    if (!tune::bodyguard || w.ranged) return false;
    // Working a camp (monsters on us, or the camp in reach): keep farming (guarding from a camp
    // cost stampede, pilgrimage and rows their farming).
    if (tune::guard_not_while_farming && o.kind == OptionKind::farm && o.target >= 0 &&
        (w.engagedGroup >= 0 || groupMemberInRange(w, o.target)))
        return false;
    const EnemySeen* threat = nullptr;
    bc::vec2 ward{-1, -1};
    int bestD = kNoFront;
    for (int s = 0; s < comms::alert_byte && s < w.allyCount; ++s) {
        if (s == w.slot || meleeType(w.allyTypes[static_cast<std::size_t>(s)])) continue;  // squishies only
        const auto p = w.allyPosCached(s);
        if (p.x < 0 || dist(p, w.pos) > tune::guard_ally_radius) continue;
        for (const auto& e : w.enemies) {
            if (e.hp <= 0 || dist(e.pos, p) > tune::guard_threat_radius) continue;
            // Only a melee enemy right on the ally and closing on it (any enemy within 4 pulled our
            // tanks off their camps into fights: -115, more of both our ranged and melee died).
            if (tune::guard_melee_closing && (e.range > 1 || !w.enemyClosingOn(e.index, e.pos, p))) continue;
            const int d = dist(e.pos, p);
            if (d < bestD) { bestD = d; threat = &e; ward = p; }
        }
    }
    if (!threat) return false;
    // Already between them: beside the ally and nearer the enemy than it is.
    if (dist(w.pos, ward) <= 2 && dist(w.pos, threat->pos) < dist(ward, threat->pos)) return false;
    const auto& s = Statics::get();
    const auto blocked = nav().blocked(w);
    bc::vec2 best{-1, -1};
    int bestScore = kNoFront;
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx) {
            const bc::vec2 q{ward.x + dx, ward.y + dy};
            if (q.x < 0 || q.y < 0 || q.x >= s.width || q.y >= s.height) continue;
            if (q != w.pos && blocked.is_set(q)) continue;
            if (dist(q, threat->pos) >= dist(ward, threat->pos)) continue;  // must be on the enemy's side
            const int score = dist(q, threat->pos) * 2 + dist(q, ward);   // close to the line, near the ally
            if (score < bestScore) { bestScore = score; best = q; }
        }
    if (best.x < 0) return false;
    bc::bitboard goal;
    goal.clear();
    goal.set(best, true);
    nav().stepToward(w, goal, true);
    combat::Focus focus{.enemy = threat->handle};
    focus.anyEnemy = true;
    combat::act(w, focus, false);
    trace().note("bodyguard: between ally and enemy");
    return true;
}

// combatBotv2: team defence. An enemy champion that can hit one of our allies and that we can hit
// this turn gets hit (ranged: in range; melee: adjacent, or one step). In the Fish games our allies
// near a dying champion were idle (39%) or hitting monsters (31%); nobody hit its attackers in 71%.
bool defendAlly(World& w) {
    if (!tune::defend_ally || w.hpFrac() < tune::defend_min_hp) return false;
    const auto& st = Statics::get();
    const EnemySeen* best = nullptr;
    for (const auto& e : w.enemies) {
        if (e.hp <= 0 || dist(e.pos, st.enemySpawnCenter) <= tune::enemy_spawn_radius) continue;
        const int d = dist(w.pos, e.pos);
        // In reach now, or a short walk from it (the allies near a dying champion stood 3-6 away).
        if (d > w.range + tune::defend_approach) continue;
        bool threatens = false;
        for (int s = 0; s < comms::alert_byte && s < w.allyCount && !threatens; ++s) {
            if (s == w.slot) continue;
            const auto p = w.allyPosCached(s);
            threatens = p.x >= 0 && dist(p, w.pos) <= tune::defend_ally_radius && dist(p, e.pos) <= e.range;
        }
        if (!threatens) continue;
        // Only where we at least match them (everyone near, not just fighters): joining fights we
        // were losing is what the fight-score version of this did.
        int ours = 1, theirs = 0;
        for (int s = 0; s < comms::alert_byte && s < w.allyCount; ++s) {
            if (s == w.slot) continue;
            const auto p = w.allyPosCached(s);
            ours += p.x >= 0 && dist(p, e.pos) <= tune::favour_radius;
        }
        for (const auto& x : w.enemies) theirs += x.hp > 0 && dist(x.pos, e.pos) <= tune::favour_radius;
        if (ours < theirs) continue;
        if (!best || e.hp < best->hp) best = &e;
    }
    if (!best) return false;
    comms::claimEnemy(w.slot, best->index);
    combat::Focus focus{.enemy = best->handle};
    if (dist(w.pos, best->pos) > w.range) {
        // Walk into reach; melee may still hit this turn, ranged shoots from next turn.
        nav().stepToward(w, best->pos, w.range, !w.ranged);
        combat::act(w, focus, false);
    } else {
        combat::act(w, focus, true);
    }
    trace().note("defending an ally");
    return true;
}

// rangeBot: the hold and front rules outside fights (farm walks, regroups, hunts, idling): an
// enemy champion in range is shot rather than stepping on, and ahead of our melee front we step
// back behind it. Returns true if the turn is spent.
bool rangedHold(World& w) {
    if (!w.ranged || !tune::ranged_hold_always) return false;
    // xBot: enemies close: the tile-scoring micro decides (or leaves the turn to the plan's walk).
    if (micro::applies(w)) return micro::run(w, micro::Request{.intent = micro::Intent::hold});
    if (rangedKite(w)) return true;
    bc::vec2 near{-1, -1};
    int nd = kNoFront;
    for (const auto& e : w.enemies)
        if (e.hp > 0 && dist(e.pos, w.pos) < nd) { nd = dist(e.pos, w.pos); near = e.pos; }
    if (near.x < 0 || nd > (tune::front_enforce_radius > 0 ? std::max(w.range + 2, tune::front_enforce_radius) : w.range + 2)) return false;
    // combatBotv2: enemies about and no melee teammate near: fall back to the nearest one (Stellar
    // stood near enemies with no melee within 6 for 10-45% of contact on eight / interlock / fragment).
    if (tune::ranged_no_cover_fall_back && w.ranged) {
        bc::vec2 refuge = Statics::get().spawnCenter;
        int refugeD = kNoFront;
        bool covered = false;
        for (int s = 0; s < comms::alert_byte && s < w.allyCount && !covered; ++s) {
            if (s == w.slot || !meleeType(w.allyTypes[static_cast<std::size_t>(s)])) continue;
            const auto p = w.allyPosCached(s);
            if (p.x < 0) continue;
            if (dist(p, w.pos) <= tune::ranged_front_radius) covered = true;
            else if (dist(p, w.pos) < refugeD) { refugeD = dist(p, w.pos); refuge = p; }
        }
        if (!covered) {
            std::vector<bc::vec2> threats;
            for (const auto& e : w.enemies)
                if (e.hp > 0) threats.push_back(e.pos);
            if (!nav().stepToward(w, refuge, 1, true)) nav().stepAway(w, threats, 4);
            combat::act(w, combat::Focus{.anyEnemy = true}, false);
            trace().note("ranged: no melee near, falling back");
            return true;
        }
    }
    const int front = frontLine(w, near);
    if (front < kNoFront && nd < front) {
        std::vector<bc::vec2> threats;
        for (const auto& e : w.enemies)
            if (e.hp > 0) threats.push_back(e.pos);
        nav().stepAway(w, threats, front);
        combat::act(w, combat::Focus{.anyEnemy = true}, false);
        trace().note("ranged: back behind the front");
        return true;
    }
    if (tune::ranged_hold_range && nd <= w.range) {
        combat::act(w, combat::Focus{.anyEnemy = true}, true);
        trace().note("ranged: hold and shoot");
        return true;
    }
    return false;
}

// rangeBot: a ranged champion's fight. Ranged champions either move or attack in a turn, so a step
// is an attack lost: with any enemy champion in range, hit (the chosen one if it is in range, else
// the best one in range). Otherwise walk to a tile at full range from the target, out of reach of
// enemy melee where there is one, and never nearer the enemy than our melee front: when the tank
// is stunned and stops, we stop behind it (or step back) instead of walking into their team.
bool rangedFight(World& w, const Option& o) {
    if (!w.ranged || (!tune::ranged_hold_range && !tune::ranged_max_range && !tune::ranged_front_line)) return false;
    if (micro::applies(w)) return micro::run(w, micro::Request{.intent = micro::Intent::engage, .target = o.enemy, .at = o.at});
    if (rangedKite(w)) return true;
    combat::Focus focus{.enemy = o.enemy};
    bool anyInRange = false, chosenInRange = false;
    for (const auto& e : w.enemies)
        if (e.hp > 0 && dist(e.pos, w.pos) <= w.range) {
            anyInRange = true;
            chosenInRange = chosenInRange || (o.enemy && e.handle == *o.enemy);
        }
    const int front = frontLine(w, o.at);
    const bool aheadOfFront = front < kNoFront && nearestEnemy(w, w.pos) < front;
    // In range and not ahead of the front: hit.
    if (tune::ranged_hold_range && anyInRange && !aheadOfFront) {
        if (!chosenInRange) focus.anyEnemy = true;
        combat::act(w, focus, true);
        trace().note("ranged: hold range");
        return true;
    }
    if (chosenInRange && !aheadOfFront) return false;  // the usual executor hits it
    // combatBotv3: no melee ally near the fight means no front line, and we used to walk to full
    // range of the target alone (eight vs Ascendance r41-50: Cassi walked into their four, Issac 6-8
    // tiles back, and Tavi took her 540 -> 210). Do not walk in: fall back toward our nearest melee
    // ally (or home) and shoot whatever is in range on the way.
    if (tune::ranged_fight_needs_front && front >= kNoFront) {
        bc::vec2 refuge = Statics::get().spawnCenter;
        int refugeD = kNoFront;
        for (int s = 0; s < comms::alert_byte && s < w.allyCount; ++s) {
            if (s == w.slot || !meleeType(w.allyTypes[static_cast<std::size_t>(s)])) continue;
            const auto p = w.allyPosCached(s);
            if (p.x >= 0 && dist(p, w.pos) < refugeD) { refugeD = dist(p, w.pos); refuge = p; }
        }
        if (anyInRange) {
            if (!chosenInRange) focus.anyEnemy = true;
            combat::act(w, focus, true);
        } else {
            nav().stepToward(w, refuge, 1, true);
            combat::act(w, combat::Focus{.anyEnemy = true}, false);
        }
        trace().note("ranged fight: no melee front, not walking in");
        return true;
    }
    // Goal tiles: within range of the target, at full range if we can.
    bc::bitboard goals = nav().standTiles(w, o.at, w.range);
    if (tune::ranged_max_range) {
        bc::bitboard closeIn;
        closeIn.clear();
        closeIn.draw_chebyshev(o.at, static_cast<float>(w.range - 1));
        if (const auto full = goals & ~closeIn; !full.is_empty()) goals = full;
        // Out of enemy melee reach next turn (a step plus range 1).
        bc::bitboard reach;
        reach.clear();
        for (const auto& e : w.enemies)
            if (e.hp > 0 && e.range <= 1) reach.draw_chebyshev(e.pos, 2.0f);
        if (const auto safe = goals & ~reach; !safe.is_empty()) goals = safe;
    }
    if (front < kNoFront) {
        // No nearer the enemy than the front: tiles at least `front` from every enemy.
        bc::bitboard ahead;
        ahead.clear();
        if (front > 0)
            for (const auto& e : w.enemies)
                if (e.hp > 0) ahead.draw_chebyshev(e.pos, static_cast<float>(front - 1));
        goals = goals & ~ahead;
        if (goals.is_empty()) {
            // Nothing in range behind the front: hold, or step back behind it.
            if (aheadOfFront) {
                std::vector<bc::vec2> threats;
                for (const auto& e : w.enemies)
                    if (e.hp > 0) threats.push_back(e.pos);
                nav().stepAway(w, threats, front);
                trace().note("ranged: behind the front");
            } else {
                trace().note("ranged: hold behind the front");
            }
            combat::act(w, focus, false);
            return true;
        }
    }
    if (goals.is_set(w.pos)) {
        // combatBotv3: in range already: a clearly safer tile that is still in range first.
        if (tune::exposure_stand && nav().saferStep(w, goals)) {
            combat::act(w, focus, false);
            return true;
        }
        combat::act(w, focus, true);
        return true;
    }
    auto avoid = w.enemyReach();
    nav().stepToward(w, goals, true, &avoid, o.at);
    combat::act(w, focus, false);
    return true;
}

// combatBotv2: safe dash. A melee dasher only dashes into a fight if the tile it would land on is
// survivable and not ahead of our front (eight: Issac dashed onto their Stellar two tiles ahead of
// Ren with four more of them arriving, and was beaten from 865 to 410 while walking back).
bool safeDash(const World& w, bc::vec2 target) {
    if (!tune::safe_dash || w.kit.dashRange <= 0) return true;
    const auto sgn = [](int v) { return (v > 0) - (v < 0); };
    const int step = static_cast<int>(w.kit.dashRange);
    const bc::vec2 land{w.pos.x + sgn(target.x - w.pos.x) * step, w.pos.y + sgn(target.y - w.pos.y) * step};
    // (1) Would we be forced out (predicted retreat) standing there?
    float theirs = w.enemyPressure(land, tune::regroup_enemy_radius);
    if (w.type == bc::champion_type::issac) theirs *= 2.0f;  // Issac's death takes Cassi with it
    const float ours = w.alliedPower(land, tune::fight_beside_radius + 2);
    if (theirs > 0 && theirs > ours) {
        int beside = 1;
        for (int s = 0; s < comms::alert_byte && s < w.allyCount; ++s) {
            if (s == w.slot) continue;
            const auto p = w.allyPosCached(s);
            if (p.x >= 0 && dist(p, land) <= tune::fight_beside_radius) ++beside;
        }
        const float toDie = w.hp * static_cast<float>(beside) / theirs;
        const float need = static_cast<float>(dist(land, Statics::get().spawnCenter) > 0 ? 2 : 0) + tune::retreat_margin_turns;
        if (toDie < need + static_cast<float>(step)) return false;
    }
    // (2) Leash: no more than safe_dash_slack tiles nearer the enemy than our nearest other melee.
    int front = kNoFront;
    for (int s = 0; s < comms::alert_byte && s < w.allyCount; ++s) {
        if (s == w.slot || !meleeType(w.allyTypes[static_cast<std::size_t>(s)])) continue;
        const auto p = w.allyPosCached(s);
        if (p.x < 0 || dist(p, target) > tune::ranged_front_radius + 2) continue;
        front = std::min(front, nearestEnemy(w, p));
    }
    if (front < kNoFront && nearestEnemy(w, land) < front - tune::safe_dash_slack) return false;
    return true;
}

// Team up before the fight: the enemies we see are nearer to us, on average, than our living
// allies are, and here they outnumber us. Step toward our allies (hitting whatever is in range)
// until they are on average no farther than the enemies, then fight together.
bool teamUp(World& w, const Option& o) {
    // Only with a bag worth protecting: empty-handed, we fight as before (team-up cost kills).
    if (!tune::team_up || w.gems < tune::team_up_min_gems) return false;
    float enemySum = 0;
    int enemies = 0, enemiesNear = 0;
    for (const auto& e : w.enemies) {
        if (e.hp <= 0) continue;
        enemySum += static_cast<float>(dist(e.pos, w.pos));
        ++enemies;
        if (dist(e.pos, w.pos) <= tune::team_up_radius) ++enemiesNear;
    }
    if (enemies == 0) return false;
    float allySum = 0, cx = 0, cy = 0;
    int allies = 0, alliesNear = 0;
    for (int s = 0; s < w.allyCount; ++s) {
        if (s == w.slot) continue;
        const auto p = w.allyPosCached(s);
        if (p.x < 0 || dist(p, w.pos) > tune::team_up_ally_radius) continue;  // too far to team up with
        allySum += static_cast<float>(dist(p, w.pos));
        cx += static_cast<float>(p.x);
        cy += static_cast<float>(p.y);
        ++allies;
        if (dist(p, w.pos) <= tune::team_up_radius) ++alliesNear;
    }
    if (allies == 0) return false;  // nobody near enough to team up with: fight or flee as before
    const float enemyAvg = enemySum / static_cast<float>(enemies), allyAvg = allySum / static_cast<float>(allies);
    const bool outnumbered = enemiesNear > 1 + alliesNear;
    if (!outnumbered || enemyAvg >= allyAvg) return false;
    const bc::vec2 centre{static_cast<int>(cx / static_cast<float>(allies) + 0.5f), static_cast<int>(cy / static_cast<float>(allies) + 0.5f)};
    combat::Focus focus{.enemy = o.enemy, .anyEnemy = true};
    nav().stepToward(w, centre, 1, true);
    combat::act(w, focus, false);
    FixedStr<64> msg;
    trace().note((msg << "team up: " << enemiesNear << "v" << (1 + alliesNear) << ", enemies " << static_cast<int>(enemyAvg)
                      << " allies " << static_cast<int>(allyAvg)).view());
    return true;
}

void fight(World& w, const Option& o) {
    // yBot: cast first. A ready spell that forfeits movement and can hit an enemy champion from here
    // beats one step closer (we cast only 12-25% of the time a ready spell had a target in reach:
    // fights stepped first, and such spells are only cast before moving).
    if (tune::cast_first && combat::exclusiveSpellReady(w)) {
        combat::act(w, combat::Focus{.enemy = o.enemy}, true);
        if (!bc::self().can_cast() || !combat::exclusiveSpellReady(w)) return;  // cast: movement is spent this turn
    }
    if (teamUp(w, o)) return;
    if (rangedFight(w, o)) return;
    // xBot: melee in contact: the tile-scoring micro picks where we hit from.
    if (!w.ranged && micro::applies(w) &&
        micro::run(w, micro::Request{.intent = micro::Intent::engage, .target = o.enemy, .at = o.at}))
        return;
    combat::Focus focus{.enemy = o.enemy};
    if (dist(w.pos, o.at) <= w.range) {
        combat::act(w, focus, true);
    } else {
        const bool dashOk = safeDash(w, o.at);
        if (!dashOk) trace().note("unsafe dash: walking");
        nav().stepToward(w, o.at, w.range, dashOk);
        combat::act(w, focus, false);
    }
}

} // namespace

// combatBotv3: outmatched before contact. Visible enemies within forecast_radius are closing and
// more of them than of us (allies within forecast_ally_radius), and comparing whole sides (each
// side's health over the other's damage, monsters on us counting for them) we would clearly lose.
// Once retreating, it holds while we are still outmatched, contact or not (turning back to the camp
// is how Stellar was caught). Eight vs loopBot r31-37: she saw 3v4 then 3v5 closing and kept
// farming at (15,8) because the scored retreat (~700) lost to farming (1,000+).
int outmatchedCount = 0;  // enemies counted by the last outmatchedAhead that held
bool outmatchedAhead(World& w) {
    static int retreatingSince = -100, lastRound = -100;
    const bool wasRetreating = lastRound == w.round - 1 && w.round - retreatingSince <= tune::retreat_override_hold;
    const auto& st = Statics::get();
    float theirHp = 0, theirDps = 0;
    int theirCount = 0;
    bool closing = false, contact = false;
    // Enemy types by roster index (read once): their Cassi and Issac are one pick and move and die
    // together, so seeing one of them means both are coming.
    static std::array<std::int8_t, 16> pairOf{};  // 0 unknown, 1 Cassi, 2 Issac, 3 other
    int seenPair = 0;                             // bit 1 Cassi counted, bit 2 Issac counted
    float pairDps = 0;
    for (const auto& e : w.enemies) {
        if (e.hp <= 0 || dist(e.pos, w.pos) > tune::forecast_radius) continue;
        // At their spawn they heal and are not coming, unless they walk toward us (eight r305-308:
        // Cassi capturing near their spawn did not count their Cassi walking at her: 2v2, not 3v2).
        if (dist(e.pos, st.enemySpawnCenter) <= tune::enemy_spawn_radius &&
            !(tune::forecast_spawn_approachers && w.enemyApproaching(e.index, e.pos)))
            continue;
        const float dps = std::max(e.strength + e.focus * 1.5f / tune::spell_cooldown, 1.0f);
        theirHp += e.hp;
        theirDps += dps;
        ++theirCount;
        if (tune::forecast_pair && e.index >= 0 && e.index < 16) {
            auto& t = pairOf[static_cast<std::size_t>(e.index)];
            if (t == 0) {
                try {
                    const auto n = e.handle.champion_name();
                    t = n == bc::champion_type::cassi ? 1 : n == bc::champion_type::issac ? 2 : 3;
                } catch (const bc::error&) {
                    t = 3;
                }
            }
            if (t == 1 || t == 2) {
                seenPair |= t;
                pairDps = dps;
            }
        }
        closing = closing || w.enemyApproaching(e.index, e.pos);
        contact = contact || dist(e.pos, w.pos) <= e.range + tune::forecast_contact_slack;
    }
    // One of their Cassi & Issac counted, the other unseen: it is alive (they die together) and with
    // them, so count it too, at its last seen damage if we have it.
    if (tune::forecast_pair && (seenPair == 1 || seenPair == 2)) {
        const int want = seenPair == 1 ? 2 : 1;
        float dps = pairDps;
        for (const auto& g : w.ghosts)
            for (std::size_t i = 0; i < pairOf.size(); ++i)
                if (pairOf[i] == want && w.round - g.round <= tune::forecast_pair_ghost_age) {
                    try {
                        if (g.handle.champion_name() == (want == 1 ? bc::champion_type::cassi : bc::champion_type::issac)) dps = std::max(g.dps, 1.0f);
                    } catch (const bc::error&) {
                    }
                    break;
                }
        theirHp += tune::forecast_pair_hp;
        theirDps += dps;
        ++theirCount;
    }
    if (theirCount < tune::forecast_min_enemies || (!wasRetreating && (!closing || contact))) return false;
    const float myDps = std::max(combat::championDamagePerTurn(w), 1.0f);
    // Allies that can join in time count too (walking back from spawn or a nearby camp): retreat
    // only if we would lose even with them. Counting only those within 6, the team backed off from
    // every group near the middle and banked 22 gems by round 200 (eight vs loopBot).
    const auto ours = w.ourSide(w.pos, tune::forecast_support_radius, -1, myDps);
    if (theirCount <= ours.count) return false;
    const float campDps = w.incoming > 0 ? w.incoming : 0.0f;
    const float edge = (ours.hp / std::max(theirDps + campDps, 1.0f)) / std::max(theirHp / std::max(ours.dps, 1.0f), 0.1f);
    if (edge >= tune::forecast_edge) return false;
    if (!wasRetreating) retreatingSince = w.round;
    lastRound = w.round;
    outmatchedCount = theirCount;
    FixedStr<64> msg;
    trace().note((msg << "outmatched " << ours.count << "v" << theirCount << " edge x10 " << static_cast<int>(edge * 10.0f) << ": retreating").view());
    return true;
}

void execute(World& w, const Option& o) {
    const auto startPos = bc::self().position();
    // combatBotv2: outnumbered where we stand, a dasher keeps its dash for escaping (eight: Issac
    // dashed in, then had none left to get out while four of them beat him down).
    w.reserveDash = tune::dash_reserve && w.kit.dashRange > 0 && o.kind != OptionKind::flee && o.kind != OptionKind::heal &&
                    !w.enemies.empty() && w.enemyPressure(w.pos, tune::regroup_enemy_radius) > w.alliedPower(w.pos, tune::fight_beside_radius + 2);
    if (w.reserveDash) trace().note("outnumbered: keeping the dash");
    // Whatever we are doing, cancelling an enemy tribute or recall is worth a stun.
    combat::interruptChannels(w);
    combat::useAltarLock(w);
    const auto& statics = Statics::get();
    const bool farming = o.kind == OptionKind::farm || o.kind == OptionKind::capture;
    if (o.kind == OptionKind::fight) comms::claimEnemy(w.slot, o.target);
    else if (o.kind == OptionKind::wait && o.target == 2) comms::claimRegroup(w.slot);
    else if (farming) {
        // Distance to the group's nearest tile, not its centre: on a big camp the party's waiting
        // ring is 10+ tiles from the centre, so nobody there counted as near and the party never
        // formed (stampede: the whole team stood on the ring from round 60 to 1,950).
        const auto& g = statics.groups[static_cast<std::size_t>(o.target)];
        int d = dist(w.pos, g.center);
        for (const auto t : g.tiles) d = std::min(d, dist(w.pos, t));
        comms::claimGroup(w.slot, o.target, d);
    }
    else if (o.kind == OptionKind::flee && (tune::retreat_follow || tune::retreat_hold || tune::ranged_follow_front || tune::team_fight_call)) comms::claimRetreat(w.slot);
    else comms::clear(w.slot);

    // combatBotv3: outmatched by a group closing on us: leave before contact, whatever we were doing
    // (not while healing, fleeing or already tributing/recalling: those move us away anyway).
    if (tune::retreat_override && o.kind != OptionKind::flee && o.kind != OptionKind::heal && outmatchedAhead(w)) {
        comms::claimRetreat(w.slot);
        // combatBotv3: rally. Call the team to a point a few tiles back toward where we fall back,
        // fixed for the retreat, with how many are coming: healthy allies gather there instead of
        // farming on (eight: we fell back one by one while their team pushed on and caught each of
        // us), and once enough of us are near, we stop falling back (outmatchedAhead counts them).
        if (tune::rally_alert && w.allyCount <= comms::alert_byte) {
            static int rallyRound = -100;
            static bc::vec2 rallyAt{-1, -1};
            if (w.round - rallyRound > 1 || rallyAt.x < 0) {
                auto to = retreatTarget(w);
                if (to.x < 0) to = Statics::get().spawnCenter;
                const int d = dist(w.pos, to);
                const int k = std::min(d, tune::rally_back);
                rallyAt = d > 0 ? bc::vec2{w.pos.x + (to.x - w.pos.x) * k / d, w.pos.y + (to.y - w.pos.y) * k / d} : w.pos;
            }
            rallyRound = w.round;
            comms::claimRally(w.slot, outmatchedCount);
            comms::raiseAlert(rallyAt.x, rallyAt.y, outmatchedCount, true, w.round);
        }
        // Before contact a plain step toward the ally we fall back to (or home) is enough: flee()
        // plans around enemy reach first (two path searches a turn: the retreats cost ~100 turns a game).
        if (tune::retreat_cheap_step) {
            auto to = retreatTarget(w);
            if (to.x < 0) to = Statics::get().spawnCenter;
            if (!nav().stepToward(w, to, 1, true)) nav().stepHome(w);
            combat::act(w, combat::Focus{.anyEnemy = true, .noCamps = true}, false);
            trace().note("outmatched: falling back");
        } else {
            flee(w);
        }
        return;
    }
    // combatBotv3: peel back. Burst down fast (peel_drop of max health in 2 rounds) with an enemy
    // champion close: step toward the nearest ally further from the threat, so the attackers have
    // to walk into our team to finish us, and still hit what is in reach.
    if (tune::peel_back && o.kind != OptionKind::flee && o.kind != OptionKind::heal) {
        static std::array<float, 3> hpAgo{};  // health 1, 2 rounds ago
        static int hpRound = -100;
        if (w.round - hpRound != 1) hpAgo = {w.hp, w.hp, w.hp};
        hpRound = w.round;
        const float drop = hpAgo[1] - w.hp;
        hpAgo[1] = hpAgo[0];
        hpAgo[0] = w.hp;
        const float limit = isTank(w.type) ? tune::peel_tank_hp : tune::peel_max_hp;
        bool close = false;
        for (const auto& e : w.enemies) close = close || (e.hp > 0 && dist(e.pos, w.pos) <= tune::peel_enemy_radius);
        if (close && w.alive && drop >= tune::peel_drop * w.maxHp && w.hpFrac() < limit) {
            const auto to = retreatTarget(w);
            if (to.x >= 0 && nav().stepToward(w, to, 1, true)) {
                combat::act(w, combat::Focus{.anyEnemy = true, .noCamps = true}, false);
                FixedStr<48> msg;
                trace().note((msg << "peel back: lost " << static_cast<int>(drop) << " in 2 rounds").view());
                return;
            }
        }
    }
    // rangeBot: an enemy in range or our front behind us outranks the walk (not captures,
    // banking, healing or fleeing: those have their own reasons to be where they are).
    const bool held = (o.kind == OptionKind::farm || o.kind == OptionKind::wait) && defendAlly(w)
                      ? true
                      : (o.kind == OptionKind::farm || o.kind == OptionKind::wait || o.kind == OptionKind::fight) && bodyguard(w, o)
                      ? true
                      : (o.kind == OptionKind::farm || o.kind == OptionKind::wait) && rangedHold(w);
    if (!held) switch (o.kind) {
    case OptionKind::farm:
    case OptionKind::capture: {
        const auto& g = statics.groups[static_cast<std::size_t>(o.target)];
        // A sealed group: dash into its pocket first.
        if (!g.reachable && g.dashIn && !g.inPocket(w.pos)) {
            nav().enterPocket(w, g);
            combat::act(w, combat::Focus{.groupId = g.id}, false);
        } else if (o.kind == OptionKind::farm) {
            w.spellClearGroup = o.spellClear ? g.id : -1;
            farmGroup(w, g, o.need);
            w.spellClearGroup = -1;
        } else {
            capture(w, g);
        }
        break;
    }
    case OptionKind::bank: bank(w, o.target); break;
    case OptionKind::heal: heal(w); break;
    case OptionKind::flee: flee(w); break;
    case OptionKind::fight: fight(w, o); break;
    case OptionKind::wait:
        if (o.target == 2) {  // regroup (o.at is our own tile when we stay with the group)
            if (o.at != w.pos) w.regroupWalkRound = w.round;
            nav().stepToward(w, o.at, tune::regroup_arrive - 1, true, &w.danger);
        }
        else if (o.target == 3) {                                                                // hunt (their spawn is not walkable)
            // Arrived where the enemy was last seen and nobody is here (or no path): the hunt is over,
            // think again next turn instead of standing (boss-battle: "hunt" with no action 30-80 rounds).
            if (!nav().stepToward(w, o.at, 5, true) && tune::hunt_ends_on_arrival) {
                w.huntUntil = w.round;
                w.forceReplan = true;
                trace().note("hunt: nothing here, rethinking");
            }
        }
        else if (!w.atSpawn) nav().stepToward(w, statics.allySpawn, false, nullptr, statics.spawnCenter, spawnPath());
        combat::act(w, combat::Focus{.noCamps = true}, false);
        break;
    }
    combat::support(w);
    // yBot free cast: we did not move this turn (capturing, at an altar, holding) and are not
    // channelling: a ready spell that forfeits movement costs nothing now, so cast it at an enemy
    // champion in reach (such spells were held while standing on capture tiles or at altars).
    if (tune::free_cast) {
        auto& me = bc::self();
        if (me.position() == startPos && !me.is_recalling() && !me.is_submitting_gems() && combat::exclusiveSpellReady(w))
            combat::act(w, combat::Focus{.anyEnemy = true, .noCamps = true}, true);
    }

    // Chaining: the camp we worked last turn is dead now (killed or captured): start the chain from it.
    if ((w.lastOption == static_cast<int>(OptionKind::farm) || w.lastOption == static_cast<int>(OptionKind::capture)) &&
        w.lastTarget >= 0 && w.lastTarget < static_cast<int>(w.groups.size()) && w.chainFrom != w.lastTarget) {
        const auto st = w.groups[static_cast<std::size_t>(w.lastTarget)].status;
        if (st == GroupStatus::reward || st == GroupStatus::respawning) {
            w.chainFrom = w.lastTarget;
            w.chainRound = w.round;
        }
    }
    if (w.lastOption == static_cast<int>(o.kind) && w.lastTarget == o.target) ++w.turnsOnOption;
    else w.turnsOnOption = 0;
    w.lastOption = static_cast<int>(o.kind);
    w.lastTarget = o.target;
}

} // namespace brain
