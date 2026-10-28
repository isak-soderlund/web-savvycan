#!/bin/bash
# web-savvycan: the browser logging tests (wasm/disklog) and the desktop's native CSV checks.
#   test/web/run-disklog-tests.sh            all that this machine can run
# The MVP (LOG_TO_DISK_LIVE false): tests/download*.spec.mjs (blob logging, one download on stop; the app
# with a fake rc1 when dist/ and the fake are there). The live-to-disk tests (disklog*.spec.mjs) still run
# against the module. The multi-GB log is separate (memory-heavy), through the heavy-job wrapper:
#   WSC_LARGE_GB=2,4,8 ~/.local/share/web-savvycan/heavy.sh npx playwright test --project=chromium-large
# Needs: the toolchain (toolchain.env), Node + npm; Firefox tests: Playwright's Firefox
# (npx playwright install firefox); real pickers: xvfb-run and xdotool.
set -euo pipefail
WEB="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$WEB/../.." && pwd)"
. "$HOME/.local/share/web-savvycan/toolchain.env"
HEAVY="$HOME/.local/share/web-savvycan/heavy.sh"
RUN=(); [ -x "$HEAVY" ] && RUN=("$HEAVY")

echo "== native CSV tool (desktop Qt)"
"${RUN[@]}" bash -c "cmake -S '$REPO/test/nativecsv' -B '$REPO/build/nativecsv' -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH='$QT_DESKTOP' >/dev/null && cmake --build '$REPO/build/nativecsv' -j\${WSC_JOBS:-2}"
TOOL="$REPO/build/nativecsv/nativecsv_tool"
mkdir -p "$WEB/test-results/disklog"
echo "== desktop continuous logging and save/load, CAN FD rows"
"$TOOL" continuous "$WEB/test-results/disklog/desktop-continuous.csv" 20000
"$TOOL" roundtrip "$WEB/test-results/disklog/desktop-roundtrip.csv" 20000

cd "$WEB"
[ -d node_modules ] || npm ci --no-audit --no-fund
PROJECTS=(--project=chromium)
if ls "$HOME/.cache/ms-playwright" | grep -q '^firefox-'; then PROJECTS+=(--project=firefox); else echo "(no Playwright Firefox: skipping)"; fi
echo "== browser tests: ${PROJECTS[*]}"
npx playwright test "${PROJECTS[@]}"
if command -v xvfb-run >/dev/null && command -v xdotool >/dev/null; then
    echo "== real pickers, permission prompts and files (headed Chromium under Xvfb)"
    xvfb-run -a -s "-screen 0 1280x900x24" npx playwright test --project=chromium-xvfb
else
    echo "(no xvfb-run/xdotool: skipping the real-picker tests)"
fi
