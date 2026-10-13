#pragma once
// Tuning knobs in one place. Each constant is referenced by name in decision
// traces, so the lab's decision explorer shows which knob produced a score.

namespace tune {

// ---- economy: what a unit of reward is worth, in gem-equivalents
inline constexpr float gold_value_early = 0.20f;   // gems per gold before late game
inline constexpr float gold_value_late = 0.03f;    // gold is nearly worthless near the end
inline constexpr int late_game_round = 1500;

// ---- farming
inline constexpr float capture_turns = 5.0f;        // engine capture time
inline constexpr float reset_attack_bonus = 1.0f / 8.0f; // attack reset adds ~1 attack per cooldown
inline constexpr float max_fight_hp_use = 0.80f;    // never plan to spend more HP than this on a camp
inline constexpr float ally_claim_factor = 0.25f;   // a group a teammate claimed is worth this fraction
inline constexpr float engaged_bonus = 1.35f;       // finishing a group we already damaged is attractive
inline constexpr int max_scored_groups = 6;        // fully score at most this many camp groups per turn
inline constexpr int max_team_per_camp = 4;
inline constexpr int max_party_wait = 12;          // turns to wait for allies before giving up on a group         // largest party we plan around for one camp group
inline constexpr float party_focus_share = 0.7f;    // in a party, share of monster hits we expect to absorb
inline constexpr float escape_margin_turns = 1.5f;  // extra turns of incoming damage kept in reserve to walk away
inline constexpr float respawn_wait_weight = 1.0f;  // turns spent waiting count like travel

// ---- banking
inline constexpr float bank_target_gems = 60.0f;    // carrying this many makes banking compelling
inline constexpr float bank_min_gems = 1.0f;
inline constexpr float carry_risk_per_gem = 0.004f; // farm scores shrink as the bag gets heavier
inline constexpr float danger_bank_boost = 2.0f;

// ---- health and recall
inline constexpr float heal_threshold = 0.35f;      // below this fraction, healing dominates
inline constexpr int recall_walk_cutoff = 6;        // walk home instead of recalling if this close
inline constexpr int monster_range = 3;

// ---- navBot2 behaviour
inline constexpr float heal_start_frac = 1.0f;  // healing urgency grows from full health... (A/B: recalling only near 20% lost ~225 gems/game)
inline constexpr float heal_urgent_frac = 0.35f;  // ...and dominates by here
inline constexpr float heal_enemy_margin = 0.25f;   // both shift up this much with enemy champions about
inline constexpr float heal_first_factor = 0.5f;    // a camp that needs a trip home first
inline constexpr float shop_gold = 150.0f;          // enough gold for an emblem makes a trip home worthwhile
inline constexpr float focus_bonus_per_damage = 0.0f;  // extra commitment per damage dealt (A/B: 1.5 cost ~130 gems/game)
inline constexpr float abandon_factor = 0.7f;  // walking away from a fight we started
inline constexpr float safe_camp_bonus = 1.4f;      // gem/gold clusters: quick, free, no risk
inline constexpr int safe_camp_radius = 12;
inline constexpr float idle_wait_factor = 0.6f;     // standing over a camp that is still respawning
inline constexpr int idle_wait_turns = 8;
inline constexpr float underpowered_factor = 0.65f; // camps needing a party before we have any items
inline constexpr int underpowered_shards = 5;
inline constexpr float gold_value_opening = 0.2f;  // gems per gold in the opening (A/B: 0.45 cost ~110 gems/game)
inline constexpr int opening_rounds = 300;
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
inline constexpr int build_variant = 7;            // 0 default, 1 treasure rush, 2 treasure on Vesper, 3 monster first,
                                                 // 4 power early, 5 home early, 6 durability on carries,
                                                 // 7 treasure>monster, 8 treasure>home, 9 treasure>power
inline constexpr bool stat_matching = false;     // Ryth buys health (bonus hp -> strength), casters focus
inline constexpr bool buy_altar_locks = true;   // keep an altar lock: cancel enemy tributes, lock enemy-side altars
inline constexpr bool buy_tp_crystals = false;

// ---- staying alive around enemy champions
inline constexpr int enemy_memory_rounds = 10;      // remember enemy sightings this long
inline constexpr int enemy_watch_radius = 7;        // enemies this close count as pressure
inline constexpr float alone_bank_factor = 0.8f;    // solo champions bank at this fraction of their target
inline constexpr int bag_avoid_gems = 20;           // carrying this many gems, path around enemy reach

// ---- fighting enemy champions
inline constexpr float kill_gold_value = 50.0f;
inline constexpr float fight_win_margin = 1.25f;    // need this edge in time-to-kill to commit

// ---- engine-ish constants used for estimates
inline constexpr int spell_cooldown = 8;

// ---- fuel (100k per turn; overrunning suspends the bot and loses the next turn)
inline constexpr unsigned long long fuel_reserve_trace = 5000;       // skip all tracing below this
inline constexpr unsigned long long fuel_reserve_full_trace = 40000; // skip the detailed trace below this
inline constexpr unsigned long long fuel_for_full_scoring = 60000;   // score max_scored_groups only above this
inline constexpr int bfs_radius = 48;
inline constexpr int replan_every = 8;                               // rescore at least this often
inline constexpr unsigned long long fuel_for_planning = 50000;      // advance the team plan only with this much left
inline constexpr float home_region_bonus = 1.5f;                     // camps in our squad's region
inline constexpr float other_region_factor = 0.4f;                   // camps in another squad's region
inline constexpr unsigned long long fuel_per_scored_group = 6000;    // farm scoring cost per group
inline constexpr unsigned long long fresh_turn_fuel = 90000;         // a fresh turn starts with ~96.8k                                // distance-from-us search cut-off

// ---- tracing
inline constexpr int trace_candidates = 3;
inline constexpr int full_trace_every = 25;
inline constexpr bool trace_fuel_profile = false;  // add per-section fuel to detailed traces (debugging)   // turns between full traces when the decision is unchanged

} // namespace tune
