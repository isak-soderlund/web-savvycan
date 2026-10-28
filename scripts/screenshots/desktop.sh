#!/bin/bash
# Screenshots of the desktop build, light and dark, without hardware: a fake GVRET device
# (fake_gvret_pty.py --tcp) streams classic and CAN FD frames, SavvyCAN starts with a saved "Network
# connection (GVRET)" to it (port 23 redirected by port23_redirect.so; SavvyCAN's serial GVRET can't open
# a pty: QSerialPortInfo doesn't list ptys), on a virtual X screen (Xvfb). Settings go to a throw-away XDG_CONFIG_HOME, never the user's.
#
#   scripts/screenshots/desktop.sh BUILD_DIR OUT_DIR
#
# Writes OUT_DIR/desktop-{light,dark}-{trace,trace-expanded,view-menu,preferences,about}.png
# Needs: xvfb-run, xdotool, ImageMagick (import), python3, Qt (toolchain.env's QT_DESKTOP).
set -euo pipefail
BUILD=$(realpath "$1"); OUT=$(realpath -m "$2"); HERE=$(dirname "$(realpath "$0")")
. ~/.local/share/web-savvycan/toolchain.env
mkdir -p "$OUT"
WS="$BUILD/write_settings"
if [ ! -x "$WS" ]; then
    g++ -std=c++17 -fPIC -O1 "$HERE/write_settings.cpp" -I"$QT_DESKTOP/include" -I"$QT_DESKTOP/include/QtCore" \
        -L"$QT_DESKTOP/lib" -lQt6Core -Wl,-rpath,"$QT_DESKTOP/lib" -o "$WS"
fi

SHIM="$BUILD/port23_redirect.so"
[ -f "$SHIM" ] || gcc -shared -fPIC -O1 "$HERE/port23_redirect.c" -o "$SHIM" -ldl

shot() { # theme
    local theme=$1 cfg port
    cfg=$(mktemp -d); port=$((20000 + RANDOM % 20000))
    "$WS" "$cfg/EVTV/SavvyCAN.ini" \
        Main/Theme="$theme" Main/Language=en_US Main/SaveRestoreConnections=true Main/SaveRestorePositions=true \
        Main/WindowSize=@size:1400,860 Main/WindowPos=@point:0,0 Main/AutoScroll=true Main/ColorsByCanId=true \
        Main/TimeSeconds=true \
        connections/portNames=@strings:127.0.0.1 connections/driverNames=@strings: \
        connections/devTypes=@ints:0 connections/serialSpeeds=@ints:1000000 connections/busSpeeds=@ints:500000 \
        connections/DataRates=@ints:2000000 connections/CanFds=@ints:1 connections/listenOnlys=@ints:0 \
        connections/actives=@ints:1
    # driverNames must have one (empty) entry like portNames
    sed -i 's/^driverNames=.*/driverNames=""/' "$cfg/EVTV/SavvyCAN.ini"
    python3 "$HERE/fake_gvret_pty.py" --tcp "$port" --seconds 60 --rate 4 &
    local fake=$!
    sleep 1
    WSC_PORT23=$port LD_PRELOAD="$SHIM" XDG_CONFIG_HOME="$cfg" OUT="$OUT" THEME="$theme" BIN="$BUILD/SavvyCAN" \
        xvfb-run -a -s "-screen 0 1400x900x24" bash "$HERE/desktop_steps.sh"
    kill $fake 2>/dev/null || true
    wait $fake 2>/dev/null || true
    rm -rf "$cfg"
}

shot light
shot dark
ls -la "$OUT"/desktop-*.png
