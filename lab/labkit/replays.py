"""Read replay bundles (.repl26a ZIPs) and decode the decision traces our bots print.

A bundle holds the binary replay plus each team's stdout. Stdout is split into
turn "positions" by NUL bytes, the same framing the official visualiser uses.

Writing stdout costs the bot ~33 fuel per byte, so basicBot uses a compact,
self-describing encoding (see bots/basicBot/src/core/trace.h):
  @K[u,id,"name","op"]   dictionary entry for slot u (kinds have op "k")
  @G[id,"label",x,y]     camp-group legend; targets "g<id>"
  @A[[x,y],...]          altar legend; targets "a<index>"
  @U[u,"champion"]       which champion plays slot u
  @P[regions,[region per group],[region per slot]]   team plan
  @t[r,u,x,y,hp,gems,gold,kind,score,fuel,"target","actions",lost]          every turn
  @T[r,u,hp,maxHp,gems,gold,fuel,lost,[[kind,"target",score,x,y,"note",[[factor,value],...]],...],"actions","note"]
  @D{...}                draft scores, once per pick
This module decodes everything back into readable dicts for the web lab and CLI.
"""
from __future__ import annotations

import json
import re
import zipfile
from dataclasses import dataclass, field
from pathlib import Path

ACTION_CODES = [
    (re.compile(r"^m(-?\d+),(-?\d+)$"), "move to ({0},{1})"),
    (re.compile(r"^d(-?\d+),(-?\d+)$"), "dash to ({0},{1})"),
    (re.compile(r"^ae(-?\d+),(-?\d+)$"), "attack enemy at ({0},{1})"),
    (re.compile(r"^ac(-?\d+),(-?\d+)$"), "attack camp at ({0},{1})"),
    (re.compile(r"^aoe(-?\d+),(-?\d+)$"), "area spell at ({0},{1})"),
    (re.compile(r"^st(-?\d+),(-?\d+)$"), "stun enemy at ({0},{1})"),
    (re.compile(r"^se(-?\d+),(-?\d+)$"), "spell enemy at ({0},{1})"),
    (re.compile(r"^sc(-?\d+),(-?\d+)$"), "spell camp at ({0},{1})"),
    (re.compile(r"^h(-?\d+),(-?\d+)$"), "heal ally at ({0},{1})"),
    (re.compile(r"^t(\d+)$"), "tribute {0} gems"),
    (re.compile(r"^b(\d+)(\w+)$"), "buy {0} {1} shard(s)"),
]
SIMPLE_ACTIONS = {"r": "attack reset", "sh": "shield", "hold": "hold for party", "cap": "hold capture",
                  "rest": "rest on spawn", "rc": "recall", "ch:rc": "channelling recall",
                  "ch:t": "channelling tribute", "ch:tp": "channelling teleport"}


def describe_action(code: str) -> str:
    if code in SIMPLE_ACTIONS:
        return SIMPLE_ACTIONS[code]
    for pattern, text in ACTION_CODES:
        m = pattern.match(code)
        if m:
            return text.format(*m.groups())
    return code


def describe_actions(actions: str) -> str:
    return "; ".join(describe_action(a) for a in actions.split(";") if a) if actions else ""


@dataclass
class TeamLog:
    turns: list[dict] = field(default_factory=list)    # compact, one per champion turn
    details: list[dict] = field(default_factory=list)  # full option breakdowns
    drafts: list[dict] = field(default_factory=list)
    other: list[dict] = field(default_factory=list)    # {"pos": n, "text": "..."}
    groups: dict[int, dict] = field(default_factory=dict)
    altars: list[list[int]] = field(default_factory=list)
    units: dict[int, str] = field(default_factory=dict)
    plan: dict | None = None
    stdout_bytes: int = 0


