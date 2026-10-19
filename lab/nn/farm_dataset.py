#!/usr/bin/env python3
"""Imitation dataset for camp choice: which camp group a team's champions attack next.

For every replay with the target team, decode it with the engine, then every 5 rounds for each
alive champion of that team: label = the camp group it attacks next (within 40 rounds), and one
feature row per candidate group. Features must match brain/farm.cpp's nn features (nnFeatures).

usage: farm_dataset.py <team name> <out.npz> [--max N]
"""
import glob, json, os, subprocess, sys
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WASM = sorted(glob.glob(os.path.expanduser('~/Library/Caches/cambc/assets/*/visualiser/assets/v0_0_3-*.wasm')))[0]
NF = 19
STEP, HORIZON = 5, 40

def mapfor(name, d, o):
    n = len(o[max(o)]['groups']); mons = set(tuple(a[4]) for a in d['actions'] if a[2] == 2); best = None
    for f in sorted(glob.glob(f'{ROOT}/lab/data/maps/{name}-*.json')) + sorted(glob.glob(f'{ROOT}/lab/data/maps/{name.replace("_","-")}-*.json')):
        j = json.load(open(f))
        if len(j['groups']) != n: continue
        sc = len(mons & set((c['x'], c['y']) for g in j['groups'] for c in g['camps']))
        if best is None or sc > best[0]: best = (sc, j)
    return best[1] if best else None

def cheb(a, b): return max(abs(a[0] - b[0]), abs(a[1] - b[1]))

def features(me, g, gi, st, r, champs, T, sp_us, sp_them, last_target, heading=0):
    # me: champ row; g: group json; st: (status, back_round)
    tiles = [(c['x'], c['y']) for c in g['camps']]
    cx = sum(t[0] for t in tiles) / len(tiles); cy = sum(t[1] for t in tiles) / len(tiles); cen = (round(cx), round(cy))
    pos = (me[3], me[4])
    d = min(cheb(pos, t) for t in tiles)
    strength = sum(c.get('str', 0) for c in g['camps']); health = sum(c.get('hp', 0) for c in g['camps'])
    allies_camp = sum(1 for c in champs if c[1] == T and c[7] and c[0] != me[0] and cheb((c[3], c[4]), cen) <= 5)
    enemies_camp = sum(1 for c in champs if c[1] != T and c[7] and cheb((c[3], c[4]), cen) <= 8 and cheb((c[3], c[4]), pos) <= 10)
    allies_me = sum(1 for c in champs if c[1] == T and c[7] and c[0] != me[0] and cheb((c[3], c[4]), pos) <= 4)
    enemies_me = sum(1 for c in champs if c[1] != T and c[7] and cheb((c[3], c[4]), pos) <= 8)
    status, back = st
    our_half = 1.0 if cheb(cen, sp_us) < cheb(cen, sp_them) else 0.0
    wait = min(1.0, max(0, back - r) / 40.0) if status == 'respawn' else 0.0
    return [d / 30.0, strength / 60.0, health / 3000.0, g['gems'] / 60.0, g['gold'] / 150.0,
            me[5] / max(me[6], 1), me[9] / 60.0, allies_camp / 3.0, enemies_camp / 3.0, our_half,
            cheb(cen, sp_us) / 40.0, r / 1000.0, 1.0 if status == 'reward' else 0.0, wait,
            allies_me / 3.0, 1.0 if last_target == gi else 0.0, me[6] / 1500.0, enemies_me / 3.0, heading / 3.0]

