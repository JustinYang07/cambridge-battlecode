#!/usr/bin/env python3
"""Forensic autopsy of a set of games for one team (stats.json from `cambc summary` + replay bundle,
and a <match>.side file holding teamA/teamB for our side).

Deaths: every one of our champion deaths with the local numbers 2 rounds before (champions within
6 tiles), whether we were isolated (no ally within 4), monster damage taken in the 3 rounds before,
Cassi & Issac linked deaths, deaths while channelling a tribute or recall, and deaths within 3 rounds
of a capture. Macro (rounds 1..--upto): monster damage dealt per 100 rounds, captures and split
captures, champion-rounds spent walking, standing still, and standing next to a capture, gems per
tribute.

usage: python3 lab/tools/autopsy.py <dir> [--upto 1500] [--maps a,b]
"""
import collections, glob, json, os, re, subprocess, sys, tempfile, zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def cheb(a, b):
    return max(abs(a[0] - b[0]), abs(a[1] - b[1]))


def norm(n):
    return n.replace(" ", "").replace("&", "").lower()


def engines():
    fs = glob.glob(os.path.expanduser("~/.cache/cambc/assets/*/visualiser/assets/v*_*_*-*.wasm")) + \
        glob.glob(os.path.expanduser("~/Library/Caches/cambc/assets/*/visualiser/assets/v*_*_*-*.wasm"))
    return sorted(fs, key=lambda f: tuple(int(v) for v in re.search(r"/v(\d+)_(\d+)_(\d+)-", f).groups()), reverse=True)


def decode(rep: Path) -> dict:
    out = rep.with_suffix(".actions.json")
    if out.exists():
        return json.loads(out.read_text())
    with tempfile.TemporaryDirectory() as tmp:
        raw = Path(tmp) / "r.repl26a"
        raw.write_bytes(zipfile.ZipFile(rep).read("replay.repl26a"))
        for w in engines():
            p = subprocess.run(["node", str(ROOT / "lab/tools/decode_replay.mjs"), str(raw), "--wasm", w], capture_output=True, text=True)
            if p.returncode == 0 and p.stdout.strip():
                out.write_text(p.stdout)
                return json.loads(p.stdout)
    raise RuntimeError(f"no engine decodes {rep}")


def cumulative(runs, upto):
    """Value of a stats timeline (list of {duration, value}) at each round 0..upto."""
    vals, r = [], 0
    for run in runs:
        for _ in range(run["duration"]):
            vals.append(float(run["value"]))
            r += 1
    while len(vals) <= upto:
        vals.append(vals[-1] if vals else 0.0)
    return vals


