#!/usr/bin/env python3
"""A/B test a bot from the terminal.

Examples:
  python3 lab/abtest.py basicBot starter-cpp                 # all maps, both sides
  python3 lab/abtest.py basicBot starter-cpp snap-basicBot-v1 --maps corners,lanes
  python3 lab/abtest.py --snapshot basicBot --label v1       # freeze current source
  python3 lab/abtest.py --snapshot basicBot --git HEAD~1      # freeze an earlier commit
Results are saved under lab/data/experiments/ and show up in the web lab.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from labkit import config, runner  # noqa: E402


def print_report(exp: dict) -> None:
    summary = exp.get("summary") or runner.aggregate(exp)
    print(f"\n{exp['name']}  ({exp['id']}, commit {exp.get('gitCommit')})")
    print(f"{'map':<12}{'opponent':<24}{'side':<6}{'result':<8}{'gems':>11}{'round':>7}")
    for g in exp["games"]:
        r = g.get("result") or {}
        if "winner" not in r:
            print(f"{g['map']:<12}{g['opponent']:<24}{g['candidateSide']:<6}ERROR   {r.get('error', '')[:60]}")
            continue
        mine = 0 if g["candidateSide"] == "A" else 1
        verdict = "WIN" if r["winner"] == g["candidateSide"] else "loss"
        gems = f"{r['teams'][mine]['gems']}-{r['teams'][1 - mine]['gems']}"
        print(f"{g['map']:<12}{g['opponent']:<24}{g['candidateSide']:<6}{verdict:<8}{gems:>11}{r['rounds']:>7}")
    o = summary["overall"]
    if o["played"]:
        n = o["played"]
        print(f"\n{exp['candidate']}: {o['wins']}W {o['losses']}L of {n}, "
              f"avg gem diff {o['gemDiff'] / n:+.0f}, faults {o['faults']} | per game: deaths {o['deaths'] / n:.1f}, "
              f"killed by enemy {o['killedByEnemy'] / n:.1f}, gems lost {o['gemsLost'] / n:.0f}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("candidate", nargs="?")
    parser.add_argument("opponents", nargs="*")
    parser.add_argument("--maps", default="all", help="comma-separated map names, or 'all'")
    parser.add_argument("--one-side", action="store_true", help="only play the candidate as team A")
    parser.add_argument("--jobs", type=int, default=None)
    parser.add_argument("--rounds", type=int, default=None)
    parser.add_argument("--name", default="")
    parser.add_argument("--snapshot", metavar="BOT", help="copy BOT's source to bots/snap-BOT-<label>")
    parser.add_argument("--label")
    parser.add_argument("--git", metavar="REF", help="with --snapshot: take the source from a git commit")
    args = parser.parse_args()

    if args.snapshot:
        print(runner.snapshot(args.snapshot, args.label, args.git))
        return 0
    if not args.candidate or not args.opponents:
        parser.error("need a candidate and at least one opponent")

    maps = config.list_maps() if args.maps == "all" else args.maps.split(",")
    spec = runner.ExperimentSpec(args.candidate, args.opponents, maps, not args.one_side, args.name,
                                 rounds=args.rounds)
    store = runner.ExperimentStore()
    exp = runner.plan(spec)
    total = len(exp["games"])

    def progress(e: dict) -> None:
        done = sum(g["status"] in ("done", "error") for g in e["games"])
        print(f"\r{done}/{total} games", end="", flush=True)

    runner.execute(exp, store, args.jobs, progress)
    print()
    if exp.get("error"):
        print(exp["error"])
        return 1
    print_report(exp)
    return 0


if __name__ == "__main__":
    sys.exit(main())
