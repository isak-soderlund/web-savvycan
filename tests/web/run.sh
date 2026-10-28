#!/bin/bash
# web-savvycan's browser tests: the fake rc1's unit tests (node), then the Playwright suite (README.md).
#   tests/web/run.sh                     unit tests + both projects (selftest; app, skipped while dist/ is missing)
#   tests/web/run.sh --long              also the 60 s full-load replays (through the rig's heavy-job wrapper)
#   tests/web/run.sh --dist DIR          the app build to test (default: <repo>/dist; env WSC_DIST)
#   tests/web/run.sh -- <playwright args>   e.g. -- --project=app -g logging
# Chromium: Playwright 1.56's (chromium-1194) from ~/.cache/ms-playwright (or PLAYWRIGHT_BROWSERS_PATH).
set -euo pipefail
cd "$(dirname "$0")"
LONG=
while [ $# -gt 0 ]; do
    case "$1" in
        --long) LONG=1; shift ;;
        --dist) export WSC_DIST="$(realpath "$2")"; shift 2 ;;
        --) shift; break ;;
        -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
        *) break ;;
    esac
done
if [ ! -d node_modules/@playwright/test ]; then npm ci --no-audit --no-fund; fi
if [ -z "${PLAYWRIGHT_BROWSERS_PATH:-}" ] && [ ! -d "$HOME/.cache/ms-playwright/chromium-1194" ] && [ -d /opt/pw-browsers ]; then
    export PLAYWRIGHT_BROWSERS_PATH=/opt/pw-browsers
fi
echo "== unit tests (fake rc1 vs the firmware's test vectors)"
node --test unit/*.test.js
echo "== Playwright (dist: ${WSC_DIST:-$(realpath -m ../../dist)})"
if [ -n "$LONG" ]; then
    export WSC_LONG=1
    HEAVY="$HOME/.local/share/web-savvycan/heavy.sh"
    if [ -x "$HEAVY" ]; then exec "$HEAVY" npx playwright test "$@"; fi
fi
exec npx playwright test "$@"
