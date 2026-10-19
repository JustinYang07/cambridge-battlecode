#!/usr/bin/env python3
"""Imitation dataset for "recall now": every turn of the team's alive, non-channelling champions,
label 1 when it starts a recall that turn. Features match brain/survive.cpp's nnRecall (same order).

usage: recall_dataset.py <team name> <out.npz>
"""
import glob, json, os, subprocess, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from farm_dataset import ROOT, WASM, mapfor, cheb

TEAM = sys.argv[1]; OUT = sys.argv[2]
NF = 12

def process(meta, path):
    side = 'A' if (meta.get('teamA') or {}).get('teamName') == TEAM else 'B' if (meta.get('teamB') or {}).get('teamName') == TEAM else None
    if side is None: return None
    out = subprocess.run(['node', f'{ROOT}/lab/tools/decode_replay.mjs', path, '--wasm', WASM], capture_output=True, text=True)
    if out.returncode != 0: return None
    d = json.loads(out.stdout); o = {}
    for x in d['rounds']: o.setdefault(x['r'], x)
    M = mapfor(meta['map'], d, o)
    if not M: return None
    T = 0 if side == 'A' else 1
    spawn = [tuple(p) for p in M['spawnA' if T == 0 else 'spawnB']]
    altars = [tuple(a) for a in M['altars']]
    camps = [(c['x'], c['y']) for g in M['groups'] for c in g['camps'] if c.get('str', 0) > 0]
    tile2g = {(c['x'], c['y']): i for i, g in enumerate(M['groups']) for c in g['camps']}
    recall = set(); chan = set(); capt = {}
    for a in d['actions']:
        if a[2] == 1 and a[1] == 'Recall':
            recall.add((a[0], a[3]))
            for k in range(1, 6): chan.add((a[0] + k, a[3]))
        if a[2] == 1 and a[1] == 'SubmitGems':
            for k in range(0, 8): chan.add((a[0] + k, a[3]))
    rs = sorted(o); X = []; Y = []; prev = None
    for r in rs:
        x = o[r]
        if prev:
            for gi, (v, pv) in enumerate(zip(x['groups'], prev['groups'])):
                if (pv & 1) and not (v & 1) and (pv >> 1) == T: capt[r] = capt.get(r, []) + [gi]
        if r < 10 or r - 3 not in o: prev = x; continue
        old = {c[0]: c for c in o[r - 3]['champs']}
        for me in x['champs']:
            if me[1] != T or not me[7] or (r, me[0]) in chan: continue
            pos = (me[3], me[4])
            if pos in spawn: continue
            ds = min(cheb(pos, s) for s in spawn); da = min(cheb(pos, a) for a in altars)
            en = sum(1 for c in x['champs'] if c[1] != T and c[7] and cheb((c[3], c[4]), pos) <= 8)
            al = sum(1 for c in x['champs'] if c[1] == T and c[7] and c[0] != me[0] and cheb((c[3], c[4]), pos) <= 6)
            dmg = max(0, (old[me[0]][5] if old[me[0]][7] else me[5]) - me[5]) / max(me[6], 1)
            campNear = 1.0 if any(cheb(pos, c) <= 3 for c in camps) else 0.0
            justCap = 1.0 if any(gi for rr in range(r - 3, r + 1) for gi in capt.get(rr, []) if any(cheb(pos, (c['x'], c['y'])) <= 2 for c in M['groups'][gi]['camps'])) else 0.0
            X.append([me[5] / max(me[6], 1), me[9] / 60.0, ds / 40.0, da / 40.0, en / 3.0, al / 3.0, r / 1000.0,
                      me[6] / 1500.0, dmg * 10, me[8] / 300.0, campNear, justCap])
            Y.append(1 if (r, me[0]) in recall else 0)
        prev = x
    return np.array(X, np.float32), np.array(Y), meta['matchId']

metas = []
for f in glob.glob(f'{ROOT}/lab/data/scraped/*.json'):
    m = json.load(open(f))
    if TEAM in ((m.get('teamA') or {}).get('teamName'), (m.get('teamB') or {}).get('teamName')): metas.append((m, f[:-5] + '.repl26a'))
Xs, Ys, Gs = [], [], []
for m, p in metas:
    res = process(m, p)
    if res is None or len(res[1]) == 0: continue
    Xs.append(res[0]); Ys.append(res[1]); Gs.append(np.full(len(res[1]), res[2]))
X = np.concatenate(Xs); Y = np.concatenate(Ys); G = np.concatenate(Gs)
np.savez(OUT, X=X, y=Y, game=G)
print(f'games {len(Xs)}, turns {len(Y)}, recalls {Y.sum()} ({Y.mean()*100:.2f}%)')
