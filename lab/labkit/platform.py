"""Client for the competition platform (https://2026a.battlecode.co.uk).

Public endpoints (no sign-in): /api/v1/ladder, /api/v1/maps/pool, /api/v1/tournaments,
/api/v1/event/status. Match lists and replays need a signed-in session: the web app
uses a session cookie, so the lab stores the Cookie header you copy from your browser
(DevTools -> Network -> any /api/v1 request -> Request Headers -> cookie). A bearer
token can be supplied instead if the platform accepts the CLI's token.

Match-list endpoints are not documented publicly, so paths are configurable and
replay links are discovered generically: any JSON string that looks like a replay
URL (".repl26a", or a key mentioning "replay") is treated as downloadable.
"""
from __future__ import annotations

import json
import re
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

from . import config

SETTINGS_PATH = config.DATA_DIR / "platform.json"
DEFAULT_SETTINGS = {
    "cookie": "",
    "token": "",
    # {teamId} is substituted; edit in the lab's Platform page once the real path is known.
    "matchesPaths": ["/api/v1/matches?teamId={teamId}", "/api/v1/matches?team={teamId}", "/api/v1/matches"],
}
USER_AGENT = "cambc-lab/1.0 (+team tooling)"


class PlatformError(RuntimeError):
    pass


class Platform:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._cache: dict[str, tuple[float, object]] = {}
        self.settings = dict(DEFAULT_SETTINGS)
        if SETTINGS_PATH.exists():
            try:
                self.settings.update(json.loads(SETTINGS_PATH.read_text()))
            except json.JSONDecodeError:
                pass

    # ------------------------------------------------------------------ settings
    def save_settings(self, **changes) -> dict:
        with self._lock:
            self.settings.update({k: v for k, v in changes.items() if v is not None})
            SETTINGS_PATH.parent.mkdir(parents=True, exist_ok=True)
            SETTINGS_PATH.write_text(json.dumps(self.settings, indent=1))
            SETTINGS_PATH.chmod(0o600)
        self._cache.clear()
        return self.public_settings()

    def public_settings(self) -> dict:
        return {"hasCookie": bool(self.settings.get("cookie")), "hasToken": bool(self.settings.get("token")),
                "matchesPaths": self.settings.get("matchesPaths", [])}

    # ------------------------------------------------------------------ http
    def _request(self, path: str, auth: bool = True, raw: bool = False, timeout: float = 20):
        url = path if path.startswith("http") else config.PLATFORM_URL + path
        if not url.startswith(config.PLATFORM_URL):
            raise PlatformError("refusing to send credentials outside the event origin")
        headers = {"User-Agent": USER_AGENT, "Accept": "application/json, */*"}
        if auth and self.settings.get("cookie"):
            headers["Cookie"] = self.settings["cookie"]
        if auth and self.settings.get("token"):
            headers["Authorization"] = f"Bearer {self.settings['token']}"
        req = urllib.request.Request(url, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                body = resp.read()
                ctype = resp.headers.get("Content-Type", "")
                status = resp.status
        except urllib.error.HTTPError as err:
            body, ctype, status = err.read(), err.headers.get("Content-Type", ""), err.code
        except urllib.error.URLError as err:
            raise PlatformError(f"cannot reach {url}: {err.reason}") from err
        if raw:
            return status, ctype, body
        try:
            data = json.loads(body.decode("utf-8")) if body else None
        except (UnicodeDecodeError, json.JSONDecodeError):
            data = {"_raw": body[:2000].decode("utf-8", errors="replace")}
        return status, data

    def _cached(self, key: str, ttl: float, fn):
        hit = self._cache.get(key)
        if hit and time.time() - hit[0] < ttl:
            return hit[1]
        value = fn()
        self._cache[key] = (time.time(), value)
        return value

    # ------------------------------------------------------------------ public data
    def ladder(self) -> dict:
        def load():
            status, data = self._request("/api/v1/ladder", auth=False)
            if status != 200:
                raise PlatformError(f"ladder: HTTP {status}")
            return data
        return self._cached("ladder", 60, load)

    def event_status(self) -> dict:
        return self._cached("event", 300, lambda: self._request("/api/v1/event/status", auth=False)[1])

    # ------------------------------------------------------------------ signed-in data
    def whoami(self) -> dict:
        status, data = self._request("/api/v1/registration/me")
        return {"status": status, **(data if isinstance(data, dict) else {})}

    def probe(self, path: str) -> dict:
        if not path.startswith("/api/"):
            raise PlatformError("probe paths must start with /api/")
        status, data = self._request(path)
        return {"status": status, "data": data}

    def find_matches(self, team_id: str) -> tuple[str, object]:
        """Try the configured match-list paths until one answers with JSON."""
        errors = []
        for template in self.settings.get("matchesPaths", []):
            path = template.replace("{teamId}", urllib.parse.quote(team_id))
            status, data = self._request(path)
            if status == 200 and data is not None:
                return path, data
            errors.append(f"{path}: HTTP {status}")
        raise PlatformError("no match endpoint answered: " + "; ".join(errors))

    def scrape(self, team_ids: list[str], limit: int = 20, log=print) -> list[dict]:
        """Download replays referenced by each team's match list into lab/data/scraped/."""
        saved = []
        for team_id in team_ids:
            path, data = self.find_matches(team_id)
            log(f"matches for {team_id} from {path}")
            for ref in list(_replay_refs(data))[:limit]:
                try:
                    saved.append(self.download(ref))
                except PlatformError as err:
                    log(f"skip {ref['url']}: {err}")
        return saved

    def download(self, ref: dict) -> dict:
        url = ref["url"]
        name = re.sub(r"[^A-Za-z0-9_.-]", "_", ref.get("id") or Path(urllib.parse.urlparse(url).path).name)
        if not name.endswith(".repl26a"):
            name += ".repl26a"
        dest = config.SCRAPED_DIR / name
        if dest.exists():
            return {"file": str(dest.relative_to(config.ROOT)), "cached": True, **ref}
        # Signed storage URLs on other hosts must not receive our cookie.
        same_origin = url.startswith("/") or url.startswith(config.PLATFORM_URL)
        if same_origin:
            status, _, body = self._request(url, raw=True, timeout=60)
        else:
            req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
            try:
                with urllib.request.urlopen(req, timeout=60) as resp:
                    status, body = resp.status, resp.read()
            except urllib.error.HTTPError as err:
                status, body = err.code, b""
        if status != 200 or not body.startswith(b"PK"):
            raise PlatformError(f"HTTP {status}, not a replay bundle")
        config.SCRAPED_DIR.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(body)
        meta = {k: v for k, v in ref.items() if k != "context"}
        meta["context"] = ref.get("context")
        dest.with_suffix(".json").write_text(json.dumps(meta, indent=1, default=str))
        return {"file": str(dest.relative_to(config.ROOT)), "cached": False, **ref}


def _replay_refs(data, context=None):
    """Yield {"url", "id", "context"} for every replay-looking link inside arbitrary JSON."""
    if isinstance(data, dict):
        ident = data.get("id") or data.get("matchId")
        for key, value in data.items():
            if isinstance(value, str) and (value.endswith(".repl26a") or "replay" in key.lower()) and (
                    value.startswith("/") or value.startswith("http")):
                yield {"url": value, "id": str(ident) if ident else None, "context": data}
            else:
                yield from _replay_refs(value, data)
    elif isinstance(data, list):
        for item in data:
            yield from _replay_refs(item, context)
