#!/bin/bash
# Build and run the desktop tests of the Web Serial stand-in (wasm/webserial/tests). Heavy: run it through
# ~/.local/share/web-savvycan/heavy.sh, which sets WSC_JOBS.
#   ~/.local/share/web-savvycan/heavy.sh wasm/webserial/scripts/build-tests.sh [build dir]
set -eo pipefail
. ~/.local/share/web-savvycan/toolchain.env
SRC="$(cd "$(dirname "$0")/.." && pwd)/tests"
BUILD="${1:-$SRC/../../../build/webserial-tests}"
cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_PREFIX_PATH="$QT_DESKTOP" -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build "$BUILD" -j"${WSC_JOBS:-1}"
ctest --test-dir "$BUILD" --output-on-failure
