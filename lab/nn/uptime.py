"""Attack uptime in camp kills: per kill window (first hit -> reward), hits per attacking champion per round,
and the share of rounds each attacker spends within range doing nothing. usage: uptime.py <src> <maps>"""
import sys, collections, statistics as stt
sys.path.insert(0, '.')
import fingerprint as F
from farm_dataset import mapfor, cheb
src = sys.argv[1]; maps = set(sys.argv[2].split(','))
res = collections.defaultdict(list)
for mp, path, T, key in F.games_of(src):
    if mp not in maps: continue
    d = F.decode(path, key)
    if d is None: continue
    o = {}
    for x in d['rounds']: o.setdefault(x['r'], x)
    M = mapfor(mp, d, o)
    if not M: continue
    G = M['groups']; tile2g = {(c['x'], c['y']): i for i, g in enumerate(G) for c in g['camps']}
    team = {c[0]: c[1] for r in sorted(o)[30:40] for c in o[r]['champs']}
    hits = collections.defaultdict(list)  # group -> (round, champ)
    for a in d['actions']:
        if a[2] == 1 and team.get(a[3]) == T and a[1] in ('Attack', 'Cast') and a[5] and tuple(a[5]) in tile2g:
            hits[tile2g[tuple(a[5])]].append((a[0], a[3], a[1]))
    prev = None
    for r in sorted(o):
        x = o[r]
        if prev:
            for gi, (v, pv) in enumerate(zip(x['groups'][:len(G)], prev['groups'][:len(G)])):
                if (v & 1) and not (pv & 1):
                    hs = [h for h in hits[gi] if r - 120 < h[0] <= r]
                    if not hs: continue
                    # window: back from the kill while hits keep coming (gaps under 8 rounds)
                    hs.sort(); start = hs[-1][0]
                    for h in reversed(hs):
                        if start - h[0] > 8: break
                        start = h[0]
                    win = [h for h in hs if h[0] >= start]; dur = r - start + 1
                    champs = {h[1] for h in win}
                    k = F.kind_of(G[gi])
                    res[k].append((dur, len(champs), len([h for h in win if h[2] == 'Attack']) / (dur * len(champs)), len([h for h in win if h[2] == 'Cast']) / (dur * len(champs))))
        prev = x
for k, v in sorted(res.items()):
    print(f'  {k:7s} kills {len(v):4d} | window {stt.median([a[0] for a in v]):5.1f}r | attackers {stt.mean([a[1] for a in v]):.2f} | basic attacks per attacker-round {stt.mean([a[2] for a in v]):.2f} | casts per attacker-round {stt.mean([a[3] for a in v]):.2f}')
