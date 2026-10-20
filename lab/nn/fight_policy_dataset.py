"""Per-champion fight decisions from scraped replays, for distilling top teams' play.

Row: a champion with an enemy champion within 6 tiles, every 2nd round. Features are its local picture
(fight_engage_dataset.side_feats around its own tile) plus its own state and the nearest enemy.
Label over the next 2 rounds: recall / engage (hit a champion, or stepped closer to the nearest enemy) /
retreat (stepped away, no champion hit) / hold. Outcome: died within 10 rounds, health lost (/1000).
usage: fight_policy_dataset.py [--since DATE] [--jobs N] [--out lab/data/policy.csv] [--limit N]
"""
import csv, glob, json, os, sys
from multiprocessing import Pool
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fingerprint as F
from farm_dataset import cheb
from fight_engage_dataset import TYPES, ROOT, arg, decode, side_feats

LABELS = ['hold', 'engage', 'retreat', 'recall']


def process(item):
    mp, path, key, meta = item
    d = decode(path, key)
    if d is None: return []
    o = {}
    for x in d['rounds']: o.setdefault(x['r'], x)
    rs = [r for r in sorted(o) if o[r]['champs']]
    if len(rs) < 60: return []
    first = o[rs[0]]['champs']; sp = {}
    for T in (0, 1):
        g = [c for c in first if c[1] == T]
        if not g: return []
        sp[T] = (round(sum(c[3] for c in g) / len(g)), round(sum(c[4] for c in g) / len(g)))
    ix = {r: i for i, r in enumerate(rs)}
    acts = {}
    for a in d['actions']:
        if a[2] == 1: acts.setdefault((a[0], a[3]), []).append(a)
    rows = []
    for r in rs[::2]:
        i = ix[r]
        if r < 20 or i + 10 >= len(rs): continue
        cs = o[r]['champs']
        for c in cs:
            if not c[7]: continue
            T = c[1]; p = (c[3], c[4])
            foes = [e for e in cs if e[1] != T and e[7]]
            if not foes: continue
            ne = min(foes, key=lambda e: cheb(p, (e[3], e[4]))); dn = cheb(p, (ne[3], ne[4]))
            if dn > 6: continue
            # Label from the next two rounds.
            label = 'hold'; hit = False; closer = farther = False
            foeTiles = set()
            for q in rs[i + 1: i + 3]:
                cq = {x[0]: x for x in o[q]['champs']}
                foeTiles |= {(e[3], e[4]) for e in cq.values() if e[1] != T and e[7]}
                for a in acts.get((q, c[0]), []):
                    if a[1] == 'Recall': label = 'recall'
                    if a[1] in ('Attack', 'Cast') and a[5] and any(cheb(tuple(a[5]), t) <= (0 if a[1] == 'Attack' else 1) for t in foeTiles): hit = True
            q2 = rs[i + 2]; m2 = next((x for x in o[q2]['champs'] if x[0] == c[0]), None)
            if m2 and m2[7]:
                # Our own step only: distance from where we end up to where the enemy stood.
                dd = cheb((m2[3], m2[4]), (ne[3], ne[4]))
                closer, farther = dd < dn, dd > dn
            if label != 'recall':
                label = 'engage' if hit or closer else 'retreat' if farther else 'hold'
            died = 0; lost = 0.0
            for q in rs[i + 1: i + 11]:
                mq = next((x for x in o[q]['champs'] if x[0] == c[0]), None)
                if not mq: continue
                if not mq[7]: died = 1; lost = c[5]; break
                lost = max(lost, c[5] - mq[5])
            row = {'game': key, 'map': mp, 'round': r, 'team': meta['team' + 'AB'[T]]['teamName'],
                   'opp': meta['team' + 'AB'[1 - T]]['teamName'], 'label': label, 'died10': died, 'lost10': lost / 1000}
            row.update(side_feats(cs, T, p, sp[T], sp[1 - T]))
            row.update({'myFrac': c[5] / max(c[6], 1), 'myMaxHp': c[6] / 1000, 'myBag': c[9] / 50,
                        'myStr': c[11] / 50, 'myFoc': c[12] / 50, 'myHpSh': c[13] / 50,
                        'nearDist': dn / 6, 'nearFrac': ne[5] / max(ne[6], 1), 'nearMaxHp': ne[6] / 1000,
                        'round1000': r / 1000})
            for k, ty in enumerate(TYPES): row['me_' + ty] = int(c[2] == ty)
            rows.append(row)
    return rows


def main():
    since = arg('--since', '2026-10-17'); out = arg('--out', f'{ROOT}/lab/data/policy.csv'); jobs = int(arg('--jobs', '10'))
    items = []
    if '--exp' in sys.argv:  # a lab experiment: team names 'cand' / 'opp'
        eid = arg('--exp', ''); e = json.load(open(f'{ROOT}/lab/data/experiments/{eid}/experiment.json'))
        for i, gm in enumerate(e['games']):
            if 'result' not in gm or 'replay' not in gm['result']: continue
            rp = gm['result']['replay']; rp = rp if os.path.isabs(rp) else f'{ROOT}/{rp}'
            a = gm['candidateSide'] == 'A'
            items.append((F.norm_map(gm['map']), rp, f'{eid}-{i}', {'teamA': {'teamName': 'cand' if a else 'opp'}, 'teamB': {'teamName': 'opp' if a else 'cand'}}))
    for f in ([] if '--exp' in sys.argv else glob.glob(f'{ROOT}/lab/data/scraped/*.json')):
        m = json.load(open(f))
        if (m.get('createdAt') or '') < since or not m.get('map'): continue
        items.append((F.norm_map(m['map']), f[:-5] + '.repl26a', m['matchId'], m))
    items = items[:int(arg('--limit', '100000'))]
    print(f'{len(items)} games since {since}', flush=True)
    rows = []
    with Pool(jobs) as p:
        for i, rr in enumerate(p.imap_unordered(process, items, chunksize=4)):
            rows += rr
            if i % 200 == 0: print(f'  {i}/{len(items)} games, {len(rows)} rows', flush=True)
    keys = list(rows[0])
    with open(out, 'w', newline='') as fh:
        w = csv.DictWriter(fh, fieldnames=keys); w.writeheader(); w.writerows(rows)
    print(f'wrote {len(rows)} rows to {out}')


if __name__ == '__main__':
    main()
