# Sneaky Bush Campers: what changed (notes, 17 Oct 2026)

Source: their ranked match groups from the event API, scraped 17 Oct (236 groups, 1,060 games since 15 Oct 00:00 UTC).
Times below are UTC; Sydney is UTC+11 (so 10:00 UTC = 9pm Sydney).
Raw data: `lab/data/scraped/sbc_groups.json`, `lab/data/scraped/sbc_games.json`.

## What the data can and cannot show

- **Available for every game:** map, length in rounds, result, opponent and opponent rating.
- **Only from about 16 Oct 04:00 UTC onward:** both teams' gem scores and drafts. Older match records have these fields empty, so gem pace before vs after the climb cannot be compared directly.
- **Not available at all:** per-champion stats (deaths, kills, gold, monster damage). They live in a binary `.camstat` summary file we can't decode yet. The replay bundles of their games hold no readable stats either.

## Timeline: the change is a step at ~10:00 UTC on 15 Oct (9pm Sydney)

| Period (UTC) | Sydney | Games | Win rate | Avg opponent rating | Rating |
|---|---|---|---|---|---|
| 15 Oct 00–05 | 11am–4pm | 150 | 55% | 1552 | ~1510 → 1565 |
| 15 Oct 05–10 | 4pm–9pm | 150 | 48% | 1543 | 1565 → ~1540 (flat / slightly down) |
| 15 Oct 10–16 | 9pm–3am | 180 | 68% | 1654 | 1543 → 1762 |
| 15 Oct 16 – 16 Oct 04 | 3am–3pm | 360 | 62% | 1834 | 1762 → ~1950 |
| 16 Oct 04 onward | 3pm onward | 220 | 66% | 1878 | ~1950 → ~2000 (rank 1) |

- **Per hour (old 9 maps only):** win rate was 40–57% in every hour from 04:00 to 09:00 UTC. From 10:00 UTC it was 67–77%.
- **Shorter wins:** wins got shorter at the same moment, from roughly 480–560 rounds to 420–520. They reach 1000 gems sooner.
- **Holds up against stronger teams:** after the jump they kept winning about 60–70% while opponents got much stronger (1540 → 1880 average). The improvement is large and real, not just an easy run of opponents.
- **Not the maps:** the map pool expansion (ambush, contest, eight, fragment, pilgrimage, rushbait_v2, stampede, swirl) arrived about 16 Oct 03:40 UTC, 18 hours after the jump. The climb happened on the old 9 maps.

Most likely they submitted a new bot version at about 10:00 UTC on 15 Oct. Opponents can't see submission versions, so this can't be confirmed directly.

## Win rate by map: 4–9pm Sydney vs after

| Map | Before (15 Oct 05–10 UTC) | After (10:00 UTC onward) |
|---|---|---|
| corners | 5/12 (42%) | 62/75 (83%) |
| rows | 8/20 (40%) | 50/67 (75%) |
| expanse | 15/17 (88%) | 55/70 (79%) |
| interlock | 4/14 (29%) | 47/72 (65%) |
| monolith | 10/20 (50%) | 44/64 (69%) |
| lanes | 10/20 (50%) | 50/80 (63%) |
| packs | 6/13 (46%) | 47/84 (56%) |
| elkring | 7/17 (41%) | 44/79 (56%) |
| collision | 7/17 (41%) | 27/63 (43%) |

- **Biggest gains:** corners, rows and interlock. Those are the maps where farming speed and control of the middle decide games.
- **Unchanged:** collision barely moved, and expanse was already strong.

## Farming pace now (16 Oct 04:00 UTC onward, where scores exist)

Median rounds to reach 1000 gems in games they won, vs our v16 (ranked games 16 Oct):

| Map | SBC rounds to 1000 | SBC win% | Our v16 rounds to 1000 | Our v16 win% |
|---|---|---|---|---|
| expanse | 291 | 82% | 396 | 100% (2 games) |
| rows | 348 | 100% | 463 | 80% |
| lanes | 399 | 56% | 395 | 33% |
| monolith | 426 | 70% | 614 | 33% |
| corners | 446 | 85% | never reached (3 games) | 0% |
| packs | 472 | 54% | 592 | 67% |
| elkring | 484 | 67% | 508 | 20% |
| swirl | 492 | 57% | never reached | 25% |
| interlock | 617 | 67% | 605 | 60% |
| fragment | 730 | 60% | 956 | 67% |
| pilgrimage | 768 | 47% | 1098 | 60% |
| stampede | 795 | 86% | 1228 | 100% |
| overall | **535** | 66% | **614** | 49% |

