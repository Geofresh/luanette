#!/bin/sh
# Builds the playground into web/build/site: the engine as WebAssembly, the page, the examples.
# Needs Emscripten (emcmake, emcc) on the PATH: https://emscripten.org/docs/getting_started/
set -e
cd "$(dirname "$0")"
emcmake cmake -S . -B build -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build build -j
rm -rf build/site && mkdir -p build/site/examples
cp index.html playground.js board.js gif.js tables.js sound-editor.js sprite-editor.js build/luanette.js build/luanette.wasm build/site/
cp ../examples/*.lua build/site/examples/
cp -r vendor build/site/
python3 ../tools/api_index.py ../docs/api.md build/site/api.json  # the editor's API help
mkdir -p build/site/gallery && cp ../gallery/*.lua build/site/gallery/
mkdir -p build/site/templates && cp ../templates/*.lua build/site/templates/  # + New cart starts from these
python3 ../tools/gallery_index.py ../gallery build/site/gallery.json  # the Gallery row
echo "built web/build/site (serve it: python3 -m http.server -d web/build/site)"
