#!/bin/bash
# Runs inside xvfb-run for desktop.sh: start SavvyCAN, take the pictures, quit.
set -u
"$BIN" >/tmp/wsc-shot-$THEME.log 2>&1 &
APP=$!
sleep 9
import -window root "$OUT/desktop-$THEME-trace.png"
# Expand All Rows (button found by its text through xdotool is not possible: use the keyboard-free
# position recorded in desktop.sh's notes) -> skipped when not found
if [ -n "${EXPAND_XY:-}" ]; then
    xdotool mousemove ${EXPAND_XY/,/ } click 1; sleep 1
    import -window root "$OUT/desktop-$THEME-trace-expanded.png"
fi
kill $APP 2>/dev/null
sleep 1
