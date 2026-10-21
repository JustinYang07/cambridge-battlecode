#!/usr/bin/env python3
"""Where our gems go: per-team leak report for an A/B experiment, from each game's stats.json.

Usage:
  python3 lab/tools/leaks.py lab/data/experiments/<id>            # per map and overall
  python3 lab/tools/leaks.py lab/data/experiments/<id> --deaths   # also list every death

Per team (candidate = the experiment's bot, opp = its opponent), averaged per game:
  banked     gems tributed                      lostDeath gems dropped on death
  collected  gems picked up from captures       carried   gems still in bags at the end
  deathsC    deaths to enemy champions          deathsM   deaths to monsters
  fuel       turns lost to fuel exhaustion      idleGold  gold unspent at the end
  firstBank  round of the first tribute         gold      gold earned
  monDmg     monster damage taken               champDmg  enemy champion damage taken
"""
from __future__ import annotations

import argparse
import collections
import json
import statistics
import sys
from pathlib import Path

FIELDS = ["banked", "collected", "lostDeath", "carried", "deathsC", "deathsM", "fuel", "gold", "idleGold",
          "firstBank", "monDmg", "champDmg"]


def team_stats(d: dict, team: str) -> dict:
    s = collections.Counter()
    s["banked"] = int(next(t["submittedGems"] for t in d["teams"] if t["team"] == team))
    first = None
    for c in d["champions"]:
        if c["team"] != team:
            continue
        m = c["metrics"]
        s["collected"] += int(m["collectedGems"])
        s["carried"] += int(m["gems"])
        s["gold"] += int(m["earnedGold"])
        s["idleGold"] += int(m["gold"])
        s["monDmg"] += float(m["monsterDamageTaken"])
        s["champDmg"] += float(m["championDamageTaken"])
    for e in d["events"]:
        if e["owner"].get("team") != team or not e["entries"]:
            continue
        if e["kind"] == "death":
            for x in e["entries"]:
                s["deathsC" if x["context"] == "champion" else "deathsM"] += 1
        elif e["kind"] == "gemLoss":
            s["lostDeath"] += sum(int(x["value"]) for x in e["entries"])
        elif e["kind"] == "fuelExhausted":
            s["fuel"] += len(e["entries"])
        elif e["kind"] == "gemSubmission":
            r = min(x["timestamp"]["round"] for x in e["entries"])
            first = r if first is None else min(first, r)
    s["firstBank"] = first if first is not None else d["match"]["roundsCompleted"]
    return s


def deaths(d: dict, team: str) -> list[str]:
    out = []
    loss = {}
    for e in d["events"]:
        if e["owner"].get("team") == team and e["kind"] == "gemLoss":
            for x in e["entries"]:
                loss[(e["owner"]["name"], x["timestamp"]["round"])] = int(x["value"])
    for e in d["events"]:
        if e["owner"].get("team") != team or e["kind"] != "death":
            continue
        for x in e["entries"]:
            r = x["timestamp"]["round"]
            out.append(f"r{r:5d} {e['owner']['name']:<14} by {x['context']:<9} at ({x['x']},{x['y']}) lost {loss.get((e['owner']['name'], r), 0)} gems")
    return sorted(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("experiment", type=Path)
    ap.add_argument("--deaths", action="store_true")
    args = ap.parse_args()
    per_map: dict[str, dict[str, list]] = collections.defaultdict(lambda: {"cand": [], "opp": []})
    for g in sorted((args.experiment / "games").iterdir()):
        f = g / "stats.json"
        if not f.exists():
            continue
        d = json.loads(f.read_text())
        name = g.name.split("-", 1)[1]
        mp, side = name.rsplit("-", 1)
        mp = mp.rsplit("-s", 1)[0] if "-s" in mp and mp.rsplit("-s", 1)[1].isdigit() else mp
        cand = "teamA" if side.startswith("A") else "teamB"
        opp = "teamB" if cand == "teamA" else "teamA"
        per_map[mp]["cand"].append(team_stats(d, cand))
        per_map[mp]["opp"].append(team_stats(d, opp))
        if args.deaths:
            print(f"== {g.name}")
            for line in deaths(d, cand):
                print("  " + line)

    def avg(rows, k):
        return statistics.mean(r[k] for r in rows) if rows else 0.0

    head = f"{'map':<24}{'who':<5}" + "".join(f"{k:>10}" for k in FIELDS)
    print(head)
    allc, allo = [], []
    for mp in sorted(per_map):
        for who in ("cand", "opp"):
            rows = per_map[mp][who]
            print(f"{mp:<24}{who:<5}" + "".join(f"{avg(rows, k):>10.0f}" for k in FIELDS))
        allc += per_map[mp]["cand"]
        allo += per_map[mp]["opp"]
    print("-" * len(head))
    for who, rows in (("cand", allc), ("opp", allo)):
        print(f"{'ALL':<24}{who:<5}" + "".join(f"{avg(rows, k):>10.0f}" for k in FIELDS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
