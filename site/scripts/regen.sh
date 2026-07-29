#!/usr/bin/env bash
# Regenerate the site from the canonical docs/ tree:
#   1. rescan ../docs -> src/data/manifest.json (tabs, cards, charts, filter)
#   2. static build   -> dist/
# Run from anywhere; safe to rerun after any edit under docs/.
set -euo pipefail
cd "$(dirname "$0")/.."

if [ ! -d node_modules ]; then
  echo "regen: installing dependencies first…"
  npm install --no-audit --no-fund
fi

node scripts/build-manifest.mjs
npx astro build
echo "regen: done — output in $(pwd)/dist"
