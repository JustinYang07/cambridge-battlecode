"""Fight-engagement dataset from scraped replays (any team), one row per episode per side.

Episode: opposing champions come within ENG tiles, with no episode within 10 tiles in the last 10 rounds.
The spot m is the midpoint of the closest pair. Features are from one team's view (both views written).
Labels over the next HORIZON rounds, for champions within 12 of m at the start:
  ourDeaths / theirDeaths, hpTrade (their health lost - ours, /1000; a death counts its health then),
  approach: mean change over the next 3 rounds in our participants' distance to the nearest enemy
  (negative: we moved in) - for imitating a team's engage/back-off choice.
usage: fight_engage_dataset.py [--since DATE] [--jobs N] [--out lab/data/fights.csv] [--limit N]
"""
import csv, glob, json, os, subprocess, sys
from multiprocessing import Pool
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fingerprint as F
from farm_dataset import cheb

ENG, NEAR, FAR, HORIZON = 5, 8, 14, 15
TYPES = ['Ryth', 'Vesper Vale', 'Vendetta', 'Cassi', 'Issac', 'Stellar', 'Prism', 'Marcella', 'Willow', 'Ren', 'Varg', 'Tavi', 'Heart of Ruin']
ROOT = F.ROOT


def arg(name, default):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


CACHE = f'{ROOT}/lab/data/decoded26s'  # decode_replay.mjs --shards: champion rows end [str, foc, hp] shards


def decode(path, key):
    f = f'{CACHE}/{key}.json'
    if os.path.exists(f) and os.path.getsize(f) > 0: return json.load(open(f))
    out = subprocess.run(['node', f'{ROOT}/lab/tools/decode_replay.mjs', path, '--wasm', F.WASM, '--shards'], capture_output=True, text=True)
    if out.returncode != 0: return None
    os.makedirs(CACHE, exist_ok=True)
    open(f, 'w').write(out.stdout); return json.loads(out.stdout)


def side_feats(cs, T, m, sp_us, sp_them):
    us = [c for c in cs if c[1] == T and c[7]]
    them = [c for c in cs if c[1] != T and c[7]]
    near = lambda grp, R: [c for c in grp if cheb((c[3], c[4]), m) <= R]
    u8, t8, u14, t14 = near(us, NEAR), near(them, NEAR), near(us, FAR), near(them, FAR)
    D = max(cheb(sp_us, sp_them), 1)
    hp = lambda g: sum(c[5] for c in g) / 1000
    mx = lambda g: sum(c[6] for c in g) / 1000
    frac = lambda g: sum(c[5] / max(c[6], 1) for c in g) / len(g) if g else 0
    f = {
        'us8': len(u8), 'them8': len(t8), 'us14': len(u14), 'them14': len(t14),
        'usAlive': len(us), 'themAlive': len(them),
        'hpUs8': hp(u8), 'hpThem8': hp(t8), 'maxHpUs8': mx(u8), 'maxHpThem8': mx(t8),
        'hpUs14': hp(u14), 'hpThem14': hp(t14),
        'fracUs8': frac(u8), 'fracThem8': frac(t8),
        'teamMaxHpUs': mx(us), 'teamMaxHpThem': mx(them),
        'bagUs8': sum(c[9] for c in u8) / 50, 'bagThem8': sum(c[9] for c in t8) / 50,
        'killsUs': sum(c[10] for c in cs if c[1] == T), 'killsThem': sum(c[10] for c in cs if c[1] != T),
        # Stat shards owned (items): strength, focus, health; /50.
        'strUs8': sum(c[11] for c in u8) / 50, 'focUs8': sum(c[12] for c in u8) / 50, 'hpShUs8': sum(c[13] for c in u8) / 50,
        'strThem8': sum(c[11] for c in t8) / 50, 'focThem8': sum(c[12] for c in t8) / 50, 'hpShThem8': sum(c[13] for c in t8) / 50,
        'teamShardsUs': sum(sum(c[11:14]) for c in cs if c[1] == T) / 100, 'teamShardsThem': sum(sum(c[11:14]) for c in cs if c[1] != T) / 100,
        'dOurSpawn': cheb(m, sp_us) / D, 'dTheirSpawn': cheb(m, sp_them) / D,
    }
    for ty in TYPES:
        f['u_' + ty] = sum(1 for c in u8 if c[2] == ty)
        f['t_' + ty] = sum(1 for c in t8 if c[2] == ty)
    return f


