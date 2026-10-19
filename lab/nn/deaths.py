#!/usr/bin/env python3
"""Every death of a team's champions in its replays, with the context that led to it.
usage: deaths.py team:<name> [--since DATE] [--json out.json]
Per death: map, round, champion, health 10 rounds before, bag, who hit it in the last 6 rounds
(enemy champions / monsters), allies and enemies within 6 then and 10 rounds before, which half,
and what it was doing over the 12 rounds before (farming a camp, fighting, walking, recalling...).
"""
import json, sys, json, collections
sys.path.insert(0, '.')
import fingerprint as F
from farm_dataset import mapfor, cheb

VER = set(int(v) for v in sys.argv[sys.argv.index('--version') + 1].split(',')) if '--version' in sys.argv else None
VMAP = json.load(open('/tmp/ourvers.json')) if VER else {}
src = sys.argv[1]
out_json = sys.argv[sys.argv.index('--json') + 1] if '--json' in sys.argv else None
rows = []
for mp, path, T, key in F.games_of(src):
    if VER and VMAP.get(key) not in VER: continue
    d = F.decode(path, key)
    if d is None: continue
    o = {}
    for x in d['rounds']: o.setdefault(x['r'], x)
    M = mapfor(mp, d, o)
    if not M: continue
    G = M['groups']; tile2g = {(c['x'], c['y']): i for i, g in enumerate(G) for c in g['camps']}
    sp_us = tuple(M['spawnA' if T == 0 else 'spawnB'][0]); sp_them = tuple(M['spawnB' if T == 0 else 'spawnA'][0])
    team = {}; ctype = {}
    for r in sorted(o)[30:60]:
        for c in o[r]['champs']: team[c[0]] = c[1]; ctype[c[0]] = c[2]
    byr = collections.defaultdict(list)
    for a in d['actions']: byr[a[0]].append(a)
    rs = sorted(o); prev = None
    for r in rs:
        x = o[r]
        if prev:
            for c, p in zip(x['champs'], prev['champs']):
                if c[1] != T or not p[7] or c[7]: continue
                pos = (p[3], p[4])
                # attackers on this champion over the last 6 rounds (by position tracking)
                champ_hits = collections.Counter(); mon_hits = 0
                for rr in range(r - 6, r + 1):
                    if rr not in o: continue
                    me = next((cc for cc in o[rr]['champs'] if cc[0] == c[0]), None)
                    prevme = o.get(rr - 1, {}).get('champs', [None] * (c[0] + 1))[c[0]] if rr - 1 in o else None
                    tiles = {(me[3], me[4])} if me and me[7] else set()
                    if prevme and prevme[7]: tiles.add((prevme[3], prevme[4]))
                    for a in byr[rr]:
                        if a[1] in ('Attack', 'Cast') and a[5] and tuple(a[5]) in tiles:
                            if a[2] == 2: mon_hits += 1
                            elif team.get(a[3]) not in (None, T): champ_hits[ctype.get(a[3], '?')] += 1
                def count(rr, rad=6):
                    if rr not in o: return (0, 0)
                    me = next((cc for cc in o[rr]['champs'] if cc[0] == c[0]), None)
                    if not me or not me[7]: return (0, 0)
                    q = (me[3], me[4])
                    al = sum(1 for e in o[rr]['champs'] if e[1] == T and e[7] and e[0] != c[0] and cheb((e[3], e[4]), q) <= rad)
                    en = sum(1 for e in o[rr]['champs'] if e[1] != T and e[7] and cheb((e[3], e[4]), q) <= rad)
                    return al, en
                a0, e0 = count(r - 1); a10, e10 = count(r - 10)
                hp10 = next((cc[5] / max(cc[6], 1) for cc in o.get(r - 10, {}).get('champs', []) if cc[0] == c[0] and cc[7]), None)
                # activity over the 12 rounds before
                act = collections.Counter()
                for rr in range(r - 12, r):
                    for a in byr[rr]:
                        if a[2] == 1 and a[3] == c[0]:
                            if a[1] in ('Attack', 'Cast') and a[5] and tuple(a[5]) in tile2g: act['farm'] += 1
                            elif a[1] in ('Attack', 'Cast'): act['fight'] += 1
                            elif a[1] == 'Recall': act['recall'] += 1
                            elif a[1] == 'SubmitGems': act['tribute'] += 1
                            elif a[1] == 'Move': act['move'] += 1
                du, dt = cheb(pos, sp_us), cheb(pos, sp_them)
                half = 'ours' if du < dt * 0.8 else 'theirs' if dt < du * 0.8 else 'middle'
                rows.append(dict(map=mp, game=key, round=r, champ=ctype.get(c[0], '?'), hp10=hp10, bag=p[9], half=half,
                                 champ_hits=dict(champ_hits), mon_hits=mon_hits, allies=a0, enemies=e0, allies10=a10, enemies10=e10,
                                 act=dict(act)))
        prev = x
n = len(rows); games = len({r['game'] for r in rows}) or 1
print(f'{n} deaths in {len({r["game"] for r in rows})} games with deaths ({src})')
def share(pred): return f'{sum(1 for r in rows if pred(r))*100/max(n,1):.0f}%'
print('killed by: champions only', share(lambda r: r['champ_hits'] and not r['mon_hits']), '| monsters only', share(lambda r: r['mon_hits'] and not r['champ_hits']),
      '| both', share(lambda r: r['mon_hits'] and r['champ_hits']), '| no hits seen', share(lambda r: not r['mon_hits'] and not r['champ_hits']))
print('where: our half', share(lambda r: r['half'] == 'ours'), '| middle', share(lambda r: r['half'] == 'middle'), '| their half', share(lambda r: r['half'] == 'theirs'))
print('numbers at death (allies+me vs enemies within 6): outnumbered', share(lambda r: r['enemies'] > r['allies'] + 1), '| even', share(lambda r: r['enemies'] == r['allies'] + 1), '| ahead', share(lambda r: 0 < r['enemies'] < r['allies'] + 1), '| no enemy', share(lambda r: r['enemies'] == 0))
print('10 rounds before: alone (no ally within 6)', share(lambda r: r['allies10'] == 0), '| enemies already within 6', share(lambda r: r['enemies10'] > 0))
print('health 10 rounds before: <30%', share(lambda r: r['hp10'] is not None and r['hp10'] < 0.3), '| 30-60%', share(lambda r: r['hp10'] is not None and 0.3 <= r['hp10'] < 0.6), '| >=60%', share(lambda r: r['hp10'] is not None and r['hp10'] >= 0.6))
print('doing before: farming', share(lambda r: r['act'].get('farm', 0) >= 2), '| fighting', share(lambda r: r['act'].get('fight', 0) >= 2), '| recalling', share(lambda r: r['act'].get('recall')), '| tributing', share(lambda r: r['act'].get('tribute')))
print('by champion:', collections.Counter(r['champ'] for r in rows).most_common())
print('by map (deaths per game):', ', '.join(f'{m} {v}' for m, v in collections.Counter(r['map'] for r in rows).most_common()))
if out_json: json.dump(rows, open(out_json, 'w'))
