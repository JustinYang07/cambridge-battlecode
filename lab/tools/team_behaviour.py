#!/usr/bin/env python3
"""Behaviour profile of teams from decoded replays (any team: uses the engine, not bot logs).

  python3 lab/tools/team_behaviour.py --scraped TEAM_ID [TEAM_ID ...]   # platform games of these teams
  python3 lab/tools/team_behaviour.py --experiment "exp name" --bot attackBot2   # our local A/B games

Prints one row per team: income curve, how champions spend their turns, banking, recalls,
fighting, spacing. Decoded replays are cached in lab/data/decoded/.
"""
from __future__ import annotations

import argparse
import collections
import glob
import json
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CACHE = ROOT / "lab/data/decoded"
DECODER = ROOT / "lab/tools/decode_replay.mjs"


def decode(path: Path) -> dict | None:
    CACHE.mkdir(parents=True, exist_ok=True)
    out = CACHE / (path.stem + ".json")
    if out.exists():
        return json.loads(out.read_text())
    # Newest engine first; replays recorded before a cambc upgrade need the older engine.
    engines = sorted(Path.home().glob("Library/Caches/cambc/assets/*/visualiser/assets/v*_*_*-*.wasm"),
                     key=lambda p: (p.name.split("-")[0], p.stat().st_mtime), reverse=True)
    r = None
    for wasm in engines or [None]:
        cmd = ["node", str(DECODER), str(path)] + (["--wasm", str(wasm)] if wasm else [])
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        if r.returncode == 0 and r.stdout:
            break
    if r is None or r.returncode != 0 or not r.stdout:
        return None
    out.write_text(r.stdout)
    return json.loads(r.stdout)


def profile(d: dict, team: int) -> dict:
    """Metrics for side `team` (0 = A, 1 = B) of one decoded game."""
    rounds = d["rounds"]
    if not rounds:
        return {}
    first = next((r for r in rounds if r["champs"]), rounds[0])
    own = [(c[3], c[4]) for c in first["champs"] if c[1] == team]
    opp = [(c[3], c[4]) for c in first["champs"] if c[1] != team]
    oc = (sum(x for x, _ in own) / max(1, len(own)), sum(y for _, y in own) / max(1, len(own)))
    ec = (sum(x for x, _ in opp) / max(1, len(opp)), sum(y for _, y in opp) / max(1, len(opp)))
    cheb = lambda a, b: max(abs(a[0] - b[0]), abs(a[1] - b[1]))

    gems_at = {}
    for r in rounds:
        for mark in (100, 200, 300, 400):
            if r["r"] <= mark:
                gems_at[mark] = r["gems"][team]
    # Per-round positions for target classification and spacing.
    pos_by_round = {r["r"]: {(c[1], c[0]): c for c in r["champs"]} for r in rounds}
    my_idx = sorted({c[0] for c in first["champs"] if c[1] == team})

    act = collections.Counter()
    acted = collections.defaultdict(set)  # (idx) -> rounds with a real action
    bag_at_submit, submits, recalls = [], 0, 0
    for rnd, kind, akind, aidx, frm, to, slot in d["actions"]:
        if akind != 1:
            continue  # only champion actions
        state = pos_by_round.get(rnd, {})
        me = next((c for (t, i), c in state.items() if i == aidx), None)
        if not me or me[1] != team:
            continue
        acted[aidx].add(rnd)
        if kind == "Attack" or kind == "Cast":
            on_champ = any((c[3], c[4]) == tuple(to) and c[1] != team for c in state.values())
            act[kind + (":champ" if on_champ else ":camp")] += 1
        elif kind == "SubmitGems":
            submits += 1
            bag_at_submit.append(me[9])
        elif kind == "Recall":
            recalls += 1
        else:
            act[kind] += 1

    alive_rounds, idle_rounds, enemy_half, spread, deaths = 0, 0, 0, [], 0
    prev_alive = {}
    for r in rounds:
        mine = [c for c in r["champs"] if c[1] == team]
        alive = [c for c in mine if c[7]]
        for c in mine:
            if prev_alive.get(c[0]) and not c[7]:
                deaths += 1
            prev_alive[c[0]] = c[7]
        for c in alive:
            alive_rounds += 1
            idle_rounds += r["r"] not in acted[c[0]]
            enemy_half += cheb((c[3], c[4]), ec) < cheb((c[3], c[4]), oc)
        if len(alive) >= 2:
            ds = [cheb((a[3], a[4]), (b[3], b[4])) for i, a in enumerate(alive) for b in alive[i + 1:]]
            spread.append(sum(ds) / len(ds))
    n = max(1, alive_rounds)
    types = sorted({c[2] for c in first["champs"] if c[1] == team})
    return {
        "won": d["finalGems"][team] > d["finalGems"][1 - team], "gems": d["finalGems"][team], "opp": d["finalGems"][1 - team],
        "rounds": d["finalRound"], "g100": gems_at.get(100), "g200": gems_at.get(200), "g300": gems_at.get(300),
        "draft": types, "bodies": len(my_idx),
        "attackCampPerBody": act["Attack:camp"] / len(my_idx), "attackChampPerBody": act["Attack:champ"] / len(my_idx),
        "castPerBody": (act["Cast:camp"] + act["Cast:champ"]) / len(my_idx), "movePerBody": act["Move"] / len(my_idx),
        "idleShare": idle_rounds / n, "enemyHalfShare": enemy_half / n,
        "spread": statistics.mean(spread) if spread else 0, "deaths": deaths,
        "submits": submits, "bagAtSubmit": statistics.mean(bag_at_submit) if bag_at_submit else 0,
        "recalls": recalls,
    }


