"""Build bots, run matches in parallel and persist A/B experiments.

An experiment pits a candidate bot against one or more opponents on a set of
maps. With `both_sides` every pairing is played twice, once as team A and once
as team B, because turn order and spawn location both matter. Matches are
deterministic for fixed bot code, so one game per (map, side) is enough.
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import tarfile
import io
import threading
import time
import uuid
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from . import config

CHAMPIONS = ["Ryth", "Vesper Vale", "Vendetta", "Cassi", "Issac", "Stellar", "Prism",
             "Marcella", "Willow", "Ren", "Varg", "Tavi"]
DRAFT_PICKS = ["Ryth", "Vesper Vale", "Vendetta", "Cassi & Issac", "Stellar", "Prism",
               "Marcella", "Willow", "Ren", "Varg", "Tavi"]
_FAMILIES = ["monster", "power", "durability", "treasure", "jungle", "home", "bruiser"]
ITEMS = [f"{fam} {kind} of {stat}" for fam in _FAMILIES for stat in ("strength", "focus", "health")
         for kind in ("shard", "emblem")]
CONSUMABLES = ["tp crystal", "altar lock"]


class BuildError(RuntimeError):
    pass


def build(bot: str) -> str:
    proc = subprocess.run([config.cambc_binary(), "build", bot], cwd=config.ROOT,
                          capture_output=True, text=True)
    if proc.returncode != 0:
        raise BuildError(f"build of {bot} failed:\n{proc.stdout[-4000:]}\n{proc.stderr[-4000:]}")
    return proc.stdout


def _named(indices: list[int], names: list[str]) -> dict[str, int]:
    return {names[i] if i < len(names) else str(i): n for i, n in enumerate(indices) if n}


def summarise_stats(stats: dict) -> dict:
    """Flatten the engine's statistics JSON into what the UI and reports need."""
    teams = []
    for t in sorted(stats.get("teams", []), key=lambda t: t["team"]):
        champs = [c for c in stats.get("champions", []) if c["team"] == t["team"]]
        teams.append({
            "gems": t.get("submittedGems", 0),
            "draft": [DRAFT_PICKS[p] for p in t.get("draftPicks", [])],
            "kills": sum(c.get("kills", 0) for c in champs),
            "deaths": sum(c.get("deaths", 0) for c in champs),
            "goldEarned": sum(c.get("earnedGold", 0) for c in champs),
            "champions": [{
                "name": CHAMPIONS[c["type"]] if c["type"] < len(CHAMPIONS) else str(c["type"]),
                "kills": c.get("kills", 0), "deaths": c.get("deaths", 0), "assists": c.get("assists", 0),
                "submittedGems": c.get("submittedGems", 0), "collectedGems": c.get("collectedGems", 0),
                "earnedGold": c.get("earnedGold", 0), "spentGold": c.get("spentGold", 0),
                "monsterDamageDone": round(c.get("monsterDamageDone", 0)),
                "monsterDamageTaken": round(c.get("monsterDamageTaken", 0)),
                "championDamageDone": round(c.get("championDamageDone", 0)),
                "championDamageTaken": round(c.get("championDamageTaken", 0)),
                "healingDone": round(c.get("healingDone", 0)),
                "items": _named(c.get("items", []), ITEMS),
                "consumablesBought": _named(c.get("consumablesBought", []), CONSUMABLES),
            } for c in champs],
        })
    outcome = stats.get("outcome", "")
    winner = "A" if outcome == "teamA" else "B" if outcome == "teamB" else None
    return {"winner": winner, "rounds": stats.get("round"), "faults": stats.get("botFaults", [0, 0]),
            "teams": teams}


def run_match(bot_a: str, bot_b: str, map_name: str, out_dir: Path, rounds: int | None = None,
              timeout: float = 600, fuel: int | None = None) -> dict:
    """Play one match; returns the summarised result plus the replay path."""
    out_dir.mkdir(parents=True, exist_ok=True)
    stats_path = out_dir / "stats.json"
    cmd = [config.cambc_binary(), "run", bot_a, bot_b, "--map", str(config.map_path(map_name)),
           "-o", str(out_dir), "--statistics", str(stats_path)]
    if rounds:
        cmd += ["--rounds", str(rounds)]
    if fuel:
        cmd += ["--fuel", str(fuel)]
    started = time.time()
    proc = subprocess.run(cmd, cwd=config.ROOT, capture_output=True, text=True, timeout=timeout)
    (out_dir / "runner.log").write_text(proc.stdout + proc.stderr)
    replay = next(iter(sorted(out_dir.glob("*.repl26a"))), None)
    result: dict = {"seconds": round(time.time() - started, 2),
                    "replay": str(replay.relative_to(config.ROOT)) if replay else None}
    if stats_path.exists():
        result.update(summarise_stats(json.loads(stats_path.read_text())))
    else:
        result["error"] = (proc.stderr or proc.stdout)[-2000:] or f"exit code {proc.returncode}"
    return result


# --------------------------------------------------------------------------- snapshots

