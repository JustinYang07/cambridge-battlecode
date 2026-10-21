#!/usr/bin/env python3
"""Per-map gem difference (candidate minus opponent) for an A/B experiment, sides and samples pooled."""
import collections, json, re, sys
from pathlib import Path

def main() -> int:
    diff = collections.defaultdict(list)
    for g in sorted((Path(sys.argv[1]) / "games").iterdir()):
        f = g / "stats.json"
        m = re.match(r"\d+-(.+)-([AB])(?:-.*)?$", g.name)
        if not f.exists() or not m:
            continue
        d = json.loads(f.read_text())
        gems = {t["team"]: int(t["submittedGems"]) for t in d["teams"]}
        c, o = ("teamA", "teamB") if m.group(2) == "A" else ("teamB", "teamA")
        diff[m.group(1)].append(gems[c] - gems[o])
    for k in sorted(diff):
        v = diff[k]
        print(f"{k:<24}{sum(v) / len(v):>7.0f}  {sum(x > 0 for x in v)}/{len(v)} won  {v}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