def process(item):
    mp, path, key, meta = item
    d = decode(path, key)
    if d is None: return []
    o = {}
    for x in d['rounds']: o.setdefault(x['r'], x)
    rs = [r for r in sorted(o) if o[r]['champs']]
    if len(rs) < 60: return []
    # Spawns: each team's champion centroid on its first round with positions.
    first = o[rs[0]]['champs']
    sp = {}
    for T in (0, 1):
        g = [c for c in first if c[1] == T]
        if not g: return []
        sp[T] = (round(sum(c[3] for c in g) / len(g)), round(sum(c[4] for c in g) / len(g)))
    last = rs[-1]; recent = []; rows = []
    for r in rs:
        if r < 20 or r > last - HORIZON: continue
        cs = o[r]['champs']
        a = [c for c in cs if c[1] == 0 and c[7]]; b = [c for c in cs if c[1] == 1 and c[7]]
        best = None
        for x in a:
            for y in b:
                dd = cheb((x[3], x[4]), (y[3], y[4]))
                if dd <= ENG and (best is None or dd < best[0]): best = (dd, x, y)
        if not best: continue
        m = ((best[1][3] + best[2][3]) // 2, (best[1][4] + best[2][4]) // 2)
        if any(r - rr < 10 and cheb(m, mm) <= 10 for rr, mm in recent): continue
        recent = [(rr, mm) for rr, mm in recent if r - rr < 10] + [(r, m)]
        part = {c[0]: c for c in cs if c[7] and cheb((c[3], c[4]), m) <= 12}
        # Outcome over the horizon.
        lost = {i: 0.0 for i in part}; died = set()
        hpNow = {i: c[5] for i, c in part.items()}
        for q in [q for q in rs if r < q <= r + HORIZON]:
            for c in o[q]['champs']:
                i = c[0]
                if i not in part or i in died: continue
                if not c[7]:
                    died.add(i); lost[i] = hpNow[i] / 1.0
                else:
                    lost[i] = max(lost[i], (part[i][5] - c[5]) * 1.0)
        # Approach over 3 rounds, per team.
        def approach(T):
            q = next((q for q in rs if q >= r + 3), None)
            if q is None: return 0.0
            later = {c[0]: c for c in o[q]['champs']}
            ds = []
            for i, c in part.items():
                if c[1] != T or cheb((c[3], c[4]), m) > NEAR or i not in later or not later[i][7]: continue
                foes0 = [e for e in cs if e[1] != T and e[7]]; foes1 = [e for e in o[q]['champs'] if e[1] != T and e[7]]
                if not foes0 or not foes1: continue
                d0 = min(cheb((c[3], c[4]), (e[3], e[4])) for e in foes0)
                d1 = min(cheb((later[i][3], later[i][4]), (e[3], e[4])) for e in foes1)
                ds.append(d1 - d0)
            return sum(ds) / len(ds) if ds else 0.0
        for T in (0, 1):
            ours = [i for i, c in part.items() if c[1] == T]; theirs = [i for i, c in part.items() if c[1] != T]
            row = {'game': key, 'map': mp, 'round': r, 'team': meta['team' + 'AB'[T]]['teamName'],
                   'opp': meta['team' + 'AB'[1 - T]]['teamName'],
                   'rating': meta['team' + 'AB'[T]].get('ratingBefore') or 0, 'oppRating': meta['team' + 'AB'[1 - T]].get('ratingBefore') or 0,
                   'won': int(meta.get('winner') == 'AB'[T])}
            row.update(side_feats(cs, T, m, sp[T], sp[1 - T]))
            row['round1000'] = r / 1000
            row['ourDeaths'] = sum(1 for i in ours if i in died)
            row['theirDeaths'] = sum(1 for i in theirs if i in died)
            row['hpTrade'] = (sum(lost[i] for i in theirs) - sum(lost[i] for i in ours)) / 1000
            row['approach'] = approach(T)
            rows.append(row)
    return rows


def main():
    since = arg('--since', '2026-10-17'); out = arg('--out', f'{ROOT}/lab/data/fights.csv'); jobs = int(arg('--jobs', '10'))
    items = []
    if '--exp' in sys.argv:  # a lab experiment: team names 'cand' / 'opp'
        eid = arg('--exp', ''); e = json.load(open(f'{ROOT}/lab/data/experiments/{eid}/experiment.json'))
        for i, gm in enumerate(e['games']):
            if 'result' not in gm or 'replay' not in gm['result']: continue
            rp = gm['result']['replay']; rp = rp if os.path.isabs(rp) else f'{ROOT}/{rp}'
            a = gm['candidateSide'] == 'A'
            meta = {'teamA': {'teamName': 'cand' if a else 'opp'}, 'teamB': {'teamName': 'opp' if a else 'cand'},
                    'winner': gm['result'].get('winner')}
            items.append((F.norm_map(gm['map']), rp, f'{eid}-{i}', meta))
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
            if i % 100 == 0: print(f'  {i}/{len(items)} games, {len(rows)} rows', flush=True)
    if not rows: print('no rows'); return
    with open(out, 'w', newline='') as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0])); w.writeheader(); w.writerows(rows)
    print(f'wrote {len(rows)} rows to {out}')


if __name__ == '__main__':
    main()
