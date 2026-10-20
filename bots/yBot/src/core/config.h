#pragma once
// Tuning knobs in one place. Each constant is referenced by name in decision
// traces, so the lab's decision explorer shows which knob produced a score.

namespace tune {

// ---- patch economy (wolf 22 -> 4 gems / 90 -> 40 gold, elk 36 -> 15 / 160 -> 90, gem cluster 24 -> 7,
// gold cluster 35 -> 20, kills 50 -> 100 gold). Scores stay in old-patch gem units: a gem now scores
// 1/gem_scale of them and gold 1/gold_scale, and bag-size thresholds (in raw gems) shrink by gem_scale.
// Both 1: the old patch's behaviour. Blind estimates (no local engine on the new patch yet): A/B them.
inline constexpr float gem_scale = 0.4f;
inline constexpr float gold_scale = 0.55f;

// ---- economy: what a unit of reward is worth, in gem-equivalents
inline constexpr float gold_value_early = 0.20f;   // gems per gold before late game
inline constexpr float gold_value_late = 0.01f;    // gold is nearly worthless near the end
inline constexpr int late_game_round = 700;

// ---- the middle of the map (brain/farm.cpp): bonus by radius from the geometric centre
inline constexpr float centre_bonus_max = 1.3f;     // farm score at the centre, falling linearly to x1 in the corners
inline constexpr float centre_zone = 0.5f;          // the centre: radius below this (0 centre .. 1 corner) for the busy-centre test

// ---- party rotation: a party moves on together instead of waiting out its camp's respawn
inline constexpr bool party_rotation = true;        // A/B switch
inline constexpr bool move_on_all_game = true;        // rangeBot: a camp a teammate holds is no option for the rest of us (they move on), all game
inline constexpr bool capture_board = false;          // rangeBot: announce captures in shared byte 5 (teams of <=5 bodies; off: -129 +/- 132, idle 16 -> 3 rounds but no gain)
inline constexpr bool bank_by_recall = true;          // rangeBot: bank via recall + walk from spawn when quicker
inline constexpr float bank_recall_margin = 3.0f;
inline constexpr bool bank_recall_any_altar = true;   // rangeBot: after a recall, bank at the altar nearest spawn
inline constexpr bool bank_recall_combo = true;       // rangeBot: a recall-bank also counts the heal and shopping it gives     // ...by at least this many turns
inline constexpr bool capture_step_off = true;        // rangeBot: off a capture's tiles when a teammate keeps it (all game)
inline constexpr float party_full_factor = 0.3f;     // rangeBot: a party camp nearer allies already claim in full: x this (1: off)
inline constexpr float ranged_cover_share = 0.15f;   // rangeBot: ranged on a camp a melee ally claims expects this share of monster hits (0: off)
inline constexpr bool capture_keeper_mobile = false;   // rangeBot: ...then one with a dash (banks sooner)
inline constexpr bool camp_focus_fire = false;         // rangeBot: every champion on a camp hits the same monster (no per-champion stickiness)
inline constexpr bool keeper_most_damaged = true;     // combatBotv2: of the champions at a dead camp, the one missing the most health keeps the capture (then recalls: banks and heals)
inline constexpr float keeper_missing_margin = 40.0f;  // ...by more than this much health (else the older rules)
inline constexpr int keeper_near_radius = 4;
inline constexpr bool keeper_stellar_first = false;  // yBot: Stellar at the camp keeps the capture (Fish: Stellar takes 7.3 captures a game, ours 2.3)
inline constexpr int stellar_keeper_radius = 6;  // ...from this far from the camp, even when a teammate is already on its tiles
inline constexpr bool late_heal = true;              // combatBotv2: tanking a camp, heal by damage taken: leave ~death_margin_turns before death
inline constexpr float death_margin_turns = 3.0f;     // ...turns of hits kept in hand after the walk out of reach
inline constexpr float late_heal_window = 2.0f;       // ...urgency ramps over this many turns before that
inline constexpr bool keeper_tank_first = false;      // combatBotv2 (as Fish): a tank at a dead camp keeps its capture (and recalls with it)
inline constexpr bool recall_on_capture = true;       // combatBotv2 (as Fish): the keeper recalls from the tile a round before the capture completes
inline constexpr float capture_recall_margin = 3.0f;  // ...when banking via spawn is within this many turns of walking to an altar
inline constexpr float capture_recall_hp = 0.5f;      // ...or below this health (yBot: 0.9 -> 0.5: Fish recalls at 30-60% after 1-3 captures)
inline constexpr bool partial_tank = true;            // combatBotv2: leading a party camp, tank until our retreat point and leave; the party finishes it
inline constexpr float partial_tank_floor = 0.3f;     // ...retreat point (health fraction; Fish's Ren left at ~29%), also what allies keep
inline constexpr float partial_tank_min_share = 0.25f; // ...worth it when we can take at least this share of the fight's damage
inline constexpr bool moving_on_real = true;          // combatBotv2: a party moving on counts only mates near us still on the job (not recalling, not the keeper)
inline constexpr int moving_on_radius = 8;
inline constexpr bool tank_cover_near = true;         // combatBotv2: a claiming tank covers us only if it is no farther from the camp than we are (+ slack)
inline constexpr int tank_cover_slack = 2;
inline constexpr bool ally_design_spells = true;     // combatBotv3: allies' spell damage from their designs (modifiers x focus / cooldown)
inline constexpr bool ally_vision_gate = true;      // combatBotv3: read allies only when in vision (out-of-sight reads throw, ~2-3k fuel each)
inline constexpr bool ally_cache_shared = true;     // combatBotv3: the ally stat cache serves every champion (they share the process)
inline constexpr int ally_read_period = 20;        // combatBotv3: re-read allies' strength / focus this often (passive and spells once)
inline constexpr bool party_real_dps = true;          // combatBotv2: camp fight estimates add the claiming allies' own damage (not ours x party size)
inline constexpr float keeper_tank_fit = 0.65f;       // combatBotv2: below this health the tank may keep a capture too (it is going home next)
inline constexpr bool avoid_teammate_capture = false; // combatBotv2: paths go around a teammate's capture tiles
inline constexpr int capture_avoid_radius = 8;          // ...for camps this close
inline constexpr bool recall_off_capture = false;    // combatBotv2: step off a teammate's capture tiles before recalling (else the recall splits it)          // ...counting champions this close to the camp
inline constexpr bool capture_keeper_not_tank = true;  // rangeBot: the capture keeper is not the tank when another champion is on the tiles
inline constexpr int rotate_capture_rounds = 150;  // rangeBot: ...only in the opening (all game: ~250 more lost turns a game)
inline constexpr bool rotate_on_capture = true;     // rangeBot: a teammate capturing our party camp ends it for the rest of the party (rows: they walked back to the first wolves)
inline constexpr int rotate_min_wait = 5;           // our party camp is down at least this long...
inline constexpr float rotate_bonus = 1.5f;         // ...the next free party camp (leader) / the mates' new camp (followers)

// ---- farming
inline constexpr float capture_turns = 5.0f;        // engine capture time
inline constexpr float reset_attack_bonus = 1.0f / 8.0f; // attack reset adds ~1 attack per cooldown
inline constexpr float max_fight_hp_use = 0.80f;    // never plan to spend more HP than this on a camp
inline constexpr float camp_budget_scale = 1.8f;     // yBot: x the camp health budget when sizing parties (Fish: solo wolves, pairs on an elk)
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
inline constexpr bool team_rush = true;
inline constexpr bool team_camp = false;           // combatBotv3: grouped, healthy, no enemy near: the team takes the best party camp together
inline constexpr int team_camp_round = 40;         // ...from this round
inline constexpr int team_camp_min = 4;            // ...with at least this many of us
inline constexpr int team_camp_radius = 8;         // ...within this of the team's centre
inline constexpr float team_camp_hp = 0.5f;        // ...each at least this healthy
inline constexpr int team_camp_enemy_radius = 10;  // ...and no enemy champion this close to the centre
inline constexpr int team_camp_max_dist = 20;      // ...camps this close to the centre
inline constexpr float team_camp_min_strength = 60.0f;  // ...that one champion could not take (monster strength per round)
inline constexpr float team_camp_body_dps = 60.0f;  // ...team damage counted per body (the same for every teammate)
inline constexpr int team_camp_grace = 3;          // ...keep it this many rounds after the conditions lapse            // a camp holding most of the map's gems (stampede): the team takes it together
inline constexpr float team_rush_share = 0.5f;     // ...at least this share of all gems in one camp
inline constexpr int team_rush_round = 50;         // ...from this round
inline constexpr int team_rush_min_alive = 4;      // ...with at least this many of us alive
inline constexpr int team_rush_party = 5;          // ...gather this many (or all alive) before the pull
inline constexpr float team_rush_melee_join = 0.25f; // ...melee step in once the camp is below this health
inline constexpr float team_rush_other_factor = 0.1f;  // ...other camps meanwhile
inline constexpr float team_rush_bonus = 3.0f;     // ...and it is never "too strong" or "underpowered", x this
inline constexpr int max_party_wait_claimed = 10;  // ...also when allies have claimed it (was 30: 100-turn holds)
inline constexpr int wait_farm_radius = 6;         // while waiting, farm gem/gold clusters this close to the camp
inline constexpr int max_party_wait = 10;          // turns to wait for allies before giving up on a group         // largest party we plan around for one camp group
inline constexpr float party_focus_share = 0.7f;    // in a party, share of monster hits we expect to absorb
inline constexpr float escape_margin_turns = 1.5f;  // extra turns of incoming damage kept in reserve to walk away
inline constexpr float respawn_wait_weight = 2.0f;  // turns spent waiting for a respawn count double (was 1)

// ---- banking
inline constexpr float bank_target_gems = 60.0f * gem_scale;    // carrying this many makes banking compelling
inline constexpr float bank_min_gems = 1.0f;
inline constexpr int endgame_window = 300;          // last rounds where tribute timing against the round limit is checked
inline constexpr int stun_range = 4;                // enemy crowd control reaches this far (cancels tributes)
inline constexpr float carry_risk_per_gem = 0.004f / gem_scale; // farm scores shrink as the bag gets heavier
inline constexpr float danger_bank_boost = 2.0f;

// ---- health and recall
inline constexpr float heal_threshold = 0.35f;      // below this fraction, healing dominates
inline constexpr float recall_turns = 5.0f;          // engine recall channel
inline constexpr int monster_range = 3;            // fallback only: camp ranges come from the engine (patch: elk 4, golem 4, monolith 5)

// ---- team fights (attackBot)
inline constexpr bool team_up = true;               // fights: outnumbered with enemies nearer than our allies, walk to the allies first
inline constexpr int team_up_radius = 6;            // ...enemies and allies this close count as here
inline constexpr int team_up_ally_radius = 12;      // ...only toward allies this close
inline constexpr int team_up_min_gems = 0;          // ...only while carrying at least this many gems (0: always; 1 or 22 lost most of the drop in gems lost on death)
inline constexpr bool party_engage = false;         // fights: allies with us and more of us than of them near the target: engage together
inline constexpr int engage_radius = 8;             // ...allies near us and enemies near the target within this
inline constexpr float engage_bonus = 2.0f;         // ...the fight scores this much more (on: deaths 7.7 -> 13.3, kills 5.8 -> 3.8)
inline constexpr bool fight_monster_damage = true;  // fights: monster damage on us (or camps by the target) counts on their side
inline constexpr float fight_spot_camp_weight = 0.5f;  // ...camps within reach of the target count at this share (may be asleep)
inline constexpr bool fight_count_ghosts = false;   // fights: enemies seen recently (not now) that could have joined count too
inline constexpr int ghost_join_age = 8;            // ...seen within this many rounds, within join radius + age tiles of the target
inline constexpr float ghost_hp = 700.0f;           // ...each with this much health (unknown now)
inline constexpr int fight_join_radius = 4;        // enemies this close to the target join its side
inline constexpr int fight_beside_radius = 3;      // allies this close to us fight (and draw fire) with us
inline constexpr float flee_horizon_turns = 10.0f;  // flee danger starts when we would die within this many turns
// Early retreat (ladder losses: retreats began at a median 393 HP, 70% died within 3 turns):
// flee while the enemies near us would need longer than our escape plus this margin to kill us.
inline constexpr bool forecast_retreat = false;     // combatBotv3: a stronger group closing on us (whole sides: health over damage) is a retreat before contact
inline constexpr int forecast_radius = 9;            // ...enemies this close
inline constexpr int forecast_ally_radius = 6;       // ...allies this close count on our side
inline constexpr int forecast_min_enemies = 2;       // ...at least this many of them
inline constexpr float forecast_edge = 0.6f;
inline constexpr int forecast_contact_slack = 1;
inline constexpr bool retreat_override = true;       // combatBotv3: outmatched before contact (forecast): the executor retreats, whatever the chosen option
inline constexpr int retreat_override_hold = 8;
inline constexpr bool retreat_cheap_step = true;     // ...the override retreats with one plain step toward an ally (or home), not flee()'s detour
inline constexpr int forecast_support_radius = 12;
inline constexpr bool forecast_spawn_approachers = true;  // ...enemies near their spawn count if they are walking toward us
inline constexpr bool forecast_pair = false;         // ...one of their Cassi & Issac counted: the unseen partner counts too
inline constexpr float forecast_pair_hp = 800.0f;    // ...at this health
inline constexpr int forecast_pair_ghost_age = 20;   // ...and at its damage when seen this recently   // ...allies this close (able to join in time) count on our side      // ...and keeps retreating up to this many rounds while still outmatched
inline constexpr bool ranged_follow_front = false;   // combatBotv3: a ranged champion follows a melee ally's retreat (enemies near); retreating allies do not count in fight estimates
inline constexpr float front_retreat_value = 12000.0f;  // ...its score (above farm and fight scores)     // ...and only before contact: no enemy within its range + this of us         // ...retreat when we last under this share of the time they do
inline constexpr bool predicted_retreat = true;    // loopBot: on (+249, deaths 11.1 -> 8.8 with the homeward flee; was -75 before it)
inline constexpr float retreat_margin_turns = 4.0f;
inline constexpr int retreat_ally_radius = 0;        // retreat toward an ally this close (away from the threat), else spawn (0: off; -29 +/- 87 at 20)
inline constexpr float focus_fire_bonus = 2.0f;
inline constexpr int team_target_memory = 3;         // rounds the team target survives out of sight
inline constexpr float team_target_bonus = 2.0f;     // fight score for the enemy we saw take damage first    // an enemy allies are already attacking
inline constexpr float contested_capture_factor = 0.05f;  // capture with an enemy on the spawn tiles (progress resets)
inline constexpr float fight_hurt_bonus = 1.0f;    // fight score x(1 + this * enemy's missing health fraction)

// ---- navBot2 behaviour
inline constexpr float heal_start_frac = 0.7f;  // healing urgency grows from full health... (A/B: recalling only near 20% lost ~225 gems/game)
inline constexpr float heal_urgent_frac = 0.35f;  // ...and dominates by here
inline constexpr float heal_enemy_margin = 0.35f;   // both shift up this much with enemy champions about
inline constexpr float shop_trip_hp = 0.6f;           // yBot: shopping adds to a trip home only below this health...
inline constexpr int shop_trip_wait = 80;            // ...or to complete an emblem affordable this many rounds
inline constexpr int altar_lock_every = 4;            // yBot: bank scoring re-reads an altar's lock this often
inline constexpr bool cast_first = true;
inline constexpr bool free_cast = false;             // yBot: cast movement-forfeiting spells at enemies on turns we did not move             // yBot: fights cast a ready movement-forfeiting spell that can hit before stepping
inline constexpr float bank_target_scale = 1.5f;     // yBot: x every role's bank target (Fish tributes bigger bags)
inline constexpr float heal_first_min_missing = 0.6f; // yBot: 0.9 -> 0.6 // yBot: "heal first" only below this health
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
inline constexpr int early_gold_skip_round = 2000;      // before this, gold-only camps are skipped unless right beside us
inline constexpr bool shop_after_first_tribute = false;  // no shop trips before our first tribute
inline constexpr bool shop_no_fight = false;         // rangeBot: no shop trip while healthy with an enemy champion near or an ally's alert near (off: +20 vs +30)
inline constexpr int shop_enemy_radius = 10;         // ...an enemy this close
inline constexpr int shop_alert_radius = 12;         // ...or an ally's enemy alert this close
inline constexpr bool skip_field_unreachable = true;  // farm: skip groups whose distance field does not reach us
// Gems-only mode (behind on banked gems in the mid game): gold-only camps score gems_only_factor.
inline constexpr bool dash_pockets = true;    // dashers farm sealed groups (dash over a hole in, and out)
inline constexpr bool dash_always = true;          // xBot: walks dash whenever a dash gets us 2+ tiles nearer (not saved for escape or a pocket)
inline constexpr int dash_exit_turns = 2;     // dash out of a pocket, beyond the walk back to the landing tile
inline constexpr int still_hold_turns = 80;   // ...holding for a party (a gem rally may be on its way)
inline constexpr int still_turns = 25;        // still this long while idle/holding: drop that camp and hunt
inline constexpr int stuck_cooloff = 120;     // the camp we were stuck on is avoided this long
inline constexpr int hunt_turns = 60;         // then go after enemies (last seen, else their spawn) this long
inline constexpr float hunt_value = 5.0f;     // hunt option score: beats idling and dropped camps, not real farming
inline constexpr float gem_rally_bonus = 2.0f;  // ally claims a gem party camp: join it (1: off)
inline constexpr int gems_only_round = 150;
inline constexpr int gems_only_lag = static_cast<int>(50 * gem_scale);             // enemy banked gems minus ours
inline constexpr bool gems_only_when_none = false;   // ...or we have banked nothing yet (off: -20 +/- 29 on 17 maps, stampede -200)
inline constexpr float gems_only_factor = 0.05f;
inline constexpr float gems_only_group_fight = 1.5f;  // fights with 2+ of us near the target (and >= their numbers) while in gems-only mode
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
inline constexpr float altar_exposure_tiles = 0.0f;  // (unfinished: off)  // bank: an altar at the enemy's end costs this many extra tiles (centre ~half)
inline constexpr float altar_enemy_tiles = 0.0f;  // (unfinished: off)     // ...enemies seen near it add this
inline constexpr float altar_exposure_bag = 50.0f * gem_scale;    // ...both grow by x1 per this many gems carried
inline constexpr bool defend_ally = false;           // combatBotv2: hit an enemy champion that can hit an ally, if we can hit it this turn
inline constexpr float defend_min_hp = 0.35f;       // ...not below this health
inline constexpr int defend_approach = 3;           // ...walking up to this many tiles beyond our range to get there
inline constexpr int defend_ally_radius = 5;        // ...for allies this close to us
inline constexpr bool power_fighting_only = false;   // combatBotv2: allied power counts only allies fighting an enemy champion
inline constexpr bool opening_hp_pool = false;     // combatBotv2: the opening camp's damage check budgets the whole crew's health
inline constexpr int front_enforce_radius = 8;     // combatBotv2: ranged front-line / hold rules apply with an enemy this close (was range + 2)
inline constexpr bool ranged_no_cover_fall_back = true;  // combatBotv2: ranged with enemies within that and no melee within 6 falls back
inline constexpr bool dash_reserve = false;        // combatBotv2: outnumbered, a dasher keeps its dash for fleeing (no dash on approach / walks)
inline constexpr bool safe_dash = true;            // combatBotv2: a melee dasher dashes into a fight only onto a survivable tile not ahead of our front
inline constexpr int safe_dash_slack = 1;          // ...tiles it may land nearer the enemy than our nearest other melee
inline constexpr bool bodyguard = true;            // combatBotv2: melee step between a squishy ally and an enemy closing on it
inline constexpr int guard_ally_radius = 6;         // ...allies this close to us are guarded
inline constexpr int guard_threat_radius = 2;       // ...from enemy champions this close to them
inline constexpr bool guard_not_while_farming = true; // ...and not while working a camp (monsters on us or the camp in reach)
inline constexpr bool guard_melee_closing = true;    // ...and only melee enemies closing on them (any enemy within 4: -115)
inline constexpr float gold_skip_max_share = 0.2f;   // combatBot: skip gold-only clusters only where they hold at most this share of the gold
inline constexpr bool routes_skip_gold_only = true;  // combatBot: gold-only clusters are never route steps (on such maps)
inline constexpr bool fixed_draft = true;            // draft a fixed comp, first N picks: Ren, Stellar, Cassi & Issac, Ryth, Varg
inline constexpr float gold_draft_share = 0.95f;     // ...maps with at least this share of camps gold-only: Stellar, Tavi, Prism, Cassi & Issac, Ren
inline constexpr bool draft_mite_small = false;     // 3-pick drafts take Mite's roster (Ren, Stellar, Cassi & Issac)
inline constexpr bool treasure_maps = false;        // build 12: treasure emblem first on treasure maps (shop.cpp)
inline constexpr int treasure_map_max_area = 1400;  // ...small maps (all gold within early reach)...
inline constexpr float treasure_map_max_elk_share = 0.8f;  // ...that are not mostly elk; or no gems outside Monoliths
inline constexpr bool farm_map_monster = true;     // combatBotv3: on farming maps Durability emblems in the orders become Monster emblems
inline constexpr bool farm_map_jungle = true;      // combatBotv3: on farming maps the per-champion orders start with a Jungle emblem
inline constexpr int medium_map_side = 50;         // map editor size classes by the longest side: small < 50 <= medium < 75 <= large
inline constexpr int large_map_side = 75;
inline constexpr float farming_walk_ratio = 1.5f;  // medium maps: spawn-to-spawn walk at least this x the straight line = farming
inline constexpr bool fish_builds = true;        // yBot: Fish's per-champion emblem orders (shop/shop.cpp), no farming-map swaps
inline constexpr bool champion_builds = true;     // combatBotv3: shop/shop.cpp per-champion item orders (Cassi, Issac, Ryth, Stellar, Tavi, Prism, Ren, Varg)
inline constexpr int build_variant = 12;           // 0 default, 1 treasure rush, 2 treasure on Vesper, 3 monster first,
                                                 // 4 power early, 5 home early, 6 durability on carries,
                                                 // 7 treasure>monster, 8 treasure>home, 9 treasure>power,
                                                 // 10 monster>treasure, 11 jungle>power>durability (casters focus)
inline constexpr bool stat_matching = false;     // Ryth buys health (bonus hp -> strength), casters focus
inline constexpr bool lock_own_altars = true;  // also lock unlocked altars on our side as we pass (A/B switch)
inline constexpr bool break_altar_locks = true;   // bank at enemy-locked altars too: tributing attacks the lock (altar_lock_hits from the engine remove it: 5 this patch)
inline constexpr int lock_break_turns = 5;          // turns a lone champion needs to break one (lock health 3 -> 5 this patch)
inline constexpr bool buy_altar_locks = true;   // keep an altar lock: cancel enemy tributes, lock enemy-side altars
inline constexpr bool buy_tp_crystals = false;

// ---- staying alive around enemy champions
inline constexpr int enemy_memory_rounds = 10;      // remember enemy sightings this long
inline constexpr int enemy_watch_radius = 7;        // enemies this close count as pressure
inline constexpr float alone_bank_factor = 0.8f;    // solo champions bank at this fraction of their target
inline constexpr int bag_avoid_gems = static_cast<int>(20 * gem_scale);           // carrying this many gems, path around enemy reach

// ---- fighting enemy champions
inline constexpr float kill_gold_value = 100.0f;   // patch: kill gold 50 -> 100 (bc::constants::kill_gold)
inline constexpr float fight_our_dps_scale = 1.0f;  // our side's damage in fight estimates (measured real/predicted ~0.21)
inline constexpr float fight_win_margin = 1.25f;    // need this edge in time-to-kill to commit
// Mid/late-game fights (combatBotv3): from fight_mid_round, a fight whose estimated win chance is
// under fight_mid_win_chance scores x fight_mid_factor (channelling targets exempt). Win chance =
// logistic(a + b ln edge), fitted on traced fights from round 300 (edge 1.3: 50%, 2.9: 60%, 7: 70%).
inline constexpr int fight_mid_round = 300;
inline constexpr float fight_mid_win_chance = 0.70f;
inline constexpr float fight_mid_factor = 0.3f;     // 1: off
inline constexpr int fight_mid_mode = 1;            // 0: win-chance cutoff; 1: pot odds (p x gain vs (1-p) x stake)
inline constexpr float death_time_value = 12.0f;    // pot odds: gem-equivalents of a death (~13 farming over 60 rounds). With model 2: 12 -8, 18 -27, 24 -10, 30 -19, 36 -28 vs model 0 at 36 (1,020 games each)
// Lab fight data (traced builds only): on a new contact (no enemy within 6 tiles for 10 rounds),
// this share of contacts does the opposite of the fight scorer for 10 rounds (0: off; data runs).
inline constexpr float fight_explore_rate = 0.0f;
inline constexpr bool fight_explore_even = false;   // lab data mode: always fight the best target when our count near it >= theirs
// combatBotv3 team fighting (each an A/B switch):
// 1. commitment: once we pick a fight (or a flight) we keep it this many rounds unless below
//    commit_break_hp; fleeing never attacks (flee_no_attack).
inline constexpr int fight_commit_rounds = 0;       // 0: off
inline constexpr float commit_break_hp = 0.25f;
inline constexpr bool flee_no_attack = false;
// 2. team call: allies within team_call_radius claiming a fight or a retreat vote; we follow the majority.
inline constexpr bool team_fight_call = false;
inline constexpr int team_call_radius = 6;
// 4. group fights: fewer than group_fight_min of us around the target scores x this (1: off);
//    from group_farm_round, camps with an ally within group_farm_radius score x group_farm_bonus.
inline constexpr float group_fight_alone_factor = 0.3f;  // A/B (204 games): +35 +/- 58, deaths 5.58 -> 5.24, banked 831 -> 862
inline constexpr int group_fight_min = 2;
inline constexpr int group_fight_radius = 6;
inline constexpr float group_farm_bonus = 1.2f;     // 1: off
inline constexpr int group_farm_round = 300;
inline constexpr int group_farm_radius = 8;
inline constexpr float fight_win_fit_a = -0.144f;
inline constexpr float fight_win_fit_b = 0.510f;
// Fight win model: 0 the curve above; 1 the edge curve refit on 1,800 self-play games (resolved
// fights we took, win = first death near the fight is theirs); 2 eight features on the same fights.
inline constexpr int fight_win_model = 2;
inline constexpr float fight_win_refit_a = -1.1700f;
inline constexpr float fight_win_refit_b = 0.4630f;
inline constexpr float fwm_bias = -0.9515f;
inline constexpr float fwm_ln_edge = 1.0331f;
inline constexpr float fwm_ln_hp = 0.3904f;       // ln(our side's health / theirs)
inline constexpr float fwm_ln_dps = -0.6413f;     // ln(our side's damage / theirs)
inline constexpr float fwm_lead = -0.3622f;       // our count - theirs around the target
inline constexpr float fwm_them = 0.2765f;        // their count
inline constexpr float fwm_their_hp = 1.5133f;    // their side's health fraction
inline constexpr float fwm_target_hp = -1.0760f;  // the target's health fraction
inline constexpr float fwm_dist = -0.1519f;       // tiles to the target
inline constexpr int favour_radius = 6;             // bodies counted this close to the target (both sides)
inline constexpr float kill_denial_value = 12.0f;   // gem-equivalents of an enemy out for 60 rounds (favourable fights; was 20)
inline constexpr float favourable_max = 1.5f;       // favourable fight score at full health (x1 at half; was x2)
inline constexpr int their_half_fight_round = 150;  // from this round, fights against enemies on their half...
inline constexpr float their_half_fight_factor = 0.25f;  // ...score this (channelling targets and raids exempt)
inline constexpr float no_chase_factor = 1.0f;     // fights against an enemy out of reach and moving away (1: off)
inline constexpr int fight_commit_gap = -1;         // fights: commit only within this many tiles beyond our reach (or hit / approached; -1: off)
inline constexpr float fight_far_factor = 0.1f;     // ...otherwise the fight scores this
// Stuns (combatBotv2): who our stuns go on, and a stunned enemy becomes the team target. Ground spells aim where they hit the most enemy champions.
inline constexpr int stun_target = 0;               // our stuns: 0 usual target, 1 squishiest, 2 biggest threat (damage, x2 when hitting us or an ally)
inline constexpr bool stunned_team_target = true;   // a stunned enemy we can see becomes the team target
inline constexpr int stun_check_radius = 5;         // read stun buffs on enemies this close (a host call each)...
inline constexpr unsigned long long stun_check_fuel = 40000;  // ...with at least this much fuel left
inline constexpr bool aoe_champions_first = true;   // ground spells: any tile in reach, champions hit first, monsters break ties
inline constexpr int chase_radius = 6;              // enemies further than this are not chased...
inline constexpr float chase_factor = 0.3f;         // ...their fight score gets this (unless channelling)

// ---- engine-ish constants used for estimates
inline constexpr int spell_cooldown = 8;

// ---- fuel (100k per turn; overrunning suspends the bot and loses the next turn)
inline constexpr unsigned long long fuel_reserve_trace = 5000;       // skip all tracing below this
inline constexpr unsigned long long trace_fuel_per_byte = 36;          // stdout cost, measured ~33
inline constexpr unsigned long long fuel_reserve_full_trace = 40000; // skip the detailed trace below this
inline constexpr unsigned long long nav_safe_floor = 0;                // below this fuel, paths skip the avoid-danger search (0: always search)
inline constexpr unsigned long long farm_floor = 0;                       // loopBot: skip farm scoring (keep the plan's camp) below this much fuel before thinking (0: off; 70k: +257 vs +361 off)
inline constexpr bool farm_skip_ignore_spawn = true;   // rangeBot: enemies on their own spawn do not trigger the farm skip
inline constexpr bool fight_skip_at_spawn = true;      // rangeBot: enemies on their own spawn are not scored as fights
inline constexpr int fight_score_max = 3;           // xBot: fight options for the nearest this many enemies (+ team target, channelling; 0: all)
inline constexpr int farm_skip_fight_radius = 6;    // xBot: skip farm scoring with an enemy champion this close (not at their spawn; -1: off)
inline constexpr int farm_skip_enemy_reach = 3;                           // loopBot: skip farm scoring while an enemy champion is this close beyond our range (-1: off)
inline constexpr bool scan_skip_far = true;        // rangeBot: the camp scan skips groups out of sight range
inline constexpr int scan_sight = 9;               // ...vision 8 (+1 slack)
inline constexpr int reward_recheck_every = 3;     // rangeBot: re-read a captured-waiting group's state every this many rounds when away from it
inline constexpr int think_floor_max_streak = 3;            // rangeBot: ...at most this many turns in a row (a stale plan deadlocked lanes)
inline constexpr unsigned long long first_turn_plan_floor = 150000;  // combatBotv2: first turn with less fuel than this before thinking: follow the draft plan (0: off)
inline constexpr unsigned long long think_floor = 60000;                  // keep last turn's plan when less fuel than this is left before thinking (0: off)
inline constexpr unsigned long long fuel_for_full_scoring = 75000;   // score max_scored_groups only above this
inline constexpr int bfs_radius = 48;
inline constexpr int replan_every = 8;                               // rescore at least this often
inline constexpr unsigned long long fuel_per_scored_group = 6000;
inline constexpr bool farm_reserve_adaptive = true;  // combatBotv3: the reserve also keeps room for the dearest camp scored so far
inline constexpr unsigned long long farm_reserve = 30000;   // combatBotv2: farm scoring stops (after one camp) below this much fuel: bank, fight, execute and trace still fit
inline constexpr bool fight_first = false;                 // combatBotv2: with an enemy about, score survival and fights before farming
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
inline constexpr int route_lookahead = 3;
// ---- loopBot
inline constexpr bool no_open_near_fight = true;     // yBot: never wake a sleeping camp next to a fight with enemy champions...
inline constexpr int fight_near_camp_radius = 8;      // ...an enemy champion this close to the camp or us, or an ally this close to it fighting / retreating
inline constexpr float open_near_fight_factor = 0.2f; // ...and such a camp scores this as a farm option
inline constexpr bool sleeping_camps_safe = true;   // sleeping camps are not danger or incoming damage
inline constexpr bool mirror_fields = false;        // xBot: their half's group/altar fields are read from our half's twins (map symmetry)
inline constexpr int unit_wall_radius = -1;          // xBot: path searches treat units as walls only this close to us (-1: all units)
inline constexpr int search_detour = -1;           // xBot: a search with a known aim stops at straight distance + this (-1: no cap)
inline constexpr bool set_paths = true;             // far walks follow precomputed fields (same tiles every lap)
inline constexpr int field_walk_tiles = 4;          // ...beyond this many tiles from the goal; the search does the rest
inline constexpr unsigned long long field_build_fuel = 35000;  // build a missing field on demand above this
inline constexpr int hop_min_gain = 3;
inline constexpr int tank_priority = 10;            // tanks act first each round (0: draft order)
inline constexpr bool opening_routes = true;       // follow the draft's per-champion opening routes
inline constexpr bool opening_camp = true;         // the crew without routes opens on the draft's party camp
// combatBotv3: team opening. The whole team opens on the best party camp on our side, routes and
// all, judged with the whole team's health; other camps wait (eight: every top team takes its
// own two elks with all five first, ignoring the single wolf; we split and lost the middle).
// combatBotv3: capture hold. Live (v4): we broke 197 captures (left the tiles before it completed)
// to their 55, 130 with no enemy within 6, mostly answering a regroup call or chasing a fight.
// On a reward's tiles with no enemy champion this close, we stay: no regroup, fights x factor.
inline constexpr int capture_hold_radius = 4;      // 0: off
inline constexpr int capture_hold_after = 2;       // ...only once we have stood on the tiles this many rounds (nearly done)
inline constexpr float capture_hold_fight_factor = 1.0f;  // 0.1 tested: fewer kills (eight, monolith, swirl), off
inline constexpr bool team_opening = true;
inline constexpr int team_opening_min_bodies = 5;
inline constexpr float team_opening_max_ratio = 0.6f;  // ...and at most this share of the enemy's distance to it from our spawn    // ...on maps with centre camps we leave to them, with at least this many bodies
inline constexpr float team_opening_other_factor = 0.1f;  // other camps' farm score while it is on
inline constexpr int opening_camp_radius = 30;      // the draft's opening camp is at most this walk from spawn...
inline constexpr int opening_camp_rounds = 60;      // ...and steers the champions without a route until captured or this round
inline constexpr float opening_camp_bonus = 1.5f;   // its farm score multiplier while it does
inline constexpr int opening_safe_margin = 5;       // ...and at least this many steps nearer our spawn than theirs
inline constexpr int passing_altar_tiles = 4;          // an altar this close on our way...
inline constexpr float passing_altar_min_gems = 15.0f * gem_scale; // ...with at least this bag...
inline constexpr float passing_altar_bonus = 2.0f;     // ...is worth topping up at
inline constexpr float farm_switch_margin = 1.7f;   // on the way to a camp, another camp also needs this (x hysteresis 1.2 = ~2x)              // dash on a walk when it saves this many steps (holes)            // candidate camps per lookahead step

// ---- farming loops (draft-planned cycles over camp groups; strategy/loop.h)
// yBot farming lanes (strategy/lanes): on farming maps each champion owns a few camps on our half.
inline constexpr float nn_recall_gamma = 0.5f;       // yBot: weight of the top-team recall classifier on the heal option (0: off)
inline constexpr float nn_recall_min = 0.3f, nn_recall_max = 3.0f;
inline constexpr float nn_farm_beta = 0.0f;          // yBot: weight of the top-team camp-choice network (lab/nn; 0: off; 0.3 tested: +58 +- 169 vs L3 on 5 maps)
inline constexpr bool lanes = true;
inline constexpr float lane_max_strength = 80.0f;     // camps in lanes: monster strength up to this (a wolf, a wolf pair, a single elk)
inline constexpr float lane_solo_strength = 40.0f;    // ...above this a camp goes to two of us (a single elk)
inline constexpr float lane_dps = 50.0f;              // damage per round per champion, for lane load
inline constexpr float lane_load_weight = 0.5f;       // tiles per turn of load when balancing lanes
inline constexpr float lane_count_weight = 30.0f;     // tiles per camp already in a lane (everyone gets one first)
inline constexpr float lane_frail_strength = 20.0f;   // camps stronger than this prefer sturdy owners...
inline constexpr float lane_frail_pref = 100.0f;      // ...extra tiles per point of survivability under 1
inline constexpr float lane_tank_pref = 15.0f;        // a pair camp's first owner: extra tiles for a non-tank
inline constexpr float lane_bonus = 2.0f;             // farm score x for our lane's camps
inline constexpr float lane_other_factor = 0.4f;      // another lane's camps while ours has work
inline constexpr unsigned long long lane_force_fuel = 60000;  // yBot: lane camps are always scored only above this much fuel
inline constexpr int lane_wait = 10;                  // a lane camp back within walk + this counts as work
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
inline constexpr float altar_time_weight = 0.0f;     // farm scores: walk to the altar, amortised over a bag
inline constexpr float centre_band = 0.2f;           // centre: |our dist - their dist| <= this x spawn-to-spawn
inline constexpr float centre_share = 0.4f;          // contest when the centre holds this share of income...
inline constexpr int centre_rules_needed = 2;        // ...and at least this many of the other 3 rules hold
inline constexpr unsigned long long draft_fuel_reserve = 350000;  // first draft turn: fields stop here (neighbour lists, routes, trace follow)
inline constexpr unsigned long long draft_plan_floor = 50000;     // below this, routes skip the lookahead
inline constexpr unsigned long long draft_prepare_floor = 150000; // first turn: neighbour lists stop here
inline constexpr unsigned long long draft_describe_floor = 45000; // below this, the draft trace skips the route text
inline constexpr unsigned long long draft_dash_floor = 350000;   // first draft turn: the dash-pocket search (statics) stops here (01islands: ~590k, ends ~330k)

// ---- raids: their team is down (all but tune::raid_max_alive bodies dead)
inline constexpr int raid_max_alive = 1;
inline constexpr bool purge_dead_ghosts = true;      // yBot: drop ghosts of enemies known dead (checked when they vanish in sight)
inline constexpr bool raid_heal = true;              // yBot: in a raid, heal only when low...
inline constexpr int raid_heal_window = 10;          // ...while the first respawn is at least this many rounds away
inline constexpr float raid_heal_start = 0.6f;       // ...urgency starts here (not full health), no enemy margin             // at most this many enemy bodies alive
inline constexpr int enemy_alive_every = 4;          // check enemy deaths every this many rounds (fuel)
inline constexpr float raid_cluster_factor = 3.0f;   // gem/gold clusters on their half (at full health)
inline constexpr float raid_camp_factor = 1.5f;      // monster camps on their half (at full health); x1 at half health
inline constexpr int raid_min_window = 12;           // whole enemy team dead with at least this many rounds to their first respawn... (yBot: 25 -> 12)
inline constexpr float raid_own_half_factor = 0.4f;  // ...our half's camps get this (at full health): take theirs first

// ---- regrouping (strategy/regroup.h): an enemy on our half pulls nearby allies to the spotter
inline constexpr bool rally_alert = true;            // combatBotv3: an outmatched retreat calls the team to a rally point behind it
inline constexpr int rally_radius = 30;              // healthy allies this close to the rally point respond
inline constexpr int rally_back = 5;                 // the rally point: this far from where the retreat began, toward where we fall back
inline constexpr float rally_factor = 1.5f;          // regroup pull per enemy rallied against beyond the first (x squishy factor)
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
// combatBotv3: ranged champions do not invade alone. A farm, fight or bank spot on their half with
// none of our melee champions alive within invade_melee_radius scores x invade_no_melee_factor
// (banks there are skipped); fights against an enemy already in range are exempt.
inline constexpr bool invade_needs_melee = false;
inline constexpr int invade_melee_radius = 8;
inline constexpr bool invade_melee_ahead = false;  // ...and that melee ally must be in front of us (no farther from their spawn)
// combatBotv3 probe: ranged champions shoot camps from the diagonal corners of their range,
// outside a circular monster reach (squared straight-line distance > camp_reach_euclid_sq; (3,2)
// = 13 is still hit, so a range-3 corner (3,3) = 18 is the candidate).
inline constexpr bool camp_corner_stand = false;
inline constexpr int camp_reach_euclid_sq = 13;
inline constexpr bool probe_spell_only_camps = false;  // lab probe: ranged never basic-attack monsters that hit back (spells only)
// combatBotv3 spell clear: a ranged champion whose damage spell reaches beyond monster_range clears a
// camp's attacking monsters alone with spells only, from tiles outside their reach (no ally within
// spell_clear_ally_radius of the camp; with one there, the usual nearest-tanks play).
inline constexpr bool spell_clear_camps = false;
inline constexpr int spell_clear_ally_radius = 4;
// ...and as a scored option: camp scoring also prices clearing alone by spells (no damage, no party,
// spell speed) and takes it when it beats the usual fight.
inline constexpr bool spell_clear_option = false;  // patch: off (user): elks reach 4 like Stellar's spells, so edge casting is no longer damage-free
inline constexpr float spell_clear_far_frac = 0.4f;   // ...Stellar only, camps at least this x map size from our spawn
inline constexpr float spell_clear_quiet_bonus = 1.5f; // ...and x this in a quiet game (see below)
inline constexpr bool spell_clear_force_quiet = false; // patch: off with spell_clear_option (user). Before: yBot: off (see stellar_steal_large); ...in a quiet game on a large map (side >= large_map_side) Stellar always takes her best far camp by spells (user; expanse +150, other maps -31 overall)
inline constexpr float spell_clear_force_bonus = 3.0f;   // ...scoring it x this
inline constexpr bool spell_clear_near_side = true;     // ...cast from the spell-range tiles nearest our spawn (within 1 of the nearest)
// Steal mode (user): on large maps Stellar goes for the far camps from stellar_steal_round, quiet or
// not; near camps score x stellar_steal_near_factor for her.
inline constexpr bool stellar_steal_large = false; // patch: off with spell_clear_option (user). Before: yBot: off (expanse: 180 rounds for one far elk capture; Fish farms the rotation)
inline constexpr int stellar_steal_round = 0;
inline constexpr float stellar_steal_near_factor = 0.05f;
inline constexpr int steal_back_off_radius = 8;    // stealing and an enemy champion this close: run back toward our side...
inline constexpr int steal_back_off_rounds = 20;   // ...and no steal for this many rounds
inline constexpr bool spell_clear_hit_clusters = false; // spell clears hit the group's clusters between spells (off: it wakes the elks; expanse +106 vs +300, Stellar deaths 1.82 vs 0.03)
// Quiet game: from quiet_min_round, no death seen yet and no enemy champion within quiet_enemy_radius
// of us for quiet_rounds (both teams just farming their own sides).
inline constexpr int quiet_min_round = 100;
inline constexpr int quiet_enemy_radius = 10;
inline constexpr int quiet_rounds = 60;
inline constexpr float invade_no_melee_factor = 0.1f;
inline constexpr bool wards = false;                  // place a ward on the halfway line when crossing it
// ---- rangeBot: ranged champions' fights
inline constexpr bool ranged_hold_range = true;   // an enemy champion in range: hit it instead of walking to the chosen one
inline constexpr bool ranged_max_range = true;    // approach to a tile at full range, out of enemy melee reach when there is one
inline constexpr bool ranged_fight_needs_front = true;  // combatBotv3: in a fight with no melee ally near the target, ranged do not walk in (shoot what is in range, fall back)
inline constexpr bool ranged_front_line = true;   // never stand nearer the enemy than our nearest melee ally in the fight
inline constexpr int ranged_front_radius = 6;     // melee allies this close to the fight form the front line
inline constexpr int ranged_front_slack = 1;      // ...and we may stand this many tiles nearer the enemy than it
// combatBotv3: Cassi & Issac are one pick and die together, so each of them is two lives (live,
// 50 games of v4: 122 pair deaths, Cassi falling first in 84; eight 22 vs their 6).
// combatBotv3: peel back. Live (v4, 50 games): 282 of our 341 deaths to champions came in even or
// better numbers, with ~1.8 allies within 4, from above 60% to dead in a median 4 rounds: burst
// down beside teammates. A champion losing health that fast steps back behind its allies.
// combatBotv3: target choice. Live (v4): with 2+ enemies in reach, they hit the lowest-health one 63%
// of the time, we 47% (our chosen / team target outranked it). 0: as before; 1: an enemy we can
// kill within finish_hits hits comes first; 2: always the lowest health in reach.
// combatBotv3: far spawns, few picks: fights are rare, farming decides. With 3 picks and spawns at
// least this far apart, draft Varg (farmer) in Ren's place (expanse live: 0-28; the teams that
// beat us without a single death drafted Cassi & Issac, Stellar, Varg). 0: off.
inline constexpr int far_spawn_farmer_dist = 50;
// Maps cut up by ditches and walls: Heart of Ruin (2-tile dash) is drafted in Varg's place.
inline constexpr bool separated_heart = true;
inline constexpr float separated_draft_share = 0.4f;  // ...at least this share of our-side camps separated (mapplan Features)
inline constexpr float separated_detour = 1.6f;       // ...a walk this x the straight-line distance counts as walled off
// combatBotv3: with this much fuel left at farm scoring, score at least min_scored_rich camps in full
// (not stopping at the first bound below the best): far but richer camps get a real score (expanse:
// the wolf pair 37 tiles out was never scored in 323 traced turns). 0: off.
inline constexpr unsigned long long rich_fuel = 0;  // 80000 tested on expanse: no change (farm options are rarely rescored; g22 never in the top bounds): off
inline constexpr int min_scored_rich = 3;
inline constexpr int target_mode = 0;  // 1 tested +496 vs +507 (weakest picks unchanged), 2: +371: off
inline constexpr float finish_hits = 2.0f;
inline constexpr bool peel_back = false;  // tested vs loopBot: +450 vs +507, deaths 202 vs 191 (swirl, monolith, rushbait worse): off
inline constexpr float peel_drop = 0.25f;     // lost this share of max health over the last 2 rounds...
inline constexpr float peel_max_hp = 0.6f;    // ...and below this health (tanks: peel_tank_hp)
inline constexpr float peel_tank_hp = 0.4f;
inline constexpr int peel_enemy_radius = 4;   // with an enemy champion this close
inline constexpr bool pair_lives = true;
inline constexpr int pair_front_slack = 1;        // Cassi's slack ahead of our front (= ranged_front_slack: off; -1, a tile behind it, tested worse)
inline constexpr float pair_risk = 1.5f;          // flee / retreat judge time to death this much shorter for either of them
inline constexpr float pair_heal_margin = 0.0f;   // and heal urgency dominates this much higher (0.1 tested neutral: off)
inline constexpr int stellar_standoff = 4;            // yBot: Stellar's fight tiles nearer an enemy than this cost (0: off)
inline constexpr float stellar_standoff_weight = 1.5f;  // ...this x D per tile nearer
inline constexpr bool ranged_kite = false;        // alone with an enemy melee near: step away from it toward our melee or spawn (off: ranged deaths 68 -> 91 vs routeBot5)
inline constexpr int ranged_kite_from = 2;        // ...when it is this close (2: it could step in and hit us this turn)
inline constexpr int ranged_kite_ally_radius = 3; // a melee ally this close means we are not alone: keep shooting
inline constexpr bool ranged_hold_always = true;  // the same hold and front rules on farm walks, regroups, hunts and idling
inline constexpr bool ward_any_bush = true;           // ward any hidden bush in placement range, whichever way we move
inline constexpr unsigned long long ward_table_fuel = 50000;  // combatBotv2: build the bush tables only with this much fuel left
// combatBotv3: ranged exposure gradient (nav/navigator.cpp exposure()): stepping with an enemy seen
// within exposure_radius in the last exposure_ghost_age rounds, a ranged champion picks the least
// exposed of the steps that make progress (4 = one step of progress).
inline constexpr bool outnumbered_ranged_pair = false;  // combatBotv3: fewer of us alive: ranged need a teammate for a monster camp
inline constexpr bool ranged_scout_ward = false;        // combatBotv3: ranged farming alone, nothing in sight: ward toward their spawn
inline constexpr int enemy_side_min_party = 0;   // combatBotv3: camps nearer their spawn than ours need this many of us (0: off)
inline constexpr float ranged_contested_factor = 1.0f;  // combatBotv3: ranged, camp not clearly ours and risky (enemy seen / bush beside): x this
inline constexpr bool contested_all = true;             // ...for every champion (ranged only split the team: swirl deaths 14 -> 21)
inline constexpr float own_side_ratio = 0.8f;           // ...clearly ours: our spawn distance at most this x theirs
inline constexpr int contested_seen_rounds = 30;        // ...risky: an enemy seen near it this recently
inline constexpr bool ranged_exposure = true;
inline constexpr int exposure_radius = 12;
inline constexpr unsigned long long exposure_table_fuel = 60000;
inline constexpr bool exposure_unseen_only = true;  // bushes we can see into do not count
inline constexpr bool melee_cover = true;           // melee: tie-break toward standing in front of our ranged
inline constexpr int melee_cover_weight = 3;        // ...per tile not ahead of our nearest ranged ally
inline constexpr int melee_cover_radius = 6;        // ...ranged allies this close
inline constexpr bool exposure_hp_scale = false;   // exposure weighs more when hurt (x1 full .. x3 near death)
inline constexpr int exposure_spread = 0;          // ranged: per other ranged ally adjacent to the tile (0: off)
inline constexpr int exposure_no_backtrack = 2;    // stepping back onto last round's tile (0: off)
inline constexpr bool exposure_stand = true;        // ranged: in range already, step to a clearly safer tile still in range
inline constexpr int exposure_step_gain = 3;        // ...at least this much less exposed  // build the bush-distance table only with this much fuel left
inline constexpr int exposure_ghost_age = 3;
inline constexpr int exposure_front_weight = 3;   // per tile nearer the enemy than our nearest melee
inline constexpr int exposure_bush_weight = 2;    // per tile closer than 3 to a bush (not standing in one)
inline constexpr bool ward_fail_tile_only = true;
// combatBotv3: a champion moves its one ward on to a hidden bush at least this far from where it
// put the last one. Ranged champions gain most from the vision (range in fights, seeing who waits
// in a bush before walking past it); melee can dash or tank out (user). Was 3 for everyone.
inline constexpr int ward_respace_ranged = 1;
inline constexpr int ward_respace_melee = 3;     // combatBotv3: a failed bush ward skips that tile only (was: 3 tiles around it, as if ours)
inline constexpr bool ward_bushes = true;             // moving toward an unseen bush: one ward on it (wards last forever; cooldown from the engine: 10 rounds this patch)
inline constexpr int ward_line_width = 1;            // halfway line: |dist to our spawn - dist to theirs| <= this
inline constexpr bool ward_once = false;             // one ward per champion, then stop trying
inline constexpr int enemy_react_radius = 0;         // ignore enemies further than this from us (0: react to all we see)
inline constexpr int support_check_rounds = 3;       // a caller still above support_hp after this many rounds stops calling (0: off)
inline constexpr float support_hp = 0.67f;
inline constexpr bool combat_call = true;            // a champion fighting enemy champions (anywhere) calls allies too
inline constexpr bool call_on_several = true;        // seeing 2+ enemies (anywhere) also calls allies
// Chaining camps: after a camp we worked dies, its nearest camps (mapplan::kNeighbours, by walking
// distance from the draft) are worth more for a while.
inline constexpr float chain_bonus = 1.5f;
inline constexpr int chain_rounds = 30;              // how long the bonus lasts
inline constexpr bool capture_handoff = true;        // one party member captures, the rest move on
inline constexpr float handoff_factor = 0.1f;        // capture score when a teammate claiming it is nearer its tiles
inline constexpr float capture_taken_factor = 0.01f; // capture score when a teammate already stands on its tiles (one capturer is enough)
inline constexpr bool retreat_follow = false;       // rangeBot: an ally beside us retreating from enemies: retreat with it
inline constexpr bool retreat_hold = false;         // rangeBot: allies beside us still fighting: our predicted retreat x retreat_hold_factor while healthy
inline constexpr int retreat_team_radius = 6;       // ...allies this close count
inline constexpr float retreat_hold_factor = 0.3f;
inline constexpr float retreat_hold_hp = 0.5f;
inline constexpr bool stand_together = false;       // allies here match the enemies: flee scores x stand_flee_factor...
inline constexpr float stand_min_hp = 0.3f;         // ...while above this health
inline constexpr float stand_flee_factor = 0.3f;
inline constexpr bool flee_home_weighted = true;    // flee steps: home distance + flee_reach_cost per enemy/monster reach
inline constexpr int flee_reach_cost = 6;           // ...(steps of home distance one enemy's reach is worth)
inline constexpr int flee_dash_gain = 1;            // flee home: dash when it beats the best step by this much (was 2)
inline constexpr bool flee_dash_then_walk = true;   // flee home: walk on after a dash in the same turn
inline constexpr int flee_bush_bonus = 4;           // flee home: a bush tile is worth this many steps (0: off)...
inline constexpr int flee_bush_chase = 8;           // ...while the nearest enemy is 3 to this many tiles away
// combatBotv3: walk around unseen bushes in the middle of the map (ambush spots): the bush and
// this many tiles around it are avoided while a route around costs at most centre_bush_detour extra
// steps; carrying centre_bush_gems or more, one tile wider and a longer detour is accepted.
inline constexpr int centre_bush_ring = -1;         // -1: off (A/B: 3 detours a game, deaths near bushes not lower, -26 banked)
inline constexpr float centre_bush_region = 0.35f;  // middle: within this fraction of the half-width/height from the centre
inline constexpr int centre_bush_detour = 4;
inline constexpr int centre_bush_gems = static_cast<int>(20 * gem_scale);
inline constexpr int centre_bush_detour_gems = 8;
inline constexpr bool flee_homeward = true;         // fleeing: equally quick escapes go toward home; else walk home
inline constexpr bool tank_opens = true;            // ranged hold fire on a sleeping camp until the tank is beside it...
inline constexpr int tank_open_radius = 15;         // ...if a tank claiming it is this close to the camp...
inline constexpr int tank_open_wait = 6;            // ...for at most this many turns
inline constexpr bool capture_by_health = true;
inline constexpr float capture_home_hp = 0.5f;      // below this health a champion is heading home: it keeps the capture     // loopBot: the lowest-health member captures (then heals and shops)
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
// ---- xBot: tile-scoring micro (micro/micro.cpp). Every term is in units of our own damage per
// turn (D); the tile with the best score is played.
inline constexpr bool micro_ranged = true;          // ranged fight / hold movement by tile scoring (off: combatBotv3 rules)
inline constexpr int micro_radius = 3;              // ...with an enemy champion within our range + this
inline constexpr float micro_target_bonus = 0.2f;   // shooting the chosen (or team) target: x(1 + this)
inline constexpr float micro_next_shot = 0.3f;      // a tile we can shoot from next turn: this x D
inline constexpr float micro_taken = 0.6f;          // per point of damage per turn we can take on the tile
inline constexpr float micro_hurt_scale = 1.5f;     // ...x(1 + this x missing health fraction)
inline constexpr float micro_shared = 0.5f;         // an enemy with one of our allies nearer it: its damage x this
inline constexpr float micro_stunned = 0.3f;        // a stunned enemy's damage x this
inline constexpr float micro_front = 0.75f;         // per tile nearer the enemy than our front line, x D
inline constexpr float micro_bush = 0.25f;          // per tile closer than 3 to a bush we cannot see into, x D
inline constexpr float micro_backtrack = 0.15f;     // stepping back onto last round's tile, x D
inline constexpr float micro_approach = 0.4f;       // engage, nothing in range: per tile toward the target, x D
inline constexpr float micro_refuge = 0.4f;         // no melee ally near: per tile toward one (or spawn), x D
inline constexpr float micro_hold_gain = 0.5f;      // hold: step only when a neighbour beats our tile by this x D
inline constexpr bool micro_melee = false;          // melee fights in contact by tile scoring (approach and dashes stay with the fight code)
inline constexpr float micro_cover = 0.3f;          // melee: per tile not in front of our nearest ranged ally, x D
inline constexpr float micro_melee_outnumbered = 0.3f;  // melee: per enemy more than us able to hit the tile, x D (a cost, not a veto)
inline constexpr float micro_leash = 0.5f;          // melee: per tile further ahead of our other melee than safe_dash_slack, x D
inline constexpr bool micro_tiers = true;           // safety tiers first (lethal < outnumbered < free hit < safe), score second
inline constexpr float micro_lethal_turns = 2.0f;   // lethal: the damage we can take there kills us within this many turns
inline constexpr float micro_edge = 0.05f;          // in range: per tile of distance from the nearest enemy (up to our range), x D      // hold: step only when a neighbour beats our tile by this x D

inline constexpr bool trace_fuel_profile = false;  // add per-section fuel to detailed traces (debugging)   // turns between full traces when the decision is unchanged

} // namespace tune
