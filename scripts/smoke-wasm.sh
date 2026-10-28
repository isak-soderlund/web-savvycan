#!/bin/bash
# Smoke test of the wasm build in dist/: serves it on localhost and opens it in headless Chromium
# (Playwright): Qt starts, the main window is drawn, no console errors. Screenshot: dist/smoke.png.
# Usage: scripts/smoke-wasm.sh [--dist DIR] [--port N] [--wait MS]
# Playwright: from scripts/node_modules (cd scripts && npm ci), else NODE_PATH, else the rig's
# ~/src/resolve-hil/tools/web/node_modules. Browsers: ~/.cache/ms-playwright (npx playwright install chromium).
set -euo pipefail
cd "$(dirname "$0")/.."
DIST="$PWD/dist" PORT=8765 WAIT=8000
while [ $# -gt 0 ]; do
    case "$1" in
        --dist) DIST="$(realpath "$2")"; shift ;;
        --port) PORT="$2"; shift ;;
        --wait) WAIT="$2"; shift ;;
        -h|--help) sed -n '2,6p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done
[ -f "$DIST/SavvyCAN.html" ] || { echo "no $DIST/SavvyCAN.html: run scripts/build-wasm.sh" >&2; exit 1; }
export NODE_PATH="$PWD/scripts/node_modules:${NODE_PATH:-}:$HOME/src/resolve-hil/tools/web/node_modules"

python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$DIST" >/dev/null 2>&1 &
SERVER=$!
trap 'kill $SERVER 2>/dev/null' EXIT
for _ in $(seq 50); do curl -sf -o /dev/null "http://127.0.0.1:$PORT/SavvyCAN.html" && break; sleep 0.1; done
node scripts/smoke-wasm.cjs "http://127.0.0.1:$PORT/SavvyCAN.html" "$DIST/smoke.png" "$WAIT"
