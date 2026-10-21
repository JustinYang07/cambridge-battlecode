#!/usr/bin/env python3
"""Farming tempo of both teams in a set of games (stats.json + replay + .side/.map as autopsy.py).

Per team, rounds 1..--upto, share of champion-rounds spent: hitting a camp, hitting a champion,
walking, idle (alive, no action), recalling, tributing, dead; captures and gems per capture;
tiles walked per capture; monster damage per 100 rounds; per champion type the same split.
usage: python3 lab/tools/tempo.py <dir> [--maps a,b] [--upto 1000]
"""
import collections, json, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import autopsy


def cheb(a, b):
    return max(abs(a[0] - b[0]), abs(a[1] - b[1]))


def main() -> int:
    folder = Path(sys.argv[1])
    only = set(sys.argv[sys.argv.index("--maps") + 1].split(",")) if "--maps" in sys.argv else None
    upto = int(sys.argv[sys.argv.index("--upto") + 1]) if "--upto" in sys.argv else 1000
    for sf in sorted(folder.glob("*.stats.json")):
        mid = sf.name[:-11]
        mp = (folder / f"{mid}.map").read_text().strip()
        if only and mp not in only:
            continue
        T = 0 if (folder / f"{mid}.side").read_text().strip() == "teamA" else 1
        d = json.loads(sf.read_text())
        g = autopsy.decode(folder / f"{mid}.repl26a")
        R = {x["r"]: x for x in g["rounds"] if x["champs"]}
        camps = set()
        for a in g["actions"]:
            if a[2] == 2:
                camps.add(tuple(a[4]))  # a monster's own tile
        acts = collections.defaultdict(list)
        chan = {}
        for a in g["actions"]:
            if a[2] == 1:
                acts[(a[0], a[3])].append(a)
                if a[1] == "Recall":
                    for k in range(5): chan[(a[0] + k, a[3])] = "recalling"
                if a[1] == "SubmitGems":
                    chan[(a[0], a[3])] = "tributing"
        print(f"== {mp}  score {[t['submittedGems'] for t in d['teams']]}  (rounds 1..{upto})")
        for team, who in ((T, "us"), (1 - T, "them")):
            c = collections.Counter(); bytype = collections.defaultdict(collections.Counter)
            walked = 0
            prev = {}
            for r in range(1, upto + 1):
                x = R.get(r)
                if not x:
                    continue
                foes = {(e[3], e[4]) for e in x["champs"] if e[1] != team and e[7]}
                for ch in x["champs"]:
                    if ch[1] != team:
                        continue
                    if not ch[7]:
                        k = "dead"
                    else:
                        A = acts.get((r, ch[0]), [])
                        hitC = any(a[1] in ("Attack", "Cast") and tuple(a[5]) in foes for a in A)
                        hitM = any(a[1] in ("Attack", "Cast") and tuple(a[5]) in camps for a in A)
                        moved = any(a[1] == "Move" for a in A)
                        k = "hit champion" if hitC else "hit camp" if hitM else chan.get((r, ch[0])) or ("walk" if moved else "idle")
                        p = prev.get(ch[0])
                        if p: walked += cheb(p, (ch[3], ch[4])) if cheb(p, (ch[3], ch[4])) <= 3 else 0
                        prev[ch[0]] = (ch[3], ch[4])
                    c[k] += 1; c["total"] += 1
                    bytype[ch[2]][k] += 1; bytype[ch[2]]["total"] += 1
            tk = "teamA" if team == 0 else "teamB"
            caps = [int(x["value"]) for e in d["events"] if e["kind"] == "gemCollection" and e["owner"].get("team") == tk
                    for x in e["entries"] if x["timestamp"]["round"] <= upto]
            mdd = 0.0
            for t in d["championTimelines"]:
                if t["owner"]["team"] == tk and t["metric"] == "monsterDamageDone":
                    mdd += autopsy.cumulative(t["runs"], upto)[upto]
            tot = c["total"]
            order = ["hit camp", "hit champion", "walk", "idle", "recalling", "tributing", "dead"]
            print(f"  {who:<5} " + "  ".join(f"{k} {c[k] / tot:4.0%}" for k in order) +
                  f" | captures {len(caps)} ({sum(caps) / max(len(caps), 1):.0f} gems avg) tiles/capture {walked / max(len(caps), 1):.0f} monDmg/100r {mdd / upto * 100:.0f}")
            for ty, cc in sorted(bytype.items()):
                t2 = cc["total"]
                print(f"      {ty:<12} " + "  ".join(f"{k} {cc[k] / t2:4.0%}" for k in order))
    return 0


if __name__ == "__main__":
    sys.exit(main())
