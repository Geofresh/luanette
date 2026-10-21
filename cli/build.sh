#!/bin/sh
# Puts the npm package together in cli/: the engine built for Node (web/build.sh makes it), the pieces
# it shares with the playground, the templates, and the API help. Then `npm pack` (in cli/) packs it.
set -e
cd "$(dirname "$0")"
[ -f ../web/build/luanette-node.js ] || { echo "run web/build.sh first (it builds the engine for Node)"; exit 1; }
rm -rf engine lib templates api.json LICENSE
mkdir -p engine lib templates
cp ../web/build/luanette-node.js engine/
cp ../web/gif.js ../web/tables.js lib/
cp ../templates/*.lua templates/
cp ../LICENSE LICENSE
python3 ../tools/api_index.py ../docs/api.md api.json > /dev/null
echo "cli/ is ready: cd cli && npm pack"
