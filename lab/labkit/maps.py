"""Map metadata (terrain grids, camps, altars, spawns) extracted with bots/tool-mapdump.

The .map26a format is not documented, so instead of parsing it we run a short
match with a helper bot that prints the map through the SDK, then cache the JSON
by the map file's hash.
"""
from __future__ import annotations

import hashlib
import json
import subprocess
import tempfile
import zipfile
from pathlib import Path

from . import config

TOOL_BOT = "tool-mapdump"
CACHE_DIR = config.DATA_DIR / "maps"


def _digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()[:16]


def map_info(name: str, refresh: bool = False) -> dict:
    path = config.map_path(name)
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    cached = CACHE_DIR / f"{name}-{_digest(path)}.json"
    if cached.exists() and not refresh:
        return json.loads(cached.read_text())

    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run([config.cambc_binary(), "run", TOOL_BOT, TOOL_BOT, "--map", str(path),
                        "--rounds", "40", "-o", tmp], cwd=config.ROOT, capture_output=True, timeout=300)
        bundles = list(Path(tmp).glob("*.repl26a"))
        if not bundles:
            raise RuntimeError(f"mapdump produced no replay for {name}")
        with zipfile.ZipFile(bundles[0]) as bundle:
            text = bundle.read("team-a.stdout").decode("utf-8", errors="replace")
    line = next((l for l in text.replace("\0", "\n").splitlines() if l.startswith("@MAP")), None)
    if not line:
        raise RuntimeError(f"mapdump printed nothing for {name}")
    info = json.loads(line[4:])
    info["name"] = name
    cached.write_text(json.dumps(info))
    return info


def summary(info: dict) -> dict:
    """Compact facts for map cards: counts of each camp type, gems on the map, sizes."""
    counts: dict[str, int] = {}
    for group in info["groups"]:
        for camp in group["camps"]:
            counts[camp["type"]] = counts.get(camp["type"], 0) + 1
    return {"name": info["name"], "w": info["w"], "h": info["h"], "teamSize": info["teamSize"],
            "symmetry": info["symmetry"], "altars": len(info["altars"]), "groups": len(info["groups"]),
            "camps": counts, "gemsPerCycle": sum(g["gems"] for g in info["groups"])}
