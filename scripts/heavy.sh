#!/bin/bash
# web-savvycan heavy job wrapper (2026-10-28). Our jobs run in their OWN slice (wscjobs.slice, 3G total, no swap), not in
# resolve-can's shared rcanjobs.slice: the firmware session's Windows VM (qemu rc1-win11, ~4.1 GB) lives there, and a
# compile of ours in the same slice twice pushed it over its 6G ceiling (17:28: the VM was killed; 17:35: a compile).
# Separate slices can't kill each other's jobs; the PC has ~14 GB (VM ~4 GB + our 3 GB + the rest).
# One job at a time (lock); use -j$WSC_JOBS. Exit 137 = our job hit 3G: reduce -j, never raise the cap.
# Usage: ~/.local/share/web-savvycan/heavy.sh <command...>
exec 9>/tmp/web-savvycan-build.lock
flock 9
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
D="$HOME/.config/systemd/user"; mkdir -p "$D"
W="[Unit]
Description=web-savvycan heavy jobs (own memory ceiling, separate from rcanjobs.slice)

[Slice]
MemoryMax=3G
MemorySwapMax=0"
if [ "$(cat "$D/wscjobs.slice" 2>/dev/null)" != "$W" ]; then echo "$W" > "$D/wscjobs.slice"; systemctl --user daemon-reload; fi
export WSC_JOBS=1   # 20:03: two compiles (one ~1.9 GB) hit the 3G slice: one at a time
echo "heavy.sh: wscjobs.slice (3G), WSC_JOBS=$WSC_JOBS" >&2
exec systemd-run --user --scope --quiet --slice=wscjobs.slice -p MemoryMax=3G -p MemorySwapMax=0 -- "$@"
