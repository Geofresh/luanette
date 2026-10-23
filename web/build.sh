#!/bin/sh
# Builds the playground into web/build/site: the engine as WebAssembly, the page, the examples.
# Needs Emscripten (emcmake, emcc) on the PATH: https://emscripten.org/docs/getting_started/
set -e
cd "$(dirname "$0")"
emcmake cmake -S . -B build -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build build -j
rm -rf build/site && mkdir -p build/site/examples
cp index.html playground.js board.js gif.js tables.js sound-editor.js sprite-editor.js map-editor.js build/luanette.js build/luanette.wasm build/site/
cp ../examples/*.lua build/site/examples/
cp -r vendor build/site/
python3 ../tools/api_index.py ../docs/api.md build/site/api.json  # the editor's API help
mkdir -p build/site/gallery && cp ../gallery/*.lua build/site/gallery/
mkdir -p build/site/templates && cp ../templates/*.lua build/site/templates/  # + New cart starts from these
cp ../llms.txt ../AGENTS.md build/site/  # for agents that start from the playground's address
python3 ../tools/gallery_index.py ../gallery build/site/gallery.json  # the gallery's cards
python3 - ../examples build/site/examples.json <<'PY'  # the examples' cards: title, by, about
import json, re, sys
from pathlib import Path
head = lambda src, k: (re.search(rf"^--\s*{k}\s*:\s*(.+?)\s*$", "\n".join(src.splitlines()[:12]), re.M) or [None, ""])[1]
carts = [{"name": p.stem, **{k: head(p.read_text(), k) for k in ("title", "by", "about")}} for p in sorted(Path(sys.argv[1]).glob("*.lua"))]
Path(sys.argv[2]).write_text(json.dumps(carts))
PY
# Each card's picture: the screen at frame 90 after a tap at 60 (most carts start on a tap), taken
# with the command-line tool, which runs the same engine (built above for Node).
mkdir -p build/site/thumbs build/thumbs
echo '[{"frame": 60, "tap": [64, 64]}]' > build/thumbs/tap.json
for cart in ../examples/*.lua ../gallery/*.lua; do
  name=$(basename "$cart" .lua)
  case "$cart" in ../gallery/*) name="gallery-$name" ;; esac
  node ../cli/luanette.mjs run "$cart" --frames 90 --shots 90 --input build/thumbs/tap.json --out "build/thumbs/$name" > /dev/null
  cp "build/thumbs/$name/frame-90.png" "build/site/thumbs/$name.png"
done
echo "built web/build/site (serve it: python3 -m http.server -d web/build/site)"
