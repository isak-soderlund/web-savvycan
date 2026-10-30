# Testing web-savvycan with a real rc1 (owner)

Stage 1 (RESOLVE.md "MVP"): connect to rc1 from Chrome, watch the frames, log them to a file, light/dark theme. Then
what came after (steps 9-14): auto-connect, graphs, loading a log file, Temporal Graph and Frame Data Analysis (the
heatmap); stage 3 (steps 15-18): DBC files, Interpret Frames, the DBC editor, signals by name, Save Decoded Frames.
Everything below has passed in headless Chromium against a **fake** rc1, and with the **real** rc1 and a real bus on
the rig (see "What the rig has checked"); this is the check with your Chrome, your OS and your car.

## What you need

- **Chrome** (or Edge/Opera/Brave: Web Serial is Chromium-only; not Firefox, not Safari), a recent version.
- **rc1** with firmware 0.1.3 (USB `1209:0001`, "resolve-can-1"), and a CAN peer that sends frames at
  **500 kbit/s** (the car, or a second adapter such as the Kvaser) on a terminated bus. Without a peer nothing ACKs
  and no frames arrive.
- No other program may have rc1's serial port open (desktop SavvyCAN, a serial monitor, `candump` on gs_usb).
- The app: a `dist/` folder from `scripts/build-wasm.sh` (`SavvyCAN.html`, `SavvyCAN.js`, `SavvyCAN.wasm.gz`,
  `qtloader.js`, `qtlogo.svg`). It must come from a web server on **localhost** (or https): Web Serial is only
  offered on a secure origin, and a `file://` page cannot fetch the wasm.

### Serving the app

Easiest: the hosted page **https://resolve-ev.com/resolve-can/savvycan** (the website's build of main; its About box
names the commit). To test a build that isn't hosted, from the rig, without copying anything (the page runs in your Chrome, rc1 is plugged into your PC):

```
ssh -L 8000:127.0.0.1:8000 hil 'node ~/src/web-savvycan/tests/web/server.mjs --port 8000 --dist ~/src/web-savvycan/dist'
```

or copy the folder and serve it on your PC:

```
scp -r hil:src/web-savvycan/dist ~/web-savvycan-dist
cd ~/web-savvycan-dist && python3 -m http.server 8000 --bind 127.0.0.1
```

Then open **http://localhost:8000/SavvyCAN.html**. `dist/SOURCE_COMMIT` names the commit the build is from (a
second line `dirty` means uncommitted changes were in it).

## The test

1. **Start.** The page loads (a few seconds: ~9 MB). SavvyCAN's main window: File, RE Tools, Connection, Help;
   only the switched-on stages' entries are there (RESOLVE.md `enabledfeatures.h`).
2. **Connect over GVRET.** Connection > Open Connection Window > Add New Device Connection > **Serial Connection
   (GVRET)** > **Choose device...** > pick *resolve-can-1* in Chrome's list > Connect > **Create New Connection**.
   - The list shows GVRET / WebSerial ... / Connected after ~3-4 s (SavvyCAN's ESP32 start: DTR/RTS low, 3 s wait,
     then the GVRET handshake and its validation polls).
   - **Bus Details**: Speed **500000**, tick **Enable Bus**, **Enable CAN FD** as your bus is (with CAN FD:
     **CAN FD Data Rate**, e.g. 2000000), then **Save Bus Settings**.
   - Close the connection window (its close button, or Escape); the connection stays up.
3. **Frames.** With the peer sending: frames appear in the trace, *Total Frames Captured* and *Frames Per Second*
   count up; *Dropped: 0* at the bottom right stays 0 (it counts frames the app had to drop on an overloaded bus). Check a few IDs and data bytes against what the peer sends (and with CAN FD: lengths over 8, BRS).
   Tick **Auto Scroll Window** to follow the newest.
