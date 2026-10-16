"""Filesystem locations shared by the CLI tools and the web server."""
from __future__ import annotations

import os
import re
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BOTS_DIR = ROOT / "bots"
MAPS_DIR = ROOT / "maps"
LAB_DIR = ROOT / "lab"
DATA_DIR = Path(os.environ.get("LAB_DATA", LAB_DIR / "data"))
EXPERIMENTS_DIR = DATA_DIR / "experiments"
SCRAPED_DIR = DATA_DIR / "scraped"
SNAPSHOT_PREFIX = "snap-"

PLATFORM_URL = os.environ.get("CAMBC_EVENT_URL", "https://2026a.battlecode.co.uk")


def cambc_binary() -> str:
    """Prefer the project venv's CLI, then whatever is on PATH."""
    for local in (ROOT / ".venv" / "bin" / "cambc", ROOT / ".venv" / "Scripts" / "cambc.exe"):
        if local.exists():
            return str(local)
    found = shutil.which("cambc")
    if not found:
        raise RuntimeError("cambc CLI not found: run `python3 -m venv .venv && .venv/bin/pip install cambc`")
    return found


def visualiser_dir() -> Path | None:
    """The official visualiser is unpacked by the CLI into its asset cache."""
    override = os.environ.get("CAMBC_VISUALISER")
    if override:
        return Path(override)
    cache = Path(os.environ.get("CAMBC_CACHE", Path.home() / "Library" / "Caches" / "cambc"))
    for fallback in (Path.home() / ".cache" / "cambc", Path(os.environ.get("LOCALAPPDATA", "")) / "cambc"):
        if cache.exists():
            break
        cache = fallback
    # Each cambc version unpacks its own copy. Their files keep packaged timestamps (all 1970), so
    # pick the one with the newest engine (assets/v<major>_<minor>_<patch>-*.wasm): it plays old and
    # new replays, while an older copy cannot load replays from a newer engine. Ties: the copy
    # unpacked last (its folder's own time).
    def engine(index: Path) -> tuple:
        versions = [tuple(int(x) for x in re.findall(r"\d+", w.name.split("-")[0]))
                    for w in (index.parent / "assets").glob("v*_*_*-*.wasm")]
        return (max(versions, default=()), index.parent.parent.stat().st_mtime)
    candidates = sorted((cache / "assets").glob("*/visualiser/index.html"), key=engine)
    return candidates[-1].parent if candidates else None


def list_bots() -> list[str]:
    return sorted(p.name for p in BOTS_DIR.iterdir() if (p / "src").is_dir()) if BOTS_DIR.exists() else []


# `cambc maps pull` saves a reworked version of a map as <name>-<uuid>.map26a. Experiments use
# the plain name; map_path finds the file behind it.
_VERSION_SUFFIX = re.compile(r"-[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")


def _map_files() -> dict[str, Path]:
    files: dict[str, Path] = {}
    for p in sorted(MAPS_DIR.glob("*.map26a"), key=lambda p: p.stat().st_mtime):
        files[_VERSION_SUFFIX.sub("", p.stem)] = p  # newest file wins if both versions exist
    return files


def list_maps() -> list[str]:
    return sorted(_map_files())


def map_path(name: str) -> Path:
    path = MAPS_DIR / f"{name}.map26a"
    if path.exists():
        return path
    path = _map_files().get(name)
    if path is None:
        raise FileNotFoundError(f"unknown map {name!r}")
    return path


def ensure_dirs() -> None:
    for d in (EXPERIMENTS_DIR, SCRAPED_DIR):
        d.mkdir(parents=True, exist_ok=True)
