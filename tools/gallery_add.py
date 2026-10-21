#!/usr/bin/env python3
"""Adds a cart to the gallery from a playground share link (from a "Submit to gallery" issue).

    python tools/gallery_add.py LINK [--name NAME] [--force]

The link is the whole URL or just its "#cart=..." part. The cart goes to gallery/NAME.lua (its name
from the link unless --name says otherwise), then check.py runs on it. --force replaces a gallery
cart of that name. Then look it over, run it in the playground, and commit it.
"""
from __future__ import annotations

import argparse
import base64
import json
import re
import subprocess
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent


def read_link(link: str) -> dict:
    """The cart in a share link: {"name", "src"} (web/playground.js shareLink: deflate-raw or not, base64url)."""
    m = re.search(r"cart=([du])([A-Za-z0-9_-]+)", link)
    if not m:
        sys.exit("that isn't a playground share link (no #cart=...)")
    data = base64.urlsafe_b64decode(m.group(2) + "=" * (-len(m.group(2)) % 4))
    raw = zlib.decompress(data, -15) if m.group(1) == "d" else data
    cart = json.loads(raw.decode())
    if not isinstance(cart.get("name"), str) or not isinstance(cart.get("src"), str):
        sys.exit("the link's cart is missing its name or code")
    return cart


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("link")
    ap.add_argument("--name")
    ap.add_argument("--force", action="store_true")
    a = ap.parse_args()
    cart = read_link(a.link)
    name = a.name or re.sub(r"[^a-z0-9_-]+", "_", cart["name"].split("/")[-1].lower()).strip("_")[:32]
    if not re.fullmatch(r"[a-z0-9_-]{1,32}", name or ""):
        sys.exit(f"'{name}' won't do as a name: pass --name")
    if (ROOT / "examples" / f"{name}.lua").exists() or name == "launcher":
        sys.exit(f"'{name}' is an example's name: pass --name")
    path = ROOT / "gallery" / f"{name}.lua"
    if path.exists() and not a.force:
        sys.exit(f"{path.relative_to(ROOT)} is there already: pass --force to replace it, or --name")
    path.write_text(cart["src"] if cart["src"].endswith("\n") else cart["src"] + "\n")
    print(f"wrote {path.relative_to(ROOT)}")
    done = subprocess.run([sys.executable, str(HERE / "check.py"), str(path)], capture_output=True, text=True)
    try:
        res = json.loads(done.stdout)
    except json.JSONDecodeError:
        sys.exit("check.py didn't run: " + (done.stderr or done.stdout)[-400:])
    if not res.get("ok"):
        sys.exit(f"check.py: {res.get('error')}  (the file is written; fix it or delete it)")
    print(f"check.py: ok, {res['bytes']} bytes, at most {res.get('max_ops')} Lua instructions a frame")
    for w in res.get("warnings", []):
        print("  warning:", w)


if __name__ == "__main__":
    main()