4. **Logging.** File > **Start Continuous Logging**: logging starts at once, the LOGGING label shows size and frames.
   Let it run (a minute, or a drive), then File > **Cease Continuous Logging**: Chrome downloads one file,
   `SavvyCAN_YYYY-MM-DD_HH-MM-SS.csv` (the start time) into your downloads folder.
   - Check: the row count is the frame count the label showed; the file loads in the desktop SavvyCAN (File > Load
     Log File, GVRET native CSV); CAN FD rows have up to 64 data bytes and `FD`/`FD+BRS` in the last column.
   - Optional: File > *Choose the Log's Name When Logging Stops...*, then Start / Cease: it asks for the name;
     *Keep Logging* goes on logging.
   - Closing the tab while logging asks first; a crash or a closed tab loses the log (accepted for the MVP).
5. **Theme.** File > **Preferences** > Theme: **Dark**: the whole window goes dark (trace rows too). Reload the page
   (Cmd-R / F5): still dark. **Light**, then **Follow system** (then it follows the OS setting).
6. **Unplug.** While connected and receiving, pull rc1's USB cable: the connection shows as disconnected, the page
   keeps working (no error page, no freeze). Plug it back in: it connects again by itself (auto-connect, step 9), or
   Add New Device Connection again (rc1 is listed without the chooser now) and Create New Connection: frames come again.
7. **Returning visit.** Close the tab, open the page again: the theme is kept, and the page connects to rc1 by itself
   (step 9; Chrome remembers the permission).
8. **Optional: LAWICEL.** Same as 2 with **LAWICEL / SLCAN Serial**: set CAN Bus Speed (and CAN-FD, its data rate)
   in the New Connection dialog before Create New Connection; there are no Bus Details to save.

9. **Auto-connect.** With a connection made (step 2), reload the page: within ~5 s the trace fills again, without the
   connection window or the chooser, with the same bus (speed, CAN FD, data rate: Connection > Open Connection Window
   shows it). Also with LAWICEL. File > Preferences > **Connect to the last device when the page opens** off, reload:
   not connected; on again, reload: connects. rc1 unplugged while the page opens: nothing happens, no error; plugged in
   later: it connects when you reload.
