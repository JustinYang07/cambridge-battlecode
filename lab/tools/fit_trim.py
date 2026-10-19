"""Trimmed fight model in raw units (for C++), plus refit edge-only; CV by game. Usage: fit_trim.py <csv>..."""
import csv, sys
import numpy as np
rows = [r for p in sys.argv[1:] for r in csv.DictReader(open(p)) if r["resolved"] == "1" and r["chose"] == "1"]
src = [p for p in sys.argv[1:] for r in csv.DictReader(open(p)) if r["resolved"] == "1" and r["chose"] == "1"]
f = lambda r, k: float(r[k])
def feats(r):
    return [np.log(max(f(r, "edge100") / 100, 0.05)),
            np.log(max(f(r, "ourHp"), 1) / max(f(r, "theirHp"), 1)),
            np.log(max(f(r, "ourDps"), 1) / max(f(r, "theirDps"), 1)),
            f(r, "us") - f(r, "them"), f(r, "them"),
            f(r, "theirHpPct") / 100, f(r, "tgtHpPct") / 100, f(r, "dist")]
names = ["ln edge", "ln hp ratio", "ln dps ratio", "us - them", "them", "their hp frac", "target hp frac", "dist"]
X = np.array([feats(r) for r in rows]); y = np.array([1.0 if r["firstDeathOurs"] == "0" else 0.0 for r in rows])
games = np.array([s + r["game"] for s, r in zip(src, rows)])
def fit(X, y, l2=1.0):
    Xb = np.hstack([np.ones((len(X), 1)), X]); w = np.zeros(Xb.shape[1]); R = l2 * np.eye(len(w)); R[0, 0] = 0
    for _ in range(40):
        p = 1 / (1 + np.exp(-Xb @ w)); w -= np.linalg.solve((Xb * (p * (1 - p))[:, None]).T @ Xb + R, Xb.T @ (p - y) + R @ w)
    return w
pred = lambda w, X: 1 / (1 + np.exp(-(np.hstack([np.ones((len(X), 1)), X]) @ w)))
ll = lambda p, y: -np.mean(y * np.log(np.clip(p, 1e-6, 1)) + (1 - y) * np.log(np.clip(1 - p, 1e-6, 1)))
def auc(p, y):
    o = np.argsort(p); r = np.empty(len(p)); r[o] = np.arange(1, len(p) + 1); n1 = y.sum(); n0 = len(y) - n1
    return (r[y == 1].sum() - n1 * (n1 + 1) / 2) / (n1 * n0)
ug = np.array(sorted(set(games))); fold = {g: i % 5 for i, g in enumerate(np.random.default_rng(7).permutation(ug))}
fid = np.array([fold[g] for g in games]); pe = np.zeros(len(y)); pm = np.zeros(len(y))
for k in range(5):
    tr, te = fid != k, fid == k
    pe[te] = pred(fit(X[tr][:, :1], y[tr], 0.0), X[te][:, :1]); pm[te] = pred(fit(X[tr], y[tr], 1.0), X[te])
print(f"n={len(y)}  held-out: edge-only log loss {ll(pe,y):.4f} AUC {auc(pe,y):.3f} | trimmed log loss {ll(pm,y):.4f} AUC {auc(pm,y):.3f}")
we = fit(X[:, :1], y, 0.0); wm = fit(X, y, 1.0)
print(f"edge-only: a={we[0]:+.4f} b={we[1]:+.4f}")
print("trimmed (raw units): intercept", f"{wm[0]:+.4f}"); [print(f"  {n:15s} {v:+.4f}") for n, v in zip(names, wm[1:])]
