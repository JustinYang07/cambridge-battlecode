#!/usr/bin/env python3
"""Strategy summary of teams from decoded replays: draft, formation, opening targets, early milestones.

  python3 lab/tools/team_strategy.py --scraped ID:Name ... [--experiment "exp name":Label ...] [--json out.json]

Needs the camp layout of each map (lab/data/maps/*.json), so games on unknown maps are skipped.
"""
from __future__ import annotations

import argparse
import collections
import glob
import json
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from team_behaviour import decode  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
MAPS = {json.loads(Path(f).read_text())["name"]: json.loads(Path(f).read_text()) for f in glob.glob(str(ROOT / "lab/data/maps/*.json"))}
cheb = lambda a, b: max(abs(a[0] - b[0]), abs(a[1] - b[1]))


def group_kind(g: dict) -> str:
    kinds = sorted({c["type"].replace("_cluster", "") for c in g["camps"]})
    return "+".join(kinds)


def formation(champs: list, team: int) -> str:
    """Group sizes of the team's live champions, linking champions within 4 tiles (e.g. '3-2')."""
    pts = [(c[3], c[4]) for c in champs if c[1] == team and c[7]]
    seen, sizes = set(), []
    for i in range(len(pts)):
        if i in seen:
            continue
        stack, size = [i], 0
        seen.add(i)
        while stack:
            j = stack.pop()
            size += 1
            for k in range(len(pts)):
                if k not in seen and cheb(pts[j], pts[k]) <= 4:
                    seen.add(k)
                    stack.append(k)
        sizes.append(size)
    return "-".join(str(s) for s in sorted(sizes, reverse=True))


def game_strategy(d: dict, team: int, mapname: str) -> dict | None:
    m = MAPS.get(mapname)
    if not m or not d["rounds"]:
        return None
    tile_group = {(c["x"], c["y"]): gi for gi, g in enumerate(m["groups"]) for c in g["camps"]}
    spawn = m["spawnA"] if team == 0 else m["spawnB"]
    sc = (sum(x for x, _ in spawn) / len(spawn), sum(y for _, y in spawn) / len(spawn))
    enemy = m["spawnB"] if team == 0 else m["spawnA"]
    ec = (sum(x for x, _ in enemy) / len(enemy), sum(y for _, y in enemy) / len(enemy))
    gdist = {gi: min(cheb(sc, (c["x"], c["y"])) for c in g["camps"]) for gi, g in enumerate(m["groups"])}
    rank = {gi: r + 1 for r, gi in enumerate(sorted(gdist, key=gdist.get))}
    first = next(r for r in d["rounds"] if r["champs"])
    mine = sorted([c for c in first["champs"] if c[1] == team], key=lambda c: c[0])
    types = {c[0]: c[2] for c in mine}
    by_round = {r["r"]: r for r in d["rounds"]}

    # Opening: the first two distinct camp groups each champion damages.
    targets = collections.defaultdict(list)
    first_tribute = first_recall = None
    for rnd, kind, akind, aidx, frm, to, slot in d["actions"]:
        if akind != 1 or aidx not in types:
            continue
        if kind in ("Attack", "Cast") and tuple(to) in tile_group:
            gi = tile_group[tuple(to)]
            if len(targets[aidx]) < 2 and gi not in [t[0] for t in targets[aidx]]:
                targets[aidx].append((gi, rnd))
        elif kind == "SubmitGems" and first_tribute is None:
            first_tribute = rnd
        elif kind == "Recall" and first_recall is None:
            first_recall = rnd
    opening = []
    for idx, tl in targets.items():
        for n, (gi, rnd) in enumerate(tl):
            g = m["groups"][gi]
            own_half = cheb(sc, (g["camps"][0]["x"], g["camps"][0]["y"])) <= cheb(ec, (g["camps"][0]["x"], g["camps"][0]["y"]))
            opening.append({"champ": types[idx], "n": n, "kind": group_kind(g), "round": rnd, "rank": rank[gi],
                            "dist": gdist[gi], "ownHalf": own_half, "group": gi})
    # Whole-team first camp: which group the team hits first and how many champions joined it.
    firsts = [o for o in opening if o["n"] == 0]
    team_first = min(firsts, key=lambda o: o["round"]) if firsts else None
    together = sum(1 for o in firsts if team_first and o["group"] == team_first["group"])

    first_kill = next((r["r"] for r in d["rounds"] for c in r["champs"] if c[1] == team and c[10] > 0), None)
    first_capture = next((r["r"] for r in d["rounds"] if any(c[1] == team and c[9] > 0 for c in r["champs"])), None)
    banked = lambda R: next((r["gems"][team] for r in reversed(d["rounds"]) if r["r"] <= R), 0)
    return {
        "map": mapname, "won": d["finalGems"][team] > d["finalGems"][1 - team], "bodies": len(mine),
        "draft": [types[c[0]] for c in mine], "form20": formation(by_round.get(20, first)["champs"], team),
        "form60": formation(by_round.get(60, first)["champs"], team), "form120": formation(by_round.get(120, first)["champs"], team),
        "opening": opening, "teamFirst": team_first, "together": together,
        "firstCapture": first_capture, "firstTribute": first_tribute, "firstRecall": first_recall, "firstKill": first_kill,
        "b100": banked(100), "b200": banked(200), "b300": banked(300),
    }


