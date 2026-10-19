#!/usr/bin/env python3
"""How a team plays its fights: per fight (same clustering as fights.py), for our side and theirs:
hits per participant-round, focus (share of the side's hits on its most-hit target), casts share,
max health and health of participants at the start, and whether hurt participants (<35%) leave.
usage: fightplay.py team:<name> [--since DATE] [--maps a,b]"""
import json, sys, collections, statistics as st
sys.path.insert(0, '.')
import fingerprint as F
from farm_dataset import mapfor, cheb
VER = set(int(v) for v in sys.argv[sys.argv.index('--version') + 1].split(',')) if '--version' in sys.argv else None
VMAP = json.load(open('/tmp/ourvers.json')) if VER else {}
src = sys.argv[1]; only = set(sys.argv[sys.argv.index('--maps') + 1].split(',')) if '--maps' in sys.argv else None
agg = collections.defaultdict(list)
for mp, path, T, key in F.games_of(src):
    if only and mp not in only: continue
    if VER and VMAP.get(key) not in VER: continue
    d = F.decode(path, key)
    if d is None: continue
    o = {}
    for x in d['rounds']: o.setdefault(x['r'], x)
    team = {}
    for r in sorted(o)[30:60]:
        for c in o[r]['champs']: team[c[0]] = c[1]
    hits = []
    for a in d['actions']:
        if a[2] != 1 or a[1] not in ('Attack', 'Cast') or not a[5]: continue
        at = team.get(a[3])
        if at is None: continue
        tgt = next((c[0] for c in o.get(a[0], {}).get('champs', []) if c[1] != at and c[7] and (c[3], c[4]) == tuple(a[5])), None)
        if tgt is not None: hits.append((a[0], a[3], tgt, tuple(a[5]), a[1]))
    hits.sort(); fights = []
    for h in hits:
        if fights and h[0] - fights[-1]['end'] <= 8 and cheb(h[3], fights[-1]['at']) <= 12: fights[-1]['end'] = h[0]; fights[-1]['hits'].append(h)
        else: fights.append(dict(start=h[0], end=h[0], at=h[3], hits=[h]))
    for f in fights:
        if len(f['hits']) < 3: continue
        dur = f['end'] - f['start'] + 1
        for side, lab in ((T, 'us'), (1 - T, 'them')):
            hs = [h for h in f['hits'] if team[h[1]] == side]
            parts = {h[1] for h in hs} | {h[2] for h in f['hits'] if team[h[2]] == side}
            if not parts: continue
            tgt = collections.Counter(h[2] for h in hs)
            s0 = o.get(f['start'], {}).get('champs', [])
            mh = [c[6] for c in s0 if c[0] in parts]; hh = [c[5] / max(c[6], 1) for c in s0 if c[0] in parts and c[7]]
            # hurt participants: did they leave (end >= 5 tiles from the fight) within 6 rounds of dropping under 35%?
            left = stay = 0
            for ci in parts:
                for r in range(f['start'], f['end'] + 1):
                    c = next((c for c in o.get(r, {}).get('champs', []) if c[0] == ci), None)
                    if c and c[7] and c[5] < 0.35 * c[6]:
                        later = next((c2 for c2 in o.get(r + 6, {}).get('champs', []) if c2[0] == ci), None)
                        if later is None or not later[7]: stay += 1
                        elif cheb((later[3], later[4]), f['at']) >= 5: left += 1
                        else: stay += 1
                        break
            agg[lab].append(dict(rate=len(hs) / (dur * len(parts)), focus=(tgt.most_common(1)[0][1] / len(hs)) if hs else 0,
                                 casts=sum(1 for h in hs if h[4] == 'Cast') / max(len(hs), 1), maxhp=st.mean(mh) if mh else 0,
                                 hp=st.mean(hh) if hh else 0, left=left, stay=stay, n=len(parts)))
for lab in ('us', 'them'):
    v = agg[lab]
    if not v: continue
    L = sum(x['left'] for x in v); S = sum(x['stay'] for x in v)
    print(f"  {lab:4s} fights {len(v):4d} | hits per champion-round {st.mean(x['rate'] for x in v):.2f} | focus {st.mean(x['focus'] for x in v):.2f} | casts share {st.mean(x['casts'] for x in v):.2f} "
          f"| max hp {st.mean(x['maxhp'] for x in v):5.0f} | hp at start {st.mean(x['hp'] for x in v):.2f} | hurt champions that left {L}/{L+S} ({L*100//max(L+S,1)}%)")
