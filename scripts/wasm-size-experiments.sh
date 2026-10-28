#!/bin/bash
# Size experiments for the wasm build: builds each variant into dist-exp/<name>/, smoke-tests it,
# and prints a table of the .wasm sizes (raw, gzip -9, brotli 11). Heavy: on the rig run it through
#   ~/.local/share/web-savvycan/heavy.sh scripts/wasm-size-experiments.sh [variant...]
# Variants: base (-Oz, Asyncify), wasmopt (+ wasm-opt -Oz), noscript (no scripting window / Qt Qml),
#           noscript-wasmopt, os (-Os), jspi (JSPI instead of Asyncify). Default: all of them.
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
# shellcheck disable=SC1090
. "$HOME/.local/share/web-savvycan/toolchain.env"
VARIANTS=("$@")
[ ${#VARIANTS[@]} -gt 0 ] || VARIANTS=(base wasmopt noscript noscript-wasmopt os jspi)
OUT="$ROOT/dist-exp"
mkdir -p "$OUT"
RESULTS=()
for v in "${VARIANTS[@]}"; do
    case "$v" in
        base)             ARGS=(--opt -Oz -- -DWEB_SAVVYCAN_SCRIPTING=ON) ;;
        wasmopt)          ARGS=(--opt -Oz --wasm-opt -- -DWEB_SAVVYCAN_SCRIPTING=ON) ;;
        noscript)         ARGS=(--opt -Oz -- -DWEB_SAVVYCAN_SCRIPTING=OFF) ;;
        noscript-wasmopt) ARGS=(--opt -Oz --wasm-opt -- -DWEB_SAVVYCAN_SCRIPTING=OFF) ;;
        os)               ARGS=(--opt -Os -- -DWEB_SAVVYCAN_SCRIPTING=ON) ;;
        jspi)             ARGS=(--opt -Oz --jspi -- -DWEB_SAVVYCAN_SCRIPTING=ON) ;;
        *) echo "unknown variant $v" >&2; exit 2 ;;
    esac
    echo "=== $v: ${ARGS[*]}"
    D="$OUT/$v"
    if ! scripts/build-wasm.sh --dist "$D" "${ARGS[@]}" > "$OUT/$v.build.log" 2>&1; then
        RESULTS+=("$v|build failed (see dist-exp/$v.build.log)"); tail -5 "$OUT/$v.build.log"; continue
    fi
    tail -7 "$OUT/$v.build.log"
    if scripts/smoke-wasm.sh --dist "$D" > "$OUT/$v.smoke.log" 2>&1; then smoke=OK; else smoke=FAIL; fi
    cat "$OUT/$v.smoke.log"
    sizes=$(python3 -c "import os,sys; d=sys.argv[1]; f=lambda n: os.path.getsize(os.path.join(d,n))/1e6 if os.path.exists(os.path.join(d,n)) else 0; print(f'{f(\"SavvyCAN.wasm\"):.2f}|{f(\"SavvyCAN.wasm.gz\"):.2f}|{f(\"SavvyCAN.wasm.br\"):.2f}|{f(\"SavvyCAN.js\"):.3f}')" "$D")
    RESULTS+=("$v|$sizes|$smoke")
done
# Back to the default configuration for later scripts/build-wasm.sh runs.
"$QT_WASM/bin/qt-cmake" -S "$ROOT" -B "${WASM_BUILD_DIR:-$ROOT/build/wasm}" -DWEB_SAVVYCAN_SCRIPTING=ON -DWEB_SAVVYCAN_JSPI=OFF -DWEB_SAVVYCAN_WASM_LINK_OPT=-Oz >/dev/null 2>&1 || true
echo
echo "| variant | wasm MB | gzip MB | brotli MB | js MB | smoke |"
echo "| --- | --- | --- | --- | --- | --- |"
for r in "${RESULTS[@]}"; do echo "| ${r//|/ | } |"; done