class _Decoder:
    def __init__(self, log: TeamLog) -> None:
        self.log = log
        self.dicts: dict[int, dict[int, tuple[str, str]]] = {}

    def name(self, slot: int, ident: int) -> tuple[str, str]:
        return self.dicts.get(slot, {}).get(ident, (f"#{ident}", "+"))

    def label(self, code: str) -> str:
        if code.startswith("g") and code[1:].isdigit():
            g = self.log.groups.get(int(code[1:]))
            return g["label"] if g else code
        if code.startswith("a") and code[1:].isdigit():
            i = int(code[1:])
            if i < len(self.log.altars):
                x, y = self.log.altars[i]
                return f"altar({x},{y})"
        if code.startswith("e("):
            return "enemy" + code[1:]
        if code.startswith("e") and code[1:].isdigit():
            return f"enemy #{code[1:]}"
        return code

    def line(self, tag: str, value, pos: int) -> None:
        log = self.log
        if tag == "@K":
            slot, ident, name, op = value
            self.dicts.setdefault(slot, {})[ident] = (name, op)
        elif tag == "@G":
            ident, label, x, y = value
            log.groups[ident] = {"label": label, "x": x, "y": y}
        elif tag == "@A":
            log.altars = value
        elif tag == "@U":
            log.units[value[0]] = value[1]
        elif tag == "@P" and isinstance(value, list) and len(value) >= 3:
            log.plan = {"regions": value[0], "groupRegion": value[1], "slotRegion": value[2]}
        elif tag == "@t" and isinstance(value, list) and len(value) >= 12:
            r, u, x, y, hp, gm, au, kind, score, fuel, target, act = value[:12]
            log.turns.append({"r": r, "u": u, "x": x, "y": y, "hp": hp, "gm": gm, "au": au,
                              "k": self.name(u, kind)[0] if isinstance(kind, int) else kind,
                              "s": score, "fuel": fuel, "t": target, "label": self.label(target), "act": act,
                              "lost": value[12] if len(value) > 12 else 0, "pos": pos})
        elif tag == "@T" and isinstance(value, list) and len(value) >= 11:
            r, u, hp, mhp, gm, au, fuel, lost, opts, act, note = value[:11]
            decoded = []
            for o in opts:
                kind, target, score, x, y, onote, factors = o
                decoded.append({
                    "k": self.name(u, kind)[0], "t": target, "label": self.label(target), "s": score,
                    "at": [x, y] if x >= 0 else None, "n": onote,
                    "f": [[*self.name(u, fid)[:1], val, self.name(u, fid)[1]] for fid, val in factors],
                })
            log.details.append({"r": r, "u": u, "hp": hp, "mhp": mhp, "gm": gm, "au": au, "fuel": fuel,
                                "lost": lost, "opts": decoded, "act": act, "note": note, "pos": pos})
        elif tag == "@D" and isinstance(value, dict):
            value["pos"] = pos
            log.drafts.append(value)


def _parse_stdout(raw: bytes, limit_other: int = 4000) -> TeamLog:
    log = TeamLog(stdout_bytes=len(raw))
    decoder = _Decoder(log)
    text = raw.decode("utf-8", errors="replace")
    for pos, segment in enumerate(text.split("\0")):
        if not segment:
            continue
        for line in segment.splitlines():
            tag = line[:2]
            if tag in ("@t", "@T", "@D", "@K", "@G", "@A", "@U", "@P"):
                try:
                    value = json.loads(line[2:])
                except json.JSONDecodeError:
                    continue
                decoder.line(tag, value, pos)
            elif line.strip() and len(log.other) < limit_other:
                log.other.append({"pos": pos, "text": line})
    return log


def read_bundle(path: Path) -> dict:
    """Return both teams' decoded logs; missing stdout files yield empty logs."""
    out: dict = {"file": path.name, "teams": {}}
    with zipfile.ZipFile(path) as bundle:
        names = set(bundle.namelist())
        for side in ("a", "b"):
            name = f"team-{side}.stdout"
            log = _parse_stdout(bundle.read(name)) if name in names else TeamLog()
            out["teams"][side] = {"turns": log.turns, "details": log.details, "drafts": log.drafts,
                                  "other": log.other, "groups": log.groups, "altars": log.altars, "units": log.units, "plan": log.plan,
                                  "stdoutBytes": log.stdout_bytes}
    return out


def find_replays(*roots: Path) -> list[Path]:
    found: list[Path] = []
    for root in roots:
        if root.exists():
            found.extend(root.rglob("*.repl26a"))
    return sorted(found, key=lambda p: p.stat().st_mtime, reverse=True)
