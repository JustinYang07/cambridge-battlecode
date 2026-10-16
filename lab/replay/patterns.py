#!/usr/bin/env python3
"""Behaviour patterns of one team across decoded replays (lab/replay/decode.mjs output).

Library use: patterns.game(decoded, team) -> dict of events and summaries; patterns.summarise(list)
prints an aggregate. Team 0 is side A. Positions on the map are described by "depth": 0 at our
spawn, 1 at theirs (straight-line distance share), so maps of any size and symmetry compare.
"""
from __future__ import annotations

import collections
import statistics

FAMILIES = ["monster", "power", "durability", "treasure", "jungle", "home", "bruiser"]
STATS = ["strength", "focus", "health"]
CHAMPS = {0: "Ryth", 1: "Vesper", 2: "Vendetta", 3: "Cassi", 4: "Issac", 5: "Stellar", 6: "Prism", 7: "Marcella",
          8: "Willow", 9: "Ren", 10: "Varg", 11: "Tavi"}
CAMPS = {0: "gem", 1: "wolf", 2: "elk", 3: "gold", 4: "monolith"}


def item_name(k: int) -> str:
    fam, rest = divmod(k, 6)
    stat, emblem = divmod(rest, 2)
    return f"{FAMILIES[fam] if fam < len(FAMILIES) else fam} {'emblem' if emblem else 'shard'} ({STATS[stat]})"


def cheb(a, b):
    return max(abs(a[0] - b[0]), abs(a[1] - b[1]))


def spawns(m):
    a = m["spawnA"]
    ca = ((a["minX"] + a["maxX"]) / 2, (a["minY"] + a["maxY"]) / 2)
    W, H, s = m["width"], m["height"], m["symmetry"]
    cb = (ca[0] if s == 1 else W - 1 - ca[0], ca[1] if s == 0 else H - 1 - ca[1])
    return ca, cb


def game(d: dict, team: int) -> dict:
    m = d["map"]
    ca, cb = spawns(m)
    home, away = (ca, cb) if team == 0 else (cb, ca)

    def depth(p):
        a, b = cheb(p, home), cheb(p, away)
        return a / max(a + b, 1)

    groups = m["groups"]
    gcen = [(sum(c["x"] for c in g["camps"]) / len(g["camps"]), sum(c["y"] for c in g["camps"]) / len(g["camps"])) for g in groups]
    gkind = ["+".join(sorted({CAMPS.get(c["type"], str(c["type"])) for c in g["camps"]})) for g in groups]
    rounds = d["rounds"]
    out = {"captures": [], "tributes": [], "items": collections.defaultdict(list), "wards": [], "encounters": [],
           "rounds": rounds[-1]["round"] if rounds else 0, "won": None}
    prev = None
    enc_open = {}  # champion -> start info
    for idx, r in enumerate(rounds):
        mine = [c for c in r["champions"] if c["team"] == team]
        theirs = [c for c in r["champions"] if c["team"] != team and c["alive"] and c["x"] >= 0]
        if prev:
            # captures by us
            for gi, (a, b) in enumerate(zip(prev["groups"], r["groups"])):
                if a & 4 and (a & 3) == team and not b & 4:
                    party = sum(1 for c in prev["champions"] if c["team"] == team and c["alive"] and cheb((c["x"], c["y"]), gcen[gi]) <= 3)
                    out["captures"].append({"round": r["round"], "kind": gkind[gi], "gems": groups[gi]["gems"], "gold": groups[gi]["gold"],
                                            "depth": depth(gcen[gi]), "party": party})
            pc = {c["i"]: c for c in prev["champions"]}
            for c in mine:
                p = pc.get(c["i"])
                if not p:
                    continue
                if p["gems"] > 0 and c["gems"] == 0 and c["alive"] and p["alive"]:
                    others = sum(1 for o in mine if o["i"] != c["i"] and pc.get(o["i"], {}).get("gems", 0) > 0 and o["gems"] == 0)
                    out["tributes"].append({"round": r["round"], "bag": p["gems"], "depth": depth((p["x"], p["y"])), "together": others})
                pinv = dict(map(tuple, p.get("inv", [])))
                for k, n in c.get("inv", []):
                    if n > pinv.get(k, 0):
                        out["items"][CHAMPS.get(c["type"], c["type"])].append((r["round"], item_name(k)))
            if r["wards"][team] > prev["wards"][team]:
                # placed by one of ours: the champion that did not move the most likely; use the nearest to the team centroid
                live = [c for c in mine if c["alive"] and c["x"] >= 0]
                if live:
                    out["wards"].append({"round": r["round"], "depths": sorted(round(depth((c["x"], c["y"])), 2) for c in live)})
        # encounters: an enemy comes within 6 tiles of one of ours (none the round before)
        for c in mine:
            if not c["alive"] or c["x"] < 0:
                e = enc_open.pop(c["i"], None)
                if e:  # died during the encounter
                    out["encounters"].append({**{k: v for k, v in e.items() if k != "kills0"}, "d5": e.get("d5"), "died": True,
                                              "team_kills": sum(o["kills"] for o in mine) - e["kills0"]})
                continue
            near = [e for e in theirs if cheb((e["x"], e["y"]), (c["x"], c["y"])) <= 6]
            if near and c["i"] not in enc_open:
                allies = sum(1 for o in mine if o["i"] != c["i"] and o["alive"] and o["x"] >= 0 and cheb((o["x"], o["y"]), (c["x"], c["y"])) <= 6)
                foes = len(near)
                dmin = min(cheb((e["x"], e["y"]), (c["x"], c["y"])) for e in near)
                enc_open[c["i"]] = {"start": idx, "allies": allies, "foes": foes, "d0": dmin, "hp0": c["hp"] / max(c["maxHp"], 1),
                                    "depth": depth((c["x"], c["y"])), "kills0": sum(o["kills"] for o in mine)}
            elif not near and c["i"] in enc_open and idx - enc_open[c["i"]]["start"] > 10:
                enc_open.pop(c["i"], None)
        # close encounters 5 and 10 rounds after they start
        for ci, e in list(enc_open.items()):
            if idx - e["start"] == 5 and "d5" not in e:
                c = next((x for x in mine if x["i"] == ci), None)
                if c and c["alive"] and c["x"] >= 0 and theirs:
                    e["d5"] = min(cheb((t["x"], t["y"]), (c["x"], c["y"])) for t in theirs)
                else:
                    e["d5"] = None
            if idx - e["start"] == 10:
                start = rounds[e["start"]]
                died = False  # deaths close the encounter early (above)
                kills = sum(o["kills"] for o in mine) - e["kills0"]
                out["encounters"].append({**{k: v for k, v in e.items() if k != "kills0"}, "died": died, "team_kills": kills})
                del enc_open[ci]
        prev = r
    return out


