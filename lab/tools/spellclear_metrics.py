"""Per tuning variant, per game: monster damage taken by our ranged champions, camps cleared by
ranged / by everyone (campClear owner), deaths, gems banked, enemy banked, gem diff, spell-clear
turns (trace notes, full traces only). Usage: spellclear_metrics.py <tuning json>..."""
import collections, json, sys
from pathlib import Path
sys.path.insert(0, "lab")
from labkit import replays  # noqa: E402
RANGED = {"Stellar", "Cassi", "Prism", "Tavi", "Willow", "Vesper Vale"}
for path in sys.argv[1:]:
    t = json.load(open(path))
    for k, v in t["results"].items():
        E = Path("lab/data/experiments") / v["experiment"]; c = collections.Counter(); g = 0
        for gd in sorted(E.glob("games/*")):
            sp = gd / "stats.json"
            if not sp.exists(): continue
            st = json.load(open(sp)); us = "teamA" if "-A" in gd.name else "teamB"; g += 1
            for tl in st["championTimelines"]:
                if tl["metric"] == "monsterDamageTaken" and tl["owner"]["team"] == us and tl["runs"]:
                    val = float(tl["runs"][-1]["value"])
                    c["monster dmg (ranged)" if tl["owner"]["name"] in RANGED else "monster dmg (melee)"] += val
            for e in st["events"]:
                o = e["owner"]
                if not isinstance(o, dict) or o.get("team") != us: continue
                if e["kind"] == "campClear":
                    c["camps cleared"] += len(e["entries"])
                    if o["name"] in RANGED: c["camps cleared by ranged"] += len(e["entries"])
                elif e["kind"] == "death": c["deaths"] += len(e["entries"])
            for tl in st["teamTimelines"]:
                if tl["runs"]: c["banked" if tl["owner"] == us else "enemy banked"] += float(tl["runs"][-1]["value"])
            try:
                tr = replays.read_bundle(next(gd.glob("*.repl26a")))["teams"]["a" if us == "teamA" else "b"]
                blob = json.dumps(tr["details"]); c["spell-clear decisions"] += blob.count("spell clear"); c["quiet-game spell clears"] += blob.count("quiet game")
            except Exception:
                pass
        G = max(g, 1)
        print(f"{k} ({g} games, gem diff {v['mean']:+.0f} +/- {1.96*v['se']:.0f}): " + " | ".join(f"{n} {c[n]/G:.1f}" for n in (
            "spell-clear decisions", "quiet-game spell clears", "monster dmg (ranged)", "monster dmg (melee)", "camps cleared by ranged", "camps cleared", "deaths", "banked", "enemy banked")))
