#!/usr/bin/env python3
"""Behaviour fingerprint of a team per map, and the gaps between two sources.

Sources:  team:<name>        scraped replays where <name> plays (lab/data/scraped)
          exp:<dir>          the candidate team of a lab experiment (lab/data/experiments/<dir>)
usage:    fingerprint.py <sourceA> <sourceB> [--maps a,b] [--top 8]
Prints per map the A vs B metrics and the largest relative gaps.
"""
import glob, json, os, subprocess, sys, collections, statistics as stt
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from farm_dataset import ROOT, WASM, mapfor, cheb

CACHE = f'{ROOT}/lab/data/decoded26'
VER = set(int(v) for v in sys.argv[sys.argv.index('--version') + 1].split(',')) if '--version' in sys.argv else None
VMAP = json.load(open('/tmp/ourvers.json')) if VER else {}
SINCE = sys.argv[sys.argv.index('--since') + 1] if '--since' in sys.argv else ''

def decode(path, key):
    f = f'{CACHE}/{key}.json'
    if os.path.exists(f) and os.path.getsize(f) > 0: return json.load(open(f))
    out = subprocess.run(['node', f'{ROOT}/lab/tools/decode_replay.mjs', path, '--wasm', WASM], capture_output=True, text=True)
    if out.returncode != 0: return None
    open(f, 'w').write(out.stdout); return json.loads(out.stdout)

def norm_map(m): return m.replace('_', '-')

def games_of(src):
    kind, arg = src.split(':', 1); res = []
    if kind == 'team':
        for f in glob.glob(f'{ROOT}/lab/data/scraped/*.json'):
            m = json.load(open(f)); a = (m.get('teamA') or {}).get('teamName'); b = (m.get('teamB') or {}).get('teamName')
            if VER and arg == 'bbas' and VMAP.get(m['matchId']) not in VER: continue
            if arg in (a, b) and (m.get('createdAt') or '') >= SINCE: res.append((norm_map(m['map']), f[:-5] + '.repl26a', 0 if a == arg else 1, m['matchId']))
    else:
        e = json.load(open(f'{ROOT}/lab/data/experiments/{arg}/experiment.json'))
        for i, g in enumerate(e['games']):
            if 'result' in g:
                rp = g['result']['replay']; rp = rp if os.path.isabs(rp) else f'{ROOT}/{rp}'
                res.append((norm_map(g['map']), rp, 0 if g['candidateSide'] == 'A' else 1, f'{arg}-{i}'))
    return res

def kind_of(g):
    t = [c['type'] for c in g['camps']]; e = t.count('elk'); w = t.count('wolf')
    if e >= 3 or (e >= 1 and len(t) > e + w): return 'bigelk'
    if e == 2: return 'elk2'
    if e == 1: return 'elk1'
    if w >= 2: return 'wolf2'
    if w == 1: return 'wolf1'
    return 'cluster'

def metrics(mapname, path, T, key):
    d = decode(path, key)
    if d is None: return None
    o = {}
    for x in d['rounds']: o.setdefault(x['r'], x)
    M = mapfor(mapname, d, o)
    if not M: return None
    G = M['groups']; rs = sorted(o); last = rs[-1]
    spawn = [tuple(p) for p in M['spawnA' if T == 0 else 'spawnB']]; sp = set(spawn)
    altars = [tuple(a) for a in M['altars']]
    tile2g = {(c['x'], c['y']): i for i, g in enumerate(G) for c in g['camps']}
    gk = [kind_of(g) for g in G]
    team = {}; ctype = {}
    for r in rs[30:60]:
        for c in o[r]['champs']: team[c[0]] = c[1]; ctype[c[0]] = c[2]
    acts = collections.defaultdict(list)
    for a in d['actions']:
        if a[2] == 1: acts[(a[0], a[3])].append(a)
    m = collections.defaultdict(float); lists = collections.defaultdict(list)
    m['rounds'] = last
    lists['draft'] = sorted(ctype[k] for k in ctype if team.get(k) == T)
    first_hit = {}; prev = None; chanUntil = collections.defaultdict(int)
    for r in rs:
        x = o[r]
        if prev:
            for gi, (v, pv) in enumerate(zip(x['groups'][:len(G)], prev['groups'][:len(G)])):
                if (pv & 1) and not (v & 1) and (pv >> 1) == T:
                    m['caps'] += 1; m['capgems'] += G[gi]['gems']; m['cap_' + gk[gi]] += 1
                    if r <= 200: m['caps200'] += 1; m['gems200'] += G[gi]['gems']
                    if r <= 500: m['caps500'] += 1; m['gems500'] += G[gi]['gems']
                if (v & 1) and not (pv & 1):
                    tiles = [(c['x'], c['y']) for c in G[gi]['camps']]
                    near = sum(1 for c in x['champs'] if c[1] == T and c[7] and min(cheb((c[3], c[4]), t) for t in tiles) <= 4)
                    if near:
                        lists['party_' + gk[gi]].append(near)
                        if gi in first_hit and r - first_hit[gi] < 120: lists['kill_' + gk[gi]].append(r - first_hit[gi])
                    first_hit.pop(gi, None)
            for c, p in zip(x['champs'], prev['champs']):
                if c[1] != T: continue
                if p[7] and not c[7]: m['deaths'] += 1
                if not c[7]: m['t_dead'] += 1; continue
                A = acts[(r, c[0])]; kinds = {a[1] for a in A}
                for a in A:
                    if a[1] in ('Attack', 'Cast') and a[5] and tuple(a[5]) in tile2g:
                        first_hit.setdefault(tile2g[tuple(a[5])], r)
                if 'Recall' in kinds:
                    chanUntil[c[0]] = r + 5; m['recalls'] += 1
                    lists['recall_hp'].append(c[5] / max(c[6], 1)); lists['recall_bag'].append(c[9])
                if 'SubmitGems' in kinds:
                    m['tributes'] += 1; lists['tribute_bag'].append(c[9])
                    t = [a for a in A if a[1] == 'SubmitGems'][0][5]
                    lists['tribute_altar_dist'].append(min(cheb(tuple(t), s) for s in spawn))
                pos = (c[3], c[4])
                if any(a[1] in ('Attack', 'Cast') and a[5] and tuple(a[5]) in tile2g for a in A): m['t_farm'] += 1
                elif r < chanUntil[c[0]]: m['t_recall'] += 1
                elif pos in tile2g and (x['groups'][tile2g[pos]] & 1): m['t_capture'] += 1
                elif 'Move' in kinds: m['t_walk'] += 1
                elif pos not in sp: m['t_stand'] += 1
                else: m['t_spawn'] += 1
                m['t_alive'] += 1
                m['bunch'] += sum(1 for e in x['champs'] if e[1] == T and e[7] and e[0] != c[0] and cheb((e[3], e[4]), pos) <= 4)
        prev = x
    return m, lists

