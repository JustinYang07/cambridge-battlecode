#!/usr/bin/env python3
"""Every death of one team in a set of games, with the situation 3 rounds before it.

For each game (a stats.json from `cambc summary` plus its replay bundle beside it), decodes the
replay (lab/replay/decode.mjs) and, for each of our deaths, prints: round, champion, killer kind,
place (our half / middle / their half), and 3 rounds earlier our health, bag, and the champions of
each side within 6 tiles with their health. Then a summary over all games.

usage: python3 lab/tools/death_report.py <team name> <dir with *.stats.json + *.repl26a> [--quiet]
"""
import collections, json, subprocess, sys, tempfile, zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TYPES = {0: "Ryth", 1: "Vesper", 2: "Vendetta", 3: "Cassi", 4: "Issac", 5: "Stellar", 6: "Prism", 7: "Marcella",
         8: "Willow", 9: "Ren", 10: "Varg", 11: "Tavi", 12: "HoR"}


def cheb(a, b):
    return max(abs(a[0] - b[0]), abs(a[1] - b[1]))


def decode(rep: Path) -> dict:
    out = rep.with_suffix(".rounds.json")
    if not out.exists():
        with tempfile.TemporaryDirectory() as tmp:
            raw = Path(tmp) / "r.repl26a"
            raw.write_bytes(zipfile.ZipFile(rep).read("replay.repl26a"))
            # Games from before and after a patch need different engines: newest first until one loads.
            import glob, os, re
            engines = sorted(glob.glob(os.path.expanduser("~/.cache/cambc/assets/*/visualiser/assets/v*_*_*-*.wasm")) +
                             glob.glob(os.path.expanduser("~/Library/Caches/cambc/assets/*/visualiser/assets/v*_*_*-*.wasm")),
                             key=lambda f: tuple(int(v) for v in re.search(r"/v(\d+)_(\d+)_(\d+)-", f).groups()), reverse=True)
            for wasm in engines:
                p = subprocess.run(["node", str(ROOT / "lab/replay/decode.mjs"), str(raw), str(out)], capture_output=True,
                                   env={**os.environ, "CAMBC_ENGINE_WASM": wasm})
                if p.returncode == 0:
                    break
            else:
                raise RuntimeError(f"no engine decodes {rep.name}")
    return json.loads(out.read_text())


def main() -> int:
    team, folder = sys.argv[1], Path(sys.argv[2])
    quiet = "--quiet" in sys.argv
    summary = collections.Counter()
    situations = collections.Counter()
    for sf in sorted(folder.glob("*.stats.json")):
        d = json.loads(sf.read_text())
        rep = sf.with_name(sf.name.replace(".stats.json", ".repl26a"))
        if not rep.exists():
            continue
        names = {}
        for t in ("teamA", "teamB"):
            pass
        meta_team = None
        # Our side: the team whose name matches (summary rosters carry team ids only, so use the
        # bundle's sides from the match-group listing saved next to it if present, else side A).
        side_file = sf.with_name(sf.name.replace(".stats.json", ".side"))
        us = side_file.read_text().strip() if side_file.exists() else "teamA"
        T = 0 if us == "teamA" else 1
        rounds = {r["round"]: r for r in decode(rep)["rounds"] if r["champions"]}
        if not rounds:
            continue
        first = rounds[min(rounds)]
        sp = {k: (sum(c["x"] for c in first["champions"] if c["team"] == k) / max(1, sum(1 for c in first["champions"] if c["team"] == k)),
                  sum(c["y"] for c in first["champions"] if c["team"] == k) / max(1, sum(1 for c in first["champions"] if c["team"] == k))) for k in (0, 1)}
        mp = sf.name
        if not quiet:
            print(f"== {mp}  we are {us}  score {[t['submittedGems'] for t in d['teams']]}")
        for e in d["events"]:
            if e["kind"] != "death" or e["owner"].get("team") != us:
                continue
            for x in e["entries"]:
                r = x["timestamp"]["round"]
                pos = (x["x"], x["y"])
                ours = cheb(pos, sp[T]); theirs = cheb(pos, sp[1 - T])
                place = "our half" if ours < 0.4 * (ours + theirs) else "their half" if ours > 0.6 * (ours + theirs) else "middle"
                before = rounds.get(r - 3) or rounds.get(r - 2) or rounds.get(r - 1)
                me = mine = them = None
                if before:
                    near = [c for c in before["champions"] if c["alive"] and cheb((c["x"], c["y"]), pos) <= 6]
                    mine = [c for c in near if c["team"] == T]
                    them = [c for c in near if c["team"] != T]
                    me = min(mine, key=lambda c: cheb((c["x"], c["y"]), pos), default=None)
                killer = x["context"]
                summary[(killer, place)] += 1
                if mine is not None:
                    key = f"{len(mine)}v{len(them)}" if killer == "champion" else "camp"
                    situations[key] += 1
                if not quiet:
                    desc = ""
                    if mine is not None:
                        desc = (f"hp {me['hp'] if me else '?'}/{me['maxHp'] if me else '?'} bag {me['gems'] if me else '?'} | "
                                f"us {len(mine)} [{','.join(TYPES.get(c['type'], c['type'])[:4] + ':' + str(c['hp']) for c in mine)}] "
                                f"them {len(them)} [{','.join(TYPES.get(c['type'], c['type'])[:4] + ':' + str(c['hp']) for c in them)}]")
                    print(f"  r{r:5d} {e['owner']['name']:<13} by {killer:<9} {place:<10} {desc}")
    print("deaths by killer and place:", dict(summary))
    print("champion deaths by local numbers 3 rounds before (us v them within 6):", dict(situations.most_common()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
