#!/bin/bash
# Install the web-savvycan toolchain in user space (no sudo):
#   - Qt 6.12 desktop (linux_gcc_64) + Serial Port / Serial Bus, into ~/Qt (aqtinstall)
#   - Qt 6.12 for WebAssembly (wasm_singlethread), into ~/Qt
#   - Emscripten 5.0.5 (the version Qt 6.12 is built with), into ~/emsdk
# and write ~/.local/share/web-savvycan/toolchain.env, which the build scripts source.
#
# Usage: scripts/setup-toolchain.sh            (idempotent: skips what is already there)
# Env:   QT_VERSION (6.12.0), EMSDK_VERSION (5.0.5), QT_ROOT (~/Qt), EMSDK (~/emsdk)
#
# Needs from the system (Ubuntu): git, python3 (+venv), cmake >= 3.21, ninja-build, g++,
# and for the desktop build the OpenGL headers (libgl-dev / libopengl-dev, libegl-dev) and
# libxkbcommon-x11-0 to run it. On the rig these are installed already.
set -euo pipefail

QT_VERSION="${QT_VERSION:-6.12.0}"
EMSDK_VERSION="${EMSDK_VERSION:-5.0.5}"
QT_ROOT="${QT_ROOT:-$HOME/Qt}"
EMSDK="${EMSDK:-$HOME/emsdk}"
STATE="$HOME/.local/share/web-savvycan"
VENV="$STATE/aqt-venv"
ENV_FILE="$STATE/toolchain.env"

# Desktop modules beyond the essentials (qtbase, qtdeclarative, qttools, qtsvg, ... come with the base).
DESKTOP_MODULES=(qtserialport qtserialbus qtwebsockets)
# Qt 6.12 for wasm offers no Serial Port / Serial Bus (they refuse to build for Emscripten).
WASM_MODULES=(qtwebsockets)

step() { echo; echo "== $*"; }

for tool in git python3 cmake ninja g++; do
    command -v "$tool" >/dev/null || { echo "missing: $tool (install it with apt)" >&2; exit 1; }
done

step "aqtinstall venv: $VENV"
mkdir -p "$STATE"
cd "$STATE"   # aqtinstall writes aqtinstall.log into the current directory
if [ ! -x "$VENV/bin/aqt" ]; then
    python3 -m venv "$VENV"
    "$VENV/bin/pip" install -q --upgrade pip
    "$VENV/bin/pip" install -q aqtinstall brotli
fi
"$VENV/bin/aqt" version

QT_DESKTOP="$QT_ROOT/$QT_VERSION/gcc_64"
QT_WASM="$QT_ROOT/$QT_VERSION/wasm_singlethread"

step "Qt $QT_VERSION desktop -> $QT_DESKTOP"
if [ ! -f "$QT_DESKTOP/lib/cmake/Qt6SerialBus/Qt6SerialBusConfig.cmake" ] \
   || [ ! -f "$QT_DESKTOP/lib/cmake/Qt6WebSockets/Qt6WebSocketsConfig.cmake" ]; then
    "$VENV/bin/aqt" install-qt linux desktop "$QT_VERSION" linux_gcc_64 \
        --outputdir "$QT_ROOT" -m "${DESKTOP_MODULES[@]}"
else
    echo "already installed"
fi

step "Qt $QT_VERSION wasm_singlethread -> $QT_WASM"
if [ ! -x "$QT_WASM/bin/qt-cmake" ] \
   || [ ! -f "$QT_WASM/lib/cmake/Qt6WebSockets/Qt6WebSocketsConfig.cmake" ]; then
    "$VENV/bin/aqt" install-qt all_os wasm "$QT_VERSION" wasm_singlethread \
        --outputdir "$QT_ROOT" -m "${WASM_MODULES[@]}"
else
    echo "already installed"
fi

step "Emscripten $EMSDK_VERSION -> $EMSDK"
if [ ! -d "$EMSDK/.git" ]; then
    git clone --depth 1 https://github.com/emscripten-core/emsdk.git "$EMSDK"
else
    git -C "$EMSDK" pull --ff-only -q || echo "(emsdk pull skipped)"
fi
"$EMSDK/emsdk" install "$EMSDK_VERSION"
"$EMSDK/emsdk" activate "$EMSDK_VERSION" >/dev/null

step "Write $ENV_FILE"
# Other agents wait for this file: write it under a temporary name, move it in after the check.
cat > "$ENV_FILE.tmp" <<EOF
# web-savvycan toolchain (written by scripts/setup-toolchain.sh on $(date -Iseconds)).
# Source it: . ~/.local/share/web-savvycan/toolchain.env
export QT_VERSION="$QT_VERSION"
export QT_DESKTOP="$QT_DESKTOP"
export QT_WASM="$QT_WASM"
export EMSDK="$EMSDK"
export EMSDK_VERSION="$EMSDK_VERSION"
export AQT_VENV="$VENV"
# Emscripten (emcc, em++, node, wasm-opt from binaryen) on PATH, quietly:
EMSDK_QUIET=1 . "$EMSDK/emsdk_env.sh" >/dev/null
export PATH="$EMSDK/upstream/bin:\$PATH"
EOF

step "Check"
# shellcheck disable=SC1090
. "$ENV_FILE.tmp"
"$QT_DESKTOP/bin/qmake" -query QT_VERSION
"$QT_WASM/bin/qmake" -query QT_VERSION 2>/dev/null || true
emcc --version | head -1
wasm-opt --version
mv "$ENV_FILE.tmp" "$ENV_FILE"
echo "toolchain ready: $ENV_FILE"
