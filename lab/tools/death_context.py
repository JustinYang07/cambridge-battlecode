#!/usr/bin/env python3
"""What each of our champions was doing in the 6 rounds before it died (folder layout as autopsy.py):
hitting a champion, hitting a camp, walking, standing, recalling; who it last hit (champion or camp),
our and their champions within 6, and our and the nearest enemy's health 6 rounds before.

usage: python3 lab/tools/death_context.py <dir> [<dir> ...] [--list]
"""
import collections, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import autopsy


def cheb(a, b):
    return max(abs(a[0] - b[0]), abs(a[1] - b[1]))


def main() -> int:
    show = "--list" in sys.argv
    C = collections.Counter()
    for folder in [Path(a) for a in sys.argv[1:] if not a.startswith("--")]:
        for sf in sorted(folder.glob("*.stats.json")):
            mid = sf.name[:-11]
            T = 0 if (folder / f"{mid}.side").read_text().strip() == "teamA" else 1
            mp = (folder / f"{mid}.map").read_text().strip()
            g = autopsy.decode(folder / f"{mid}.repl26a")
            R = {x["r"]: x for x in g["rounds"] if x["champs"]}
            acts = collections.defaultdict(list)
            for a in g["actions"]:
                if a[2] == 1:
                    acts[a[3]].append(a)
            prev = {}
            for r in sorted(R):
                for c in R[r]["champs"]:
                    if c[1] == T and prev.get(c[0]) == 1 and not c[7]:
                        b = R.get(r - 6) or R.get(r - 5)
                        if not b:
                            continue
                        me = next(z for z in b["champs"] if z[0] == c[0])
                        champsAt = lambda q: {(z[3], z[4]) for z in (R.get(q) or {"champs": []})["champs"] if z[7] and z[1] != T}
                        recent = [a for a in acts[c[0]] if r - 6 <= a[0] <= r]
                        hitChamp = sum(1 for a in recent if a[1] in ("Attack", "Cast") and tuple(a[5]) in champsAt(a[0] - 1))
                        hitCamp = sum(1 for a in recent if a[1] in ("Attack", "Cast")) - hitChamp
                        recall = any(a[1] == "Recall" for a in recent)
                        near = [z for z in b["champs"] if z[7] and cheb((z[3], z[4]), (me[3], me[4])) <= 6]
                        us = sum(1 for z in near if z[1] == T); them = sum(1 for z in near if z[1] != T)
                        foes = [z for z in near if z[1] != T]
                        foeHp = sum(z[5] for z in foes)
                        allyHp = sum(z[5] for z in near if z[1] == T)
                        what = "recalling" if recall else "fighting" if hitChamp and not hitCamp else "farming+fighting" if hitChamp else "farming" if hitCamp else "walking/idle"
                        C[what] += 1
                        C["hp " + ("ahead" if allyHp >= 1.2 * foeHp else "even" if allyHp >= 0.8 * foeHp else "behind")] += 1
                        C[f"{what} | hp {'ahead' if allyHp >= 1.2 * foeHp else 'even' if allyHp >= 0.8 * foeHp else 'behind'}"] += 1
                        if show:
                            print(f"{mp:<20} r{r:5d} {c[2]:<12} {what:<17} us {us} ({allyHp:5.0f} hp) them {them} ({foeHp:5.0f} hp) [{','.join(z[2][:4] + ':' + str(z[5]) + '/' + str(z[6]) for z in foes)}]")
                    prev[c[0]] = c[7]
    for k, v in sorted(C.items(), key=lambda kv: (kv[0].count('|'), -kv[1])):
        print(f"  {k:<36} {v}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
