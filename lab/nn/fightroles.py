#!/usr/bin/env python3
"""Per champion type, during fights (rounds within a fight window where an enemy champion is within 8):
share of rounds attacking (champion target), casting, moving only, idle; median distance to the nearest
enemy; hits taken per round. usage: fightroles.py team:<name> [--since DATE] [--maps a,b]"""
import sys, collections, statistics as st
sys.path.insert(0, '.')
import fingerprint as F
from farm_dataset import cheb
import json
VER = set(int(v) for v in sys.argv[sys.argv.index('--version') + 1].split(',')) if '--version' in sys.argv else None
VMAP = json.load(open('/tmp/ourvers.json')) if VER else {}
src = sys.argv[1]; only = set(sys.argv[sys.argv.index('--maps') + 1].split(',')) if '--maps' in sys.argv else None
agg = collections.defaultdict(lambda: collections.Counter()); dists = collections.defaultdict(list)
for mp, path, T, key in F.games_of(src):
    if only and mp not in only: continue
    if VER and VMAP.get(key) not in VER: continue
    d = F.decode(path, key)
    if d is None: continue
    o = {}
    for x in d['rounds']: o.setdefault(x['r'], x)
    team = {}; ctype = {}
    for r in sorted(o)[30:60]:
        for c in o[r]['champs']: team[c[0]] = c[1]; ctype[c[0]] = c[2]
    acts = collections.defaultdict(list)
    for a in d['actions']:
        if a[2] == 1: acts[(a[0], a[3])].append(a)
    hit_on = collections.Counter()
    for a in d['actions']:
        if a[2] == 1 and a[1] in ('Attack', 'Cast') and a[5]:
            for c in o.get(a[0], {}).get('champs', []):
                if c[7] and (c[3], c[4]) == tuple(a[5]) and c[1] != team.get(a[3]): hit_on[(a[0], c[0])] += 1
    for r in sorted(o):
        x = o[r]
        for c in x['champs']:
            if c[1] != T or not c[7]: continue
            p = (c[3], c[4])
            en = [e for e in x['champs'] if e[1] != T and e[7]]
            if not en: continue
            dn = min(cheb(p, (e[3], e[4])) for e in en)
            if dn > 8: continue
            k = ctype[c[0]]; A = acts[(r, c[0])]; kinds = {a[1] for a in A}
            champ_tiles = {(e[3], e[4]) for e in en}
            att = any(a[1] == 'Attack' and a[5] and tuple(a[5]) in champ_tiles for a in A)
            cas = any(a[1] == 'Cast' for a in A)
            g = agg[k]; g['rounds'] += 1
            if att: g['attack'] += 1
            if cas: g['cast'] += 1
            if not att and not cas and 'Move' in kinds: g['move_only'] += 1
            if not A: g['idle'] += 1
            g['hit_taken'] += hit_on[(r, c[0])]
            dists[k].append(dn)
for k, g in sorted(agg.items(), key=lambda t: -t[1]['rounds']):
    n = g['rounds']
    print(f"  {k:8s} rounds {n:6d} | attack {g['attack']*100/n:4.0f}% cast {g['cast']*100/n:4.0f}% move-only {g['move_only']*100/n:4.0f}% idle {g['idle']*100/n:4.0f}% | nearest enemy {st.median(dists[k])} tiles | hits taken/round {g['hit_taken']/n:.2f}")
