# CAMBC Lab

Team tooling for Cambridge Battlecode 2026a: A/B testing, a decision explorer for our
bots, map previews, and the live ladder and replay scraping. Python standard library
only. The frontend is plain JS modules with no build step.

## Quick start

```bash
python3 -m venv .venv && .venv/bin/pip install cambc   # once per machine (the lab finds .venv/bin/cambc)
.venv/bin/cambc init --yes .                           # SDK, maps, starters (already done in this repo)
python3 lab/server.py                                  # http://localhost:8765
```

To share with teammates on the same network:

```bash
python3 lab/server.py --share        # prints http://<host>:8765/#key=…  Send them that link.
```

Each teammate can also run their own copy from the repo. Lab data lives in `lab/data/`,
which is git-ignored (experiments, replays, scraped games, platform session).

## Pages

| Page | What it is for |
|---|---|
| **Dashboard** | Latest experiment, bots, the live ladder top 10 |
| **A/B Lab** | Run a candidate bot against opponents on chosen maps, both sides. Snapshot a bot (from the working tree or a git ref) so the next edit can be tested against it. |
| **Experiment** | Live progress, win/loss and average gem difference, a map × side matrix (click a cell to open the replay), per-champion stats, and comparison against an earlier experiment |
| **Replays** | Every lab game, every scraped replay, and anything in `replays/` |
| **Decision explorer** | A per-champion timeline of decisions (colour = option kind, white line = health, red tick = turn lost to fuel). For any round it shows the options the bot weighed and the points behind each score. Includes the embedded official visualiser, draft reasoning and raw logs. |
| **Maps** | Terrain, camps, altars and constants for the map pool |
| **Platform** | Live ladder, connecting your account, replay scraping (ticked teams or the latest games from everyone), scouting of drafts and records, and an API explorer |

## How decisions are scored

Each turn the bot's brain (`bots/basicBot/src/brain/`) builds **options** (farm a camp
group, capture, bank, heal, flee, fight, idle). Each option is a list of **factors**:

- `+` reward in gem-equivalents (gems, gold × gold value, an enemy's bag)
- `t` turns it will take (travel, kill time, capture, respawn wait, …)
- `×` multipliers (our share of a party kill, an ally already claiming the camp, danger, …)

`score = Σ(+) ÷ Σ(t) × 100 × Π(×)`, i.e. expected gem-equivalents per 100 turns. The
explorer shows exactly these numbers, so "why did Ryth walk away from that elk?" is a
lookup, not a guess. All tuning constants live in `bots/basicBot/src/core/config.h`.

## Command line

```bash
python3 lab/abtest.py basicBot starter-cpp                 # all maps, both sides
python3 lab/abtest.py basicBot snap-basicBot-v1 --maps lanes,rows
python3 lab/abtest.py --snapshot basicBot --label v1       # freeze current source
python3 lab/abtest.py --snapshot basicBot --git HEAD~1     # freeze an older commit
python3 lab/traces.py <replay.repl26a>                     # decisions, fuel, deaths overview
python3 lab/traces.py <replay> --team a --slot 0 --rounds 100-160 --details
python3 lab/traces.py <replay> --deaths
```

## Things to know

- **Games are deterministic.** Identical code on the same map and side gives an identical
  game, so an A/B run is one game per (map, side, opponent). Small code changes still
  ripple chaotically: treat ±2–3 wins in 18 games as noise and watch the average gem
  difference and per-map patterns.
- **Fuel.** About 1 fuel per WASM instruction and roughly 97k per turn. A turn that overruns
  is suspended and swallows the next turn. The explorer marks lost turns in red, and the
  bot's fuel rules are in `core/fuel.h` and the memory notes.
- **Trace format.** Writing stdout and formatting text costs fuel, and the engine keeps about
  1 MiB of stdout per team, so the bot writes a compact, self-describing encoding
  (`core/trace.h`). `labkit/replays.py` decodes it.
- **Scraping.** Needs a signed-in session. Copy the `event_session` cookie from your browser
  (DevTools → Application → Cookies → 2026a.battlecode.co.uk) into the Cookie box on the
  Platform page. It is stored only in `lab/data/platform.json` (mode 600, git-ignored) and
  sent only to the event site. Sessions expire, so paste a fresh one if scraping reports
  "not signed in". The scraper walks `/api/v1/match-groups?scope=all[&team=…]` (newest
  first, paging with `before=<createdAt>_<id>`), opens each finished group, and downloads
  `/api/v1/matches/<id>/bundle`. Platform bundles contain only the replay, with no stdout,
  so other teams' games open in the official visualiser. The match statistics (drafts,
  gems, kills, items) feed the scouting table.

## Layout

```
lab/
  server.py          start the website
  abtest.py          A/B tests from the terminal
  traces.py          trace summaries from the terminal
  labkit/            config, runner (matches/experiments), jobs, maps (via bots/tool-mapdump),
                     replays (trace decoder), platform (ladder/scraper), server (HTTP API)
  web/               index.html, app.css, js/{app,ui,mapview}.js, js/pages/*.js
bots/tool-mapdump    helper bot that prints map data for the lab
bots/tool-fuelprobe  helper bot that measures SDK fuel costs
```
