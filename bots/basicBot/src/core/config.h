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
inline constexpr unsigned long long fuel_per_scored_group = 6000;    // farm scoring cost per group
inline constexpr unsigned long long fresh_turn_fuel = 90000;         // a fresh turn starts with ~96.8k                                // distance-from-us search cut-off

// ---- tracing
inline constexpr int trace_candidates = 3;
inline constexpr int full_trace_every = 25;
inline constexpr bool trace_fuel_profile = false;  // add per-section fuel to detailed traces (debugging)   // turns between full traces when the decision is unchanged

} // namespace tune
