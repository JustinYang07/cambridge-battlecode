"""Client for the competition platform (https://2026a.battlecode.co.uk).

Public endpoints (no sign-in): /api/v1/ladder, /api/v1/maps/pool, /api/v1/tournaments,
/api/v1/event/status. Match lists and replays need a signed-in session: the web app
uses a session cookie, so the lab stores the Cookie header you copy from your browser
(DevTools -> Network -> any /api/v1 request -> Request Headers -> cookie). A bearer
token can be supplied instead if the platform accepts the CLI's token.

Signed-in endpoints used for scraping (found in the event web app's own code):
  GET /api/v1/match-groups?scope=all[&team=<id>][&before=<createdAt>_<id>]
      ranked match groups across the event, newest first, 25 per page ("more": bool)
  GET /api/v1/match-groups/<groupId>   the group's sides plus each match (statistics, mapId)
  GET /api/v1/matches/<matchId>/bundle  replay bundle ZIP (replay.repl26a only, no stdout)
  GET /api/v1/teams/<id>/performance    rating history
"""
from __future__ import annotations

import json
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

from . import config

SETTINGS_PATH = config.DATA_DIR / "platform.json"
DEFAULT_SETTINGS = {"cookie": "", "token": ""}
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
        return {"hasCookie": bool(self.settings.get("cookie")), "hasToken": bool(self.settings.get("token"))}

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

    def _get(self, path: str) -> dict:
        status, data = self._request(path)
        if status == 401:
            raise PlatformError("not signed in: paste a fresh session cookie on the Platform page")
        if status != 200 or not isinstance(data, dict):
            raise PlatformError(f"{path}: HTTP {status}")
        return data

    def map_names(self) -> dict[str, str]:
        def load():
            status, data = self._request("/api/v1/maps/pool", auth=False)
            return {m["id"]: m["name"] for m in (data or {}).get("maps", [])} if status == 200 else {}
        return self._cached("mappool", 3600, load)

    def match_groups(self, team_id: str | None = None, before: str | None = None) -> dict:
        params = {"scope": "all"}
        if team_id:
            params["team"] = team_id
        if before:
            params["before"] = before
        return self._get("/api/v1/match-groups?" + urllib.parse.urlencode(params))

    def scrape(self, team_ids: list[str], max_groups: int = 20, log=print, pause: float = 0.15) -> list[dict]:
        """Download every finished game in the newest `max_groups` match groups per team
        (or across the whole event when no team is given) into lab/data/scraped/."""
        saved: list[dict] = []
        maps = self.map_names()
        for team_id in team_ids or [None]:
            label = team_id or "all teams"
            seen, before, pages = 0, None, 0
            # Only finished groups count; queued/running ones sit at the top of the list.
            while seen < max_groups and pages < max_groups + 10:
                pages += 1
                page = self.match_groups(team_id, before)
                groups = page.get("groups", [])
                for group in groups:
                    if seen >= max_groups:
                        break
                    if group.get("status") != "finished":
                        continue
                    seen += 1
                    detail = self._get(f"/api/v1/match-groups/{group['id']}")
                    for match in detail.get("matches", []):
                        if match.get("status") != "finished":
                            continue
                        try:
                            saved.append(self.download_match(match, detail.get("group", group), maps))
                        except PlatformError as err:
                            log(f"skip match {match.get('id')}: {err}")
                        time.sleep(pause)
                    names = " vs ".join(sd.get("teamName", "?") for sd in group.get("sides", []))
                    log(f"[{label}] group {seen}/{max_groups}: {names}")
                if not page.get("more") or not groups:
                    break
                before = f"{groups[-1]['createdAt']}_{groups[-1]['id']}"
        return saved

    def download_match(self, match: dict, group: dict, maps: dict[str, str]) -> dict:
        from .runner import summarise_stats  # local import: runner imports config only

        match_id = match["id"]
        dest = config.SCRAPED_DIR / f"{match_id}.repl26a"
        if dest.exists():
            return {"file": str(dest.relative_to(config.ROOT)), "cached": True}
        status, _, body = self._request(f"/api/v1/matches/{urllib.parse.quote(match_id)}/bundle", raw=True, timeout=60)
        if status != 200 or not body.startswith(b"PK"):
            raise PlatformError(f"bundle HTTP {status}")
        config.SCRAPED_DIR.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(body)
        sides = {sd["side"]: sd for sd in group.get("sides", [])}
        stats = match.get("statistics") or {}
        summary = summarise_stats(stats) if stats else {}
        meta = {
            "matchId": match_id, "groupId": group.get("id"), "kind": match.get("kind"),
            "createdAt": group.get("createdAt"), "map": maps.get(match.get("mapId"), match.get("mapId")),
            "teamA": {k: sides.get("a", {}).get(k) for k in ("teamId", "teamName", "ratingBefore", "ratingAfter")},
            "teamB": {k: sides.get("b", {}).get(k) for k in ("teamId", "teamName", "ratingBefore", "ratingAfter")},
            "winner": summary.get("winner"), "rounds": summary.get("rounds"), "result": summary,
        }
        dest.with_suffix(".json").write_text(json.dumps(meta, indent=1))
        return {"file": str(dest.relative_to(config.ROOT)), "cached": False}


def scouting() -> list[dict]:
    """Per-team summary of every scraped game: record, gems, rounds, favourite drafts and maps."""
    teams: dict[str, dict] = {}
    for side_file in config.SCRAPED_DIR.glob("*.json"):
        try:
            meta = json.loads(side_file.read_text())
        except json.JSONDecodeError:
            continue
        result = meta.get("result") or {}
        for idx, key in enumerate(("teamA", "teamB")):
            info = meta.get(key) or {}
            if not info.get("teamId"):
                continue
            t = teams.setdefault(info["teamId"], {"teamId": info["teamId"], "name": info.get("teamName"), "games": 0,
                                                  "wins": 0, "gems": 0, "oppGems": 0, "rounds": 0,
                                                  "picks": {}, "maps": {}, "rating": None, "lastSeen": ""})
            t["games"] += 1
            t["wins"] += meta.get("winner") == ("A" if idx == 0 else "B")
            if result.get("teams"):
                t["gems"] += result["teams"][idx]["gems"]
                t["oppGems"] += result["teams"][1 - idx]["gems"]
                for pick in result["teams"][idx].get("draft", []):
                    t["picks"][pick] = t["picks"].get(pick, 0) + 1
            t["rounds"] += meta.get("rounds") or 0
            if meta.get("map"):
                t["maps"][meta["map"]] = t["maps"].get(meta["map"], 0) + 1
            if (meta.get("createdAt") or "") >= t["lastSeen"]:
                t["lastSeen"] = meta.get("createdAt") or ""
                t["rating"] = info.get("ratingAfter")
    out = sorted(teams.values(), key=lambda t: -(t["rating"] or 0))
    for t in out:
        t["picks"] = sorted(t["picks"].items(), key=lambda kv: -kv[1])
    return out