def summarise(rows):
    """rows: list of (m, lists) -> dict of scalar features"""
    n = len(rows); S = collections.OrderedDict()
    if not n: return S
    tot = lambda k: sum(r[0][k] for r in rows)
    rounds = tot('rounds'); alive = max(tot('t_alive'), 1)
    bodies = stt.mean(len(r[1]['draft']) for r in rows)
    S['games'] = n
    S['caps by r200'] = tot('caps200') / n; S['gems by r200'] = tot('gems200') / n
    S['caps by r500'] = tot('caps500') / n; S['gems by r500'] = tot('gems500') / n
    S['caps per 100r'] = tot('caps') * 100 / rounds
    for k in ('wolf1', 'wolf2', 'elk1', 'elk2', 'bigelk', 'cluster'): S[f'  {k} caps per 100r'] = tot('cap_' + k) * 100 / rounds
    for k in ('wolf1', 'wolf2', 'elk1', 'elk2', 'bigelk'):
        p = [v for r in rows for v in r[1].get('party_' + k, [])]; kt = [v for r in rows for v in r[1].get('kill_' + k, [])]
        if p: S[f'  {k} party size'] = stt.mean(p)
        if kt: S[f'  {k} kill rounds'] = stt.median(kt)
    S['deaths per game'] = tot('deaths') / n
    S['recalls per champ per 100r'] = tot('recalls') * 100 / rounds / bodies
    rh = [v for r in rows for v in r[1]['recall_hp']]; rb = [v for r in rows for v in r[1]['recall_bag']]
    if rh: S['  recall hp (median)'] = stt.median(rh); S['  recall hp (q75)'] = stt.quantiles(rh, n=4)[2] if len(rh) > 3 else rh[0]
    if rb: S['  recall bag (median)'] = stt.median(rb)
    S['tributes per champ per 100r'] = tot('tributes') * 100 / rounds / bodies
    tb = [v for r in rows for v in r[1]['tribute_bag']]; ta = [v for r in rows for v in r[1]['tribute_altar_dist']]
    if tb: S['  tribute bag (median)'] = stt.median(tb)
    if ta: S['  tribute altar dist from spawn (median)'] = stt.median(ta)
    for k in ('farm', 'capture', 'walk', 'stand', 'recall', 'spawn', 'dead'):
        S[f'time {k} %'] = tot('t_' + k) * 100 / max(tot('t_alive') + tot('t_dead'), 1)
    S['teammates within 4'] = tot('bunch') / alive
    drafts = collections.Counter(c for r in rows for c in r[1]['draft']); S['draft'] = ', '.join(f'{k}{v}' for k, v in drafts.most_common())
    return S

def main():
    A, B = sys.argv[1], sys.argv[2]
    only = set(sys.argv[sys.argv.index('--maps') + 1].split(',')) if '--maps' in sys.argv else None
    top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 8
    data = {}
    for src in (A, B):
        per = collections.defaultdict(list)
        for mp, path, T, key in games_of(src):
            if only and mp not in only: continue
            r = metrics(mp, path, T, key)
            if r: per[mp].append(r)
        data[src] = per
    for mp in sorted(set(data[A]) & set(data[B])):
        sa, sb = summarise(data[A][mp]), summarise(data[B][mp])
        print(f'\n===== {mp}   ({A}: {sa["games"]} games | {B}: {sb["games"]} games)')
        gaps = []
        for k in sa:
            if k in ('games',): continue
            va, vb = sa.get(k), sb.get(k)
            if isinstance(va, str) or isinstance(vb, str):
                print(f'  {k:42s} {va!s:>28s} | {vb!s}'); continue
            if va is None or vb is None: continue
            print(f'  {k:42s} {va:10.2f} | {vb:10.2f}')
            denom = max(abs(va), abs(vb), 1e-6)
            gaps.append((abs(va - vb) / denom, k, va, vb))
        gaps.sort(reverse=True)
        print('  biggest gaps: ' + '; '.join(f'{k.strip()} {va:.2f} vs {vb:.2f}' for _, k, va, vb in gaps[:top]))

if __name__ == '__main__':
    main()
