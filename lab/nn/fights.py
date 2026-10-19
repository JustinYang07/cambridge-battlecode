#!/usr/bin/env python3
"""Team fights from replays: clusters of champion-vs-champion hits (gaps under 8 rounds, within 12 tiles).
Per fight: who struck first, how many of each side took part, deaths on each side, where, round.
usage: fights.py team:<name> [--since DATE] [--maps a,b]"""
import json, sys, collections, statistics as st
sys.path.insert(0, '.')
import fingerprint as F
from farm_dataset import mapfor, cheb
VER = set(int(v) for v in sys.argv[sys.argv.index('--version') + 1].split(',')) if '--version' in sys.argv else None
VMAP = json.load(open('/tmp/ourvers.json')) if VER else {}
src = sys.argv[1]; only = set(sys.argv[sys.argv.index('--maps') + 1].split(',')) if '--maps' in sys.argv else None
per = collections.defaultdict(list)
for mp, path, T, key in F.games_of(src):
    if only and mp not in only: continue
    if VER and VMAP.get(key) not in VER: continue
    d = F.decode(path, key)
    if d is None: continue
    o = {}
    for x in d['rounds']: o.setdefault(x['r'], x)
    M = mapfor(mp, d, o)
    if not M: continue
    sp_us = tuple(M['spawnA' if T == 0 else 'spawnB'][0]); sp_them = tuple(M['spawnB' if T == 0 else 'spawnA'][0])
    team = {}
    for r in sorted(o)[30:60]:
        for c in o[r]['champs']: team[c[0]] = c[1]
    pos = {}
    for r in o:
        for c in o[r]['champs']: pos[(r, c[0])] = (c[3], c[4]) if c[7] else None
    # champion-vs-champion hits: attacker index -> target tile; target = champion of the other team standing there
    hits = []
    for a in d['actions']:
        if a[2] != 1 or a[1] not in ('Attack', 'Cast') or not a[5]: continue
        at = team.get(a[3])
        if at is None: continue
        r = a[0]; tgt = None
        for c in o.get(r, {}).get('champs', []):
            if c[1] != at and c[7] and (c[3], c[4]) == tuple(a[5]): tgt = c[0]
        if tgt is not None: hits.append((r, a[3], tgt, tuple(a[5])))
    hits.sort()
    fights = []
    for h in hits:
        if fights and h[0] - fights[-1]['end'] <= 8 and cheb(h[3], fights[-1]['at']) <= 12:
            f = fights[-1]; f['end'] = h[0]; f['hits'].append(h)
        else: fights.append(dict(start=h[0], end=h[0], at=h[3], hits=[h]))
    deaths = []
    rs = sorted(o)
    for i in range(1, len(rs)):
        for c, p in zip(o[rs[i]]['champs'], o[rs[i - 1]]['champs']):
            if p[7] and not c[7]: deaths.append((rs[i], c[0], (p[3], p[4])))
    for f in fights:
        if len(f['hits']) < 3: continue
        first = team[f['hits'][0][1]]
        us = {h[1] for h in f['hits'] if team[h[1]] == T} | {h[2] for h in f['hits'] if team[h[2]] == T}
        them = {h[1] for h in f['hits'] if team[h[1]] != T} | {h[2] for h in f['hits'] if team[h[2]] != T}
        dus = sum(1 for (r, ci, q) in deaths if f['start'] <= r <= f['end'] + 3 and team[ci] == T and cheb(q, f['at']) <= 14)
        dthem = sum(1 for (r, ci, q) in deaths if f['start'] <= r <= f['end'] + 3 and team[ci] != T and cheb(q, f['at']) <= 14)
        du, dt = cheb(f['at'], sp_us), cheb(f['at'], sp_them)
        half = 'ours' if du < dt * 0.8 else 'theirs' if dt < du * 0.8 else 'middle'
        per[mp].append(dict(game=key, start=f['start'], first='us' if first == T else 'them', n_us=len(us), n_them=len(them), d_us=dus, d_them=dthem, half=half))
games = collections.Counter()
for mp, path, T, key in F.games_of(src):
    if not only or mp in only: games[mp] += 1
for mp, fs in sorted(per.items()):
    g = len({f['game'] for f in fs}) or 1
    ng = games[mp]
    won = sum(1 for f in fs if f['d_them'] > f['d_us']); lost = sum(1 for f in fs if f['d_us'] > f['d_them'])
    print(f"{mp:12s} games {ng:2d} | fights/game {len(fs)/ng:4.1f} | we start {sum(f['first']=='us' for f in fs)*100//max(len(fs),1):3d}% | won {won*100//max(len(fs),1)}% lost {lost*100//max(len(fs),1)}% "
          f"| deaths/fight us {sum(f['d_us'] for f in fs)/max(len(fs),1):.2f} them {sum(f['d_them'] for f in fs)/max(len(fs),1):.2f} "
          f"| size us {st.mean(f['n_us'] for f in fs):.1f} them {st.mean(f['n_them'] for f in fs):.1f} | where ours/mid/theirs {sum(f['half']=='ours' for f in fs)}/{sum(f['half']=='middle' for f in fs)}/{sum(f['half']=='theirs' for f in fs)}"
          f" | lost when we started {sum(1 for f in fs if f['first']=='us' and f['d_us']>f['d_them'])}, when they started {sum(1 for f in fs if f['first']=='them' and f['d_us']>f['d_them'])}")
