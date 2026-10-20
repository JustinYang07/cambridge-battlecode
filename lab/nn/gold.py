"""Where each champion's gold comes from: every rise in a champion's gold, attributed to a capture
completing on its tile (group reward taken by its team that round), an enemy champion dying near it
(kill gold), or other. usage: gold.py team:<name> [--version list] [--max N]"""
import sys, json, collections, statistics as st
sys.path.insert(0, '.')
import fingerprint as F
from farm_dataset import mapfor, cheb
MAX = int(sys.argv[sys.argv.index('--max') + 1]) if '--max' in sys.argv else 60
src = sys.argv[1]; games = F.games_of(src)[:MAX]
inc = collections.defaultdict(collections.Counter); caps = collections.defaultdict(collections.Counter); nrounds = collections.Counter()
for mp, path, T, key in games:
    d = F.decode(path, key)
    if d is None: continue
    o = {}
    for x in d['rounds']: o.setdefault(x['r'], x)
    M = mapfor(mp, d, o)
    if not M: continue
    G = M['groups']; tiles = [set((c['x'], c['y']) for c in g['camps']) for g in G]
    rs = sorted(o); prev = None
    for r in rs:
        x = o[r]
        if prev and r <= 500:
            capt = [gi for gi, (v, pv) in enumerate(zip(x['groups'][:len(G)], prev['groups'][:len(G)])) if (pv & 1) and not (v & 1) and (pv >> 1) == T]
            deaths = [(p[3], p[4]) for c, p in zip(x['champs'], prev['champs']) if c[1] != T and p[7] and not c[7]]
            for c, p in zip(x['champs'], prev['champs']):
                if c[1] != T: continue
                if r == rs[1] or True: nrounds[c[2]] += 0
                dg = c[8] - p[8]
                if dg <= 0: continue
                pos = (p[3], p[4])
                src_ = 'other'
                if any(pos in tiles[gi] or (c[3], c[4]) in tiles[gi] for gi in capt): src_ = 'capture'
                elif any(cheb(pos, q) <= 6 for q in deaths): src_ = 'kill'
                inc[c[2]][src_] += dg
                if src_ == 'capture': caps[c[2]]['n'] += 1
        prev = x
    for c in o[rs[min(30, len(rs) - 1)]]['champs']:
        if c[1] == T: nrounds[c[2]] += 1
print('gold earned by round 500, per game: total (capture / kill / other) | captures taken')
for ch, v in sorted(inc.items(), key=lambda t: -sum(t[1].values())):
    n = max(nrounds[ch], 1)
    print(f"  {ch:8s} {sum(v.values())/n:6.0f}  ({v['capture']/n:5.0f} / {v['kill']/n:4.0f} / {v['other']/n:4.0f}) | {caps[ch]['n']/n:4.1f}")
