#!/bin/bash
# WebAssembly build of web-savvycan (Qt 6.12 wasm_singlethread + Emscripten 5.0.5).
#
# Usage: scripts/build-wasm.sh [--clean] [--no-gzip-loader] [--dist DIR] [-- extra cmake args]
#   The wasm ships only as SavvyCAN.wasm.gz: the page fetches it and decompresses it with
#   DecompressionStream('gzip') (Chrome 80+, Firefox 113+, Safari 16.4+), so no file is over a static
#   host's 20 MB limit (Shopify Oxygen). --no-gzip-loader: the raw .wasm instead.
#   --dist       output directory (default dist/)
# Output: dist/SavvyCAN.{html,js,wasm.gz}, qtloader.js, qtlogo.svg, each also as .gz and .br;
# sizes printed at the end.
#
# Toolchain from ~/.local/share/web-savvycan/toolchain.env (scripts/setup-toolchain.sh). On the
# rig, run it through the heavy-job wrapper (build lock, memory cap, sets WSC_JOBS):
#   ~/.local/share/web-savvycan/heavy.sh scripts/build-wasm.sh
# Parallel jobs: WSC_JOBS, else 2.
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

CLEAN=0 GZLOADER=1 DIST="$ROOT/dist" JOBS="${WSC_JOBS:-2}" EXTRA=()
while [ $# -gt 0 ]; do
    case "$1" in
        --clean) CLEAN=1 ;;
        --no-gzip-loader) GZLOADER=0 ;;
        --dist) DIST="$(realpath -m "$2")"; shift ;;
        --) shift; EXTRA=("$@"); break ;;
        -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

B="${WASM_BUILD_DIR:-$ROOT/build/wasm}"
[ "$CLEAN" = 1 ] && rm -rf "$B"

LAUNCHER=()
command -v ccache >/dev/null && LAUNCHER=(-DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_C_COMPILER_LAUNCHER=ccache)

echo "== wasm -> $B"
"$QT_WASM/bin/qt-cmake" -S "$ROOT" -B "$B" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DQT_HOST_PATH="$QT_DESKTOP" \
    "${LAUNCHER[@]}" "${EXTRA[@]}"
cmake --build "$B" -j "$JOBS"

echo "== dist -> $DIST"
mkdir -p "$DIST"
rm -f "$DIST"/SavvyCAN.* "$DIST"/qtloader.js* "$DIST"/qtlogo.svg*
FILES=(SavvyCAN.html SavvyCAN.js SavvyCAN.wasm qtloader.js qtlogo.svg)
for f in "${FILES[@]}"; do
    [ -f "$B/$f" ] && cp "$B/$f" "$DIST/"
done
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

if [ "$GZLOADER" = 1 ]; then
    "$PY" - "$DIST/SavvyCAN.html" <<'PYEOF'
import sys
p = sys.argv[1]; s = open(p).read()
anchor = 'entryFunction: window.SavvyCAN_entry,'
if anchor not in s:
    sys.exit('gzip loader: entryFunction line not found in SavvyCAN.html')
s = s.replace(anchor, anchor + """
                        // web-savvycan: the wasm ships gzip-compressed (static hosts' 20 MB per-file
                        // limit); the browser decompresses it while compiling.
                        module: WebAssembly.compileStreaming(fetch('SavvyCAN.wasm.gz').then(r => {
                            if (!r.ok) throw new Error('SavvyCAN.wasm.gz: HTTP ' + r.status);
                            return new Response(r.body.pipeThrough(new DecompressionStream('gzip')),
                                                { headers: { 'Content-Type': 'application/wasm' } });
                        })),""", 1)
open(p, 'w').write(s)
PYEOF
    rm -f "$DIST/SavvyCAN.wasm" "$DIST/SavvyCAN.wasm.br"
    "$PY" -c "import gzip,sys
p=sys.argv[1]; d=open(p,'rb').read(); open(p+'.gz','wb').write(gzip.compress(d,9,mtime=0))
try:
    import brotli; open(p+'.br','wb').write(brotli.compress(d,quality=11))
except ImportError: pass" "$DIST/SavvyCAN.html"
    echo "gzip loader: SavvyCAN.html loads SavvyCAN.wasm.gz ($(stat -c %s "$DIST/SavvyCAN.wasm.gz") bytes); no raw .wasm in $DIST"
fi
