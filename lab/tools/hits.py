#!/usr/bin/env python3
"""Champion-on-champion hits in a set of games (same folder layout as autopsy.py): for each team,
how many basic attacks and casts landed on enemy champions, what the attacker was doing around it
(a camp within 1 of the attacker, i.e. it left a camp hit for it), the target's health, the local
numbers within 6, and whether the attacker died within 10 rounds (the trade).

usage: python3 lab/tools/hits.py <dir> [<dir> ...]
"""
import collections, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import autopsy


def cheb(a, b):
    return max(abs(a[0] - b[0]), abs(a[1] - b[1]))


def main() -> int:
    S = {k: collections.Counter() for k in ("us", "them")}
    for folder in map(Path, sys.argv[1:]):
        for sf in sorted(folder.glob("*.stats.json")):
            mid = sf.name[:-11]
            T = 0 if (folder / f"{mid}.side").read_text().strip() == "teamA" else 1
            g = autopsy.decode(folder / f"{mid}.repl26a")
            R = {x["r"]: x for x in g["rounds"] if x["champs"]}
            deathRounds = collections.defaultdict(list)
            prev = {}
            for r in sorted(R):
                for c in R[r]["champs"]:
                    if prev.get(c[0]) == 1 and not c[7]:
                        deathRounds[c[0]].append(r)
                    prev[c[0]] = c[7]
            for a in g["actions"]:
                rnd, kind, actorKind, idx, frm, to = a[:6]
                if actorKind != 1 or kind not in ("Attack", "Cast"):
                    continue
                b = R.get(rnd - 1) or R.get(rnd)
                if not b:
                    continue
                me = next((c for c in b["champs"] if c[0] == idx), None)
                if not me:
                    continue
                team = "us" if me[1] == T else "them"
                tgt = next((c for c in b["champs"] if c[7] and c[1] != me[1] and (c[3], c[4]) == tuple(to)), None)
                if not tgt:
                    S[team][kind + " other"] += 1
                    continue
                k = S[team]
                k[kind + " champion"] += 1
                near = [c for c in b["champs"] if c[7] and cheb((c[3], c[4]), (me[3], me[4])) <= 6]
                ours = sum(1 for c in near if c[1] == me[1]); theirs = sum(1 for c in near if c[1] != me[1])
                died = any(rnd <= d <= rnd + 10 for d in deathRounds[idx])
                killed = any(rnd <= d <= rnd + 10 for d in deathRounds[tgt[0]])
                hpRatio = tgt[5] / max(me[5], 1)
                frac = tgt[5] / max(tgt[6], 1)
                bucket = "target hp<=0.5x ours" if hpRatio <= 0.5 else "target hp 0.5-1.5x" if hpRatio <= 1.5 else "target hp>1.5x ours"
                nums = "outnumbered" if ours < theirs else "even" if ours == theirs else "outnumber"
                for key in (bucket, nums, f"target frac {'<0.4' if frac < 0.4 else '0.4-0.8' if frac < 0.8 else '>=0.8'}", "all"):
                    k[f"{key}: hits"] += 1
                    k[f"{key}: attacker died<=10"] += died
                    k[f"{key}: target died<=10"] += killed
    for team, k in S.items():
        print(f"== {team}: attacks on champions {k['Attack champion']}, casts on champions {k['Cast champion']}, "
              f"attacks elsewhere {k['Attack other']}, casts elsewhere {k['Cast other']}")
        keys = sorted({x.split(':')[0] for x in k if x.endswith(': hits')})
        for key in keys:
            n = k[f"{key}: hits"]
            print(f"   {key:<24} hits {n:5d}  attacker died within 10 {k[f'{key}: attacker died<=10'] / n:5.1%}  target died within 10 {k[f'{key}: target died<=10'] / n:5.1%}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
