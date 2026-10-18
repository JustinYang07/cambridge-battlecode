"""Fit P(win) for fights we take (first death near the fight is theirs) on @F episodes.
Models: committed edge curve (fixed), edge-only logistic, multi-feature logistic (L2).
5-fold cross-validation grouped by game. Usage: fit_fight.py <csv>..."""
import csv
import sys
import numpy as np

rows = []
for p in sys.argv[1:]:
    for r in csv.DictReader(open(p)):
        if r["resolved"] == "1" and r["chose"] == "1":
            r["src"] = p
            rows.append(r)
f = lambda r, k: float(r[k])
def feats(r):
    edge = max(f(r, "edge100") / 100, 0.05)
    toUs, toThem = f(r, "toOurSpawn"), f(r, "toTheirSpawn")
    return {
        "ln edge": np.log(edge),
        "ln hp ratio": np.log(max(f(r, "ourHp"), 1) / max(f(r, "theirHp"), 1)),
        "ln dps ratio": np.log(max(f(r, "ourDps"), 1) / max(f(r, "theirDps"), 1)),
        "us - them": f(r, "us") - f(r, "them"),
        "them": f(r, "them"),
        "ghosts": f(r, "ghosts"),
        "my hp %": f(r, "myHpPct") / 100,
        "target hp %": f(r, "tgtHpPct") / 100,
        "their hp %": f(r, "theirHpPct") / 100,
        "my bag /50": f(r, "myBag") / 50,
        "their bags /50": f(r, "theirBags") / 50,
        "my stun ready": f(r, "myStun"),
        "ally stuns ready": f(r, "allyStuns"),
        "enemy stunners": f(r, "enemyStunners"),
        "their half": toUs / max(toUs + toThem, 1),
        "monster dps /100": f(r, "monsterDps") / 100,
        "dist to target": f(r, "dist"),
        "round /1000": f(r, "r") / 1000,
    }
names = list(feats(rows[0]).keys())
X = np.array([[v for v in feats(r).values()] for r in rows])
y = np.array([1.0 if r["firstDeathOurs"] == "0" else 0.0 for r in rows])
games = np.array([r["src"] + r["game"] for r in rows])
print(f"{len(rows)} resolved fights we took, {len(set(games))} games, win rate {y.mean():.3f}")

def fit(X, y, l2=1.0, iters=30):
    Xb = np.hstack([np.ones((len(X), 1)), X])
    w = np.zeros(Xb.shape[1])
    R = l2 * np.eye(Xb.shape[1]); R[0, 0] = 0
    for _ in range(iters):
        p = 1 / (1 + np.exp(-Xb @ w))
        g = Xb.T @ (p - y) + R @ w
        H = (Xb * (p * (1 - p))[:, None]).T @ Xb + R
        w -= np.linalg.solve(H, g)
    return w
def pred(w, X): return 1 / (1 + np.exp(-(np.hstack([np.ones((len(X), 1)), X]) @ w)))
def logloss(p, y): p = np.clip(p, 1e-6, 1 - 1e-6); return -np.mean(y * np.log(p) + (1 - y) * np.log(1 - p))
def auc(p, y):
    o = np.argsort(p); r = np.empty(len(p)); r[o] = np.arange(1, len(p) + 1)
    n1 = y.sum(); n0 = len(y) - n1
    return (r[y == 1].sum() - n1 * (n1 + 1) / 2) / (n1 * n0)

# standardise for the multi-feature fit (weights reported back in raw units)
mu, sd = X.mean(0), X.std(0) + 1e-9
Z = (X - mu) / sd
ug = np.array(sorted(set(games)))
rng = np.random.default_rng(7)
fold = {g: i for i, g in enumerate(rng.permutation(ug))}
fid = np.array([fold[g] % 5 for g in games])
out = {k: np.zeros(len(y)) for k in ("committed curve", "edge only", "multi-feature")}
ie = names.index("ln edge")
for k in range(5):
    tr, te = fid != k, fid == k
    out["committed curve"][te] = 1 / (1 + np.exp(-(-0.144 + 0.510 * X[te, ie])))
    w1 = fit(X[tr][:, [ie]], y[tr], l2=0.0)
    out["edge only"][te] = pred(w1, X[te][:, [ie]])
    wm = fit(Z[tr], y[tr], l2=5.0)
    out["multi-feature"][te] = pred(wm, Z[te])
base = logloss(np.full(len(y), y.mean()), y)
print(f"\nheld-out by game (5 folds); baseline log loss (constant {y.mean():.2f}) {base:.4f}")
for k, p in out.items():
    print(f"  {k:16s} log loss {logloss(p, y):.4f}  brier {np.mean((p - y) ** 2):.4f}  AUC {auc(p, y):.3f}")
print("\ncalibration (held-out multi-feature vs edge-only): predicted bucket -> actual win rate")
for k in ("edge only", "multi-feature"):
    p = out[k]
    line = []
    for lo in np.arange(0, 1, 0.1):
        m = (p >= lo) & (p < lo + 0.1)
        if m.sum() >= 30:
            line.append(f"{int(lo*100)}-{int(lo*100)+10}%: {100*y[m].mean():.0f}% (n={m.sum()})")
    print(f"  {k}: " + "; ".join(line))
w = fit(Z, y, l2=5.0)
print("\nfull-data multi-feature weights (per standard deviation; + helps winning):")
for n, v in sorted(zip(names, w[1:]), key=lambda t: -abs(t[1])):
    print(f"  {n:18s} {v:+.3f}")
