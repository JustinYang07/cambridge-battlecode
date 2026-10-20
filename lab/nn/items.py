"""Item builds per champion type from replays (raw engine probe). usage: items.py team:<name> [--version list] [--max N]"""
import sys, json, os, subprocess, collections, statistics as st, glob
sys.path.insert(0, '.')
import fingerprint as F
PROBE = sys.argv[sys.argv.index('--probe') + 1]
MAX = int(sys.argv[sys.argv.index('--max') + 1]) if '--max' in sys.argv else 25
FAM = ['monster', 'power', 'durab', 'treasure', 'jungle', 'home', 'bruiser']; STAT = ['str', 'foc', 'hp']
src = sys.argv[1]; games = F.games_of(src)
if '--version' in sys.argv:
    VER = set(int(v) for v in sys.argv[sys.argv.index('--version') + 1].split(',')); VM = json.load(open('/tmp/ourvers.json'))
    games = [g for g in games if VM.get(g[3]) in VER]
games = sorted(games, key=lambda g: g[3])[:MAX]
order = collections.defaultdict(collections.Counter); shards = collections.defaultdict(lambda: collections.defaultdict(list)); hp = collections.defaultdict(lambda: collections.defaultdict(list))
for mp, path, T, key in games:
    out = subprocess.run(['node', PROBE, path, '--wasm', F.WASM, '--every', '50'], capture_output=True, text=True)
    if out.returncode: continue
    d = json.loads(out.stdout); seen = {}
    rlist = sorted(x['r'] for x in d['rounds'])
    near = {R: min(rlist, key=lambda r: abs(r - R)) for R in (200, 400, 600)}
    near = {R: v for R, v in near.items() if abs(v - R) <= 30}
    for x in d['rounds']:
        r = x['r']
        for c in x['champs']:
            if c[1] != T or len(c) < 12: continue
            raw = [v[0] for v in c[11]][:42]
            tot = sum(v * ((6 if j // 6 == 1 else 5) if j % 2 else 1) for j, v in enumerate(raw))
            for R in (200, 400, 600):
                if near.get(R) == r:
                    shards[c[2]][R].append(tot); hp[c[2]][R].append(c[6])
            # first emblem families in order of completion
            emb = tuple(j for j, v in enumerate(raw) if j % 2 and v > 0)
            prev = seen.get(c[0], ())
            for j in emb:
                if j not in prev: order[c[2]][f'{len(prev)+1}:{FAM[j//6]}-{STAT[(j%6)//2]}'] += 1
            seen[c[0]] = tuple(sorted(set(prev) | set(emb)))
for ch in sorted(shards, key=lambda k: -len(shards[k][400])):
    s = shards[ch]; h = hp[ch]
    print(f"  {ch:8s} shards r200 {st.mean(s[200]) if s[200] else 0:5.1f} r400 {st.mean(s[400]) if s[400] else 0:5.1f} r600 {st.mean(s[600]) if s[600] else 0:5.1f} | maxHp r400 {st.mean(h[400]) if h[400] else 0:5.0f}")
    top = sorted(order[ch].items(), key=lambda t: (int(t[0].split(':')[0]), -t[1]))
    by = collections.defaultdict(list)
    for k, v in top: by[k.split(':')[0]].append((k.split(':')[1], v))
    print('        emblem order: ' + ' | '.join(f"#{i}: " + ', '.join(f'{n} {v}' for n, v in by[str(i)][:2]) for i in range(1, 5) if str(i) in by))
