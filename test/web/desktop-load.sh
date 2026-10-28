#!/bin/bash
# Load a log file into the desktop SavvyCAN (File > Load Log File, autodetect) under Xvfb and take a
# screenshot: the frame count is on the right ("Total Frames Captured"). Uses its own settings dir.
#   test/web/desktop-load.sh <log.csv> <screenshot.png> [SavvyCAN binary]
# (xvfb-run, xdotool and ImageMagick's import needed)
set -euo pipefail
LOG="$(realpath "$1")"; SHOT="$(realpath -m "$2")"
BIN="${3:-$(dirname "$0")/../../build/desktop-release/SavvyCAN}"
. "$HOME/.local/share/web-savvycan/toolchain.env"
CFG="$(mktemp -d)"
cat > "$CFG/run.sh" <<EOS
#!/bin/bash
export XDG_CONFIG_HOME="$CFG" QT_QPA_PLATFORM=xcb LD_LIBRARY_PATH="$QT_DESKTOP/lib"
"$BIN" >"$CFG/app.log" 2>&1 &
APP=\$!
for i in \$(seq 1 60); do xdotool search --onlyvisible --name "Savvy CAN" >/dev/null 2>&1 && break; sleep 0.5; done
sleep 2
W=\$(xdotool search --onlyvisible --name "Savvy CAN" | head -1)
xdotool windowfocus --sync \$W key ctrl+o; sleep 2
xdotool type --delay 10 "$LOG"; xdotool key Return; sleep 8
import -window root "$SHOT"
kill \$APP
EOS
chmod +x "$CFG/run.sh"
xvfb-run -a -s "-screen 0 1280x900x24" "$CFG/run.sh"
rm -rf "$CFG"
echo "screenshot: $SHOT"
