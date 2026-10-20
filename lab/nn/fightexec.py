#!/usr/bin/env python3
"""Fight execution per champion type: rounds with an enemy champion within 6 tiles, split by
whether an enemy is in reach (basic range: 1 melee, 3 ranged; Chebyshev) and what the champion did:
  in reach: hit a champion (attack or cast on a champion tile) / hit a camp / moved / nothing
  out of reach: closed in (nearest enemy distance fell) / held / backed off
usage: fightexec.py <src> [--version list] [--since DATE] [--max N]"""
import sys, json, collections
sys.path.insert(0, '.')
import fingerprint as F
from farm_dataset import cheb
RANGED = {'Cassi', 'Stellar', 'Tavi', 'Prism', 'Vesper Vale', 'Willow'}
MAX = int(sys.argv[sys.argv.index('--max') + 1]) if '--max' in sys.argv else 10 ** 9
src = sys.argv[1]
st = collections.defaultdict(collections.Counter)
for mp, path, T, key in F.games_of(src)[:MAX]:
    d = F.decode(path, key)
    if d is None: continue
    o = {}
    for x in d['rounds']: o.setdefault(x['r'], x)
    rs = [r for r in sorted(o) if o[r]['champs']]
    if len(rs) < 40: continue
    team = {c[0]: (c[1], c[2]) for c in o[rs[min(35, len(rs) - 1)]]['champs']}
    acts = collections.defaultdict(list)
    for a in d['actions']:
        if a[2] == 1: acts[(a[0], a[3])].append(a)
    prev = {}
    for r in rs:
        cs = o[r]['champs']
        champTiles = {(c[3], c[4]) for c in cs if c[1] != T and c[7]}
        for c in cs:
            if c[1] != T or not c[7]: continue
            p = (c[3], c[4]); ty = c[2]
            es = [cheb(p, (e[3], e[4])) for e in cs if e[1] != T and e[7]]
            if not es or min(es) > 6: prev.pop(c[0], None); continue
            dn = min(es); reach = 3 if ty in RANGED else 1
            A = acts.get((r, c[0]), [])
            hitChamp = any(a[1] in ('Attack', 'Cast') and a[5] and (tuple(a[5]) in champTiles or any(cheb(tuple(a[5]), t) <= 1 for t in champTiles) and a[1] == 'Cast') for a in A)
            hitCamp = any(a[1] == 'Attack' and a[5] and tuple(a[5]) not in champTiles for a in A)
            moved = any(a[1] == 'Move' for a in A)
            s = st[ty]; s['rounds'] += 1
            if dn <= reach:
                s['in reach'] += 1
                s['in: hit champ' if hitChamp else 'in: hit camp' if hitCamp else 'in: moved only' if moved else 'in: nothing'] += 1
            else:
                s['out of reach'] += 1
                if hitChamp: s['out: hit champ (spell)'] += 1
                pd = prev.get(c[0])
                s['out: closed in' if pd is not None and dn < pd else 'out: backed off' if pd is not None and dn > pd else 'out: held'] += 1
            prev[c[0]] = dn
print(f"{src}: champion rounds with an enemy within 6")
keys = ['in reach', 'in: hit champ', 'in: hit camp', 'in: moved only', 'in: nothing', 'out of reach', 'out: hit champ (spell)', 'out: closed in', 'out: held', 'out: backed off']
for ty, s in sorted(st.items(), key=lambda t: -t[1]['rounds']):
    if s['rounds'] < 300: continue
    n = s['rounds']; ni = max(s['in reach'], 1); no = max(s['out of reach'], 1)
    print(f"  {ty:8s} {n:6d} | in reach {s['in reach']/n:4.0%}: champ {s['in: hit champ']/ni:4.0%} camp {s['in: hit camp']/ni:4.0%} moved {s['in: moved only']/ni:4.0%} nothing {s['in: nothing']/ni:4.0%}"
          f" | out {s['out of reach']/n:4.0%}: spell hit {s['out: hit champ (spell)']/no:4.0%} closed {s['out: closed in']/no:4.0%} held {s['out: held']/no:4.0%} backed {s['out: backed off']/no:4.0%}")