- **Faster farming:** where both teams win, they hit 1000 about 15–30% faster on most maps. Examples: expanse 291 vs 396, rows 348 vs 463, monolith 426 vs 614.
- **Gems they allow:** opponents average 713 gems against them, and 564 in their wins. They don't just out-farm; they keep the other team well short.
- **Corners and swirl:** we never reached 1000 on corners or swirl. They win corners 85% of the time.

## Drafts (16 Oct 04:00 UTC onward)

- **Stellar every game:** Stellar is picked in essentially every draft, almost always first or second.
- **First pick:** usually Cassi & Issac (71 of 125 drafts), otherwise Ren (35) or Ryth (13).
- **Common rosters:**
  - 5 picks: C&I > Stellar > Varg > Tavi > Ren (21), Ren > Stellar > C&I > Ryth > Varg (17), and C&I > Stellar > Willow/Prism > … (17).
  - 4 picks: Ren > Stellar > C&I > Ryth, Ryth > Stellar > Ren > Tavi, C&I > Stellar > Varg > Tavi/Vendetta.
  - 3 picks: C&I > Stellar > Varg (12), Ren > Stellar > Ryth (8).
- **Willow and Prism:** they show up in some 5-pick drafts. We never take either.
- **Pick order varies** between games, so they probably draft in response to the opponent's picks.

## What this suggests for us

1. **Farming throughput is the visible gap.** Even on maps where both teams win, they reach 1000 roughly 100 rounds sooner. Our time-budget work (walking about 40%, waiting, lost turns) is the right area.
2. **Corners, rows and interlock are where their new version gained most**, and where we struggle: 0% on corners. That points at early contest of the centre and fast centre farming.
3. **Draft:** Stellar is central to every roster they run; we draft Stellar only in 5-pick games. Testing Stellar in 3- and 4-pick drafts is cheap.
4. **Deaths, kills and gold are still unknown.** They need either the `.camstat` summary format decoded or replay positions read. That's worth doing if we want to see *how* they deny opponents (713 opponent gems on average).

## Replay analysis: before vs after the climb (decoded with the official engine)

How it was measured:
- **Tools:** replays decoded with `lab/replay/decode.mjs`, which runs the engine that ships with the cambc visualiser. Metrics come from `lab/replay/analyse.py`.
- **Games:** 45 games on the original 9 maps, picked at random. 21 are from 15 Oct 05:00–10:00 UTC (4–9pm Sydney) and 24 from 10:00–16:00 UTC (9pm–3am Sydney). 3 downloads failed.
- **Sample win rates:** 62% before and 58% after, against 48% and 68% for the full periods. The win rates in the sample are noise; the per-game behaviour is the useful part.

| Metric (per game, averages) | SBC before | SBC after | Opponents before | Opponents after |
|---|---|---|---|---|
| Round reaching 250 banked gems | 204 | **156** | 201 | 206 |
| Round reaching 500 | 343 | **281** | 348 | 328 |
| Round reaching 1000 (when reached) | 542 | **460** | 542 | 506 |
| Game length | 542 | 479 | | |
| Gems collected per 100 rounds | 201 | **226** | 191 | 193 |
| Gold earned per 100 rounds | 539 | 478 | 523 | 497 |
| Opponent banked gems | 871 | **776** | | |
| Kills | 3.1 | 3.5 | 4.1 | 3.3 |
| Deaths | 5.8 | **4.3** | 4.6 | 4.6 |
| Share of time dead | 11% | 8% | 9% | 10% |
| First death (round) | 137 | **185** | 165 | 163 |
| First kill (round) | 180 | 163 | 130 | 185 |
| Time on the enemy half | 8% | **14%** | 11% | 10% |
| Allies within 5 tiles (average) | 1.16 | 1.34 | 1.10 | **0.91** |
| Rounds with the whole team grouped | 18% | 20% | 15% | 12% |
| Tributes / bag at tribute | 24 / 42 | 25 / 40 | 21 / 47 | 19 / 44 |
| Items by round 200 / 400 | 9 / 20 | 10 / 21 | 9 / 21 | 11 / 22 |
| Wards up at game end | **0** | **3.9** | 2.2 | 3.2 |

