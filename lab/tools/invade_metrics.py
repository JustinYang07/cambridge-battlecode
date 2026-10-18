"""Per tuning variant, whole game, per game: our deaths on their half (all / ranged champions),
ranged turns on their half with no melee ally within 8, deaths, kills, gems dropped, banked, enemy
banked, gem diff. Spawns: each team's champion positions on its first turn (both teams traced).
Usage: invade_metrics.py <tuning json>"""
import collections, json, sys
from pathlib import Path
sys.path.insert(0, "lab")
from labkit import replays  # noqa: E402
MELEE = {"Ren", "Ryth", "Varg", "Issac", "Marcella", "Vendetta"}
D = lambda a, b: max(abs(a[0] - b[0]), abs(a[1] - b[1]))
t = json.load(open(sys.argv[1]))
for k, v in t["results"].items():
    E = Path("lab/data/experiments") / v["experiment"]; c = collections.Counter(); g = 0
    for gd in sorted(E.glob("games/*")):
        sp = gd / "stats.json"
        if not sp.exists(): continue
        st = json.load(open(sp)); us = "teamA" if "-A" in gd.name else "teamB"
        try: b = replays.read_bundle(next(gd.glob("*.repl26a")))
        except Exception: continue
        mine, theirs = b["teams"]["a" if us == "teamA" else "b"], b["teams"]["b" if us == "teamA" else "a"]
        def spawn(tr):
            pts = [(x["x"], x["y"]) for x in tr["turns"] if x["r"] <= 2]
            return (sum(p[0] for p in pts) / len(pts), sum(p[1] for p in pts) / len(pts)) if pts else None
        s_us, s_them = spawn(mine), spawn(theirs)
        if not s_us or not s_them: continue
        g += 1
        half = lambda x, y: D((x, y), s_them) < D((x, y), s_us)
        for e in st["events"]:
            o = e["owner"]
            if not isinstance(o, dict): continue
            for x in e["entries"]:
                tg = x["target"]; champ = isinstance(tg, dict) and tg.get("kind") == "champion"
                if e["kind"] == "death" and o["team"] == us:
                    c["deaths"] += 1
                    if half(x["x"], x["y"]):
                        c["deaths on their half"] += 1; c["ranged deaths on their half"] += o["name"] not in MELEE
                elif e["kind"] == "kill" and o["team"] == us and champ: c["kills"] += 1
                elif e["kind"] == "gemCollection" and o["team"] != us and x.get("context") == "champion": c["gems dropped"] += int(x["value"])
        for tl in st["teamTimelines"]:
            if tl["runs"]: c["banked" if tl["owner"] == us else "enemy banked"] += float(tl["runs"][-1]["value"])
        units = {int(a): n for a, n in mine["units"].items()}
        pos = collections.defaultdict(dict)
        for x in mine["turns"]:
            if x["k"] not in ("dead", "channel"): pos[x["r"]][x["u"]] = (x["x"], x["y"])
        for r, ps in pos.items():
            for u, p in ps.items():
                if units.get(u) in MELEE or units.get(u) is None or not half(*p): continue
                c["ranged turns on their half"] += 1
                if not any(units.get(w) in MELEE and D(q, p) <= 8 for w, q in ps.items() if w != u): c["...with no melee within 8"] += 1
    G = max(g, 1)
    print(f"{k} ({g} games, gem diff {v['mean']:+.0f} +/- {1.96*v['se']:.0f})")
    print("   " + " | ".join(f"{n} {c[n]/G:.2f}" for n in ("deaths", "deaths on their half", "ranged deaths on their half", "kills", "gems dropped", "banked", "enemy banked")))
    print(f"   ranged turns on their half {c['ranged turns on their half']/G:.0f}/game, with no melee within 8: {100*c['...with no melee within 8']/max(c['ranged turns on their half'],1):.0f}%")
