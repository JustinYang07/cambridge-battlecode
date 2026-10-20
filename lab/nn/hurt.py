#!/usr/bin/env python3
"""What champions do after dropping below 35% health with an enemy champion within 6 tiles.
Over the next 8 rounds: died / got away (nearest enemy 7+ tiles, alive) / still in it; recall
started; rounds attacking or casting at champions, moving toward or away from the nearest enemy.
usage: hurt.py <src> [--version list] [--max N]"""
import sys, collections
sys.path.insert(0, '.')
import fingerprint as F
from farm_dataset import cheb
MAX = int(sys.argv[sys.argv.index('--max') + 1]) if '--max' in sys.argv else 10 ** 9
src = sys.argv[1]
st = collections.defaultdict(collections.Counter); allc = collections.Counter()
for mp, path, T, key in F.games_of(src)[:MAX]:
    d = F.decode(path, key)
    if d is None: continue
    o = {}
    for x in d['rounds']: o.setdefault(x['r'], x)
    rs = [r for r in sorted(o) if o[r]['champs']]
    if len(rs) < 40: continue
    acts = collections.defaultdict(list)
    for a in d['actions']:
        if a[2] == 1: acts[(a[0], a[3])].append(a[1])
    ix = {r: i for i, r in enumerate(rs)}
    prevHp = {}; last = {}
    for r in rs:
        cs = {c[0]: c for c in o[r]['champs']}
        for c in cs.values():
            if c[1] != T: continue
            f = c[5] / max(c[6], 1); pf = prevHp.get(c[0], 1.0); prevHp[c[0]] = f if c[7] else 1.0
            if not c[7] or not (f < 0.35 <= pf) or r - last.get(c[0], -99) < 15: continue
            p = (c[3], c[4])
            es = [cheb(p, (e[3], e[4])) for e in cs.values() if e[1] != T and e[7]]
            if not es or min(es) > 6: continue
            last[c[0]] = r
            s = st[c[2]]; s['n'] += 1
            outcome = 'in it'; d0 = min(es); toward = away = hit = recall = 0
            for q in rs[ix[r] + 1: ix[r] + 9]:
                cq = {x[0]: x for x in o[q]['champs']}; m = cq.get(c[0])
                if not m: continue
                if not m[7]: outcome = 'died'; break
                A = acts.get((q, c[0]), [])
                recall += 'Recall' in A; hit += ('Attack' in A or 'Cast' in A)
                eq = [cheb((m[3], m[4]), (e[3], e[4])) for e in cq.values() if e[1] != T and e[7]]
                dq = min(eq) if eq else 99
                if 'Move' in A: toward += dq < d0; away += dq > d0
                d0 = dq
                if dq >= 7: outcome = 'got away'
                elif outcome == 'got away': outcome = 'in it'
            s[outcome] += 1; s['recall'] += recall > 0; s['hit'] += hit; s['toward'] += toward; s['away'] += away
            allc[outcome] += 1; allc['n'] += 1
n = allc['n']
print(f"{src}: {n} hurt events | died {allc['died']/n:.0%}  got away {allc['got away']/n:.0%}  still in it {allc['in it']/n:.0%}")
for ty, s in sorted(st.items(), key=lambda t: -t[1]['n']):
    k = s['n']
    if k < 15: continue
    print(f"  {ty:8s} {k:4d} | died {s['died']/k:4.0%} away {s['got away']/k:4.0%} in it {s['in it']/k:4.0%} | recall {s['recall']/k:4.0%} | per event: hit rounds {s['hit']/k:.1f}, steps toward {s['toward']/k:.1f}, away {s['away']/k:.1f}")
