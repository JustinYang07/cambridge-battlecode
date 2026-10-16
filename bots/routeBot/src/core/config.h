#pragma once
// Tuning knobs in one place. Each constant is referenced by name in decision
// traces, so the lab's decision explorer shows which knob produced a score.

namespace tune {

// ---- economy: what a unit of reward is worth, in gem-equivalents
inline constexpr float gold_value_early = 0.20f;   // gems per gold before late game
inline constexpr float gold_value_late = 0.01f;    // gold is nearly worthless near the end
inline constexpr int late_game_round = 700;

// ---- the middle of the map (brain/farm.cpp): bonus by radius from the geometric centre
inline constexpr float centre_bonus_max = 1.3f;     // farm score at the centre, falling linearly to x1 in the corners
inline constexpr float centre_zone = 0.5f;          // the centre: radius below this (0 centre .. 1 corner) for the busy-centre test

// ---- party rotation: a party moves on together instead of waiting out its camp's respawn
inline constexpr bool party_rotation = true;        // A/B switch
inline constexpr int rotate_min_wait = 5;           // our party camp is down at least this long...
inline constexpr float rotate_bonus = 1.5f;         // ...the next free party camp (leader) / the mates' new camp (followers)

// ---- farming
inline constexpr float capture_turns = 5.0f;        // engine capture time
inline constexpr float reset_attack_bonus = 1.0f / 8.0f; // attack reset adds ~1 attack per cooldown
inline constexpr float max_fight_hp_use = 0.80f;    // never plan to spend more HP than this on a camp
inline constexpr float ally_claim_factor = 0.25f;   // a group a teammate claimed is worth this fraction
inline constexpr float engaged_bonus = 1.35f;       // finishing a group we already damaged is attractive
inline constexpr float defensive_stun_hp = 0.75f;  // pure stuns go on the monster hitting us below this health
inline constexpr float idle_rescue_score = 2.0f;   // best farm score below this: also score the nearest unscored groups
inline constexpr int idle_rescue_groups = 2;
inline constexpr float contested_camp_factor = 0.35f;  // an enemy champion was seen at the camp in the last 10 rounds
inline constexpr int race_end_gems = 850;          // enemy banked this many: bank what we carry now
inline constexpr float race_end_bank_boost = 6.0f;
inline constexpr int enemy_spawn_radius = 7;       // fights this close to the enemy spawn are not worth taking
inline constexpr float travel_weight = 3.0f;       // walking turns count this much in farm scores
inline constexpr int max_scored_groups = 6;        // fully score at most this many camp groups per turn
inline constexpr int max_team_per_camp = 4;
inline constexpr int max_party_wait_claimed = 10;  // ...also when allies have claimed it (was 30: 100-turn holds)
inline constexpr int wait_farm_radius = 6;         // while waiting, farm gem/gold clusters this close to the camp
inline constexpr int max_party_wait = 10;          // turns to wait for allies before giving up on a group         // largest party we plan around for one camp group
inline constexpr float party_focus_share = 0.7f;    // in a party, share of monster hits we expect to absorb
inline constexpr float escape_margin_turns = 1.5f;  // extra turns of incoming damage kept in reserve to walk away
inline constexpr float respawn_wait_weight = 2.0f;  // turns spent waiting for a respawn count double (was 1)

// ---- banking
inline constexpr float bank_target_gems = 60.0f;    // carrying this many makes banking compelling
inline constexpr float bank_min_gems = 1.0f;
inline constexpr int endgame_window = 300;          // last rounds where tribute timing against the round limit is checked
inline constexpr int stun_range = 4;                // enemy crowd control reaches this far (cancels tributes)
inline constexpr float carry_risk_per_gem = 0.004f; // farm scores shrink as the bag gets heavier
inline constexpr float danger_bank_boost = 2.0f;

// ---- health and recall
inline constexpr float heal_threshold = 0.35f;      // below this fraction, healing dominates
inline constexpr float recall_turns = 5.0f;          // engine recall channel
inline constexpr int monster_range = 3;

// ---- team fights (attackBot)
inline constexpr int fight_join_radius = 4;        // enemies this close to the target join its side
inline constexpr int fight_beside_radius = 3;      // allies this close to us fight (and draw fire) with us
inline constexpr float flee_horizon_turns = 10.0f;  // flee danger starts when we would die within this many turns
// Early retreat (ladder losses: retreats began at a median 393 HP, 70% died within 3 turns):
// flee while the enemies near us would need longer than our escape plus this margin to kill us.
inline constexpr bool predicted_retreat = false;   // -75 with it (retreats cost more than they saved)
inline constexpr float retreat_margin_turns = 4.0f;
inline constexpr int retreat_ally_radius = 0;        // retreat toward an ally this close (away from the threat), else spawn (0: off; -29 +/- 87 at 20)
inline constexpr float focus_fire_bonus = 2.0f;
inline constexpr int team_target_memory = 3;         // rounds the team target survives out of sight
inline constexpr float team_target_bonus = 2.0f;     // fight score for the enemy we saw take damage first    // an enemy allies are already attacking
inline constexpr float contested_capture_factor = 0.05f;  // capture with an enemy on the spawn tiles (progress resets)
inline constexpr float fight_hurt_bonus = 1.0f;    // fight score x(1 + this * enemy's missing health fraction)

// ---- navBot2 behaviour
inline constexpr float heal_start_frac = 1.0f;  // healing urgency grows from full health... (A/B: recalling only near 20% lost ~225 gems/game)
inline constexpr float heal_urgent_frac = 0.35f;  // ...and dominates by here
inline constexpr float heal_enemy_margin = 0.35f;   // both shift up this much with enemy champions about
inline constexpr float heal_first_factor = 0.5f;    // a camp that needs a trip home first
inline constexpr bool shop_trips = true;            // trips home to spend gold once we can afford shards (A/B switch)
inline constexpr int shop_min_shards = 3;            // ...this many affordable shards, or the rest of an emblem
inline constexpr float shop_shard_value = 8.0f;      // gem-equivalents per shard bought now
inline constexpr float shop_urgency_per_round = 0.02f;  // the trip's bonus grows this much per round the gold waits...
inline constexpr float shop_urgency_max = 3.0f;      // ...up to this
inline constexpr float focus_bonus_per_damage = 0.0f;  // extra commitment per damage dealt (A/B: 1.5 cost ~130 gems/game)
inline constexpr float abandon_factor = 0.7f;  // walking away from a fight we started
inline constexpr float safe_camp_bonus = 1.4f;      // gem/gold clusters: quick, free, no risk
inline constexpr int safe_camp_radius = 12;
inline constexpr float idle_wait_factor = 0.4f;     // standing over a camp that is still respawning (was 0.6)
inline constexpr int idle_wait_turns = 8;
inline constexpr float nearest_camp_bonus = 1.4f;    // the nearest camp that is up now (no respawn wait)
inline constexpr float underpowered_factor = 0.65f; // camps needing a party before we have any items
inline constexpr int underpowered_shards = 5;
inline constexpr float gold_value_opening = 0.2f;  // gems per gold in the opening (A/B: 0.45 cost ~110 gems/game)
inline constexpr int opening_rounds = 300;
// Opening gem rush (Sneaky Bush Campers bank their first gems ~r50 and 100 by ~r73; we took ~68 / ~115):
inline constexpr int early_gold_skip_round = 0;      // before this, gold-only camps are skipped unless right beside us
inline constexpr bool shop_after_first_tribute = false; // no shop trips before our first tribute
inline constexpr int early_bank_round = 150;         // before this, bank at early_bank_factor of the usual bag
inline constexpr float early_bank_factor = 1.0f;   // opening gem rush off: -207 +/- 95 vs v1.14 (0.5 tested)
inline constexpr float opening_gold_bonus = 1.0f;   // gold-only clusters in the opening (A/B: 1.8 cost ~125 gems/game, 3.0 neutral; off)

// ---- decision stability
inline constexpr float switch_hysteresis = 1.2f;    // last turn's decision gets this edge
inline constexpr int flip_lock_turns = 6;           // after A,B,A,B commit to the newest choice this long
inline constexpr float lock_emergency_hp = 0.3f;    // below this health a lock no longer binds
inline constexpr int party_commit_turns = 8;        // once a party is ready, engage for at least this long
inline constexpr float stuck_factor = 0.3f;         // a decision that left us bouncing between two tiles...
inline constexpr int stuck_turns = 8;               // ...is penalised this long

// ---- purchasing (A/B switches; see shop.cpp). Chosen 2026-10-14: treasure rush (+~210 gems/game
//      vs jungle-first), then monster; altar locks (+~115); stat matching hurt (-~75).
inline constexpr bool draft_mite_small = false;     // 3-pick drafts take Mite's roster (Ren, Stellar, Cassi & Issac)
inline constexpr int build_variant = 12;           // 0 default, 1 treasure rush, 2 treasure on Vesper, 3 monster first,
                                                 // 4 power early, 5 home early, 6 durability on carries,
                                                 // 7 treasure>monster, 8 treasure>home, 9 treasure>power,
                                                 // 10 monster>treasure, 11 jungle>power>durability (casters focus)
inline constexpr bool stat_matching = false;     // Ryth buys health (bonus hp -> strength), casters focus
inline constexpr bool lock_own_altars = true;  // also lock unlocked altars on our side as we pass (A/B switch)
inline constexpr bool buy_altar_locks = true;   // keep an altar lock: cancel enemy tributes, lock enemy-side altars
inline constexpr bool buy_tp_crystals = false;

// ---- staying alive around enemy champions
inline constexpr int enemy_memory_rounds = 10;      // remember enemy sightings this long
inline constexpr int enemy_watch_radius = 7;        // enemies this close count as pressure
inline constexpr float alone_bank_factor = 0.8f;    // solo champions bank at this fraction of their target
inline constexpr int bag_avoid_gems = 20;           // carrying this many gems, path around enemy reach

// ---- fighting enemy champions
inline constexpr float kill_gold_value = 50.0f;
inline constexpr float fight_our_dps_scale = 1.0f;  // our side's damage in fight estimates (measured real/predicted ~0.21)
inline constexpr float fight_win_margin = 1.25f;    // need this edge in time-to-kill to commit
inline constexpr int favour_radius = 6;             // bodies counted this close to the target (both sides)
inline constexpr float kill_denial_value = 12.0f;   // gem-equivalents of an enemy out for 60 rounds (favourable fights; was 20)
inline constexpr float favourable_max = 1.5f;       // favourable fight score at full health (x1 at half; was x2)
inline constexpr int their_half_fight_round = 150;  // from this round, fights against enemies on their half...
inline constexpr float their_half_fight_factor = 0.25f;  // ...score this (channelling targets and raids exempt)
inline constexpr float no_chase_factor = 1.0f;     // fights against an enemy out of reach and moving away (1: off)
inline constexpr int chase_radius = 6;              // enemies further than this are not chased...
inline constexpr float chase_factor = 0.3f;         // ...their fight score gets this (unless channelling)

// ---- engine-ish constants used for estimates
inline constexpr int spell_cooldown = 8;

// ---- fuel (100k per turn; overrunning suspends the bot and loses the next turn)
inline constexpr unsigned long long fuel_reserve_trace = 5000;       // skip all tracing below this
inline constexpr unsigned long long trace_fuel_per_byte = 36;          // stdout cost, measured ~33
inline constexpr unsigned long long fuel_reserve_full_trace = 40000; // skip the detailed trace below this
inline constexpr unsigned long long nav_safe_floor = 0;                // below this fuel, paths skip the avoid-danger search (0: always search)
inline constexpr unsigned long long think_floor = 60000;                  // keep last turn's plan when less fuel than this is left before thinking (0: off)
inline constexpr unsigned long long fuel_for_full_scoring = 75000;   // score max_scored_groups only above this
inline constexpr int bfs_radius = 48;
inline constexpr int replan_every = 8;                               // rescore at least this often
inline constexpr unsigned long long fuel_for_planning = 50000;      // advance the team plan only with this much left
inline constexpr float home_region_bonus = 1.0f;                     // camps in our squad's region
inline constexpr float other_region_factor = 1.0f;                   // camps in another squad's region
inline constexpr unsigned long long fuel_per_scored_group = 6000;
inline constexpr unsigned long long fuel_reserve_fields = 30000;     // build distance fields only above this (one costs <=12k)    // farm scoring cost per group
inline constexpr unsigned long long fresh_turn_fuel = 90000;         // a fresh turn starts with ~96.8k                                // distance-from-us search cut-off

// ---- routing (routeBot; see strategy/mapplan.h). Thresholds are shares and ratios, never
//      tile or camp counts, so they carry over to unseen maps.
inline constexpr float route_lambda = gold_value_opening;  // gold -> gems in route scores (attackBot2's tuned value)
inline constexpr float route_risk_weight = 0.3f;    // w_risk: score = value/turns - w_risk * risk
inline constexpr float route_max_hp_use = 0.8f;      // routes only include camps soloable within this much health
inline constexpr float route_bonus = 1.6f;           // farm score multiplier for the route's current camp
inline constexpr int route_max_round = 300;          // routes are the opening; the brain takes over after
inline constexpr float route_end_hp = 0.45f;         // below this health the route ends
inline constexpr int route_enemy_radius = 8;         // an enemy this close to the route camp ends the route
inline constexpr int route_lookahead = 3;            // candidate camps per lookahead step

// ---- farming loops (draft-planned cycles over camp groups; strategy/loop.h)
inline constexpr bool loops = false;                 // A/B switch (lost to v1.11: -201 +/- 122)
inline constexpr int loop_candidates = 18;           // camp groups the loop solver considers
inline constexpr float loop_dps_scale = 6.0f;        // real damage vs the profile's farmPower (wolf trio: ~14 turns solo)
inline constexpr float loop_party_overhead = 3.0f;   // turns a party loses walking in and spreading out
inline constexpr int loop_max_sweeps = 4;            // local search passes per number of loops
inline constexpr float loop_handoff_turns = 1.0f;    // capture cost per camp when one stays behind
inline constexpr float loop_heal_hp = 0.6f;          // health spent between trips home
inline constexpr float loop_split_factor = 0.9f;     // each extra loop's rates count this much less
inline constexpr float loop_near_side = 0.45f;       // share of the spawn-to-spawn walk: full weight up to here
inline constexpr float loop_far_side = 0.65f;        // ...down to loop_far_weight from here on
inline constexpr float loop_far_weight = 0.35f;
inline constexpr float loop_bonus = 2.5f;            // farm score multiplier for the loop's current camp
inline constexpr float loop_off_factor = 0.6f;       // camps not on our loop
inline constexpr int loop_max_wait = 8;              // a respawning loop camp is skipped if it is back later than this
inline constexpr int loop_skip_rounds = 40;          // a loop camp too strong for us now is skipped this long
inline constexpr unsigned long long draft_loop_floor = 190000;     // first draft turn: loop solving stops here
inline constexpr unsigned long long draft_neighbour_floor = 260000; // first draft turn: nearest-camp lists stop here
inline constexpr unsigned long long draft_neighbour_floor_late = 92000; // later picks: a little more, keeping planning fuel
inline constexpr unsigned long long draft_loop_floor_late = 86000; // later picks: continue solving down to here
inline constexpr float altar_time_weight = 1.0f;     // farm scores: walk to the altar, amortised over a bag
inline constexpr float centre_band = 0.2f;           // centre: |our dist - their dist| <= this x spawn-to-spawn
inline constexpr float centre_share = 0.4f;          // contest when the centre holds this share of income...
inline constexpr int centre_rules_needed = 2;        // ...and at least this many of the other 3 rules hold
inline constexpr unsigned long long draft_fuel_reserve = 350000;  // first draft turn: fields stop here (neighbour lists, routes, trace follow)
inline constexpr unsigned long long draft_plan_floor = 50000;     // below this, routes skip the lookahead
inline constexpr unsigned long long draft_prepare_floor = 150000; // first turn: neighbour lists stop here
inline constexpr unsigned long long draft_describe_floor = 45000; // below this, the draft trace skips the route text

// ---- raids: their team is down (all but tune::raid_max_alive bodies dead)
inline constexpr int raid_max_alive = 1;             // at most this many enemy bodies alive
inline constexpr int enemy_alive_every = 4;          // check enemy deaths every this many rounds (fuel)
inline constexpr float raid_cluster_factor = 3.0f;   // gem/gold clusters on their half (at full health)
inline constexpr float raid_camp_factor = 1.5f;      // monster camps on their half (at full health); x1 at half health
inline constexpr int raid_min_window = 25;           // whole enemy team dead with at least this many rounds to their first respawn...
inline constexpr float raid_own_half_factor = 0.4f;  // ...our half's camps get this (at full health): take theirs first

// ---- regrouping (strategy/regroup.h): an enemy on our half pulls nearby allies to the spotter
inline constexpr int regroup_radius = 10;            // allies this close to a sturdy spotter respond...
inline constexpr int regroup_radius_squishy = 25;    // ...this close to a squishy one (not a tank, or below...
inline constexpr float squishy_hp = 0.6f;            // ...this health)
// Behind on tributed gems: gems (camps and the bag) are worth up to 1 + catchup_max_boost, reached
// when the enemy has banked tune::catchup_full_deficit more than us.
inline constexpr float catchup_max_boost = 0.5f;
inline constexpr float catchup_full_deficit = 200.0f;
inline constexpr float flee_value = 300.0f;           // flee score at full danger (deaths: 180 of 423 right after fleeing)
// Team cohesion (strategy/cohesion.h): farm around the highest-damage active champion.
inline constexpr bool cohesion = false;              // off: -88 +/- 99 vs v1.14
inline constexpr float cohesion_leader_bonus = 1.6f;  // the leader's camp, for followers
inline constexpr float cohesion_near_bonus = 1.25f;   // camps among its nearest
inline constexpr float cohesion_away_factor = 0.75f;  // camps elsewhere
inline constexpr float cohesion_alone_fight = 0.3f;   // fights with less than half the active team near
inline constexpr bool invade_needs_lead = false;       // farm, fight or bank on their half only with more of us alive
inline constexpr float no_invade_factor = 0.02f;
inline constexpr bool wards = false;                  // place a ward on the halfway line when crossing it
inline constexpr int ward_line_width = 1;            // halfway line: |dist to our spawn - dist to theirs| <= this
inline constexpr bool ward_once = false;             // one ward per champion, then stop trying
inline constexpr int enemy_react_radius = 0;         // ignore enemies further than this from us (0: react to all we see)
inline constexpr bool combat_call = true;            // a champion fighting enemy champions (anywhere) calls allies too
inline constexpr bool call_on_several = true;        // seeing 2+ enemies (anywhere) also calls allies
// Chaining camps: after a camp we worked dies, its nearest camps (mapplan::kNeighbours, by walking
// distance from the draft) are worth more for a while.
inline constexpr float chain_bonus = 1.5f;
inline constexpr int chain_rounds = 30;              // how long the bonus lasts
inline constexpr bool capture_handoff = true;        // one party member captures, the rest move on
inline constexpr float handoff_factor = 0.1f;        // capture score for the members moving on
inline constexpr float regroup_squishy_factor = 2.0f;  // regroup pull for a squishy spotter
inline constexpr int regroup_enemy_radius = 8;       // the alert holds only while the enemy is this close to the spotter
inline constexpr int regroup_arrive = 4;             // this close to the spotter counts as grouped
inline constexpr int regroup_leash = 6;              // while regrouping, only camps this close to the spotter...
inline constexpr float regroup_farm_factor = 0.2f;   // ...score normally; the rest get this
inline constexpr float regroup_value = 10.0f;        // regroup option: this / walking turns x 100
inline constexpr int regroup_end_rounds = 2;         // responders stand down this soon after the alert lapses (was 10)
inline constexpr float regroup_min_hp = 0.5f;        // hurt champions heal instead of responding

// ---- tracing
inline constexpr int trace_candidates = 4;
inline constexpr int full_trace_every = 25;
inline constexpr bool trace_fuel_profile = false;  // add per-section fuel to detailed traces (debugging)   // turns between full traces when the decision is unchanged

} // namespace tune
