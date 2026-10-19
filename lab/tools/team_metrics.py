"""Per tuning variant, whole game, per game: fight<->flee swaps within 3 rounds, fight turns with an
ally within 6 fleeing (split calls), flee turns attacking, allies within 6 when fighting, ranged
fight decisions ahead of every melee (from @F rows), deaths, deaths to champions, kills, gems
dropped, banked, enemy banked, gem diff. Usage: team_metrics.py <tuning json>..."""
import collections, json, sys
from pathlib import Path
sys.path.insert(0, "lab")
from labkit import replays  # noqa: E402
MELEE = {"Ren", "Ryth", "Varg", "Issac", "Marcella", "Vendetta"}
D = lambda a, b: max(abs(a[0] - b[0]), abs(a[1] - b[1]))
for path in sys.argv[1:]:
    t = json.load(open(path))
    for k, v in t["results"].items():
        E = Path("lab/data/experiments") / v["experiment"]; c = collections.Counter(); g = 0
        for gd in sorted(E.glob("games/*")):
            sp = gd / "stats.json"
            if not sp.exists(): continue
            st = json.load(open(sp)); us = "teamA" if "-A" in gd.name else "teamB"
            try: tr = replays.read_bundle(next(gd.glob("*.repl26a")))["teams"]["a" if us == "teamA" else "b"]
            except Exception: continue
            g += 1
            for e in st["events"]:
                o = e["owner"]
                if not isinstance(o, dict): continue
                for x in e["entries"]:
                    tg = x["target"]; champ = isinstance(tg, dict) and tg.get("kind") == "champion"
                    if e["kind"] == "death" and o["team"] == us: c["deaths"] += 1; c["deaths to champions"] += champ
                    elif e["kind"] == "kill" and o["team"] == us and champ: c["kills"] += 1
                    elif e["kind"] == "gemCollection" and o["team"] != us and x.get("context") == "champion": c["gems dropped"] += int(x["value"])
            for tl in st["teamTimelines"]:
                if tl["runs"]: c["banked" if tl["owner"] == us else "enemy banked"] += float(tl["runs"][-1]["value"])
            units = {int(a): b for a, b in tr["units"].items()}; turns = tr["turns"]
            pos = collections.defaultdict(dict); kind = collections.defaultdict(dict)
            for x in turns: pos[x["r"]][x["u"]] = (x["x"], x["y"]); kind[x["r"]][x["u"]] = x["k"]
            hit = lambda a: any(s[:2] in ("ae", "se", "st") or s.startswith("aoe") for s in a.split(";"))
            byu = collections.defaultdict(list)
            for x in sorted(turns, key=lambda x: x["r"]): byu[x["u"]].append(x)
            for u, xs in byu.items():
                last = None
                for x in xs:
                    if x["k"] == "flee": c["_flee"] += 1; c["_flee hit"] += hit(x.get("act") or "")
                    if x["k"] in ("fight", "flee"):
                        if last and last["k"] != x["k"] and x["r"] - last["r"] <= 3: c["swaps"] += 1
                        last = x
            for r, ks in kind.items():
                for u, kk in ks.items():
                    if kk != "fight": continue
                    c["_fight"] += 1
                    near = [w for w in ks if w != u and D(pos[r][w], pos[r][u]) <= 6]
                    c["_allies"] += len(near); c["_split"] += any(ks[w] == "flee" for w in near)
            for f in tr.get("fights", []):
                u, r = f["u"], f["r"]; nm = units.get(u)
                if f["chose"] != 1 or nm is None or nm in MELEE or u not in pos[r]: continue
                mel = [w for w, n2 in units.items() if n2 in MELEE and w in pos[r]]
                if not mel: continue
                tt = (f["tx"], f["ty"]); c["_rf"] += 1; c["_ahead"] += D(pos[r][u], tt) < min(D(pos[r][w], tt) for w in mel)
        G = max(g, 1)
        print(f"{k} ({g} games, gem diff {v['mean']:+.0f} +/- {1.96*v['se']:.0f})")
        print(f"   swaps/game {c['swaps']/G:.1f} | split-call fight turns {100*c['_split']/max(c['_fight'],1):.1f}% | flee turns attacking {100*c['_flee hit']/max(c['_flee'],1):.1f}% | allies within 6 when fighting {c['_allies']/max(c['_fight'],1):.2f} | ranged ahead of all melee {100*c['_ahead']/max(c['_rf'],1):.0f}%")
        print(f"   deaths {c['deaths']/G:.2f} | to champions {c['deaths to champions']/G:.2f} | kills {c['kills']/G:.2f} | gems dropped {c['gems dropped']/G:.1f} | banked {c['banked']/G:.0f} | enemy banked {c['enemy banked']/G:.0f}")
