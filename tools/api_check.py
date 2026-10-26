#!/usr/bin/env python3
"""Every name a cart sees is in docs/api.md, and every call there is one a cart sees.

    python tools/api_check.py RESULT.json

RESULT.json is check.py's (or the command-line tool's) result for tests/carts/api.lua, which logs
the name of every global a cart sees and every hd and ui function. tests/parity.py already holds
check.py and the board's runtime to the same list (it compares the logs); this holds that list to
the docs, so a call added to the runtime and check.py but not documented (or documented but not
added) fails CI. The playground's autocomplete and the command-line tool's help come from the docs
(tools/api_index.py), so they follow.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from api_index import index  # noqa: E402

# Globals a cart has that aren't documented calls: the API's tables, and two names kept for later.
NOT_CALLS = {"_G", "hd", "ui",
             "ask", "say"}  # reserved: calling either says "not on the board yet"


def documented(md: str) -> tuple[set[str], set[str]]:
    """The calls in api.md's tables, and the Lua names its Limits section lists as there."""
    calls = {e["name"] for e in index(md)}
    lua = set()
    m = re.search(r"^- No `io`.*?are there\.", md, re.S | re.M)
    if not m:
        sys.exit("api_check: docs/api.md's Limits no longer lists Lua's names (\"- No `io`... are there.\")")
    there = m.group(0).split(".", 1)[1]  # after "No io, os, ... package."
    lua.update(re.findall(r"`(\w+)`", there))
    return calls, lua


def main() -> None:
    res = json.loads(Path(sys.argv[1]).read_text())
    if not res.get("ok"):
        sys.exit(f"api_check: the names cart didn't run: {res.get('error')}")
    seen = set(" ".join(res.get("log", [])).split())
    calls, lua = documented((ROOT / "docs" / "api.md").read_text())
    undocumented = sorted(seen - calls - lua - NOT_CALLS)
    missing = sorted(calls - seen)
    lua_missing = sorted(lua - seen)
    for what, names in (("a cart has these, but docs/api.md doesn't document them", undocumented),
                        ("docs/api.md documents these, but a cart doesn't have them", missing),
                        ("docs/api.md says Lua gives these, but a cart doesn't have them", lua_missing)):
        if names:
            print(f"{what}: {', '.join(names)}")
    if undocumented or missing or lua_missing:
        sys.exit(1)
    print(f"api_check: {len(seen)} names, all documented")


if __name__ == "__main__":
    main()