def summarise(label: str, rows: list[dict]) -> None:
    rows = [r for r in rows if r]
    if not rows:
        print(f"{label}: no games"); return
    m = lambda k: statistics.mean(r[k] for r in rows if r.get(k) is not None) if any(r.get(k) is not None for r in rows) else 0
    wins = sum(r["won"] for r in rows)
    drafts = collections.Counter(c for r in rows for c in r["draft"]).most_common(6)
    print(f"{label:<30} games {len(rows):>3}  won {wins:>3}  gems {m('gems'):5.0f}-{m('opp'):<5.0f} rounds {m('rounds'):4.0f}"
          f" | banked @100 {m('g100'):4.0f} @200 {m('g200'):4.0f} @300 {m('g300'):4.0f}")
    print(f"{'':<30} per body: camp attacks {m('attackCampPerBody'):5.0f}  champ attacks {m('attackChampPerBody'):4.0f}"
          f"  casts {m('castPerBody'):4.0f}  moves {m('movePerBody'):4.0f} | idle {m('idleShare'):.0%}"
          f"  enemy half {m('enemyHalfShare'):.0%}  spread {m('spread'):4.1f}")
    print(f"{'':<30} deaths {m('deaths'):.1f}  tributes {m('submits'):.1f} (bag {m('bagAtSubmit'):.0f})  recalls {m('recalls'):.1f}"
          f"  draft {drafts}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--scraped", nargs="*")
    ap.add_argument("--names", nargs="*")
    ap.add_argument("--experiment")
    ap.add_argument("--bot")
    a = ap.parse_args()
    if a.scraped:
        for k, tid in enumerate(a.scraped):
            rows = []
            for meta_file in glob.glob(str(ROOT / "lab/data/scraped/*.json")):
                meta = json.loads(Path(meta_file).read_text())
                side = 0 if (meta.get("teamA") or {}).get("teamId") == tid else 1 if (meta.get("teamB") or {}).get("teamId") == tid else None
                if side is None:
                    continue
                d = decode(Path(meta_file).with_suffix(".repl26a"))
                if d:
                    rows.append(profile(d, side))
            summarise(a.names[k] if a.names else tid[:8], rows)
    if a.experiment:
        e = [json.loads(Path(f).read_text()) for f in glob.glob(str(ROOT / "lab/data/experiments/*/experiment.json"))]
        e = [x for x in e if x.get("name") == a.experiment][-1]
        ours, theirs = [], []
        for g in e["games"]:
            d = decode(ROOT / g["result"]["replay"])
            if not d:
                continue
            me = 0 if g["candidateSide"] == "A" else 1
            ours.append(profile(d, me))
            theirs.append(profile(d, 1 - me))
        summarise(f"{a.bot or e['candidate']} (ours)", ours)
        summarise(f"{e['opponents'][0]} (opponent)", theirs)
    return 0


if __name__ == "__main__":
    sys.exit(main())
