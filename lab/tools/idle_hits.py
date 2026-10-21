#!/usr/bin/env python3
"""Champion-rounds spent taking damage while doing nothing (no move, attack, cast, capture, recall,
shop or tribute that round), per team, in a set of games (folder layout as autopsy.py). A champion
that stands in a camp's reach without fighting back is a stall in the bot, not a choice.
Prints totals per folder, and with --list the longest such runs (champion, map, rounds, damage).

usage: python3 lab/tools/idle_hits.py <dir> [<dir> ...] [--list]
"""
import collections, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import autopsy


def main() -> int:
    show = "--list" in sys.argv
    for folder in [Path(a) for a in sys.argv[1:] if not a.startswith("--")]:
        C = collections.Counter()
        runs = []
        for sf in sorted(folder.glob("*.stats.json")):
            mid = sf.name[:-11]
            T = 0 if (folder / f"{mid}.side").read_text().strip() == "teamA" else 1
            mp = (folder / f"{mid}.map").read_text().strip()
            g = autopsy.decode(folder / f"{mid}.repl26a")
            R = {x["r"]: x for x in g["rounds"] if x["champs"]}
            acted = {(a[0], a[3]) for a in g["actions"] if a[2] == 1}
            cur = {}
            for r in sorted(R):
                if r - 1 not in R:
                    continue
                before = {c[0]: c for c in R[r - 1]["champs"]}
                for c in R[r]["champs"]:
                    p = before.get(c[0])
                    team = "us" if c[1] == T else "them"
                    if not (p and p[7] and c[7]):
                        cur.pop(c[0], None)
                        continue
                    C[team + " alive"] += 1
                    hurt = c[5] < p[5]
                    idle = (r, c[0]) not in acted
                    if hurt and idle:
                        C[team + " hurt+idle"] += 1
                        run = cur.setdefault(c[0], [r, r, 0, c[2], team])
                        run[1] = r
                        run[2] += p[5] - c[5]
                    elif c[0] in cur:
                        run = cur.pop(c[0])
                        if run[1] - run[0] >= 3:
                            runs.append((run[1] - run[0] + 1, mp, run[3], run[4], run[0], run[2]))
        print(f"{folder}: " + "  ".join(f"{t}: {C[t + ' hurt+idle']} hurt+idle rounds of {C[t + ' alive']} alive ({C[t + ' hurt+idle'] / max(C[t + ' alive'], 1):.2%})" for t in ("us", "them")))
        long = [x for x in runs if x[0] >= 4]
        print(f"   runs of 4+ rounds: us {sum(1 for x in long if x[3] == 'us')}, them {sum(1 for x in long if x[3] == 'them')}")
        if show:
            for n, mp, name, team, r0, dmg in sorted(long, reverse=True)[:25]:
                print(f"   {team:<4} {mp:<22} {name:<13} r{r0}-{r0 + n - 1} ({n} rounds) {dmg} damage")
    return 0


if __name__ == "__main__":
    sys.exit(main())
