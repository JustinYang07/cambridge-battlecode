#!/usr/bin/env python3
"""Distil top teams' fight decisions (fight_policy_dataset.py): softmax over hold / engage / retreat /
recall from a champion's local picture (NF -> H -> 4, ReLU). Trained on --teams, held out by game.
Then, on --compare (our team's rows): how often our choice matches the distilled policy, and our
death rate within 10 rounds when we matched vs when we did something else.
usage: train_policy.py [--csv lab/data/policy.csv] [--teams "A,B"] [--compare bbas] [--hidden 16]
                       [--epochs 12] [--out header.h]"""
import csv, os, sys
import numpy as np
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
arg = lambda k, d: sys.argv[sys.argv.index(k) + 1] if k in sys.argv else d
CSV = arg('--csv', f'{ROOT}/lab/data/policy.csv')
TEAMS = set(arg('--teams', 'Intercontinental Fish,Mite,Univardisty of Shingtonwash,Sneaky Bush Campers,Nyx').split(','))
COMPARE = arg('--compare', 'bbas'); H = int(arg('--hidden', '16')); EP = int(arg('--epochs', '12')); OUT = arg('--out', '')
LABELS = ['hold', 'engage', 'retreat', 'recall']
META = {'game', 'map', 'round', 'team', 'opp', 'label', 'died10', 'lost10'}
FOG = {'usAlive', 'themAlive', 'killsUs', 'killsThem', 'teamMaxHpUs', 'teamMaxHpThem', 'teamShardsUs', 'teamShardsThem'}

rows = [r for r in csv.DictReader(open(CSV)) if r['team'] in TEAMS or r['team'] == COMPARE]
# --extra CSV: more rows to compare (e.g. a lab experiment's 'cand' rows), appended with the same columns.
if '--extra' in sys.argv:
    rows += [r for r in csv.DictReader(open(arg('--extra', ''))) if r['team'] == COMPARE]
