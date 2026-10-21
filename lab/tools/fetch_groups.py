#!/usr/bin/env python3
"""Download our recent match groups against one team (bundle + stats.json + .side/.map, the folder
layout autopsy.py and the other lab/tools reports read). Waits for queued groups to finish and for
bundles that are still being prepared (HTTP 202).

usage: python3 lab/tools/fetch_groups.py <opponent team id> <out dir> [--groups N] [--version V] [--cambc PATH]
"""
import json, subprocess, sys, time, urllib.request
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from labkit import config, platform as P

US = "46ba6067-4045-4e84-a065-951204db6b5d"


def arg(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


def main() -> int:
    team, out = sys.argv[1], Path(sys.argv[2])
    n = int(arg("--groups", "1"))
    version = arg("--version")
    cambc = arg("--cambc", str(config.cambc_binary()))
    c = P.Platform()
    out.mkdir(parents=True, exist_ok=True)
    for _ in range(240):
        gs = [g for g in c.match_groups(US)["groups"][:40]
              if team in [s["teamId"] for s in g["sides"]] and (not version or int(version) in [s["version"] for s in g["sides"]])][:n]
        if len(gs) >= n and all(g["status"] == "finished" for g in gs):
            break
        time.sleep(20)
    todo = []
    for g in gs:
        for m in c._get(f"/api/v1/match-groups/{g['id']}")["matches"]:
            todo.append(m)
    for _ in range(30):
        left = []
        for m in todo:
            mid = m["id"]
            a = next(s for s in m["sides"] if s["teamId"] == US)
            if (out / f"{mid}.stats.json").exists():
                continue
            if not (out / f"{mid}.repl26a").exists():
                st, _, body = c._request(f"/api/v1/matches/{mid}/bundle", raw=True, timeout=60)
                time.sleep(0.5)
                if st != 200 or not body.startswith(b"PK"):
                    left.append(m)
                    continue
                (out / f"{mid}.repl26a").write_bytes(body)
            (out / f"{mid}.side").write_text("teamA" if a["side"] == "a" else "teamB")
            (out / f"{mid}.map").write_text(m["map"])
            (out / f"{mid}.result").write_text(json.dumps({"result": a["result"], "us": a["score"],
                                                           "them": next(s for s in m["sides"] if s["teamId"] != US)["score"], "rounds": m["rounds"]}))
            info = json.loads(c._request(f"/api/v1/matches/{mid}/summary", raw=True)[2])
            if info.get("status") != "ready":
                left.append(m)
                continue
            raw = out / f"{mid}.camstat"
            raw.write_bytes(urllib.request.urlopen(info["url"], timeout=60).read())
            subprocess.run([cambc, "summary", str(raw), "-o", str(out / f"{mid}.stats.json")], check=True, capture_output=True)
            raw.unlink()
        todo = left
        if not todo:
            break
        time.sleep(20)
    print(f"{len(gs)} groups, {len(todo)} matches not fetched")
    for f in sorted(out.glob("*.result")):
        r = json.loads(f.read_text())
        print(f"  {(out / (f.stem + '.map')).read_text():<24} {r['result']:<5} {r['us']:>5}-{r['them']:<5} r{r['rounds']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
