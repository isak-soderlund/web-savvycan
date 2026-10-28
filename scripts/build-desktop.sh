#!/bin/bash
# Desktop build of web-savvycan (Qt 6.12 gcc_64, CMake + Ninja).
#
# Usage: scripts/build-desktop.sh [release|asan|tsan|all] [--clean]
#   release  Release build                                  -> build/desktop-release/SavvyCAN
#   asan     Debug + AddressSanitizer/UBSan (USE_SANITIZER)  -> build/desktop-asan/SavvyCAN
#   tsan     Debug + ThreadSanitizer (USE_TSAN)              -> build/desktop-tsan/SavvyCAN
#   all      the three
#
# Toolchain from ~/.local/share/web-savvycan/toolchain.env (scripts/setup-toolchain.sh), or set
# QT_DESKTOP to a Qt 6 gcc_64 prefix. On the rig, run it through the heavy-job wrapper
# (build lock, memory cap, sets WSC_JOBS):
#   ~/.local/share/web-savvycan/heavy.sh scripts/build-desktop.sh
# Parallel jobs: WSC_JOBS, else 2. The desktop tests: tests/desktop/run.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"

ENV_FILE="$HOME/.local/share/web-savvycan/toolchain.env"
if [ -z "${QT_DESKTOP:-}" ] && [ -f "$ENV_FILE" ]; then
    # shellcheck disable=SC1090
    . "$ENV_FILE"
fi
[ -n "${QT_DESKTOP:-}" ] || { echo "QT_DESKTOP not set (run scripts/setup-toolchain.sh)" >&2; exit 1; }

VARIANTS=() CLEAN=0 JOBS="${WSC_JOBS:-2}"
while [ $# -gt 0 ]; do
    case "$1" in
        release|asan|tsan) VARIANTS+=("$1") ;;
        all) VARIANTS+=(release asan tsan) ;;
        --clean) CLEAN=1 ;;
        -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done
[ ${#VARIANTS[@]} -gt 0 ] || VARIANTS=(release)

LAUNCHER=()
command -v ccache >/dev/null && LAUNCHER=(-DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_C_COMPILER_LAUNCHER=ccache)

for v in "${VARIANTS[@]}"; do
    case "$v" in
        release) OPTS=(-DCMAKE_BUILD_TYPE=Release) ;;
        asan)    OPTS=(-DCMAKE_BUILD_TYPE=Debug -DUSE_SANITIZER=ON) ;;
        tsan)    OPTS=(-DCMAKE_BUILD_TYPE=Debug -DUSE_TSAN=ON) ;;
    esac
    B="$ROOT/build/desktop-$v"
    [ "$CLEAN" = 1 ] && rm -rf "$B"
    echo "== desktop $v -> $B"
    cmake -S "$ROOT" -B "$B" -G Ninja -DCMAKE_PREFIX_PATH="$QT_DESKTOP" "${LAUNCHER[@]}" "${OPTS[@]}"
    cmake --build "$B" -j "$JOBS"
    ls -l "$B/SavvyCAN"
done
