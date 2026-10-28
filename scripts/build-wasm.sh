#!/bin/bash
# WebAssembly build of web-savvycan (Qt 6.12 wasm_singlethread + Emscripten 5.0.5).
#
# Usage: scripts/build-wasm.sh [--clean] [--jspi] [--opt -Oz|-Os|-O2] [--wasm-opt] [--dist DIR]
#                              [-j N] [-- extra cmake args]
#   --jspi      JSPI instead of Asyncify (WEB_SAVVYCAN_JSPI; Chrome 137+)
#   --opt       optimisation level for our code and the link (default -Oz)
#   --wasm-opt  an extra `wasm-opt -Oz` pass over the linked .wasm before it goes to dist/
#   --dist      output directory (default dist/)
# Output: dist/SavvyCAN.{html,js,wasm}, qtloader.js, qtlogo.svg, each also as .gz and .br;
# sizes printed at the end (Shopify Oxygen allows 20 MB per file).
#
# Toolchain from ~/.local/share/web-savvycan/toolchain.env (scripts/setup-toolchain.sh). On the
# rig, run it through the heavy-job wrapper (build lock, memory cap, sets WSC_JOBS):
#   ~/.local/share/web-savvycan/heavy.sh scripts/build-wasm.sh
# Parallel jobs: -j N, else WSC_JOBS, else 2.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"

ENV_FILE="$HOME/.local/share/web-savvycan/toolchain.env"
if [ -z "${QT_WASM:-}" ] && [ -f "$ENV_FILE" ]; then
    # shellcheck disable=SC1090
    . "$ENV_FILE"
fi
[ -n "${QT_WASM:-}" ] && [ -n "${QT_DESKTOP:-}" ] || { echo "QT_WASM/QT_DESKTOP not set (run scripts/setup-toolchain.sh)" >&2; exit 1; }
command -v emcc >/dev/null || { echo "emcc not on PATH (source $ENV_FILE)" >&2; exit 1; }

CLEAN=0 JSPI=OFF OPT=-Oz WASMOPT=0 DIST="$ROOT/dist" JOBS="${WSC_JOBS:-2}" EXTRA=()
while [ $# -gt 0 ]; do
    case "$1" in
        --clean) CLEAN=1 ;;
        --jspi) JSPI=ON ;;
        --opt) OPT="$2"; shift ;;
        --wasm-opt) WASMOPT=1 ;;
        --dist) DIST="$(realpath -m "$2")"; shift ;;
        -j) JOBS="$2"; shift ;;
        --) shift; EXTRA=("$@"); break ;;
        -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

B="${WASM_BUILD_DIR:-$ROOT/build/wasm}"   # --jspi, --opt and -D options only re-link/re-configure here
[ "$CLEAN" = 1 ] && rm -rf "$B"

LAUNCHER=()
command -v ccache >/dev/null && LAUNCHER=(-DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_C_COMPILER_LAUNCHER=ccache)

echo "== wasm ($OPT, JSPI=$JSPI) -> $B"
"$QT_WASM/bin/qt-cmake" -S "$ROOT" -B "$B" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DQT_HOST_PATH="$QT_DESKTOP" \
    -DWEB_SAVVYCAN_JSPI="$JSPI" -DWEB_SAVVYCAN_WASM_LINK_OPT="$OPT" \
    "${LAUNCHER[@]}" "${EXTRA[@]}"
cmake --build "$B" -j "$JOBS"

echo "== dist -> $DIST"
mkdir -p "$DIST"
rm -f "$DIST"/SavvyCAN.* "$DIST"/qtloader.js* "$DIST"/qtlogo.svg*
FILES=(SavvyCAN.html SavvyCAN.js SavvyCAN.wasm qtloader.js qtlogo.svg)
for f in "${FILES[@]}"; do
    [ -f "$B/$f" ] && cp "$B/$f" "$DIST/"
done
if [ "$WASMOPT" = 1 ]; then
    before=$(stat -c %s "$DIST/SavvyCAN.wasm")
    wasm-opt -Oz --all-features "$DIST/SavvyCAN.wasm" -o "$DIST/SavvyCAN.wasm.opt"
    mv "$DIST/SavvyCAN.wasm.opt" "$DIST/SavvyCAN.wasm"
    echo "wasm-opt -Oz: $before -> $(stat -c %s "$DIST/SavvyCAN.wasm") bytes"
fi
# The commit this build is from (GPL: every hosted build names its source).
( git -C "$ROOT" rev-parse HEAD; git -C "$ROOT" status --porcelain --untracked-files=no | grep -q . && echo dirty || true ) > "$DIST/SOURCE_COMMIT"

PY="${AQT_VENV:+$AQT_VENV/bin/}python3"
"$PY" - "$DIST" "${FILES[@]}" <<'PYEOF'
import gzip, os, sys
try:
    import brotli
except ImportError:
    brotli = None
dist, files = sys.argv[1], sys.argv[2:]
limit = 20 * 1000 * 1000
print(f"{'file':<16}{'raw':>14}{'gzip -9':>14}{'brotli 11':>14}")
for f in files:
    p = os.path.join(dist, f)
    if not os.path.exists(p):
        continue
    data = open(p, 'rb').read()
    gz = gzip.compress(data, 9, mtime=0)
    open(p + '.gz', 'wb').write(gz)
    br = None
    if brotli:
        br = brotli.compress(data, quality=11)
        open(p + '.br', 'wb').write(br)
    mb = lambda n: f"{n/1e6:.2f} MB" if n is not None else "-"
    flag = "  > 20 MB" if len(data) > limit else ""
    print(f"{f:<16}{mb(len(data)):>14}{mb(len(gz)):>14}{mb(len(br) if br else None):>14}{flag}")
PYEOF
