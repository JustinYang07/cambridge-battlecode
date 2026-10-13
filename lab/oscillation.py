#!/usr/bin/env python3
"""Find champions stuck in loops, from an experiment's decision traces.

  python3 lab/oscillation.py "navBot2 antiflip6 vs basicBot"     # experiment name or id
  python3 lab/oscillation.py <experiment> --top 15

Two kinds of loop are reported:
  decision flip-flop  alternating between two decisions (A, B, A, B, ...) for 5+ rounds
  tile ping-pong      same decision, but stepping back and forth between two tiles for 5+ rounds
"""
from __future__ import annotations

import argparse
import collections
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from labkit import replays, runner  # noqa: E402


def runs(seq, key):
    """Yield (start index, length) of alternating runs: key(x[i]) == key(x[i+2]) != key(x[i+1])."""
    i = 0
    while i < len(seq) - 4:
        if key(seq[i]) != key(seq[i + 1]) and seq[i + 1]["r"] - seq[i]["r"] == 1:
            j = i
            while j + 2 < len(seq) and key(seq[j + 2]) == key(seq[j]) and seq[j + 2]["r"] - seq[j]["r"] == 2:
                j += 1
            n = j - i + 2
            if n >= 5:
                yield i, n
                i = j + 2
                continue
        i += 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("experiment", help="experiment name or id")
    parser.add_argument("--top", type=int, default=8)
    args = parser.parse_args()
    store = runner.ExperimentStore()
    matches = [e for e in store.list() if args.experiment in (e["id"], e.get("name"))]
    if not matches:
        parser.error(f"no experiment named {args.experiment!r}")
    exp = store.load(matches[0]["id"])
    flips, pongs = [], []
    pairs = collections.Counter()
    games = 0
    for g in exp["games"]:
        replay = (g.get("result") or {}).get("replay")
        if not replay:
            continue
        games += 1
        team = replays.read_bundle(runner.config.ROOT / replay)["teams"]["a" if g["candidateSide"] == "A" else "b"]
        by = collections.defaultdict(list)
        for t in team["turns"]:
            by[t["u"]].append(t)
        for u, seq in by.items():
            who = team["units"].get(u, f"slot {u}")
            for i, n in runs(seq, lambda x: (x["k"], x["t"])):
                a, b = seq[i], seq[i + 1]
                pairs[tuple(sorted((a["k"], b["k"])))] += n
                flips.append((n, g["map"], g["candidateSide"], who, a["r"], f"{a['k']} {a['label']}", f"{b['k']} {b['label']}"))
            alive = [x for x in seq if x["x"] >= 0]
            for i, n in runs(alive, lambda x: (x["x"], x["y"])):
                pongs.append((n, g["map"], g["candidateSide"], who, alive[i]["r"], f"{alive[i]['k']} {alive[i]['label']}"))
    games = max(games, 1)
    print(f"{exp['name']}: {games} games")
    print(f"decision flip-flop: {sum(f[0] for f in flips) / games:.1f} turns/game, {len(flips)} episodes; by pair {pairs.most_common(6)}")
    for f in sorted(flips, reverse=True)[: args.top]:
        print("   ", f)
    print(f"tile ping-pong: {sum(p[0] for p in pongs) / games:.1f} turns/game, {len(pongs)} episodes")
    for p in sorted(pongs, reverse=True)[: args.top]:
        print("   ", p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
