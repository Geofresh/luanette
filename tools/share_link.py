#!/usr/bin/env python3
"""Playground share links, made and read here as the playground does (web/playground.js makeLink).

    python tools/share_link.py CART.lua [--name NAME]     # the link that opens CART.lua
    python tools/share_link.py --check DOC.md DIR          # every link in DOC opens DIR/<step>.lua

A link is the playground's address, "#cart=", then the cart ({"name", "src"}) as JSON, deflated (raw
deflate, "d" first) and in base64url. --check is for docs/tutorial.md: each link there is followed
by the file it should open, "(tests/tutorial/step3.lua)", and the cart in the link must be that
file exactly, so the tutorial's links can't fall behind its carts.
"""
from __future__ import annotations

import argparse
import base64
import json
import re
import sys
import zlib
from pathlib import Path

PLAYGROUND = "https://geofresh.github.io/luanette/"


def make(name: str, src: str) -> str:
    raw = json.dumps({"name": name, "src": src}, ensure_ascii=False, separators=(",", ":")).encode()
    c = zlib.compressobj(9, zlib.DEFLATED, -15)  # raw deflate, what DecompressionStream("deflate-raw") reads
    data = c.compress(raw) + c.flush()
    return PLAYGROUND + "#cart=d" + base64.urlsafe_b64encode(data).decode().rstrip("=")


def read(link: str) -> dict:
    """The cart in a share link: {"name", "src"}. ValueError when it isn't one."""
    m = re.search(r"cart=([du])([A-Za-z0-9_-]+)", link)
    if not m:
        raise ValueError("not a playground share link (no #cart=...)")
    data = base64.urlsafe_b64decode(m.group(2) + "=" * (-len(m.group(2)) % 4))
    cart = json.loads((zlib.decompress(data, -15) if m.group(1) == "d" else data).decode())
    if not isinstance(cart.get("name"), str) or not isinstance(cart.get("src"), str):
        raise ValueError("the link's cart is missing its name or code")
    return cart


def check(doc: Path, root: Path) -> None:
    text = doc.read_text()
    links = re.findall(r"\((https://[^)\s]*#cart=[^)\s]+)\)[^\n]*?\(`?([\w./-]+\.lua)`?\)", text)
    if not links:
        sys.exit(f"{doc}: no share links followed by the file they open")
    for link, file in links:
        cart, want = read(link), (root / file).read_text()
        if cart["src"] != want:
            sys.exit(f"{doc}: the link for {file} opens something else; make it again with "
                     f"python tools/share_link.py {file} --name {cart['name']}")
    print(f"{len(links)} links in {doc} match their carts")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("cart", nargs="?", type=Path)
    ap.add_argument("--name")
    ap.add_argument("--check", nargs=2, metavar=("DOC", "ROOT"))
    a = ap.parse_args()
    if a.check:
        check(Path(a.check[0]), Path(a.check[1]))
    elif a.cart:
        print(make(a.name or a.cart.stem, a.cart.read_text()))
    else:
        ap.error("a cart, or --check DOC ROOT")


if __name__ == "__main__":
    main()
