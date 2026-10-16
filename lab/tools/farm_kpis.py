#!/usr/bin/env python3
"""Farming throughput of both teams in A/B experiments: the numbers the top ladder teams beat us on.

  python3 lab/tools/farm_kpis.py "experiment name" ["another experiment" ...] [--reasons]

Per team (candidate and opponent), averaged over the experiment's games:
  dmg/body/rd  monster damage per champion per round (engine statistics)   Mite ~23, top-5 20-24
  gems/rd      gems collected per round, gold/rd gold earned per round
  hit/move/bank/nothing   share of living champion-turns whose best action was an attack or cast,
               a move, a tribute or recall, or nothing at all (from the engine replay)   Mite 32/50/2/16
  deaths       per game
--reasons breaks the candidate's do-nothing turns down by what its decision trace says it was doing.
"""
from __future__ import annotations

import argparse
import collections
import glob
import json
import statistics
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "lab"))
sys.path.insert(0, str(ROOT / "lab" / "tools"))
from labkit import replays  # noqa: E402
from team_behaviour import decode  # noqa: E402

PRIORITY = {"Attack": 3, "Cast": 3, "SubmitGems": 2, "Recall": 2, "Move": 1}


def engine_stats(result: dict, i: int) -> dict:
    t = result["teams"][i]
    ch = t["champions"]
    n, rounds = max(1, len(ch)), max(1, result["rounds"])
    return {"dmg": sum(c.get("monsterDamageDone", 0) for c in ch) / n / rounds,
            "gems": sum(c.get("collectedGems", 0) for c in ch) / rounds,
            "gold": sum(c.get("earnedGold", 0) for c in ch) / rounds,
            "deaths": t["deaths"], "rounds": result["rounds"]}


def turn_use(d: dict, team: int) -> tuple[dict, dict]:
    """Share of living turns by best action, and the (champion idx, round) turns with no action."""
    best: dict = collections.defaultdict(dict)
    for rnd, kind, akind, aidx, *_ in d["actions"]:
        p = PRIORITY.get(kind, 0)
        if akind == 1 and p > best[aidx].get(rnd, 0):
            best[aidx][rnd] = p
    counts = collections.Counter()
    idle = collections.defaultdict(list)
    for r in d["rounds"]:
        for c in r["champs"]:
            if c[1] != team or not c[7]:
                continue
            p = best[c[0]].get(r["r"], 0)
            counts[{3: "hit", 2: "bank", 1: "move", 0: "nothing"}[p]] += 1
            if p == 0:
                idle[c[0]].append((r["r"], c[3], c[4]))
    total = max(1, sum(counts.values()))
    return {k: counts[k] / total for k in ("hit", "move", "bank", "nothing")}, idle


def idle_reasons(bundle_team: dict, idle: dict) -> collections.Counter:
    """Label each do-nothing turn with the trace's decision for that champion and round."""
    by_pos = {}
    for x in bundle_team["turns"]:
        by_pos[(x["r"], x["x"], x["y"])] = x
    out = collections.Counter()
    for _, turns in idle.items():
        for rnd, x, y in turns:
            t = by_pos.get((rnd, x, y))
            if not t:
                out["(no trace: lost turn)"] += 1
                continue
            act = (t.get("act") or "").strip()
            label = (t.get("label") or "").split("@")[0]
            if "cap" in act:
                why = "capture: standing on tiles"
            elif "hold" in act:
                why = "farm: holding for party"
            elif t["k"] in ("farm", "capture") and not act:
                why = f"{t['k']}: no action ({'cluster' if 'gem' in label or 'gold' in label else 'monster camp'})"
            elif t["k"] == "wait":
                why = f"wait: {label or 'idle'}"
            else:
                why = f"{t['k']}: {act or 'no action'}"
            out[why] += 1
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("experiments", nargs="+")
    ap.add_argument("--reasons", action="store_true")
    a = ap.parse_args()
    exps = {}
    for f in sorted(glob.glob(str(ROOT / "lab/data/experiments/*/experiment.json"))):
        e = json.loads(Path(f).read_text())
        exps[e.get("name")] = e
    print(f"{'experiment / team':<46}{'win':>5}{'dmg/body/rd':>12}{'gems/rd':>8}{'gold/rd':>8}"
          f"{'hit':>6}{'move':>6}{'bank':>6}{'nothing':>8}{'deaths':>7}{'rounds':>7}")
    for name in a.experiments:
        e = exps.get(name)
        if not e:
            print(f"{name}: not found")
            continue
        rows = {"cand": [], "opp": []}
        reasons = collections.Counter()
        for g in e["games"]:
            r = g.get("result") or {}
            if "teams" not in r:
                continue
            me = 0 if g["candidateSide"] == "A" else 1
            d = decode(ROOT / r["replay"])
            for who, i in (("cand", me), ("opp", 1 - me)):
                st = engine_stats(r, i)
                st["win"] = r["teams"][i]["gems"] > r["teams"][1 - i]["gems"]
                if d and d["rounds"]:
                    use, idle = turn_use(d, i)
                    st.update(use)
                    if who == "cand" and a.reasons:
                        bundle = replays.read_bundle(ROOT / r["replay"])
                        reasons += idle_reasons(bundle["teams"]["a" if i == 0 else "b"], idle)
                rows[who].append(st)
        for who, label in (("cand", e["candidate"]), ("opp", e["opponents"][0])):
            rs = rows[who]
            if not rs:
                continue
            m = lambda k: statistics.mean(x.get(k, 0) for x in rs)  # noqa: E731
            print(f"{(name[:28] + ' / ' + label)[:45]:<46}{sum(x['win'] for x in rs):>3}/{len(rs):<2}{m('dmg'):>11.1f}"
                  f"{m('gems'):>8.2f}{m('gold'):>8.1f}{m('hit'):>6.0%}{m('move'):>6.0%}{m('bank'):>6.0%}"
                  f"{m('nothing'):>8.0%}{m('deaths'):>7.1f}{m('rounds'):>7.0f}")
        if a.reasons and reasons:
            total = sum(reasons.values())
            print(f"  candidate's do-nothing turns ({total}):")
            for why, n in reasons.most_common(12):
                print(f"    {n / total:>5.0%}  {why}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
