"""Expanse: per tuning variant, per game: Stellar camp captures and far ones (>= 32 tiles from our
spawn), captures of the ditch wolf pair (our side's: (41,6) for A / mirrored for B), dash-across and
steal notes (full traces), deaths, banked, enemy banked, gem diff. Usage: steal_metrics.py <json>..."""
import collections, json, sys
from pathlib import Path
sys.path.insert(0, "lab")
from labkit import replays  # noqa: E402
for path in sys.argv[1:]:
    t = json.load(open(path))
    for k, v in t["results"].items():
        E = Path("lab/data/experiments") / v["experiment"]; c = collections.Counter(); g = 0
        for gd in sorted(E.glob("games/*")):
            sp = gd / "stats.json"
            if not sp.exists(): continue
            st = json.load(open(sp)); us = "teamA" if "-A" in gd.name else "teamB"; g += 1
            home = (4, 2) if us == "teamA" else (75, 77)
            ditch = (41, 6) if us == "teamA" else (38, 73)
            for e in st["events"]:
                o = e["owner"]
                if not isinstance(o, dict) or o.get("team") != us: continue
                for x in e["entries"]:
                    if e["kind"] == "gemCollection" and x.get("context") == "campGroup":
                        if o["name"] == "Stellar":
                            c["Stellar captures"] += 1
                            c["Stellar far captures"] += max(abs(x["x"] - home[0]), abs(x["y"] - home[1])) >= 32
                        if max(abs(x["x"] - ditch[0]), abs(x["y"] - ditch[1])) <= 2: c["ditch wolf captures"] += 1
                    elif e["kind"] == "death": c["deaths"] += 1
            for tl in st["teamTimelines"]:
                if tl["runs"]: c["banked" if tl["owner"] == us else "enemy banked"] += float(tl["runs"][-1]["value"])
            try:
                blob = json.dumps(replays.read_bundle(next(gd.glob("*.repl26a")))["teams"]["a" if us == "teamA" else "b"]["details"])
                c["dash-across notes"] += blob.count("dash across the ditch"); c["steal decisions"] += blob.count("quiet game: steal it")
            except Exception:
                pass
        G = max(g, 1)
        print(f"{k} ({g} games, gem diff {v['mean']:+.0f} +/- {1.96*v['se']:.0f}): " + " | ".join(f"{n} {c[n]/G:.2f}" for n in (
            "Stellar captures", "Stellar far captures", "steal decisions", "ditch wolf captures", "dash-across notes", "deaths", "banked", "enemy banked")))
