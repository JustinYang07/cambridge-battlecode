#!/usr/bin/env python3
"""One line per game of macro numbers for both teams (folder layout as autopsy.py): camp clears
(and how many of the enemy's were in our half), deaths, gold earned by round 600, champion-turns
idle (alive, no action), and average spread (mean distance between living teammates).

usage: python3 lab/tools/macro.py <dir> [<dir> ...]
"""
import collections, json, statistics, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import autopsy


def main() -> int:
    tot = collections.Counter()
    print(f"{'map':<22} {'score':>11} | clears us/them (theirs in our half) | deaths us/them | gold@600 us/them | idle us/them | spread us/them")
    for folder in map(Path, sys.argv[1:]):
        for sf in sorted(folder.glob("*.stats.json")):
            mid = sf.name[:-11]
            side = (folder / f"{mid}.side").read_text().strip()
            T = 0 if side == "teamA" else 1
            mp = (folder / f"{mid}.map").read_text().strip()
            s = json.loads(sf.read_text())
            g = autopsy.decode(folder / f"{mid}.repl26a")
            R = {x["r"]: x for x in g["rounds"] if x["champs"]}
            first = R[min(R)]["champs"]
            sp = {t: (sum(c[3] for c in first if c[1] == t) / 5, sum(c[4] for c in first if c[1] == t) / 5) for t in (0, 1)}
            acted = {(a[0], a[3]) for a in g["actions"] if a[2] == 1}
            n = collections.Counter()
            for e in s["events"]:
                who = "us" if e["owner"].get("team") == side else "them"
                for x in e["entries"]:
                    if e["kind"] == "campClear":
                        n["clears " + who] += 1
                        if who == "them" and autopsy.cheb((x["x"], x["y"]), sp[T]) < 0.8 * autopsy.cheb((x["x"], x["y"]), sp[1 - T]):
                            n["their clears in our half"] += 1
                    elif e["kind"] == "death" and e["owner"].get("kind") == "champion":
                        n["deaths " + who] += 1
            for t in s["championTimelines"]:
                if t["metric"] == "earnedGold":
                    n["gold " + ("us" if t["owner"]["team"] == side else "them")] += autopsy.cumulative(t["runs"], 600)[600]
            spread = {0: [], 1: []}
            idle = collections.Counter()
            for r in range(50, max(R), 5):
                if r not in R:
                    continue
                for t in (0, 1):
                    p = [(c[3], c[4]) for c in R[r]["champs"] if c[1] == t and c[7]]
                    if len(p) >= 2:
                        spread[t].append(statistics.mean(autopsy.cheb(a, b) for i, a in enumerate(p) for b in p[i + 1:]))
                    for c in R[r]["champs"]:
                        if c[1] == t and c[7]:
                            idle[(t, "alive")] += 1
                            idle[(t, "idle")] += (r, c[0]) not in acted
            sc = [x["submittedGems"] for x in s["teams"]]
            us_s, them_s = (sc[0], sc[1]) if T == 0 else (sc[1], sc[0])
            iu = idle[(T, "idle")] / max(idle[(T, "alive")], 1)
            it = idle[(1 - T, "idle")] / max(idle[(1 - T, "alive")], 1)
            print(f"{mp:<22} {us_s:>5}-{them_s:<5} | {n['clears us']:4d}/{n['clears them']:<4d} ({n['their clears in our half']:3d})            | "
                  f"{n['deaths us']:3d}/{n['deaths them']:<3d}        | {n['gold us']:6.0f}/{n['gold them']:<6.0f}   | {iu:4.0%}/{it:<4.0%}    | "
                  f"{statistics.mean(spread[T] or [0]):4.1f}/{statistics.mean(spread[1 - T] or [0]):4.1f}")
            for k in ("clears us", "clears them", "their clears in our half", "deaths us", "deaths them", "gold us", "gold them"):
                tot[k] += n[k]
            tot["games"] += 1
            tot["idle us"] += iu
            tot["idle them"] += it
    g = max(tot["games"], 1)
    print(f"per game: clears {tot['clears us'] / g:.1f}/{tot['clears them'] / g:.1f} (theirs in our half {tot['their clears in our half'] / g:.1f}), "
          f"deaths {tot['deaths us'] / g:.1f}/{tot['deaths them'] / g:.1f}, gold@600 {tot['gold us'] / g:.0f}/{tot['gold them'] / g:.0f}, "
          f"idle {tot['idle us'] / g:.0%}/{tot['idle them'] / g:.0%}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