def snapshot(bot: str, label: str | None = None, git_ref: str | None = None) -> str:
    """Freeze a bot's source as bots/snap-<bot>-<label> so later edits can be A/B tested.

    With `git_ref`, the source comes from that commit instead of the working tree.
    """
    label = label or (git_ref or time.strftime("%m%d-%H%M"))
    safe = "".join(ch if ch.isalnum() or ch in "-_" else "-" for ch in label)
    name = f"{config.SNAPSHOT_PREFIX}{bot}-{safe}"
    dest = config.BOTS_DIR / name
    if dest.exists():
        shutil.rmtree(dest)
    (dest / "src").mkdir(parents=True)
    if git_ref:
        archive = subprocess.run(["git", "archive", "--format=tar", git_ref, f"bots/{bot}/src"],
                                 cwd=config.ROOT, capture_output=True, check=True).stdout
        prefix = f"bots/{bot}/src/"
        with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
            for member in tar.getmembers():
                if member.isfile() and member.name.startswith(prefix):
                    target = dest / "src" / member.name[len(prefix):]
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(tar.extractfile(member).read())
    else:
        shutil.rmtree(dest / "src")
        shutil.copytree(config.BOTS_DIR / bot / "src", dest / "src")
    return name


# --------------------------------------------------------------------------- experiments

@dataclass
class ExperimentSpec:
    candidate: str
    opponents: list[str]
    maps: list[str]
    both_sides: bool = True
    name: str = ""
    notes: str = ""
    rounds: int | None = None
    # Fuel per turn for each sample. Games are deterministic, but tiny fuel differences change the
    # bots' fuel-dependent choices and the games diverge from there: playing each (map, side,
    # opponent) at several nearby budgets samples that noise, so averages can be trusted.
    fuels: list[int] | None = None


def fuel_samples(n: int, base: int = 100_000, step: int = 1_000) -> list[int]:
    """n budgets centred on the engine default (100k), `step` apart: 3 -> 99k, 100k, 101k."""
    return [base + step * (k - (n - 1) // 2) for k in range(n)]


class ExperimentStore:
    """Experiments live as JSON files under lab/data/experiments/<id>/experiment.json."""

    def __init__(self) -> None:
        config.ensure_dirs()
        self._lock = threading.Lock()

    def path(self, exp_id: str) -> Path:
        return config.EXPERIMENTS_DIR / exp_id / "experiment.json"

    def load(self, exp_id: str) -> dict:
        return json.loads(self.path(exp_id).read_text())

    def save(self, exp: dict) -> None:
        path = self.path(exp["id"])
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_suffix(".tmp")
        with self._lock:
            tmp.write_text(json.dumps(exp, indent=1))
            # On Windows the replace fails while another process (the web lab, a reader) has the
            # file open: retry briefly instead of losing the experiment.
            for attempt in range(20):
                try:
                    os.replace(tmp, path)
                    break
                except PermissionError:
                    if attempt == 19:
                        raise
                    time.sleep(0.1)

    def list(self) -> list[dict]:
        out = []
        for path in config.EXPERIMENTS_DIR.glob("*/experiment.json"):
            try:
                exp = json.loads(path.read_text())
            except json.JSONDecodeError:
                continue
            exp.pop("games", None)
            out.append(exp)
        return sorted(out, key=lambda e: e.get("createdAt", 0), reverse=True)

    def delete(self, exp_id: str) -> None:
        shutil.rmtree(config.EXPERIMENTS_DIR / exp_id, ignore_errors=True)


def plan(spec: ExperimentSpec) -> dict:
    exp_id = time.strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:4]
    games = []
    for fuel in spec.fuels or [None]:
        for opponent in spec.opponents:
            for map_name in spec.maps:
                sides = ("A", "B") if spec.both_sides else ("A",)
                for side in sides:
                    bot_a, bot_b = (spec.candidate, opponent) if side == "A" else (opponent, spec.candidate)
                    games.append({"idx": len(games), "map": map_name, "opponent": opponent,
                                  "candidateSide": side, "botA": bot_a, "botB": bot_b, "status": "queued",
                                  **({"fuel": fuel} if fuel else {})})
    return {"id": exp_id, "name": spec.name or f"{spec.candidate} vs {', '.join(spec.opponents)}",
            "notes": spec.notes, "candidate": spec.candidate, "opponents": spec.opponents,
            "maps": spec.maps, "bothSides": spec.both_sides, "rounds": spec.rounds, "fuels": spec.fuels,
            "createdAt": time.time(), "status": "queued", "games": games, "gitCommit": _git_head()}


def _git_head() -> str | None:
    proc = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=config.ROOT, capture_output=True, text=True)
    dirty = subprocess.run(["git", "status", "--porcelain", "bots"], cwd=config.ROOT, capture_output=True, text=True)
    return (proc.stdout.strip() + ("+dirty" if dirty.stdout.strip() else "")) if proc.returncode == 0 else None


