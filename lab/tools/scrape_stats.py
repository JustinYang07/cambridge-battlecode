#!/usr/bin/env python3
"""Fetch the match statistics (.camstat -> JSON) for every scraped replay in lab/data/scraped/.

Platform bundles hold only the replay; /api/v1/matches/<id>/summary links the binary summary,
which `cambc summary` turns into the same JSON as a local game's stats.json (lab/tools/leaks.py).
Writes lab/data/scraped/<match>.stats.json; already fetched matches are skipped.
"""
import json, subprocess, sys, urllib.request
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from labkit import config, platform as P

def main() -> int:
    c = P.Platform()
    cambc = config.cambc_binary()
    for meta in sorted(config.SCRAPED_DIR.glob("*.json")):
        if meta.name.endswith(".stats.json"):
            continue
        mid = json.loads(meta.read_text())["matchId"]
        out = config.SCRAPED_DIR / f"{mid}.stats.json"
        if out.exists():
            continue
        st, _, body = c._request(f"/api/v1/matches/{mid}/summary", raw=True)
        info = json.loads(body) if st == 200 else {}
        if info.get("status") != "ready":
            print(f"{mid}: summary {st} {info.get('status')}")
            continue
        raw = config.SCRAPED_DIR / f"{mid}.camstat"
        raw.write_bytes(urllib.request.urlopen(info["url"], timeout=60).read())
        subprocess.run([str(cambc), "summary", str(raw), "-o", str(out)], check=True, capture_output=True)
        raw.unlink()
    return 0

if __name__ == "__main__":
    sys.exit(main())