def _avg(xs):
    xs = [x for x in xs if x is not None]
    return statistics.mean(xs) if xs else None


def summarise(games: list[dict], label: str) -> dict:
    caps = [c for g in games for c in g["captures"]]
    early = [c for g in games for c in g["captures"] if c["round"] <= 200]
    trib = [t for g in games for t in g["tributes"]]
    trib_early = [t for t in trib if t["round"] <= 200]
    n = max(len(games), 1)
    s = {
        "games": len(games),
        "captures by r100": sum(1 for c in early if c["round"] <= 100) / n,
        "captures by r200": len(early) / n,
        "captures /100r (all)": 100 * len(caps) / max(sum(g["rounds"] for g in games), 1),
        "early capture kinds": collections.Counter(c["kind"] for c in early).most_common(5),
        "early gold-only share": sum(1 for c in early if c["gems"] == 0) / max(len(early), 1),
        "early capture depth": _avg(c["depth"] for c in early),
        "capture depth (all)": _avg(c["depth"] for c in caps),
        "share of captures past halfway": sum(1 for c in caps if c["depth"] > 0.5) / max(len(caps), 1),
        "party at capture (early)": _avg(c["party"] for c in early),
        "party at capture (all)": _avg(c["party"] for c in caps),
        "first capture round": _avg(min((c["round"] for c in g["captures"]), default=None) for g in games),
        "tributes by r200": len(trib_early) / n,
        "bag at tribute (early)": _avg(t["bag"] for t in trib_early),
        "bag at tribute (all)": _avg(t["bag"] for t in trib),
        "tribute depth": _avg(t["depth"] for t in trib),
        "tributes with another banking": sum(1 for t in trib if t["together"]) / max(len(trib), 1),
        "wards per game": sum(len(g["wards"]) for g in games) / n,
        "ward round (first)": _avg(min((w["round"] for w in g["wards"]), default=None) for g in games),
        "ward placer depth": _avg(w["depths"][len(w["depths"]) // 2] for g in games for w in g["wards"]),
    }
    enc = [e for g in games for e in g["encounters"]]
    def bucket(e):
        return "outnumbered" if e["foes"] > e["allies"] + 1 else "even" if e["foes"] == e["allies"] + 1 else "ahead"
    for b in ("outnumbered", "even", "ahead"):
        es = [e for e in enc if bucket(e) == b]
        moved = [e["d5"] - e["d0"] for e in es if e.get("d5") is not None]
        s[f"encounters {b}: n/game"] = len(es) / n
        s[f"encounters {b}: died within 10"] = sum(e["died"] for e in es) / max(len(es), 1)
        s[f"encounters {b}: team kills within 10"] = _avg(e["team_kills"] for e in es)
        s[f"encounters {b}: backed off (+2 tiles)"] = sum(1 for x in moved if x >= 2) / max(len(moved), 1)
        s[f"encounters {b}: closed in (-2 tiles)"] = sum(1 for x in moved if x <= -2) / max(len(moved), 1)
    s["encounter depth"] = _avg(e["depth"] for e in enc)
    s["allies at encounter"] = _avg(e["allies"] for e in enc)
    # item order: first four purchases per champion type
    orders = collections.defaultdict(collections.Counter)
    for g in games:
        for champ, seq in g["items"].items():
            names = []
            for _, nm in sorted(seq):
                if not names or names[-1] != nm:
                    names.append(nm)
            orders[champ][" > ".join(names[:4])] += 1
    s["item orders"] = {k: v.most_common(2) for k, v in orders.items()}
    return s
