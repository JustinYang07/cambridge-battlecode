#!/usr/bin/env python3
"""Fight models from scraped top-team replays (lab/data/fights_fish.csv, fight_engage_dataset.py).

Two logistic models on features the bot can see in a game (counts, health and bags around the
contact, alive counts, spawn distances, round):
  engage  imitation: does the team (default Intercontinental Fish) move in (approach < -0.3)
          rather than back off (approach > 0.3)? Neutral rows are dropped.
  good    outcome over the next 15 rounds, any team: more of them die than of us, or as many
          and they lose more health.
Prints held-out AUC (split by game) and the weights in raw feature units, as C++ constants.
usage: .venv/bin/python lab/nn/train_fish_fight.py [--csv lab/data/fights_fish.csv] [--team NAME]
"""
import csv, sys
import numpy as np
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import roc_auc_score

FEATS = ['us8', 'them8', 'us14', 'them14', 'usAlive', 'themAlive', 'hpUs8', 'hpThem8', 'maxHpUs8', 'maxHpThem8',
         'fracUs8', 'fracThem8', 'bagUs8', 'bagThem8', 'dOurSpawn', 'dTheirSpawn', 'round1000']


def arg(name, default):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


def fit(X, y, games, name):
    keys = sorted(set(games))
    rng = np.random.default_rng(7)
    test = set(rng.choice(keys, size=max(1, len(keys) // 5), replace=False))
    te = np.array([g in test for g in games])
    mu, sd = X[~te].mean(0), X[~te].std(0) + 1e-6
    m = LogisticRegression(C=1.0, max_iter=2000).fit((X[~te] - mu) / sd, y[~te])
    auc = roc_auc_score(y[te], m.predict_proba((X[te] - mu) / sd)[:, 1])
    full = LogisticRegression(C=1.0, max_iter=2000).fit((X - mu) / sd, y)
    w = full.coef_[0] / sd
    b = full.intercept_[0] - float((full.coef_[0] * mu / sd).sum())
    print(f'{name}: {len(y)} rows ({y.mean():.0%} positive), held-out AUC {auc:.3f}')
    print(f'inline constexpr float {name}_bias = {b:.5f}f;')
    print(f'inline constexpr std::array<float, {len(FEATS)}> {name}_w{{' + ', '.join(f'{v:.5f}f' for v in w) + '};')
    print('  // ' + ', '.join(FEATS))
    order = np.argsort(-np.abs(full.coef_[0]))
    print('  strongest (standardised):', ', '.join(f'{FEATS[i]} {full.coef_[0][i]:+.2f}' for i in order[:8]))


def main():
    rows = list(csv.DictReader(open(arg('--csv', 'lab/data/fights_fish.csv'))))
    team = arg('--team', 'Intercontinental Fish')
    X = lambda rs: np.array([[float(r[f]) for f in FEATS] for r in rs])
    mine = [r for r in rows if r['team'] == team and abs(float(r['approach'])) > 0.3]
    fit(X(mine), np.array([float(r['approach']) < 0 for r in mine], dtype=int), [r['game'] for r in mine], 'fish_engage')
    good = lambda r: int(r['theirDeaths']) > int(r['ourDeaths']) or (r['theirDeaths'] == r['ourDeaths'] and float(r['hpTrade']) > 0)
    fit(X(rows), np.array([good(r) for r in rows], dtype=int), [r['game'] for r in rows], 'fight_good')


if __name__ == '__main__':
    main()
