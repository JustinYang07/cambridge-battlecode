#!/usr/bin/env python3
"""SPSA tuning of several core/config.h constants at once (the method chess engines tune with).

Each step perturbs every parameter by +/- c (in units of its range) at random, builds the two
variants, plays them head-to-head on every map from both sides at one random fuel budget, and
moves every parameter towards the side that won. A step costs one batch of games however many
parameters there are, and many cheap noisy steps average out the chaos that a few precise A/Bs
cannot afford. Confirm the result afterwards with abtest.py --samples against a fixed pool.

  python3 lab/spsa.py snap-routeBot4-latest --param travel_weight=1:8 --param bank_target_gems=30:100 --steps 100
  python3 lab/spsa.py snap-routeBot4-latest --resume lab/data/spsa/<run>.json --steps 50   # continue a run

Integer constants are rounded when written. Progress is saved after every step under lab/data/spsa/.
"""
from __future__ import annotations

import argparse
import json
import random
import re
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from labkit import config, runner  # noqa: E402
from tune import make_variant  # noqa: E402

CONST = re.compile(r"inline constexpr (float|int|double|unsigned long long) (\w+) = ([^;]+);")


def read_constants(bot: str) -> dict[str, tuple[str, float]]:
    text = (config.BOTS_DIR / bot / "src" / "core" / "config.h").read_text()
    out = {}
    for kind, name, value in CONST.findall(text):
        try:
            out[name] = (kind, float(value.rstrip("fF").replace("1.0f / 8.0f", "0.125")))
        except ValueError:
            pass  # expressions (e.g. other constants) are not tunable here
    return out


def literal(kind: str, value: float) -> str:
    return str(int(round(value))) if kind in ("int", "unsigned long long") else f"{value:.4g}"


def play_pair(plus: str, minus: str, map_name: str, fuel: int, out: Path) -> float | None:
    """Gem difference (plus - minus) averaged over both sides of one map, or None on failure."""
    diffs = []
    for a, b, plus_is_a in ((plus, minus, True), (minus, plus, False)):
        r = runner.run_match(a, b, map_name, out / f"{map_name}-{'P' if plus_is_a else 'M'}A", fuel=fuel)
        if "teams" not in r:
            return None
        ga, gb = r["teams"][0]["gems"], r["teams"][1]["gems"]
        diffs.append(ga - gb if plus_is_a else gb - ga)
    return sum(diffs) / 2


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("base", help="bot to tune (a frozen snapshot)")
    ap.add_argument("--param", action="append", default=[], help="name=lo:hi (starts from BASE's value)")
    ap.add_argument("--steps", type=int, default=100)
    ap.add_argument("--c", type=float, default=0.15, help="perturbation, as a share of each range")
    ap.add_argument("--a", type=float, default=0.08, help="step size, as a share of each range per 500 gems")
    ap.add_argument("--maps", default="all")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--resume", help="continue from a saved run")
    args = ap.parse_args()

    consts = read_constants(args.base)
    if args.resume:
        state = json.loads(Path(args.resume).read_text())
        params = state["params"]
    else:
        params = {}
        for spec in args.param:
            name, rng = spec.split("=", 1)
            lo, hi = (float(x) for x in rng.split(":"))
            if name not in consts:
                raise SystemExit(f"{name} not found in {args.base}/src/core/config.h")
            kind, start = consts[name]
            params[name] = {"kind": kind, "lo": lo, "hi": hi, "x": (min(max(start, lo), hi) - lo) / (hi - lo)}
        state = {"base": args.base, "params": params, "history": [], "started": time.strftime("%Y%m%d-%H%M%S")}
    if not params:
        ap.error("give at least one --param")
    out_dir = config.DATA_DIR / "spsa"
    out_dir.mkdir(parents=True, exist_ok=True)
    save_path = Path(args.resume) if args.resume else out_dir / f"{state['started']}.json"
    maps = config.list_maps() if args.maps == "all" else args.maps.split(",")
    value = lambda p, x: p["lo"] + x * (p["hi"] - p["lo"])

    start_k = len(state["history"])
    for k in range(start_k, start_k + args.steps):
        ck = args.c / (k + 1) ** 0.101
        ak = args.a / (k + 1 + 10) ** 0.602 * 10 ** 0.602  # ~a at the start, decaying
        delta = {n: random.choice((-1, 1)) for n in params}
        sides = {}
        for sign, label in ((1, "plus"), (-1, "minus")):
            sides[label] = {n: literal(p["kind"], value(p, min(1.0, max(0.0, p["x"] + sign * ck * delta[n]))))
                            for n, p in params.items()}
        plus = make_variant(args.base, "spsa-plus", sides["plus"])
        minus = make_variant(args.base, "spsa-minus", sides["minus"])
        with ThreadPoolExecutor(2) as pool:
            list(pool.map(runner.build, [plus, minus]))
        fuel = random.choice(runner.fuel_samples(9))
        games = out_dir / "games" / f"step{k:04d}"
        with ThreadPoolExecutor(max(1, args.jobs // 2)) as pool:
            results = list(pool.map(lambda m: play_pair(plus, minus, m, fuel, games), maps))
        diffs = [r for r in results if r is not None]
        if not diffs:
            print(f"step {k}: no games finished", flush=True)
            continue
        r = sum(diffs) / len(diffs)
        for n, p in params.items():
            p["x"] = min(1.0, max(0.0, p["x"] + ak * (r / 500.0) * delta[n]))
        now = {n: round(value(p, p["x"]), 4) for n, p in params.items()}
        state["history"].append({"step": k, "fuel": fuel, "r": r, "units": len(diffs), "delta": delta, "theta": now})
        save_path.write_text(json.dumps(state, indent=1))
        print(f"step {k:3d} fuel {fuel} plus-minus {r:+7.0f}  " + "  ".join(f"{n}={v:g}" for n, v in now.items()), flush=True)

    final = {n: literal(p["kind"], value(p, p["x"])) for n, p in params.items()}
    print("\nfinal: " + " ".join(f"--set {n}={v}" for n, v in final.items()))
    print(f"saved {save_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
