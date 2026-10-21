#!/usr/bin/env python3
"""The API's calls from docs/api.md, as JSON for the playground's editor (autocomplete and hover help).

    python tools/api_index.py docs/api.md OUT.json

It reads the "| call | does |" and "| call | gives |" tables: each row's first cell has one or
more signatures in backticks, the second says what they do. A name can have several rows (shape()
has one per kind of shape); its entry keeps them all. web/build.sh runs this, so the help is always
the docs as they are.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path


def cells(row: str) -> list[str]:
    """A table row's cells; a pipe escaped as \\| stays in its cell."""
    parts = re.split(r"(?<!\\)\|", row.strip().strip("|"))
    return [p.replace("\\|", "|").strip() for p in parts]


def plain(text: str) -> str:
    return re.sub(r"\*\*|`", "", text).strip()


def index(md: str) -> list[dict]:
    entries: dict[str, dict] = {}
    section, in_table = "", False
    for line in md.splitlines():
        if line.startswith("## "):
            section = plain(line[3:])
            in_table = False
        elif re.match(r"^\|\s*call\s*\|\s*(does|gives)\s*\|", line):
            in_table = True
        elif in_table and line.startswith("|---"):
            continue
        elif in_table and line.startswith("|"):
            call, does = cells(line)[:2]
            for sig in re.findall(r"`([^`]+)`", call):
                m = re.match(r"([A-Za-z_][\w.]*)", sig)
                if not m or sig.startswith(("s:", "s.")):  # a scroller's methods: not names a cart types as-is
                    continue
                name = m.group(1)
                e = entries.setdefault(name, {"name": name, "section": section, "help": []})
                if not any(h["sig"] == sig for h in e["help"]):
                    e["help"].append({"sig": sig, "does": plain(does)})
        else:
            in_table = False
    return sorted(entries.values(), key=lambda e: e["name"])


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit(__doc__.split("\n\n")[1].strip())
    out = index(Path(sys.argv[1]).read_text())
    Path(sys.argv[2]).write_text(json.dumps(out, ensure_ascii=False, separators=(",", ":")))
    print(f"{len(out)} names from {sys.argv[1]}")


if __name__ == "__main__":
    main()