def main() -> int:
    folder = Path(sys.argv[1])
    upto = int(sys.argv[sys.argv.index("--upto") + 1]) if "--upto" in sys.argv else 1500
    only = set(sys.argv[sys.argv.index("--maps") + 1].split(",")) if "--maps" in sys.argv else None
    D = collections.Counter()
    numbers = collections.Counter()
    for sf in sorted(folder.glob("*.stats.json")):
        mid = sf.name[:-11]
        d = json.loads(sf.read_text())
        side = (folder / f"{mid}.side").read_text().strip()
        rep = folder / f"{mid}.repl26a"
        mp = (folder / f"{mid}.map").read_text().strip() if (folder / f"{mid}.map").exists() else mid[:8]
        if only and mp not in only:
            continue
        T = 0 if side == "teamA" else 1
        g = decode(rep)
        R = {x["r"]: x for x in g["rounds"] if x["champs"]}
        last = max(R)
        # Stats timelines by (team, name).
        tl = collections.defaultdict(dict)
        for t in d["championTimelines"]:
            o = t["owner"]
            tl[(o["team"], norm(o["name"]))][t["metric"]] = cumulative(t["runs"], last + 5)
        ours = "teamA" if T == 0 else "teamB"
        # Deaths: alive 1 -> 0 for our champions.
        prev = {}
        lastpos = {}
        kinds = {}
        for e in d["events"]:
            if e["kind"] == "death" and e["owner"].get("team") == ours:
                for x in e["entries"]:
                    kinds[(norm(e["owner"]["name"]), x["timestamp"]["round"])] = x["context"]
        caps = [(x["timestamp"]["round"], (x["x"], x["y"]), e["owner"].get("team"), int(x["value"]))
                for e in d["events"] if e["kind"] == "gemCollection" for x in e["entries"]]
        acts = collections.defaultdict(list)
        for a in g["actions"]:
            if a[2] == 1:
                acts[a[3]].append(a)
        gd = {"deaths": 0, "champ": 0}
        for r in sorted(R):
            for c in R[r]["champs"]:
                idx, team, name, x, y, hp, mx, alive = c[:8]
                if team != T:
                    continue
                if prev.get(idx) == 1 and not alive:
                    # A dead champion's tile is not where it fell: use its last living tile.
                    pos = lastpos.get(idx, (x, y))
                    kind = next((kinds[(norm(name), q)] for q in (r, r - 1, r + 1) if (norm(name), q) in kinds), "?")
                    D["deaths"] += 1
                    D["deaths:" + kind] += 1
                    gd["deaths"] += 1
                    b = R.get(r - 2) or R.get(r - 1)
                    if b and kind == "champion":
                        gd["champ"] += 1
                        near = [z for z in b["champs"] if z[7] and cheb((z[3], z[4]), pos) <= 6]
                        us = sum(1 for z in near if z[1] == T); them = sum(1 for z in near if z[1] != T)
                        numbers["fewer" if us < them else "even" if us == them else "more"] += 1
                        numbers[f"{us}v{them}"] += 1
                        close = [z for z in b["champs"] if z[7] and z[1] == T and z[0] != idx and cheb((z[3], z[4]), pos) <= 4]
                        if not close and them >= 1:
                            D["isolated (no ally within 4)"] += 1
                        if not close and them >= 2:
                            D["isolated vs 2+"] += 1
                        mdt = tl[(ours, norm(name))].get("monsterDamageTaken")
                        if mdt:
                            took = mdt[min(r, len(mdt) - 1)] - mdt[max(r - 3, 0)]
                            if took > 0:
                                D["took monster damage in last 3 rounds"] += 1
                                D["monster damage total (last 3 rounds)"] += int(took)
                    if name in ("Cassi", "Issac"):
                        partner = next((z for z in (R.get(r - 1) or R[r])["champs"] if z[1] == T and z[2] == ("Issac" if name == "Cassi" else "Cassi")), None)
                        if partner and partner[7] and partner[5] > 0.4 * partner[6]:
                            now = next((z for z in R[r]["champs"] if z[0] == partner[0]), None)
                            if now and not now[7]:
                                D["Cassi/Issac linked death (partner >40% hp)"] += 1
                    mine = acts[idx]
                    if any(a[1] == "Recall" and r - 5 <= a[0] <= r for a in mine):
                        D["while recalling"] += 1
                    if any(a[1] == "SubmitGems" and r - 12 <= a[0] <= r for a in mine) and not any(a[1] == "GemsSubmitted" and r - 12 <= a[0] <= r for a in mine):
                        D["while tributing"] += 1
                    if any(cr <= r <= cr + 3 and cheb(cp, pos) <= 3 and ct == ours for cr, cp, ct, _ in caps):
                        D["within 3 rounds of our capture"] += 1
                prev[idx] = alive
                if alive:
                    lastpos[idx] = (x, y)
        print(f"{mp:<18} we are {side}  score {[t['submittedGems'] for t in d['teams']]}  our deaths {gd['deaths']} (to champions {gd['champ']})")
        # Macro, rounds 1..upto, both teams.
        for team, tname in ((T, "us"), (1 - T, "them")):
            tk = "teamA" if team == 0 else "teamB"
            mdd = sum(v["monsterDamageDone"][min(upto, len(v["monsterDamageDone"]) - 1)] for (tt, n), v in tl.items() if tt == tk and "monsterDamageDone" in v)
            mine = [c for c in caps if c[2] == tk and c[0] <= upto]
            groups = collections.Counter((c[0], c[1]) for c in mine)
            by_round = collections.defaultdict(list)
            for c in mine:
                by_round[c[0]].append(c)
            split = sum(1 for rr, cs in by_round.items() for i, a in enumerate(cs) for b2 in cs[i + 1:] if cheb(a[1], b2[1]) <= 3)
            walk = still = 0
            beside = 0
            for r in range(2, min(upto, last)):
                if r not in R or r - 1 not in R:
                    continue
                for c in R[r]["champs"]:
                    if c[1] != team or not c[7]:
                        continue
                    p = next((z for z in R[r - 1]["champs"] if z[0] == c[0]), None)
                    if p and p[7]:
                        if (p[3], p[4]) != (c[3], c[4]):
                            walk += 1
                        else:
                            still += 1
            # Allies within 2 tiles of each capture in the 5 rounds before it, beyond the capturer.
            for cr, cp, ct, _ in mine:
                for q in range(cr - 5, cr):
                    if q in R:
                        beside += max(0, sum(1 for z in R[q]["champs"] if z[1] == team and z[7] and cheb((z[3], z[4]), cp) <= 2) - 1)
            subs = [int(x["value"]) for e in d["events"] if e["kind"] == "gemSubmission" and e["owner"].get("team") == tk for x in e["entries"] if x["timestamp"]["round"] <= upto]
            print(f"   {tname:<5} monster dmg/100r {mdd / max(upto, 1) * 100:7.0f}  captures {len(mine):3d} (split {split:2d})  "
                  f"walking {walk:5d}  still {still:5d}  extra allies at captures {beside:4d}  tributes {len(subs):3d} avg {sum(subs) / max(len(subs), 1):5.1f}")
    print("--- our deaths over all games")
    for k, v in sorted(D.items(), key=lambda kv: -kv[1]):
        print(f"  {k:<45} {v}")
    print("  local numbers 2 rounds before (champion kills):", dict(numbers))
    return 0


if __name__ == "__main__":
    sys.exit(main())
