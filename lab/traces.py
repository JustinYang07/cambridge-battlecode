#!/usr/bin/env python3
"""Summarise the decision traces inside a replay bundle from the terminal.

  python3 lab/traces.py replay.repl26a                  # overview for both teams
  python3 lab/traces.py replay.repl26a --team a --slot 0 --rounds 60-110   # turn-by-turn
  python3 lab/traces.py replay.repl26a --deaths          # what each champion did before dying
"""
from __future__ import annotations

import argparse
import collections
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from labkit import replays  # noqa: E402


def overview(side: str, team: dict) -> None:
    turns = team["turns"]
    print(f"team {side.upper()}: {len(turns)} traced turns, {len(team['details'])} full traces, "
          f"{team['stdoutBytes'] / 1024:.0f} KiB stdout")
    if not turns:
        if team["other"]:
            print("  (no basicBot traces; first stdout lines:)")
            for o in team["other"][:5]:
                print("   ", o["text"][:120])
        return
    for d in team["drafts"]:
        print(f"  draft pick {d.get('pick')}: {d.get('chosen')}")
    kinds = collections.Counter(t["k"] for t in turns)
    print("  decisions:", ", ".join(f"{k} {n}" for k, n in kinds.most_common()))
    fuel = sorted(t["fuel"] for t in turns)
    over = sum(f > 100_000 for f in fuel)
    print(f"  fuel: median {fuel[len(fuel) // 2]}, p95 {fuel[int(len(fuel) * .95)]}, max {fuel[-1]}, "
          f"{over} turns over 100k")
    deaths = death_events(turns)
    print(f"  deaths: {len(deaths)}")
    lost: dict[int, int] = {}
    for t in turns:
        lost[t["u"]] = max(lost.get(t["u"], 0), t.get("lost", 0))
    print(f"  turns lost to fuel overruns: {sum(lost.values())} ({', '.join(f'u{u}:{n}' for u, n in sorted(lost.items()))})")


def fuel_report(side: str, team: dict) -> None:
    """Aggregate the "section 12k ..." notes the bot writes into full traces."""
    import re
    import statistics
    per: dict[str, list[float]] = collections.defaultdict(list)
    splits = total = 0
    for d in team["details"]:
        note = d.get("note", "")
        found = re.findall(r"([a-z.]+) (\d+)k", note)
        if not found:
            continue
        total += 1
        splits += "SPLIT" in note
        for name, k in found:
            per[name].append(int(k))
    gaps = collections.Counter()
    last: dict[int, int] = {}
    for t in team["turns"]:
        if t["u"] in last:
            gaps[t["r"] - last[t["u"]]] += 1
        last[t["u"]] = t["r"]
    acted = sum(gaps.values())
    print(f"team {side.upper()}: {splits}/{total} profiled turns split; "
          f"{gaps.get(1, 0) / max(acted, 1):.0%} of traced turns came one round after the previous")
    for name, vals in sorted(per.items(), key=lambda kv: -statistics.median(kv[1])):
        print(f"  {name:<16} median {statistics.median(vals):>5.0f}k  p90 {sorted(vals)[int(len(vals) * .9)]:>4}k  max {max(vals):>4}k")


def death_events(turns: list[dict]) -> list[tuple[int, int, dict]]:
    events, prev = [], {}
    for t in turns:
        p = prev.get(t["u"])
        if p and p["k"] != "dead" and t["k"] == "dead":
            events.append((t["u"], t["r"], p))
        prev[t["u"]] = t
    return events


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("replay", type=Path)
    parser.add_argument("--team", choices=("a", "b"))
    parser.add_argument("--slot", type=int)
    parser.add_argument("--rounds", help="e.g. 60-110")
    parser.add_argument("--deaths", action="store_true")
    parser.add_argument("--details", action="store_true", help="also print full option breakdowns")
    parser.add_argument("--fuel", action="store_true", help="fuel profile per section and split-turn rate")
    args = parser.parse_args()

    bundle = replays.read_bundle(args.replay)
    sides = [args.team] if args.team else ["a", "b"]
    lo, hi = (map(int, args.rounds.split("-")) if args.rounds else (0, 10 ** 9))

    for side in sides:
        team = bundle["teams"][side]
        if args.fuel:
            fuel_report(side, team)
            continue
        if args.slot is None and not args.rounds and not args.deaths:
            overview(side, team)
            continue
        if args.deaths:
            for slot, rnd, last in death_events(team["turns"]):
                print(f"team {side} slot {slot} died round {rnd}; last: hp {last['hp']} gems {last['gm']} "
                      f"{last['k']} {last.get('label', last['t'])} | {last['act']}")
            continue
        details = {(d["u"], d["r"]): d for d in team["details"]}
        for t in team["turns"]:
            if (args.slot is not None and t["u"] != args.slot) or not lo <= t["r"] <= hi:
                continue
            print(f"r{t['r']:>4} u{t['u']} ({t['x']},{t['y']}) hp {t['hp']:>5} gems {t['gm']:>3} "
                  f"{t["k"]:<8} {t["s"]:>6} {t["label"]:<22} {t["act"]}")
            d = details.get((t["u"], t["r"]))
            if args.details and d:
                for o in d["opts"]:
                    factors = " ".join(f"{n}{'×' if op == '*' else ('+' if op == '+' else ' t')}{v:g}"
                                       for n, v, op in o["f"])
                    print(f"        {o['k']:<8} {o['s']:>7.1f} {o['label']:<22} {factors}  {o.get('n', '')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
