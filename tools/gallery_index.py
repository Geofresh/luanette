#!/usr/bin/env python3
"""The gallery's carts (gallery/*.lua) as JSON for the playground: name, title, by, about.

    python tools/gallery_index.py gallery OUT.json

A cart's name is its file's (a-z, 0-9, _ and -, at most 32), and can't be an example's. Each needs
-- title:, -- by: and -- about: in its header, as check.py asks. Anything wrong stops it with an
error, so CI catches a cart added by hand that breaks a rule.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
NAME = re.compile(r"[a-z0-9_-]{1,32}")


def header(src: str, key: str) -> str | None:
    m = re.search(rf"^--\s*{key}\s*:\s*(.+?)\s*$", "\n".join(src.splitlines()[:12]), re.M)
    return m.group(1) if m else None


def index(folder: Path) -> list[dict]:
    examples = {p.stem for p in (HERE.parent / "examples").glob("*.lua")}
    out, problems = [], []
    for path in sorted(folder.glob("*.lua")):
        name, src = path.stem, path.read_text()
        if not NAME.fullmatch(name):
            problems.append(f"{path.name}: a name is 1 to 32 of a-z, 0-9, _ and -")
        if name in examples or name == "launcher":
            problems.append(f"{path.name}: that's an example's name")
        entry = {"name": name}
        for key in ("title", "by", "about"):
            entry[key] = header(src, key)
            if not entry[key]:
                problems.append(f"{path.name}: no '-- {key}: ...' in its header")
        made = header(src, "made with")  # optional: the agent or tool that made it
        if made:
            entry["made_with"] = made
        out.append(entry)
    if problems:
        sys.exit("gallery: " + "; ".join(problems))
    return out


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit(__doc__.split("\n\n")[1].strip())
    out = index(Path(sys.argv[1]))
    Path(sys.argv[2]).write_text(json.dumps(out, ensure_ascii=False, separators=(",", ":")))
    print(f"{len(out)} gallery carts")


if __name__ == "__main__":
    main()