def process(meta, path):
    side = 'A' if (meta.get('teamA') or {}).get('teamName') == TEAM else 'B' if (meta.get('teamB') or {}).get('teamName') == TEAM else None
    if side is None: return []
    out = subprocess.run(['node', f'{ROOT}/lab/tools/decode_replay.mjs', path, '--wasm', WASM], capture_output=True, text=True)
    if out.returncode != 0: return []
    d = json.loads(out.stdout); o = {}
    for x in d['rounds']: o.setdefault(x['r'], x)
    M = mapfor(meta['map'], d, o)
    if not M: return []
    G = M['groups']; T = 0 if side == 'A' else 1
    sp_us = tuple(M['spawnA' if T == 0 else 'spawnB'][0]); sp_them = tuple(M['spawnB' if T == 0 else 'spawnA'][0])
    tile2g = {(c['x'], c['y']): i for i, g in enumerate(G) for c in g['camps']}
    rs = sorted(o)
    # who attacks which group when (team T champions)
    hits = {}
    chan = set()
    for a in d['actions']:
        if a[2] == 1 and a[1] in ('Attack', 'Cast') and a[5] and tuple(a[5]) in tile2g:
            hits.setdefault(a[3], []).append((a[0], tile2g[tuple(a[5])]))
        if a[2] == 1 and a[1] == 'Recall':
            for k in range(5): chan.add((a[0] + k, a[3]))
    for v in hits.values(): v.sort()
    # group status timeline
    status = ['alive'] * len(G); back = [0] * len(G); stat_at = {}; prev = None
    for r in rs:
        x = o[r]
        for gi, v in enumerate(x['groups'][:len(G)]):
            if status[gi] == 'respawn' and r >= back[gi]: status[gi] = 'alive'
            if v & 1: status[gi] = 'reward' if (v >> 1) in (2, T) else 'contested'
            elif prev and (prev['groups'][gi] & 1): status[gi] = 'respawn'; back[gi] = r + G[gi]['respawn']
        stat_at[r] = (list(status), list(back))
        prev = x
    rows = []
    for r in rs:
        if r % STEP or r < 5: continue
        x = o[r]; st, bk = stat_at[r]
        for me in x['champs']:
            if me[1] != T or not me[7] or (r, me[0]) in chan: continue
            h = hits.get(me[0], [])
            nxt = next((g for (rr, g) in h if r <= rr <= r + HORIZON), None)
            if nxt is None: continue
            last = next((g for (rr, g) in reversed(h) if r - 10 <= rr < r), None)
            cands = [gi for gi in range(len(G)) if (G[gi]['gems'] > 0 or G[gi]['gold'] > 0) and
                     (st[gi] in ('alive', 'reward') or (st[gi] == 'respawn' and bk[gi] - r <= 20))]
            if nxt not in cands or len(cands) < 2: continue
            # teammates heading to each camp: their next attacked group (the bot reads this from claims)
            heads = {}
            for mate in x['champs']:
                if mate[1] != T or not mate[7] or mate[0] == me[0]: continue
                nm = next((g for (rr, g) in hits.get(mate[0], []) if r <= rr <= r + HORIZON), None)
                if nm is not None: heads[nm] = heads.get(nm, 0) + 1
            F = [features(me, G[gi], gi, (st[gi], bk[gi]), r, x['champs'], T, sp_us, sp_them, last, heads.get(gi, 0)) for gi in cands]
            rows.append((np.array(F, np.float32), cands.index(nxt), meta['matchId'], meta['map']))
    return rows

def main():
    global TEAM
    TEAM = sys.argv[1]; OUT = sys.argv[2]
    MAX = int(sys.argv[sys.argv.index('--max') + 1]) if '--max' in sys.argv else 10**9
    metas = []
    for f in glob.glob(f'{ROOT}/lab/data/scraped/*.json'):
        m = json.load(open(f))
        if TEAM in ((m.get('teamA') or {}).get('teamName'), (m.get('teamB') or {}).get('teamName')):
            metas.append((m, f[:-5] + '.repl26a'))
    metas.sort(key=lambda t: t[0].get('createdAt') or '', reverse=True)
    allrows = []; used = 0
    for m, p in metas[:MAX]:
        rows = process(m, p)
        if rows: used += 1
        allrows += rows
        print(f"{m['map']:12s} {len(rows):5d} samples", file=sys.stderr)
    X = np.concatenate([r[0] for r in allrows]); off = np.cumsum([0] + [len(r[0]) for r in allrows])
    y = np.array([r[1] for r in allrows]); game = np.array([r[2] for r in allrows]); mp = np.array([r[3] for r in allrows])
    np.savez(OUT, X=X, off=off, y=y, game=game, map=mp)
    print(f'games {used}, samples {len(allrows)}, rows {len(X)}', file=sys.stderr)

if __name__ == '__main__':
    main()