10. **Graphs.** With frames coming: right-click a row of the trace > **Add to graph** (or RE Tools > New Graphing
    Window > Graph > **Add New Graph**: ID, start bit, length; the bit grid shows 64 bytes for an FD ID). The graph
    grows and its right edge follows the newest value (View > Follow End of Graph in the graph window, on by default);
    zoom with the mouse wheel, drag to pan; R or View > Reset View shows the whole session. Try a byte past byte 8 of an
    FD frame. Leave a graph running a long time (an hour): the page stays smooth and its memory doesn't keep growing
    (Chrome's Task Manager, Shift-Esc). Dark theme: lines and axes readable.
11. **Loading a log file.** File > **Load Log File**: Chrome's file picker; pick a log you saved in step 4 (or one from
    the desktop SavvyCAN, candump, PCAN, Vector ASC/BLF). Every frame is in the trace (*Total Frames Captured* = the
    file's rows), in order, CAN FD rows with their lengths. A big SavvyCAN CSV (hundreds of MB, up to 1000 MB) loads in
    seconds to a minute with a progress dialog; other formats are refused over 200 MB with a message. Then graph a
    signal of the file (step 10).
12. **Temporal Graph.** RE Tools > **Temporal Graph**, on a loaded log and on a live bus: one row per ID (hex, 11- and
    29-bit apart), colour = how many frames per time slice; the summary line gives IDs, frames and span. Live it grows
    and follows the newest frames; zoom/pan; R: the whole capture again. On a long log (an hour) it opens in seconds.
13. **Frame Data Analysis (heatmap).** RE Tools > **Frame Data Analysis**: pick an ID in the list: the bit-change
    heatmap (grid as large as the frame: 8 to 64 bytes), the bit histogram, the details tree (lengths, intervals, per
    byte), a graph per byte. On a live bus new IDs join the list; click an ID again for its newer frames. **Save details
    to file**: a download `FrameDetails.txt`. Check an FD ID with 64-byte frames and a classic one.
14. **A long log.** Log (step 4) for more than 72 min (a charging session): the file's times keep increasing past
    71.6 min (rc1's clock is 32-bit µs; the app makes it 64-bit), no jump back to 0.
15. **DBC.** File > **DBC File Manager** > **Load**: Chrome's file picker; pick your car's DBC (e.g. the Leaf's
    `EV-can_AZE0.dbc`; `tests/web/fixtures/stage3_test.dbc` is a small one with CAN FD messages). Its row appears (bus
    -1 = all). Tick **Interpret Frames** in the main window: rows of the DBC's messages show `<Message>` and, when you
    click a row, `Signal: value unit` lines; the values make sense for the car (speed, SOC, voltage). Dark theme: every
    row readable (a message's own colour kept, text black or white on it). Reload the page: the DBC is still loaded.
16. **DBC editor.** In the File Manager select the file > **Edit**: the tree of nodes, messages, signals; double-click
    a message (length up to 64, CAN FD, BRS) and a signal (the bit grid; 64 bytes for an FD message). Change a signal,
    **Save** in the File Manager: a download of the DBC; load it in the desktop SavvyCAN (or Load it back here): the
    change is in it.
17. **Signals by name.** RE Tools > New Graphing Window > Graph > **Add New Graph** > Graph a DBC Signal: node,
    message, signal, **Copy Signal Parameters**, Add this graph: the graph shows the signal's values (as in the trace).
    Or right-click a `Signal: value` line of an expanded trace row > **Add to a new graphing window**. RE Tools >
    **Signal Viewer**: add a few signals: their values follow the bus live.
18. **Save Decoded Frames.** File > **Save Decoded Frames** and **Save Decoded Frames CSV**: downloads
    (`SavvyCAN_decoded.txt` / `.csv`) with every frame of the trace and its decoded signals.
19. **Capture Bisector.** Load a log (step 11), RE Tools > **Capture Bisector**: the main list's frame count and every ID
    in the two ID lists. Pick Frame Number (or Percentage), Lower or Upper Section, **Calculate Split**: *Saved Frames
    after split* shows how many; **Replace main list with split frames**: the trace holds only those (other windows
    follow). Repeat to narrow down when a frame appears; ID Range and Bus Number split the same way. **Save split frames
    to a new file**: a download `BisectedFrames.csv` that File > Load Log File (and the desktop SavvyCAN) reads back.
20. **File Comparison.** RE Tools > **File Comparison**: **Load New File** (side 1) and **Load A File** (side 2, more files
    add to it) open Chrome's file picker. The tree lists IDs found on one side only and, for IDs on both, the bits and
    each byte's values seen on one side only (all 64 bytes of FD frames). Try two logs of the same car, one with a
    button pressed. *Show only data unique to Side 1* applies at the next load; **Clear Frames** empties side 2; **Save
    Details to File**: a download. Two logs of an hour each load in seconds.
21. **DBC Comparison.** RE Tools > **DBC Comparison**: Load New File / Load A File pick two DBC files (they don't go into
    the DBC File Manager). The tree: nodes and messages missing on either side, messages with signals missing, signals
    whose start bit, size, factor or offset changed. **Save Details to File**: a download.
22. **Frame Search.** RE Tools > **Frame Search** on a loaded log: ID (tick Enable), Frame Length, Direction, the bits
    (start bit, number of bits, Intel/signed, scale, bias; click the grid, which shows 64 bytes for FD), then *Specific
    value* (decimal, `0x..` hex or `0b..` binary) or *Any change*, **Search**: "Match 1 of N" and that row selected in the
    trace; **Next >** / **< Previous** step through. Try a byte past byte 8 of an FD frame.
23. **Sniffer.** On a live bus, RE Tools > **Sniffer**: one row per ID, bytes going up green, down red; IDs that stop
    turn red and go after 5 s (Never Expire keeps them). Leave the car (or bus) as it is and press **Notch** a few
    times over a few seconds: the colours stop. Now do one thing (a button, a door, the steering): only its bytes light
    up. **Unnotch**: everything again. Try View Bits, Fade inactive bytes, an unchecked ID in Filters; an FD ID shows up
    to 64 byte columns. Dark theme: readable.
24. **Flow View.** RE Tools > **Flow View**, pick an ID: the bytes (table and grid) of one frame, the graphs around it.
    Step with the arrows, play, pause, drag the timeline; type a value in a byte's **Seek** cell: playing stops there;
    click a bit in the grid (it turns blue): playing stops where that bit changes (also past byte 8 of an FD ID). Tick
    **Live Mode**: it follows the newest frame. Leave it running long on a busy bus: smooth, memory not growing.
25. **Range State.** On a log or after a while of live traffic, RE Tools > **Range State** > **Recalculate Candidate
    Signals** (e.g. min 8, max 16, granularity 8): a list of candidates; arrow through it and look at the graphs for
    speed-, temperature- or pedal-like curves. The page stays responsive while it works (a 64-byte ID at granularity 1
    takes seconds).
26. **Single/Multi State.** RE Tools > **Single/Multi State**, Realtime, Number of States 2: **Go for it**, then do as
    the big label says: "Go to state 1" (e.g. button released), "Go to state 2" (pressed), each time for the toggle
    time, waiting at "Wait....". At DONE the list shows the ID and bits that followed you. Try 3 states with a 3-way
    switch. "From logged data": the fields of the checked IDs with exactly that many values in the trace.

Please note for each step: works / doesn't (what you saw, a screenshot), Chrome's version and the OS, and for any
problem the browser console (View > Developer > JavaScript Console): copy its red lines.

## What the rig has checked (real rc1, real bus)

`tests/web/run.sh --rig` (tests/web/rig/README.md) runs the app in headless Chromium against the **real** rc1
(firmware 0.1.3) with the rig's Kvaser Leaf v3 as a second node on rc1's bus; only Chrome's serial transport is
replaced (a bridge to rc1's tty, because Chromium's own Web Serial loses USB serial ports on the rig's Linux:
`read()` returning 0 right after open, which Chromium takes as "device lost"). Passed 2026-10-29 on main 8a5145d:
- GVRET and LAWICEL, classic 500 kbit/s: 2000 frames exactly (order, IDs, data); CAN FD 500k/2M: every FD length,
  BRS on/off, 11/29-bit, classic and remote frames exactly;
- a continuous log of 2000 FD frames: the download's rows exactly as sent, rc1's µs times spanning the 2 s;
- auto-connect after a reload (GVRET and LAWICEL, FD): same bus, no chooser, no connection window, frames arrive;
- rc1 unplugged for real (its USB port's data and 5 V off) mid-stream: disconnected, page alive; back on: reconnected,
  frames again;
- full rate: 40 000 classic frames at 1 Mbit/s (~7 500/s) and 20 000 64-byte FD BRS frames at 500k/4M (~4 600/s):
  every frame, in order, none dropped, no stall;
- a live graph (every frame of its ID), Temporal Graph (every frame of every ID), Frame Data Analysis of 64-byte FD
  IDs (every bit's changes and sets as sent);
- a log of 75 min (450 000 frames) past rc1's clock wrap at 71.6 min: every row, times increasing across the wrap.

## What only the fake has checked

The automated suite (`tests/web/run.sh`) runs the real app build in headless Chromium, but the device, the USB link
and the bus are simulated (`tests/web/fake/rc1.js`, a port of the firmware's serial code; RESOLVE.md "Tests"). Not
checked by it, so checked only by this test:

- **Chrome's real Web Serial**: the chooser dialog, the permission remembered across visits, Chrome's own USB timing
  and buffering and unplug events (the rig's run goes through a bridge to rc1's tty instead; the mock follows Chrome's
  documented behaviour).
- **A car's bus**: error frames, bus-off, a busy bus with many nodes (the rig's bus has rc1 and the Kvaser only).
- **The download**: headless Chromium's download event; Chrome's own download UI ("Ask where to save each file"),
  your downloads folder and multi-GB logs on your PC are not (multi-GB was measured on the rig: RESOLVE.md "Tests").
- **The theme on a real OS**: the suite emulates `prefers-color-scheme`; switching the OS theme while the page is
  open is not tested.
- **Settings across browser restarts**: the suite reloads the page; quitting Chrome is not tested.
- **Other browsers**: the app was only run in Chromium.

## Known issues (2026-10-29)

- With a **screen reader** (Qt's accessibility layer switched on), unplugging rc1 makes Qt 6.12 throw an error in its
  accessibility code and the window stops drawing; reload the page. Without a screen reader unplugging is fine.
