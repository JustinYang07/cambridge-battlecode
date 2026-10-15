"""Filesystem locations shared by the CLI tools and the web server."""
from __future__ import annotations

import os
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
    candidates = sorted((cache / "assets").glob("*/visualiser/index.html"), key=lambda p: p.stat().st_mtime)
    return candidates[-1].parent if candidates else None


def list_bots() -> list[str]:
    return sorted(p.name for p in BOTS_DIR.iterdir() if (p / "src").is_dir()) if BOTS_DIR.exists() else []


def list_maps() -> list[str]:
    return sorted(p.stem for p in MAPS_DIR.glob("*.map26a"))


def map_path(name: str) -> Path:
    path = MAPS_DIR / f"{name}.map26a"
    if not path.exists():
        raise FileNotFoundError(f"unknown map {name!r}")
    return path


def ensure_dirs() -> None:
    for d in (EXPERIMENTS_DIR, SCRAPED_DIR):
        d.mkdir(parents=True, exist_ok=True)