FEATS = [k for k in rows[0] if k not in META and k not in FOG]
X = np.array([[float(r[k]) for k in FEATS] for r in rows], dtype=np.float32)
y = np.array([LABELS.index(r['label']) for r in rows]); died = np.array([int(r['died10']) for r in rows])
team = np.array([r['team'] for r in rows]); g = np.array([r['game'] for r in rows])
top = np.isin(team, list(TEAMS)); ours = team == COMPARE
rng = np.random.default_rng(5); games = np.unique(g[top]); rng.shuffle(games)
test = top & np.isin(g, games[: len(games) // 5]); train = top & ~test
mu, sd = X[train].mean(0), X[train].std(0) + 1e-6; Xn = (X - mu) / sd
NF, K = X.shape[1], len(LABELS)
print(f'{train.sum()} train / {test.sum()} test rows from {len(games)} top-team games; {ours.sum()} rows of {COMPARE}; {NF} features')
print('  top-team label mix:', {l: f'{(y[top] == i).mean():.2f}' for i, l in enumerate(LABELS)})


def fwd(P, A):
    h = np.maximum(0, A @ P[0] + P[1]); z = h @ P[2] + P[3]
    z = z - z.max(1, keepdims=True); e = np.exp(z); return h, e / e.sum(1, keepdims=True)


r = np.random.default_rng(1)
P = [r.normal(0, 0.2, (NF, H)), np.zeros(H), r.normal(0, 0.2, (H, K)), np.zeros(K)]
m = [np.zeros_like(p) for p in P]; v = [np.zeros_like(p) for p in P]; t = 0
tr = np.where(train)[0]; Y = np.eye(K)[y]
for ep in range(EP):
    r.shuffle(tr)
    for k in range(0, len(tr), 1024):
        b = tr[k:k + 1024]; h, p = fwd(P, Xn[b]); gz = (p - Y[b]) / len(b)
        gh = (gz @ P[2].T) * (h > 0)
        G = [Xn[b].T @ gh + 1e-5 * P[0], gh.sum(0), h.T @ gz, gz.sum(0)]
        t += 1
        for j in range(4):
            m[j] = 0.9 * m[j] + 0.1 * G[j]; v[j] = 0.999 * v[j] + 0.001 * G[j] ** 2
            P[j] = P[j] - 0.002 * (m[j] / (1 - 0.9 ** t)) / (np.sqrt(v[j] / (1 - 0.999 ** t)) + 1e-8)
_, pt = fwd(P, Xn[test])
prior = np.bincount(y[train], minlength=K) / train.sum()
ll = -np.mean(np.log(pt[np.arange(test.sum()), y[test]] + 1e-9)); llp = -np.mean(np.log(prior[y[test]]))
print(f'  held-out top-team games: accuracy {(pt.argmax(1) == y[test]).mean():.3f} (majority {prior.max():.3f}), log loss {ll:.3f} (label mix {llp:.3f})')
for i, l in enumerate(LABELS):
    sel = y[test] == i
    print(f'    {l:8s} recall of label {(pt[sel].argmax(1) == i).mean():.2f}')

# Our team against the distilled policy.
_, po = fwd(P, Xn[ours]); yo = y[ours]; do = died[ours]; pick = po.argmax(1)
print(f'\n{COMPARE}: label mix', {l: f'{(yo == i).mean():.2f}' for i, l in enumerate(LABELS)},
      '| policy would pick', {l: f'{(pick == i).mean():.2f}' for i, l in enumerate(LABELS)})
match = pick == yo
print(f'  match {match.mean():.3f}; death within 10 rounds when matching {do[match].mean():.3f}, when not {do[~match].mean():.3f}')
print('  where we differ (ours -> policy): share of rows, our death rate')
for a in range(K):
    for b in range(K):
        sel = (yo == a) & (pick == b)
        if a != b and sel.mean() > 0.02:
            print(f'    {LABELS[a]:8s} -> {LABELS[b]:8s} {sel.mean():.3f}  died {do[sel].mean():.3f}')
# Same comparison for the top teams' own held-out rows (sanity: matching should not be deadlier).
pk = pt.argmax(1); mt = pk == y[test]
print(f'  (top teams held out: death when matching {died[test][mt].mean():.3f}, when not {died[test][~mt].mean():.3f})')

# Does retreating where the policy says so lower deaths? Death rate by the policy's retreat
# probability, split by what the champion did (retreat or recall vs stayed).
print('\n  p(retreat) band | top teams: died if left / stayed (rows) | ours: died if left / stayed (rows)')
_, pall = fwd(P, Xn)
for lo, hi in ((0, .15), (.15, .3), (.3, .45), (.45, .6), (.6, 1.01)):
    band = (pall[:, 2] + pall[:, 3] >= lo) & (pall[:, 2] + pall[:, 3] < hi)
    left = (y == 2) | (y == 3)
    def rate(sel): return f'{died[sel].mean():.3f}' if sel.sum() > 30 else '  -  '
    a, b = band & test & left, band & test & ~left; c, d = band & ours & left, band & ours & ~left
    lt = (band & test & left).sum() / max((band & test).sum(), 1); lo_ = (band & ours & left).sum() / max((band & ours).sum(), 1)
    print(f'    {lo:.2f}-{min(hi,1):.2f}       | {rate(a)} / {rate(b)} ({(band & test).sum():6d}, left {lt:.0%}) | {rate(c)} / {rate(d)} ({(band & ours).sum():6d}, left {lo_:.0%})')

if OUT:
    def lit(x):
        s = f'{float(x):.6g}'
        return (s if ('.' in s or 'e' in s or 'n' in s) else s + '.0') + 'f'
    arr = lambda a: ', '.join(lit(x) for x in np.ravel(a))
    with open(OUT, 'w') as f:
        f.write(f'#pragma once\n// Generated by lab/nn/train_policy.py: top teams\' fight decisions ({", ".join(sorted(TEAMS))}).\n')
        f.write(f'// Held-out accuracy {(pt.argmax(1) == y[test]).mean():.3f}. Outputs: ' + ', '.join(LABELS) + '.\n')
        f.write('// Inputs (in order): ' + ', '.join(FEATS) + '\n#include <array>\n#include <cmath>\n\nnamespace nnpolicy {\n\n')
        f.write(f'inline constexpr int kIn = {NF}, kHidden = {H}, kOut = {K};\n')
        for nm, a in (('kMean', mu), ('kStd', sd)): f.write(f'inline constexpr std::array<float, kIn> {nm}{{{arr(a)}}};\n')
        f.write(f'inline constexpr std::array<float, kIn * kHidden> kW1{{{arr(P[0])}}};\ninline constexpr std::array<float, kHidden> kB1{{{arr(P[1])}}};\n')
        f.write(f'inline constexpr std::array<float, kHidden * kOut> kW2{{{arr(P[2])}}};\ninline constexpr std::array<float, kOut> kB2{{{arr(P[3])}}};\n\n')
        f.write('inline std::array<float, kOut> probs(const std::array<float, kIn>& x) {\n    std::array<float, kHidden> h{};\n    for (int j = 0; j < kHidden; ++j) h[j] = kB1[j];\n')
        f.write('    for (int i = 0; i < kIn; ++i) {\n        const float v = (x[i] - kMean[i]) / kStd[i];\n        for (int j = 0; j < kHidden; ++j) h[j] += v * kW1[i * kHidden + j];\n    }\n')
        f.write('    std::array<float, kOut> z{};\n    float mx = -1e30f;\n    for (int k = 0; k < kOut; ++k) {\n        z[k] = kB2[k];\n        for (int j = 0; j < kHidden; ++j) z[k] += (h[j] > 0 ? h[j] : 0.0f) * kW2[j * kOut + k];\n        mx = z[k] > mx ? z[k] : mx;\n    }\n')
        f.write('    float s = 0;\n    for (auto& v : z) { v = std::exp(v - mx); s += v; }\n    for (auto& v : z) v /= s;\n    return z;\n}\n\n} // namespace nnpolicy\n')
    print('wrote', OUT)
