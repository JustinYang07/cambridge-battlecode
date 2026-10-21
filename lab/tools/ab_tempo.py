#!/usr/bin/env python3
"""Tempo of the candidate against the opponent in an A/B experiment (lab/data/experiments/<id>):
camp clears, gold earned and monster damage per game for each side, from each game's stats.json.

usage: python3 lab/tools/ab_tempo.py <experiment dir or name>
"""
import collections, glob, json, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main() -> int:
    arg = sys.argv[1]
    d = Path(arg)
    if not d.exists():
        d = next(Path(p).parent for p in glob.glob(str(ROOT / "lab/data/experiments/*/experiment.json"))
                 if json.loads(Path(p).read_text()).get("name") == arg)
    exp = json.loads((d / "experiment.json").read_text())
    tot = collections.Counter()
    per_map = collections.defaultdict(collections.Counter)
    for g in exp["games"]:
        side = g["candidateSide"]
        gd = next((p for p in (d / "games").iterdir() if p.name.startswith(f"{g['idx']:03d}-")), None)
        if not gd or not (gd / "stats.json").exists():
            continue
        s = json.loads((gd / "stats.json").read_text())
        mine = "teamA" if side == "A" else "teamB"
        for e in s["events"]:
            if e["kind"] == "campClear":
                k = "clears " + ("cand" if e["owner"].get("team") == mine else "opp")
                tot[k] += len(e["entries"])
                per_map[g["map"]][k] += len(e["entries"])
        for c in s.get("champions", []):
            k = "cand" if c.get("team") == mine else "opp"
            m = c.get("metrics") or {}
            tot["gold " + k] += float(m.get("earnedGold", 0) or 0)
            tot["mon " + k] += float(m.get("monsterDamageDone", 0) or 0)
        tot["games"] += 1
        per_map[g["map"]]["games"] += 1
    n = max(tot["games"], 1)
    print(f"{exp['name']}: {n} games  clears/game cand {tot['clears cand'] / n:.1f} opp {tot['clears opp'] / n:.1f}  "
          f"gold/game cand {tot['gold cand'] / n:.0f} opp {tot['gold opp'] / n:.0f}  monster dmg/game cand {tot['mon cand'] / n:.0f} opp {tot['mon opp'] / n:.0f}")
    for m, c in sorted(per_map.items()):
        print(f"   {m:<24} clears cand {c['clears cand'] / c['games']:5.1f}  opp {c['clears opp'] / c['games']:5.1f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