def aggregate(exp: dict) -> dict:
    """Candidate-centric totals: overall, per map and per opponent."""
    def blank():
        return {"played": 0, "wins": 0, "losses": 0, "gemDiff": 0, "gemsFor": 0, "gemsAgainst": 0,
                "rounds": 0, "faults": 0, "deaths": 0, "killedByEnemy": 0, "gemsLost": 0}

    overall, per_map, per_opp = blank(), {}, {}
    for g in exp["games"]:
        r = g.get("result")
        if not r or "winner" not in r:
            continue
        mine = 0 if g["candidateSide"] == "A" else 1
        gems_for, gems_against = r["teams"][mine]["gems"], r["teams"][1 - mine]["gems"]
        for bucket in (overall, per_map.setdefault(g["map"], blank()), per_opp.setdefault(g["opponent"], blank())):
            bucket["played"] += 1
            bucket["wins"] += r["winner"] == g["candidateSide"]
            bucket["losses"] += r["winner"] not in (None, g["candidateSide"])
            bucket["gemsFor"] += gems_for
            bucket["gemsAgainst"] += gems_against
            bucket["gemDiff"] += gems_for - gems_against
            bucket["rounds"] += r.get("rounds") or 0
            bucket["faults"] += r["faults"][mine]
            ours, theirs = r["teams"][mine], r["teams"][1 - mine]
            bucket["deaths"] += ours.get("deaths", 0)
            bucket["killedByEnemy"] += theirs.get("kills", 0)
            # Gems picked up but never tributed (lost to deaths, or still carried at the end).
            bucket["gemsLost"] += sum(c.get("collectedGems", 0) - c.get("submittedGems", 0) for c in ours.get("champions", []))
    return {"overall": overall, "perMap": per_map, "perOpponent": per_opp, "paired": paired_stats(exp)}


def paired_stats(exp: dict) -> dict:
    """Gem difference per game with a standard error, per opponent.

    Maps are strongly side-biased, so each unit is one (opponent, map, fuel) with both sides
    averaged; units are then treated as independent samples. 95% CI is about mean +/- 2 SE.
    """
    units: dict[tuple, list[int]] = {}
    for g in exp["games"]:
        r = g.get("result")
        if not r or "winner" not in r:
            continue
        mine = 0 if g["candidateSide"] == "A" else 1
        diff = r["teams"][mine]["gems"] - r["teams"][1 - mine]["gems"]
        units.setdefault((g["opponent"], g["map"], g.get("fuel")), []).append(diff)
    out: dict = {}
    for opp in dict.fromkeys(k[0] for k in units):
        vals = [sum(v) / len(v) for k, v in units.items() if k[0] == opp]
        n = len(vals)
        mean = sum(vals) / n if n else 0.0
        var = sum((x - mean) ** 2 for x in vals) / (n - 1) if n > 1 else 0.0
        out[opp] = {"units": n, "mean": round(mean, 1), "se": round((var / n) ** 0.5, 1) if n else 0.0}
    return out


def execute(exp: dict, store: ExperimentStore, jobs: int | None = None,
            on_update: Callable[[dict], None] | None = None,
            should_stop: Callable[[], bool] | None = None, build_bots: bool = True) -> dict:
    """Build every bot once (unless the caller already did), then play all queued games in parallel."""
    jobs = jobs or max(1, (os.cpu_count() or 2) - 1)
    exp["status"] = "building"
    store.save(exp)
    try:
        for bot in dict.fromkeys([exp["candidate"], *exp["opponents"]]) if build_bots else ():
            build(bot)
    except BuildError as err:
        exp["status"], exp["error"] = "failed", str(err)
        store.save(exp)
        return exp

    exp["status"] = "running"
    exp["startedAt"] = time.time()
    store.save(exp)
    lock = threading.Lock()

    def play(game: dict) -> None:
        with lock:
            if should_stop and should_stop():
                game["status"] = "cancelled"
                store.save(exp)
                return
            game["status"] = "running"
        fuel = game.get("fuel")
        suffix = f"-f{fuel // 1000}k" if fuel else ""
        out = config.EXPERIMENTS_DIR / exp["id"] / "games" / f"{game['idx']:03d}-{game['map']}-{game['candidateSide']}{suffix}"
        result: dict = {}
        # Parallel runs on Windows occasionally collide on a build file ("Access is denied"): retry.
        for _attempt in range(3):
            try:
                result = run_match(game["botA"], game["botB"], game["map"], out, exp.get("rounds"), fuel=fuel)
            except Exception as err:  # noqa: BLE001 - surface any runner failure in the UI
                result = {"error": str(err)}
            if "error" not in result or "denied" not in str(result["error"]).lower():
                break
        with lock:
            game["result"] = result
            game["status"] = "error" if "error" in result else "done"
            exp["summary"] = aggregate(exp)
            store.save(exp)
        if on_update:
            on_update(exp)

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        list(pool.map(play, [g for g in exp["games"] if g["status"] != "done"]))
    exp["status"] = "cancelled" if should_stop and should_stop() else "done"
    exp["finishedAt"] = time.time()
    exp["summary"] = aggregate(exp)
    store.save(exp)
    return exp
