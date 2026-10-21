#!/usr/bin/env python3
"""How many turns dashes can save on each map's trips (lab/data/maps/*.json): from our spawn to every
camp group (a tile beside one of its camps), walking only, our greedy rule (dash when the landing is
2+ steps nearer on the walking field, then walk on), and the best plan with dashes (range 2, cooldown
CD turns, dash + walk in one turn; a dash crosses holes but not walls, and lands on open floor).

usage: python3 lab/tools/dash_routes.py [--cd 8] [--maps a,b]
"""
import collections, glob, heapq, json, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CD = int(sys.argv[sys.argv.index("--cd") + 1]) if "--cd" in sys.argv else 8
ONLY = set(sys.argv[sys.argv.index("--maps") + 1].split(",")) if "--maps" in sys.argv else None
STEPS = [(dx, dy) for dx in (-1, 0, 1) for dy in (-1, 0, 1) if dx or dy]


def load(f):
    m = json.loads(Path(f).read_text())
    W, H = m["w"], m["h"]
    wall = [[m["walls"][y][x] == "1" for x in range(W)] for y in range(H)]
    hole = [[m["holes"][y][x] == "1" for x in range(W)] for y in range(H)]
    return m, W, H, wall, hole


def main():
    rows = []
    for f in sorted(glob.glob(str(ROOT / "lab/data/maps/*.json"))):
        m, W, H, wall, hole = load(f)
        if ONLY and m["name"] not in ONLY:
            continue
        free = lambda x, y: 0 <= x < W and 0 <= y < H and not wall[y][x] and not hole[y][x]
        camp_tiles = {(c["x"], c["y"]) for g in m["groups"] for c in g["camps"]}

        def walk_field(goals):
            d = {t: 0 for t in goals}
            q = collections.deque(goals)
            while q:
                x, y = q.popleft()
                for dx, dy in STEPS:
                    t = (x + dx, y + dy)
                    if t not in d and free(*t) and t not in camp_tiles:
                        d[t] = d[(x, y)] + 1
                        q.append(t)
            return d

        def dash_targets(x, y):
            for dx in range(-2, 3):
                for dy in range(-2, 3):
                    if max(abs(dx), abs(dy)) != 2:
                        continue
                    t = (x + dx, y + dy)
                    if not free(*t) or t in camp_tiles:
                        continue
                    # an intermediate tile next to both that is not a wall (holes are fine)
                    if any(0 <= x + ax < W and 0 <= y + ay < H and not wall[y + ay][x + ax] and max(abs(x + ax - t[0]), abs(y + ay - t[1])) <= 1
                           for ax in (-1, 0, 1) for ay in (-1, 0, 1) if ax or ay):
                        yield t

        spawn = [tuple(p) for p in m["spawnA"]]
        res = []
        for gi, g in enumerate(m["groups"]):
            goals = {(c["x"] + dx, c["y"] + dy) for c in g["camps"] for dx, dy in STEPS if free(c["x"] + dx, c["y"] + dy) and (c["x"] + dx, c["y"] + dy) not in camp_tiles}
            if not goals:
                continue
            F = walk_field(goals)
            starts = [s for s in spawn if s in F]
            if not starts:
                continue
            s0 = min(starts, key=lambda s: F[s])
            walk = F[s0]
            # greedy: dash when ready and a landing is 2+ nearer, then walk a step
            pos, t, cd = s0, 0, 0
            while F[pos] > 0 and t < 500:
                if cd == 0:
                    best = min(dash_targets(*pos), key=lambda q: F.get(q, 10**9), default=None)
                    if best and F.get(best, 10**9) <= F[pos] - 2:
                        pos, cd = best, CD
                if F[pos] > 0:
                    pos = min(((pos[0] + dx, pos[1] + dy) for dx, dy in STEPS), key=lambda q: F.get(q, 10**9))
                t += 1
                cd = max(cd - 1, 0)
            greedy = t
            # optimal: Dijkstra over (tile, cooldown); a turn is a walk, or a dash then a walk
            best = {(s0, 0): 0}
            pq = [(0, s0, 0)]
            opt = None
            while pq:
                tt, p, c = heapq.heappop(pq)
                if best.get((p, c)) != tt:
                    continue
                if p in goals:
                    opt = tt
                    break
                nc = max(c - 1, 0)
                outs = [(p, nc)] + [((p[0] + dx, p[1] + dy), nc) for dx, dy in STEPS if free(p[0] + dx, p[1] + dy) and (p[0] + dx, p[1] + dy) not in camp_tiles]
                if c == 0:
                    for q in dash_targets(*p):
                        outs.append((q, CD - 1))
                        outs += [((q[0] + dx, q[1] + dy), CD - 1) for dx, dy in STEPS if free(q[0] + dx, q[1] + dy) and (q[0] + dx, q[1] + dy) not in camp_tiles]
                for q, qc in outs:
                    if best.get((q, qc), 10**9) > tt + 1:
                        best[(q, qc)] = tt + 1
                        heapq.heappush(pq, (tt + 1, q, qc))
            res.append((gi, walk, greedy, opt if opt is not None else walk))
        tw = sum(r[1] for r in res); tg = sum(r[2] for r in res); to = sum(r[3] for r in res)
        big = max(res, key=lambda r: r[2] - r[3], default=None)
        rows.append((m["name"], len(res), tw, tg, to, big))
        print(f"{m['name']:<22} groups {len(res):3d}  turns to every camp: walk {tw:5d}  greedy {tg:5d}  optimal {to:5d}  "
              f"(greedy saves {1 - tg / max(tw, 1):5.1%}, optimal {1 - to / max(tw, 1):5.1%})  largest greedy-optimal gap g{big[0]}: {big[2]} vs {big[3]}" if big else "")
    tw = sum(r[2] for r in rows); tg = sum(r[3] for r in rows); to = sum(r[4] for r in rows)
    print(f"ALL: walk {tw}  greedy {tg} ({1 - tg / tw:.1%})  optimal {to} ({1 - to / tw:.1%})")


if __name__ == "__main__":
    main()