def summarise(label: str, games: list[dict]) -> dict:
    games = [g for g in games if g]
    if not games:
        return {"team": label, "games": 0}
    med = lambda k: statistics.median([g[k] for g in games if g[k] is not None]) if any(g[k] is not None for g in games) else None
    by_size = collections.defaultdict(collections.Counter)
    for g in games:
        by_size[g["bodies"]][" > ".join(g["draft"])] += 1
    drafts = {s: c.most_common(2) for s, c in sorted(by_size.items())}
    forms = {k: collections.Counter(g[k] for g in games).most_common(3) for k in ("form20", "form60", "form120")}
    firsts = [o for g in games for o in g["opening"] if o["n"] == 0]
    seconds = [o for g in games for o in g["opening"] if o["n"] == 1]
    kinds = collections.Counter(o["kind"] for o in firsts)
    kinds2 = collections.Counter(o["kind"] for o in seconds)
    team_first_kinds = collections.Counter(g["teamFirst"]["kind"] for g in games if g["teamFirst"])
    per_map = collections.defaultdict(collections.Counter)
    for g in games:
        if g["teamFirst"]:
            per_map[g["map"]][g["teamFirst"]["kind"] + f" (#{g['teamFirst']['rank']} nearest)"] += 1
    return {
        "team": label, "games": len(games), "winRate": sum(g["won"] for g in games) / len(games),
        "drafts": drafts, "formations": forms,
        "firstTargetKinds": kinds.most_common(4), "secondTargetKinds": kinds2.most_common(4),
        "firstTargetNearestShare": sum(o["rank"] <= 2 for o in firsts) / max(1, len(firsts)),
        "firstTargetOwnHalf": sum(o["ownHalf"] for o in firsts) / max(1, len(firsts)),
        "firstTargetRound": statistics.median(o["round"] for o in firsts) if firsts else None,
        "firstTargetDist": statistics.median(o["dist"] for o in firsts) if firsts else None,
        "teamFirstKinds": team_first_kinds.most_common(3),
        "teamFirstTogether": statistics.mean(g["together"] for g in games if g["teamFirst"]) if any(g["teamFirst"] for g in games) else 0,
        "perMapOpener": {m: c.most_common(1)[0] for m, c in sorted(per_map.items())},
        "firstCapture": med("firstCapture"), "firstTribute": med("firstTribute"), "firstRecall": med("firstRecall"),
        "firstKill": med("firstKill"), "b100": med("b100"), "b200": med("b200"), "b300": med("b300"),
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--scraped", nargs="*", default=[])
    ap.add_argument("--experiment", nargs="*", default=[])
    ap.add_argument("--json")
    a = ap.parse_args()
    out = []
    metas = [json.loads(Path(f).read_text()) | {"_file": f} for f in glob.glob(str(ROOT / "lab/data/scraped/*.json"))]
    for spec in a.scraped:
        tid, name = spec.split(":", 1)
        games = []
        for meta in metas:
            side = 0 if (meta.get("teamA") or {}).get("teamId") == tid else 1 if (meta.get("teamB") or {}).get("teamId") == tid else None
            if side is None or meta.get("map") not in MAPS:
                continue
            d = decode(Path(meta["_file"]).with_suffix(".repl26a"))
            if d:
                games.append(game_strategy(d, side, meta["map"]))
        out.append(summarise(name, games))
    for spec in a.experiment:
        name, label = spec.rsplit(":", 1)
        e = [json.loads(Path(f).read_text()) for f in glob.glob(str(ROOT / "lab/data/experiments/*/experiment.json"))]
        e = [x for x in e if x.get("name") == name][-1]
        games = []
        for g in e["games"]:
            d = decode(ROOT / g["result"]["replay"])
            if d:
                games.append(game_strategy(d, 0 if g["candidateSide"] == "A" else 1, g["map"]))
        out.append(summarise(label, games))
    print(json.dumps(out, indent=1, default=str))
    if a.json:
        Path(a.json).write_text(json.dumps(out, indent=1, default=str))
    return 0


if __name__ == "__main__":
    sys.exit(main())
