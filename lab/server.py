#!/usr/bin/env python3
"""Start the CAMBC Lab website.

  python3 lab/server.py                       # http://localhost:8765 (only this machine)
  python3 lab/server.py --share               # bind to the LAN; prints a URL with an access key
  python3 lab/server.py --port 9000
"""
from __future__ import annotations

import argparse
import os
import secrets
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from labkit import server  # noqa: E402


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=int(os.environ.get("LAB_PORT", 8765)))
    parser.add_argument("--share", action="store_true", help="listen on all interfaces and require an access key")
    parser.add_argument("--key", default=os.environ.get("LAB_KEY"), help="access key to require (default: random with --share)")
    args = parser.parse_args()
    host = "0.0.0.0" if args.share else args.host
    key = args.key or (secrets.token_urlsafe(12) if host not in ("127.0.0.1", "localhost") else None)
    server.serve(host, args.port, key)


if __name__ == "__main__":
    main()
