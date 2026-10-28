#!/bin/bash
# web-savvycan's browser tests: the unit tests (node), then the Playwright suite (README.md).
#   tests/web/run.sh                     unit tests + every project (app, download, firefox)
#   tests/web/run.sh --long              also the 60 s full-load replays (through the heavy-job wrapper)
#   tests/web/run.sh --large             also the multi-GB log (WSC_LARGE_GB=2,4; through the heavy-job wrapper)
#   tests/web/run.sh --screenshots       also the pictures of the app in light and dark
#   tests/web/run.sh --dist DIR          the app build to test (default: <repo>/dist; env WSC_DIST)
#   tests/web/run.sh -- <playwright args>   e.g. -- --project=app -g logging
# Browsers: Playwright 1.56's (chromium-1194, firefox-1495) from ~/.cache/ms-playwright (or PLAYWRIGHT_BROWSERS_PATH).
set -euo pipefail
cd "$(dirname "$0")"
RUN=() HEAVY=
while [ $# -gt 0 ]; do
    case "$1" in
        --long|--large) RUN+=("${1#--}"); HEAVY=1; shift ;;
        --screenshots) RUN+=(screenshots); shift ;;
        --dist) export WSC_DIST="$(realpath "$2")"; shift 2 ;;
        --) shift; break ;;
        -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
        *) break ;;
    esac
done
export WSC_RUN="$(IFS=,; echo "${RUN[*]:-}")"
if [ ! -d node_modules/@playwright/test ]; then npm ci --no-audit --no-fund; fi
if [ -z "${PLAYWRIGHT_BROWSERS_PATH:-}" ] && [ ! -d "$HOME/.cache/ms-playwright/chromium-1194" ] && [ -d /opt/pw-browsers ]; then
    export PLAYWRIGHT_BROWSERS_PATH=/opt/pw-browsers
fi
echo "== unit tests (the fake rc1, the Web Serial bridge)"
node --test --test-reporter=dot unit/*.test.js unit/*.test.mjs
echo "== Playwright (dist: ${WSC_DIST:-$(realpath -m ../../dist)})"
WRAP="$HOME/.local/share/web-savvycan/heavy.sh"
if [ -n "$HEAVY" ] && [ -x "$WRAP" ]; then exec "$WRAP" npx playwright test "$@"; fi
exec npx playwright test "$@"
