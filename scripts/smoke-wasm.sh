#!/bin/bash
# Smoke test of the wasm build: tests/web's smoke spec (Qt starts, the main window is drawn, a modal exec() dialog
# opens and closes, no console errors). Pictures: tests/web/results/smoke*.png.
# Usage: scripts/smoke-wasm.sh [--dist DIR]   (default dist/; WSC_PORT for another port than 8751)
set -euo pipefail
cd "$(dirname "$0")/.."
DIST="$PWD/dist"
[ "${1:-}" = --dist ] && DIST="$(realpath "$2")"
[ -f "$DIST/SavvyCAN.html" ] || { echo "no $DIST/SavvyCAN.html: run scripts/build-wasm.sh" >&2; exit 1; }
cd tests/web
[ -d node_modules/@playwright/test ] || npm ci --no-audit --no-fund
WSC_DIST="$DIST" npx playwright test --project=app specs/smoke.spec.mjs
