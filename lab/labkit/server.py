"""CAMBC Lab web server: JSON API + static frontend + the official visualiser.

Stdlib only (http.server). Run with `python3 lab/server.py`; see lab/README.md.
When bound to a non-loopback host, every API call needs the access key printed at
start-up (teammates open the printed URL once; the page remembers the key).
"""
from __future__ import annotations

import json
import mimetypes
import secrets
import subprocess
import threading
import time
import traceback
import urllib.parse
from collections import OrderedDict
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from . import config, jobs, maps, platform, replays, runner

WEB_DIR = config.LAB_DIR / "web"
mimetypes.add_type("application/wasm", ".wasm")
mimetypes.add_type("text/javascript", ".js")
mimetypes.add_type("text/css", ".css")


class Lab:
    """Shared state behind the HTTP handler."""

    def __init__(self, access_key: str | None) -> None:
        config.ensure_dirs()
        self.access_key = access_key
        self.store = runner.ExperimentStore()
        self.jobs = jobs.JobQueue(self.store)
        self.platform = platform.Platform()
        self._decoded: "OrderedDict[tuple[str, float], dict]" = OrderedDict()
        self._lock = threading.Lock()
        self.scrape_log: list[str] = []
        threading.Thread(target=self._warm_maps, daemon=True).start()

    def _warm_maps(self) -> None:
        for name in config.list_maps():
            try:
                maps.map_info(name)
            except Exception:  # noqa: BLE001 - a broken map should not stop the server
                pass

    # ------------------------------------------------------------------ replays
    def replay_index(self) -> list[dict]:
        out: list[dict] = []
        for exp in self.store.list():
            try:
                full = self.store.load(exp["id"])
            except (OSError, json.JSONDecodeError):
                continue
            for g in full.get("games", []):
                r = g.get("result") or {}
                if not r.get("replay"):
                    continue
                teams = r.get("teams", [{}, {}])
                out.append({"path": r["replay"], "source": "experiment", "experiment": exp["id"],
                            "experimentName": exp.get("name"), "map": g["map"], "botA": g["botA"], "botB": g["botB"],
                            "winner": r.get("winner"), "rounds": r.get("rounds"),
                            "gems": [t.get("gems") for t in teams] if teams else None,
                            "time": exp.get("createdAt")})
        for root, source in ((config.SCRAPED_DIR, "scraped"), (config.ROOT / "replays", "local")):
            for p in replays.find_replays(root):
                meta = {}
                side = p.with_suffix(".json")
                if side.exists():
                    try:
                        meta = json.loads(side.read_text())
                    except json.JSONDecodeError:
                        meta = {}
                result = meta.get("result") or {}
                out.append({"path": str(p.relative_to(config.ROOT)), "source": source, "map": meta.get("map"),
                            "botA": (meta.get("teamA") or {}).get("teamName"), "botB": (meta.get("teamB") or {}).get("teamName"),
                            "winner": meta.get("winner"), "rounds": meta.get("rounds"),
                            "gems": [t.get("gems") for t in result.get("teams", [])] or None,
                            "title": p.stem, "time": p.stat().st_mtime})
        out.sort(key=lambda r: r.get("time") or 0, reverse=True)
        return out

    def decode(self, rel: str) -> dict:
        path = safe_path(rel)
        key = (str(path), path.stat().st_mtime)
        with self._lock:
            if key in self._decoded:
                self._decoded.move_to_end(key)
                return self._decoded[key]
        data = replays.read_bundle(path)
        data["path"] = rel
        data["game"] = self._game_for(rel)
        side = path.with_suffix(".json")
        if data["game"] is None and side.exists():
            try:
                meta = json.loads(side.read_text())
                data["game"] = {"map": meta.get("map"), "botA": (meta.get("teamA") or {}).get("teamName"),
                                "botB": (meta.get("teamB") or {}).get("teamName"), "result": meta.get("result"),
                                "scraped": meta}
            except json.JSONDecodeError:
                pass
        with self._lock:
            self._decoded[key] = data
            while len(self._decoded) > 12:
                self._decoded.popitem(last=False)
        return data

    def _game_for(self, rel: str) -> dict | None:
        parts = Path(rel).parts
        if "experiments" in parts:
            exp_id = parts[parts.index("experiments") + 1]
            try:
                exp = self.store.load(exp_id)
            except (OSError, json.JSONDecodeError):
                return None
            for g in exp.get("games", []):
                if (g.get("result") or {}).get("replay") == rel:
                    return {**g, "experiment": exp_id, "experimentName": exp.get("name")}
        return None


