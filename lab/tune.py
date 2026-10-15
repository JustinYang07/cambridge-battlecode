#!/usr/bin/env python3
"""Parameter sweeps: play copies of a bot with one tuning constant changed against the original.

Each variant is a snapshot of BASE (bots/snap-tune-<name>-<value>) whose core/config.h has that one
`inline constexpr ... name = value;` replaced. Variants are built one at a time (parallel builds
collide on Windows), then played head-to-head against BASE in parallel, each over every map, both
sides and --samples fuel budgets. The table reports the gem difference per game with a 95% CI
(each map x fuel unit averages both sides, as in abtest.py).

Examples:
  python3 lab/tune.py snap-routeBot-v1-8 --param favourable_max=1.25,2.0 --param chase_radius=4,9
  python3 lab/tune.py snap-routeBot-v1-8 --set favourable_max=2.0 --set chase_radius=9   # one combined variant
Results are also saved under lab/data/tuning/.
"""
from __future__ import annotations

import argparse
import json
import re
import shutil
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from labkit import config, runner  # noqa: E402


def make_variant(base: str, label: str, overrides: dict[str, str]) -> str:
    """Copy BASE's source to bots/snap-tune-<label> with the given constants replaced."""
    name = f"{config.SNAPSHOT_PREFIX}tune-{label}"
    dest = config.BOTS_DIR / name
    if dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(config.BOTS_DIR / base / "src", dest / "src")
    cfg = dest / "src" / "core" / "config.h"
    text = cfg.read_text()
    for key, value in overrides.items():
        pattern = re.compile(rf"(inline constexpr (?:float|int|bool|double|unsigned long long) {re.escape(key)} = )([^;]+);")
        match = pattern.search(text)
        if not match:
            raise SystemExit(f"{key} not found in {base}/src/core/config.h")
        literal = value
        if "float" in match.group(1) and not value.endswith("f"):
            literal = (value if "." in value else value + ".0") + "f"
        text = text[:match.start()] + match.group(1) + literal + ";" + text[match.end():]
    cfg.write_text(text)
    return name


def safe(value: str) -> str:
    return re.sub(r"[^0-9A-Za-z_]+", "-", value)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("base", help="bot to tune (use a frozen snapshot)")
    parser.add_argument("--param", action="append", default=[], help="name=v1,v2,... : one variant per value")
    parser.add_argument("--set", action="append", default=[], help="name=value : all --set values in one variant")
    parser.add_argument("--samples", type=int, default=3)
    parser.add_argument("--parallel", type=int, default=3, help="experiments at once")
    parser.add_argument("--maps", default="all")
    parser.add_argument("--opponent", default=None, help="play against this bot instead of BASE")
    args = parser.parse_args()

    variants: list[tuple[str, dict[str, str]]] = []
    for spec in args.param:
        key, values = spec.split("=", 1)
        for v in values.split(","):
            variants.append((f"{key}={v}", {key: v}))
    if args.set:
        combo = dict(s.split("=", 1) for s in args.set)
        variants.append((" ".join(f"{k}={v}" for k, v in combo.items()), combo))
    if not variants:
        parser.error("give at least one --param or --set")

    opponent = args.opponent or args.base
    names = []
    for label, overrides in variants:
        names.append(make_variant(args.base, safe(label)[:60], overrides))
    print(f"building {len(names)} variants and {opponent} ...", flush=True)
    for bot in dict.fromkeys([opponent, *names]):
        runner.build(bot)

    maps = config.list_maps() if args.maps == "all" else args.maps.split(",")
    store = runner.ExperimentStore()
    cores = 12
    jobs = max(1, cores // max(1, args.parallel))
    results: dict[str, dict] = {}
    lock = threading.Lock()
    queue = list(zip([v[0] for v in variants], names))

    def worker() -> None:
        while True:
            with lock:
                if not queue:
                    return
                label, bot = queue.pop(0)
            spec = runner.ExperimentSpec(bot, [opponent], maps, True, f"tune {label} vs {opponent}",
                                         fuels=runner.fuel_samples(args.samples) if args.samples > 1 else None)
            exp = runner.plan(spec)
            try:
                runner.execute(exp, store, jobs, build_bots=False)  # built above: parallel builds collide on Windows
            except Exception as err:  # noqa: BLE001 - keep the sweep going; report what failed
                with lock:
                    results[label] = {"error": str(err), "experiment": exp["id"]}
                    print(f"  {label:<40} FAILED: {err}", flush=True)
                continue
            stats = runner.paired_stats(exp).get(opponent, {})
            with lock:
                results[label] = {**stats, "experiment": exp["id"]}
                print(f"  {label:<40} {stats.get('mean', 0):+7.0f} +/- {2 * stats.get('se', 0):.0f}"
                      f"   ({stats.get('units', 0)} units)", flush=True)

    threads = [threading.Thread(target=worker) for _ in range(max(1, args.parallel))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    print(f"\nvs {opponent}, gem difference per game (95% CI); significant if the interval excludes 0:")
    for label, _ in variants:
        st = results.get(label, {})
        if "error" in st or not st.get("units"):
            print(f"  {label:<40} not measured ({st.get('error', 'no games')[:60]})")
            continue
        lo, hi = st.get("mean", 0) - 2 * st.get("se", 0), st.get("mean", 0) + 2 * st.get("se", 0)
        flag = "  better" if lo > 0 else "  worse" if hi < 0 else ""
        print(f"  {label:<40} {st.get('mean', 0):+7.0f} +/- {2 * st.get('se', 0):<5.0f}{flag}")
    out = config.DATA_DIR / "tuning"
    out.mkdir(parents=True, exist_ok=True)
    path = out / (time.strftime("%Y%m%d-%H%M%S") + ".json")
    path.write_text(json.dumps({"base": args.base, "opponent": opponent, "samples": args.samples, "results": results}, indent=1))
    print(f"saved {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
