"""Camp attempts that end without a kill. usage: abandon.py <src> [maps]"""
import sys, collections
sys.path.insert(0, '.')
import fingerprint as F
from farm_dataset import mapfor
src = sys.argv[1]; only = set(sys.argv[2].split(',')) if len(sys.argv) > 2 else None
per = collections.defaultdict(lambda: collections.Counter())
for mp, path, T, key in F.games_of(src):
    if only and mp not in only: continue
    d = F.decode(path, key)
    if d is None: continue
    o = {}
    for x in d['rounds']: o.setdefault(x['r'], x)
    M = mapfor(mp, d, o)
    if not M: continue
    G = M['groups']; tile2g = {(c['x'], c['y']): i for i, g in enumerate(G) for c in g['camps']}
    team = {c[0]: c[1] for r in sorted(o)[30:40] for c in o[r]['champs']}
    hits = collections.defaultdict(list)
    for a in d['actions']:
        if a[2] == 1 and team.get(a[3]) == T and a[1] in ('Attack', 'Cast') and a[5] and tuple(a[5]) in tile2g:
            g = tile2g[tuple(a[5])]
            if F.kind_of(G[g]) != 'cluster': hits[g].append(a[0])
    kills = collections.defaultdict(list); prev = None
    for r in sorted(o):
        x = o[r]
        if prev:
            for gi, (v, pv) in enumerate(zip(x['groups'][:len(G)], prev['groups'][:len(G)])):
                if (v & 1) and not (pv & 1): kills[gi].append(r)
        prev = x
    c = per[mp]; c['games'] += 1; c['rounds'] += max(o)
    for g, hs in hits.items():
        hs.sort(); eps = []; cur = [hs[0]]
        for h in hs[1:]:
            if h - cur[-1] > 8: eps.append(cur); cur = [h]
            else: cur.append(h)
        eps.append(cur)
        for e in eps:
            killed = any(e[0] <= k <= e[-1] + 3 for k in kills[g])
            c['attempts'] += 1; c['abandoned'] += 0 if killed else 1
            if not killed: c['wasted_hits'] += len(e)
            c['hits'] += len(e)
tot = collections.Counter()
for mp, c in sorted(per.items()):
    tot.update(c)
    print(f"  {mp:12s} attempts/100r {c['attempts']*100/c['rounds']:5.2f}  abandoned {c['abandoned']*100/max(c['attempts'],1):4.0f}%  hits wasted {c['wasted_hits']*100/max(c['hits'],1):4.0f}%")
print(f"  {'ALL':12s} attempts/100r {tot['attempts']*100/tot['rounds']:5.2f}  abandoned {tot['abandoned']*100/max(tot['attempts'],1):4.0f}%  hits wasted {tot['wasted_hits']*100/max(tot['hits'],1):4.0f}%")
