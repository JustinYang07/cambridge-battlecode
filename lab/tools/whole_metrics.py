"""Per tuning variant, whole game, per game: deaths, deaths to champions, kills, gems dropped on
death, gems banked, enemy gems banked. Usage: whole_metrics.py <tuning json>"""
import collections, json, sys
from pathlib import Path
t = json.load(open(sys.argv[1]))
for k, v in t["results"].items():
    E = Path("lab/data/experiments") / v["experiment"]; c = collections.Counter(); g = 0
    for gd in sorted(E.glob("games/*")):
        sp = gd / "stats.json"
        if not sp.exists(): continue
        st = json.load(open(sp)); g += 1; us = "teamA" if "-A" in gd.name else "teamB"
        for e in st["events"]:
            o = e["owner"]
            if not isinstance(o, dict): continue
            for x in e["entries"]:
                tg = x["target"]; champ = isinstance(tg, dict) and tg.get("kind") == "champion"
                if e["kind"] == "death" and o["team"] == us:
                    c["deaths"] += 1; c["deaths to champions"] += champ
                elif e["kind"] == "kill" and o["team"] == us and champ: c["kills"] += 1
                elif e["kind"] == "gemCollection" and o["team"] != us and x.get("context") == "champion": c["gems dropped"] += int(x["value"])
        for tl in st["teamTimelines"]:
            if tl["runs"]: c["banked" if tl["owner"] == us else "enemy banked"] += float(tl["runs"][-1]["value"])
    print(f"{k} ({g} games, gem diff {v['mean']:+.0f} +/- {1.96*v['se']:.0f}): " + ", ".join(f"{n} {c[n]/max(g,1):.2f}" for n in ("deaths", "deaths to champions", "kills", "gems dropped", "banked", "enemy banked")))
