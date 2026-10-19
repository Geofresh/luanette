#!/usr/bin/env python3
"""A cart's source as it ships: comments out, header lines kept. Plain Python, no dependencies, so
the firmware build can run it on the carts it carries as well as push.py on the ones it sends.

    python tools/cartsrc.py IN.lua OUT.lua
"""
import re
import sys

_HEADER = re.compile(r"--\s*(title|by|about|api|canvas)\s*:")


def shipped(src: str) -> str:
    """The cart as it goes to the board: comments out (the header lines stay: the board reads them),
    and trailing spaces. Line breaks stay, so a runtime error's line number is the source's.
    Strings, including long [[...]] / [==[...]==] ones, are left alone."""
    out, i, n = [], 0, len(src)
    line_start = True
    while i < n:
        c = src[i]
        if c in "\"'":  # a quoted string, escapes included
            j = i + 1
            while j < n and src[j] != c and src[j] != "\n":
                j += 2 if src[j] == "\\" else 1
            out.append(src[i:j + 1])
            i, line_start = j + 1, False
            continue
        m = re.match(r"\[(=*)\[", src[i:i + 64]) if c == "[" else None
        if m:  # a long string
            end = src.find("]" + m.group(1) + "]", i)
            end = n if end < 0 else end + len(m.group(1)) + 2
            out.append(src[i:end])
            i, line_start = end, False
            continue
        if src.startswith("--", i):
            if line_start and _HEADER.match(src, i):  # keep "-- title: ..." and friends
                j = src.find("\n", i)
                j = n if j < 0 else j
                out.append(src[i:j])
                i = j
                continue
            m = re.match(r"--\[(=*)\[", src[i:i + 64])
            if m:  # a block comment: its line breaks stay
                end = src.find("]" + m.group(1) + "]", i)
                end = n if end < 0 else end + len(m.group(1)) + 2
                out.append("\n" * src.count("\n", i, end))
                i = end
            else:
                j = src.find("\n", i)
                i = n if j < 0 else j
            continue
        out.append(c)
        if c == "\n":
            line_start = True
        elif not c.isspace():
            line_start = False
        i += 1
    return "\n".join(l.rstrip() for l in "".join(out).split("\n"))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    with open(sys.argv[1], encoding="utf-8") as f:
        src = shipped(f.read())
    with open(sys.argv[2], "w", encoding="utf-8") as f:
        f.write(src)