What changed in their play:
1. **Faster gem farming from the start.** They reach 250 gems about 50 rounds sooner and 1000 about 80 rounds sooner (about 15%). Gems collected per 100 rounds rose 12% while gold earned fell 11%, so they now favour gem camps over gold.
2. **Fewer, later deaths.** Deaths dropped from 5.8 to 4.3 per game, and their first death comes about 50 rounds later. Kills rose only slightly (3.1 to 3.5), so it isn't a more aggressive bot. It loses less.
3. **Opponents get less.** Opponents bank about 95 fewer gems against them (871 to 776). The opponents' own grouping drops (1.10 to 0.91 allies within 5 tiles), which suggests SBC's play breaks them up or catches them apart.
4. **More forward and slightly tighter.** They spend 14% of their time on the enemy half, up from 8%. Allies within 5 tiles rose from 1.16 to 1.34.
5. **They started warding.** No wards were up at the end of any game before; afterwards about 4 per game. Wards probably feed both the safer farming and the extra time on the enemy half.
6. **Draft barely changed.** Cassi & Issac and Stellar are in almost every game, plus Varg/Tavi/Ryth. Willow shows up a bit more often after.

Note on our own ward test: wards made our bot worse (−220 gems/game vs v1.14) because our decision logic over-reacts to far-away sightings. SBC evidently uses ward vision well, so it's worth revisiting once we know what they do with it, for example avoiding enemies seen through wards instead of reacting to them.

## Behaviour patterns (lab/replay/patterns.py): SBC before vs after, and our v1.14

Same 45 SBC games as above; ours = 60 lab games of v1.14 (mostly against a near-copy of itself, so
our numbers are a rough guide). "Depth" = 0 at own spawn, 1 at theirs. Early = rounds 0-200.

| Metric | SBC before | SBC after | Our v1.14 |
|---|---|---|---|
| Captures by round 100 / 200 | 5.0 / 10.1 | 5.2 / **11.5** | 4.3 / 9.4 |
| Early captures that are gold-only | 0% | 1% | **13%** |
| First capture (round) | 32 | 28 | 29 |
| Champions at a capture (early / all) | 1.45 / 1.32 | 1.76 / 1.59 | 1.85 / 1.69 |
| Capture depth (all) / share past halfway | 0.38 / 10% | 0.35 / 11% | 0.35 / 11% |
| Tributes by round 200 | 7.9 | **9.6** | 6.0 |
| Bag at tribute (early / all) | 37 / 38 | 38 / 38 | 38 / 41 |
| Wards per game / first ward round | 0 / – | **3.6 / round 24** | – |
| Depth of the champion placing a ward | – | **0.30 (own side)** | – |
| Times outnumbered by enemies, per game | 3.7 | 3.3 | **5.8** |
| ...died within 10 rounds when outnumbered | 27% | 18% | **34%** |
| ...died within 10 rounds when numbers even | 9% | 7% | 12% |
| Closed in when ahead (2+ tiles in 5 rounds) | 26% | **15%** | 31% |
| Team kills within 10 rounds when ahead | 0.22 | 0.17 | 0.40 |
| Teammates within 6 at an encounter | 1.39 | 1.85 | 1.79 |

**Items:** after the change, their tanks (Issac, Ryth, Varg) went durability, then jungle emblem, then
a bruiser emblem of health, then a durability emblem. Their casters (Tavi, Ren, Cassi) went jungle,
then durability, then jungle emblem. Before the change they rushed power emblems. Our v1.14 build
(variant 12) is already essentially their new build.

What this says about their new version:
1. **More captures early, no gold detours.** They make 11.5 captures by round 200 against our 9.4, and almost none are gold-only (ours are 13%). They bank more often (9.6 tributes by round 200 against our 6.0) at the same bag size, because they simply collect more gems.
2. **Early wards on their own side, not on the halfway line.** The first goes down around round 24, placed from about 30% of the way to the enemy spawn. That looks like covering the approaches to their own camps, which fits being outnumbered less often.
3. **They avoid getting caught.** They're outnumbered 3.3 times per game against our 5.8. When it happens they die 18% of the time, against our 34%.
4. **They don't chase.** Ahead on numbers, they close in only 15% of the time (they did 26% before the change); we do 31%. They take fewer kills but lose fewer champions overall.
5. **Slightly more grouped at captures** than before (1.76 champions early, up from 1.45), now about where we already are.

Candidate changes to test on a clean base. The old opening-rush test ran on a regressed build, so it needs redoing:
- skip gold-only camps early (we take 13%, they take about none);
- early wards on our own approaches (depth around 0.3), with ward sightings used only for avoiding enemies;
- avoid being outnumbered: treat seeing more enemies than allies near us as a reason to rotate away early;
- no chasing when ahead: drop the bonus for closing distance on enemies that are moving away.
