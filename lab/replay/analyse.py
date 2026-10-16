#!/usr/bin/env python3
"""Per-team metrics from replays, decoded with the official engine (lab/replay/decode.mjs).

Usage:
  python lab/replay/analyse.py <bundle.zip|replay.repl26a> [...]          # print metrics per game
  python lab/replay/analyse.py --json out.json <files...>                 # also save them

Each game yields one record per team (0 = side A, 1 = side B): result, banked gems over time,
kills, deaths, time dead, gold earned, gems collected, tributes, bag size at tribute, time spent
on the enemy half, grouping, items, wards. Decoded per-round JSON is cached next to the input
(<name>.rounds.json), so re-running is cheap.
"""
from __future__ import annotations

import argparse
import io
import json
import math
import statistics
import subprocess
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TYPES = {0: "Ryth", 1: "Vesper", 2: "Vendetta", 3: "Cassi", 4: "Issac", 5: "Stellar", 6: "Prism", 7: "Marcella",
         8: "Willow", 9: "Ren", 10: "Varg", 11: "Tavi"}


def decode(path: Path) -> dict:
    cache = path.with_suffix(path.suffix + ".rounds.json")
    if cache.exists():
        d = json.loads(cache.read_text())
        if d.get("version") == 3:
            return d
    if path.suffix == ".zip" or zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as z:
            raw = z.read("replay.repl26a")
        tmp = cache.with_suffix(".tmp.repl26a")
        tmp.write_bytes(raw)
        src = tmp
    else:
        src = path
    try:
        subprocess.run(["node", str(HERE / "decode.mjs"), str(src), str(cache)], check=True, capture_output=True, text=True)
    finally:
        if src != path:
            src.unlink(missing_ok=True)
    return json.loads(cache.read_text())


def team_metrics(d: dict, team: int) -> dict:
    rounds = d["rounds"]
    if not rounds:
        return {}
    first = rounds[0]
    # Spawn centres: each team's champion positions at the start.
    def centre(tm):
        pts = [(c["x"], c["y"]) for c in first["champions"] if c["team"] == tm and c["x"] >= 0]
        if not pts:
            for r in rounds[:30]:
                pts = [(c["x"], c["y"]) for c in r["champions"] if c["team"] == tm and c["x"] >= 0]
                if pts:
                    break
        return (sum(p[0] for p in pts) / len(pts), sum(p[1] for p in pts) / len(pts)) if pts else (0, 0)
    ours_c, theirs_c = centre(team), centre(1 - team)
    cheb = lambda a, b: max(abs(a[0] - b[0]), abs(a[1] - b[1]))

    deaths = 0
    alive_rounds = dead_rounds = their_half = 0
    gold_earned = collected = tributes = 0
    bag_at_tribute = []
    together = []          # allies within 5 tiles of each alive champion, averaged
    grouped_rounds = 0     # rounds where every alive champion is within 6 of the team centroid
    prev = {}
    reach = {}
    first_kill = first_death = None
    items_at = {}
    for r in rounds:
        champs = [c for c in r["champions"] if c["team"] == team]
        live = [c for c in champs if c["alive"] and c["x"] >= 0]
        for c in champs:
            p = prev.get(c["i"])
            if p:
                if p["alive"] and not c["alive"]:
                    deaths += 1
                    first_death = first_death or r["round"]
                if c["gold"] > p["gold"]:
                    gold_earned += c["gold"] - p["gold"]
                if c["gems"] > p["gems"]:
                    collected += c["gems"] - p["gems"]
                if p["gems"] > 0 and c["gems"] == 0 and c["alive"] and p["alive"]:
                    tributes += 1
                    bag_at_tribute.append(p["gems"])
                if c["kills"] > p["kills"]:
                    first_kill = first_kill or r["round"]
            prev[c["i"]] = c
            if c["alive"]:
                alive_rounds += 1
                if c["x"] >= 0 and cheb((c["x"], c["y"]), theirs_c) < cheb((c["x"], c["y"]), ours_c):
                    their_half += 1
            else:
                dead_rounds += 1
        if len(live) >= 2:
            for c in live:
                together.append(sum(1 for o in live if o is not c and cheb((o["x"], o["y"]), (c["x"], c["y"])) <= 5))
            cx = sum(c["x"] for c in live) / len(live)
            cy = sum(c["y"] for c in live) / len(live)
            if all(cheb((c["x"], c["y"]), (cx, cy)) <= 6 for c in live):
                grouped_rounds += 1
        g = r["gems"][team]
        for t in (250, 500, 750, 1000):
            if g >= t and t not in reach:
                reach[t] = r["round"]
        for t in (200, 400):
            if r["round"] == t:
                items_at[t] = sum(c["items"] for c in champs)
    last = rounds[-1]
    n_rounds = last["round"]
    total = alive_rounds + dead_rounds
    return {
        "rounds": n_rounds,
        "gems": last["gems"][team], "opp_gems": last["gems"][1 - team],
        "won": (d.get("outcome") == 1 and team == 0) or (d.get("outcome") == 2 and team == 1),
        "r250": reach.get(250), "r500": reach.get(500), "r750": reach.get(750), "r1000": reach.get(1000),
        "kills": sum(c["kills"] for c in last["champions"] if c["team"] == team),
        "deaths": deaths,
        "dead_share": dead_rounds / max(total, 1),
        "gold_per100": 100 * gold_earned / max(n_rounds, 1),
        "collected_per100": 100 * collected / max(n_rounds, 1),
        "tributes": tributes,
        "bag_at_tribute": statistics.mean(bag_at_tribute) if bag_at_tribute else None,
        "their_half_share": their_half / max(alive_rounds, 1),
        "allies_within5": statistics.mean(together) if together else None,
        "grouped_share": grouped_rounds / max(n_rounds, 1),
        "first_kill": first_kill, "first_death": first_death,
        "items200": items_at.get(200), "items400": items_at.get(400),
        "wards_end": last["wards"][team],
        "champions": next(([TYPES.get(c["type"], c["type"]) for c in r["champions"] if c["team"] == team]
                           for r in rounds if any(c["team"] == team for c in r["champions"])), []),
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--json")
    a = ap.parse_args()
    out = []
    for f in a.files:
        p = Path(f)
        try:
            d = decode(p)
        except subprocess.CalledProcessError as e:
            print(f"{p.name}: decode failed: {e.stderr.strip()[:200]}", file=sys.stderr)
            continue
        rec = {"file": p.name, "teams": [team_metrics(d, 0), team_metrics(d, 1)]}
        out.append(rec)
        t0, t1 = rec["teams"]
        print(f"{p.name[:12]} rounds {t0.get('rounds')} gems {t0.get('gems')}-{t1.get('gems')} "
              f"kills {t0.get('kills')}-{t1.get('kills')} deaths {t0.get('deaths')}-{t1.get('deaths')}")
    if a.json:
        Path(a.json).write_text(json.dumps(out, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