def safe_path(rel: str) -> Path:
    """Resolve a repo-relative path, refusing anything outside the data/replay folders."""
    path = (config.ROOT / rel).resolve()
    allowed = [config.DATA_DIR.resolve(), (config.ROOT / "replays").resolve()]
    if not any(path.is_relative_to(a) for a in allowed) or not path.is_file():
        raise FileNotFoundError(rel)
    return path


def bot_list() -> list[dict]:
    out = []
    dirty = subprocess.run(["git", "status", "--porcelain", "bots"], cwd=config.ROOT, capture_output=True,
                           text=True).stdout
    for name in config.list_bots():
        src = config.BOTS_DIR / name / "src"
        files = [p for p in src.rglob("*") if p.is_file()]
        kind = ("snapshot" if name.startswith(config.SNAPSHOT_PREFIX) else "tool" if name.startswith("tool-")
                else "starter" if name.startswith("starter-") else "bot")
        out.append({"name": name, "kind": kind, "files": len(files),
                    "modified": max((p.stat().st_mtime for p in files), default=0),
                    "dirty": f"bots/{name}/" in dirty})
    return out


class Handler(BaseHTTPRequestHandler):
    lab: Lab
    server_version = "CambcLab/1.0"

    def log_message(self, fmt, *args):  # quieter console: only errors
        if args and str(args[1]).startswith(("4", "5")) and not str(args[1]).startswith("404"):
            super().log_message(fmt, *args)

    # ------------------------------------------------------------------ plumbing
    def _send(self, status: int, body: bytes, ctype: str, extra: dict | None = None) -> None:
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _json(self, data, status: int = 200) -> None:
        self._send(status, json.dumps(data, default=str).encode(), "application/json")

    def _error(self, status: int, message: str) -> None:
        self._json({"error": message}, status)

    def _body(self) -> dict:
        length = int(self.headers.get("Content-Length") or 0)
        return json.loads(self.rfile.read(length) or b"{}") if length else {}

    def _authorised(self) -> bool:
        key = self.lab.access_key
        if not key:
            return True
        supplied = self.headers.get("X-Lab-Key") or ""
        cookie = self.headers.get("Cookie") or ""
        return secrets.compare_digest(supplied, key) or f"lab_key={key}" in cookie

    def _static(self, root: Path, rel: str) -> None:
        path = (root / rel).resolve()
        if not path.is_relative_to(root.resolve()) or not path.is_file():
            path = root / "index.html" if root == WEB_DIR and "." not in Path(rel).name else None
            if path is None or not path.is_file():
                return self._error(404, "not found")
        ctype = mimetypes.guess_type(path.name)[0] or "application/octet-stream"
        self._send(200, path.read_bytes(), ctype)

    # ------------------------------------------------------------------ routes
    def do_GET(self):  # noqa: N802 - http.server API
        url = urllib.parse.urlparse(self.path)
        q = {k: v[0] for k, v in urllib.parse.parse_qs(url.query).items()}
        try:
            if url.path.startswith("/api/"):
                if not self._authorised():
                    return self._error(401, "access key required")
                return self._api_get(url.path, q)
            if url.path.startswith("/files/"):
                if not self._authorised():
                    return self._error(401, "access key required")
                path = safe_path(urllib.parse.unquote(url.path[len("/files/"):]))
                return self._send(200, path.read_bytes(), "application/zip")
            if url.path.startswith("/visualiser/"):
                vis = config.visualiser_dir()
                if not vis:
                    return self._error(404, "official visualiser not found: run `cambc view` once")
                return self._static(vis, url.path[len("/visualiser/"):] or "index.html")
            return self._static(WEB_DIR, url.path.lstrip("/") or "index.html")
        except FileNotFoundError as err:
            self._error(404, f"not found: {err}")
        except Exception as err:  # noqa: BLE001 - report to the UI instead of dropping the connection
            traceback.print_exc()
            self._error(500, str(err))

    def do_POST(self):  # noqa: N802
        url = urllib.parse.urlparse(self.path)
        try:
            if not self._authorised():
                return self._error(401, "access key required")
            return self._api_post(url.path, self._body())
        except (platform.PlatformError, runner.BuildError, ValueError, subprocess.CalledProcessError) as err:
            self._error(400, str(err))
        except Exception as err:  # noqa: BLE001
            traceback.print_exc()
            self._error(500, str(err))

    def do_DELETE(self):  # noqa: N802
        url = urllib.parse.urlparse(self.path)
        if not self._authorised():
            return self._error(401, "access key required")
        if url.path.startswith("/api/experiments/"):
            self.lab.store.delete(url.path.rsplit("/", 1)[1])
            return self._json({"ok": True})
        self._error(404, "not found")

    def _api_get(self, path: str, q: dict) -> None:
        lab = self.lab
        if path == "/api/status":
            return self._json({"bots": bot_list(), "maps": config.list_maps(), "jobs": lab.jobs.status(),
                               "visualiser": bool(config.visualiser_dir()), "platform": lab.platform.public_settings(),
                               "eventUrl": config.PLATFORM_URL, "cpu": __import__("os").cpu_count()})
        if path == "/api/maps":
            return self._json([maps.summary(maps.map_info(m)) for m in config.list_maps()])
        if path.startswith("/api/maps/"):
            return self._json(maps.map_info(path.rsplit("/", 1)[1]))
        if path == "/api/experiments":
            return self._json(lab.store.list())
        if path.startswith("/api/experiments/"):
            exp = lab.store.load(path.rsplit("/", 1)[1])
            exp["summary"] = runner.aggregate(exp)
            return self._json(exp)
        if path == "/api/jobs":
            return self._json(lab.jobs.status())
        if path == "/api/replays":
            return self._json(lab.replay_index())
        if path == "/api/replay":
            return self._json(lab.decode(q["path"]))
        if path == "/api/platform/ladder":
            return self._json(lab.platform.ladder())
        if path == "/api/platform/me":
            return self._json(lab.platform.whoami())
        if path == "/api/platform/probe":
            return self._json(lab.platform.probe(q.get("path", "")))
        if path == "/api/platform/scrape-log":
            return self._json(lab.scrape_log[-200:])
        if path == "/api/platform/scouting":
            return self._json(platform.scouting())
        self._error(404, "unknown endpoint")

    def _api_post(self, path: str, body: dict) -> None:
        lab = self.lab
        if path == "/api/experiments":
            maps_ = body.get("maps") or config.list_maps()
            unknown = set(maps_) - set(config.list_maps())
            if unknown:
                raise ValueError(f"unknown maps: {', '.join(sorted(unknown))}")
            bots = set(config.list_bots())
            for b in [body.get("candidate"), *body.get("opponents", [])]:
                if b not in bots:
                    raise ValueError(f"unknown bot {b!r}")
            if not body.get("opponents"):
                raise ValueError("pick at least one opponent")
            spec = runner.ExperimentSpec(body["candidate"], body["opponents"], maps_, bool(body.get("bothSides", True)),
                                         body.get("name", ""), body.get("notes", ""), body.get("rounds") or None)
            return self._json(lab.jobs.submit(spec))
        if path.startswith("/api/experiments/") and path.endswith("/cancel"):
            lab.jobs.cancel(path.split("/")[3])
            return self._json({"ok": True})
        if path == "/api/bots/snapshot":
            name = runner.snapshot(body["bot"], body.get("label") or None, body.get("gitRef") or None)
            return self._json({"name": name})
        if path == "/api/platform/settings":
            return self._json(lab.platform.save_settings(cookie=body.get("cookie"), token=body.get("token")))
        if path == "/api/platform/scrape":
            team_ids = body.get("teamIds") or []
            limit = max(1, min(int(body.get("limit") or 20), 500))

            def work():
                log = lambda line: lab.scrape_log.append(time.strftime("%H:%M:%S ") + line)  # noqa: E731
                try:
                    saved = lab.platform.scrape(team_ids, limit, log)
                    log(f"done: {len(saved)} replays ({sum(not s['cached'] for s in saved)} new)")
                except Exception as err:  # noqa: BLE001
                    log(f"error: {err}")
            threading.Thread(target=work, daemon=True).start()
            return self._json({"started": True})
        self._error(404, "unknown endpoint")


def serve(host: str = "127.0.0.1", port: int = 8765, access_key: str | None = None) -> None:
    lab = Lab(access_key)
    Handler.lab = lab
    httpd = ThreadingHTTPServer((host, port), Handler)
    shown = "localhost" if host in ("127.0.0.1", "0.0.0.0", "::") else host
    suffix = f"#key={access_key}" if access_key else ""
    print(f"CAMBC Lab running at http://{shown}:{port}/{suffix}")
    if access_key:
        print("Share that URL (with the key) with teammates on your network.")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
