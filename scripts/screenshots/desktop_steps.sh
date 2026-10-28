#!/bin/bash
# Runs inside xvfb-run for desktop.sh: start SavvyCAN, take the pictures, quit.
# Positions are for the 1400x860 main window at 0,0 (no window manager on Xvfb).
set -u
"$BIN" >/tmp/wsc-shot-$THEME.log 2>&1 &
APP=$!
pic() { sleep 1.5; import -window root "$OUT/desktop-$THEME-$1.png"; }
unset LD_PRELOAD
sleep 10
pic trace
xdotool mousemove 1217 163 click 1          # Suspend Capturing (rows stay put)
sleep 0.5
xdotool mousemove 1217 377 click 1          # Expand All Rows
pic trace-expanded
xdotool mousemove 1217 408 click 1          # Collapse All Rows
xdotool mousemove 1217 163 click 1          # Resume
xdotool mousemove 57 10 click 1             # View menu
pic view-menu
xdotool key Escape
xdotool mousemove 178 10 click 1            # Help > About web-savvycan
sleep 0.5; xdotool key Down Return
pic about
xdotool key Return                           # Close (default button; About runs with exec())
sleep 1
sleep 0.5
xdotool mousemove 20 10 click 1             # File > Preferences (second from the bottom)
sleep 0.5; xdotool key Up Up Return
pic preferences
if [ "$THEME" = light ]; then                # switch at run time: View > Dark Theme
    xdotool search --name "Main Settings" windowclose 2>/dev/null; sleep 0.5
    xdotool mousemove 57 10 click 1; sleep 0.5; xdotool key Down Down Return
    pic switched-to-dark
    grep -H "^Theme" "$XDG_CONFIG_HOME/EVTV/SavvyCAN.ini"
fi
kill $APP 2>/dev/null
sleep 1
