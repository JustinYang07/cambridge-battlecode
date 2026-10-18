"""Build a fight-episode dataset from @F rows (combatBotv3 lab traces) and stats.json.

Episode: a champion's first @F row after 10+ rounds without one; its features are that row.
Resolution: the first champion death (either team) within 8 tiles of the target spot within 20
rounds. Aftermath, from that death: deaths within 8 tiles of it in the next 10 rounds, and gems
each team picks up from dropped bags within 15 rounds (any position: bags are picked up nearby).
Unresolved episodes (no death) are kept with resolved=0.
Usage: fight_dataset.py <tuning json> <out csv>"""
import csv
import json
import sys
from pathlib import Path

sys.path.insert(0, "lab")
from labkit import replays  # noqa: E402

t = json.load(open(sys.argv[1]))
out = sys.argv[2]
cols = None
rows = []
for v in t["results"].values():
    E = Path("lab/data/experiments") / v["experiment"]
    for gd in sorted(E.glob("games/*")):
        sp = gd / "stats.json"
        if not sp.exists():
            continue
        st = json.load(open(sp))
        us = "teamA" if "-A" in gd.name else "teamB"
        deaths = []   # (round, x, y, ours)
        pickups = []  # (round, ours, value) gems from dropped bags
        for e in st["events"]:
            o = e["owner"]
            if not isinstance(o, dict):
                continue
            for x in e["entries"]:
                r = x["timestamp"]["round"]
                if e["kind"] == "death":
                    deaths.append((r, x["x"], x["y"], o["team"] == us))
                elif e["kind"] == "gemCollection" and x.get("context") == "champion":
                    pickups.append((r, o["team"] == us, int(x["value"])))
        deaths.sort()
        try:
            fights = replays.read_bundle(next(gd.glob("*.repl26a")))["teams"]["a" if us == "teamA" else "b"].get("fights", [])
        except Exception:
            continue
        last = {}
        for f in sorted(fights, key=lambda f: (f["u"], f["r"])):
            u, r = f["u"], f["r"]
            new = r - last.get(u, -100) >= 10
            last[u] = r
            if not new:
                continue
            row = dict(f)
            row["game"] = gd.parent.parent.name + "/" + gd.name
            first = next(((dr, dx, dy, mine) for dr, dx, dy, mine in deaths
                          if r <= dr <= r + 20 and max(abs(dx - f["tx"]), abs(dy - f["ty"])) <= 8), None)
            row.update(resolved=0, ourDeaths=0, theirDeaths=0, gemsWeGot=0, gemsTheyGot=0, firstDeathOurs=-1, roundsToDeath=-1)
            if first:
                d0, x0, y0, mine = first
                near = [d for d in deaths if d0 <= d[0] <= d0 + 10 and max(abs(d[1] - x0), abs(d[2] - y0)) <= 8]
                row.update(resolved=1, firstDeathOurs=int(mine), roundsToDeath=d0 - r,
                           ourDeaths=sum(1 for d in near if d[3]), theirDeaths=sum(1 for d in near if not d[3]),
                           gemsWeGot=sum(p[2] for p in pickups if d0 <= p[0] <= d0 + 15 and p[1]),
                           gemsTheyGot=sum(p[2] for p in pickups if d0 <= p[0] <= d0 + 15 and not p[1]))
            rows.append(row)
cols = list(rows[0].keys()) if rows else []
with open(out, "w", newline="") as fh:
    w = csv.DictWriter(fh, fieldnames=cols)
    w.writeheader()
    w.writerows(rows)
res = [r for r in rows if r["resolved"]]
print(f"{len(rows)} episodes, {len(res)} resolved ({100*len(res)/max(len(rows),1):.0f}%), games {len({r['game'] for r in rows})}")
for chose in (1, 0):
    for ex in (0, 1, 2):
        sel = [r for r in res if r["chose"] == chose and r["explore"] == ex]
        if not sel:
            continue
        swing = [r["gemsWeGot"] - r["gemsTheyGot"] for r in sel]
        print(f"  chose fight={chose} explore={ex}: n={len(sel)}, mean gem swing {sum(swing)/len(sel):+.1f}, "
              f"we died first {100*sum(r['firstDeathOurs'] for r in sel)/len(sel):.0f}%, "
              f"deaths us/them {sum(r['ourDeaths'] for r in sel)/len(sel):.2f}/{sum(r['theirDeaths'] for r in sel)/len(sel):.2f}")
