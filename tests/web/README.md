# Browser tests: a fake resolve-can-1 and the Playwright suite

No rc1 and no rig hardware (except `rig/`, the real-rc1 tests on the rig): the tests run against a fake rc1 that speaks GVRET and LAWICEL/slcan exactly as rc1's
firmware does, behind a `navigator.serial` (Web Serial) mock injected before the page loads.

```
tests/web/run.sh                      # unit tests + the Playwright suite (projects app, download, firefox)
tests/web/run.sh --dist path/to/dist  # the app build to test (default <repo>/dist, env WSC_DIST)
tests/web/run.sh --long               # also the 60 s full-load replays (through ~/.local/share/web-savvycan/heavy.sh)
tests/web/run.sh --large              # also the multi-GB log (WSC_LARGE_GB=2,4,8; through heavy.sh)
tests/web/run.sh --screenshots        # also pictures of the app in light and dark (results/screenshots/)
tests/web/run.sh --rig                # only the REAL rc1 on the rig, with a CAN peer (rig/README.md)
tests/web/run.sh -- --project=app -g logging    # Playwright arguments after --
scripts/smoke-wasm.sh                 # the smoke spec alone (CI)
tests/desktop/run.sh                  # the desktop C++ tests (through heavy.sh), also builds nativecsv_tool
python3 tests/desktop/check_pty.py    # the desktop fake (pty) checked with resolve-can's serial_proto.py
```

Needs Node 22 and Playwright 1.56.1 (`npm ci` in `tests/web`, done by `run.sh`) with its Chromium and Firefox
(chromium-1194, firefox-1495 in `~/.cache/ms-playwright`, or `PLAYWRIGHT_BROWSERS_PATH`). The download specs need
`build/desktop-tests/nativecsv_tool` (`tests/desktop/run.sh`; else they are skipped). Output: `tests/web/results/`
(report.json, traces of failures, downloads, `fullload-*.json`).

## What is here

| Path | What |
| --- | --- |
| `fake/rc1.js` | The fake rc1, one dependency-free file for the browser and Node: the firmware's serial engines ported line by line (`firmware/proto/src/serial.rs`, `gvret.rs`, `slcan.rs`; session end on DTR as `app/src/serialport.rs`), a CAN bus with traffic modes, the Web Serial mock, an event-loop probe, a GVRET host parser and SavvyCAN's CSV parser |
| `fake/rc1-stdio.js` | The same fake on stdin/stdout, for the desktop pty |
| `unit/rc1.test.js` | The fake against the firmware's own unit-test vectors (`node --test`) |
| `unit/bridge.test.mjs` | The Web Serial bridge (`wasm/webserial/webserial.js`) in Node against the fake and the mock |
| `lib/harness.mjs` | Fixtures: the init script, console-error collection (every test fails on a console error or uncaught exception), the app driver |
| `lib/disklog.html` | The logging module (`wasm/disklog/disklog.js`) alone on a page, for `download` and `large` |
| `specs/*.spec.mjs` | The suite (below) |
| `rig/` | The real-rc1 tests on the rig: a bridge from the mock to rc1's tty, a CAN peer, their specs (`rig/README.md`) |
| `server.mjs` | Static server: `dist/` at `/`, `tests/web` at `/__harness/`, the repository at `/__src/` |
| `../desktop/rc1_pty.py` | The fake on a pseudo-terminal (`/tmp/rc1-fake`) and optionally TCP, for the desktop build |
| `../desktop/check_pty.py` | Runs resolve-can's `test/hil/serial_proto.py` (the firmware's M8 host tool) against the pty: GVRET classic+FD and slcan, both ways |

## The fake rc1

As the firmware (resolve-can `firmware` branch, version 0.1.3, USB 1209:0001 "resolve-can-1"):
- the port starts in slcan; `E7 E7` switches it to GVRET (closing a channel slcan had open); closing the port, a
  falling DTR, or a repeated "DTR and RTS low" ends the session (back to slcan, CAN channel closed);
- GVRET: one bus, FD capable and on (2 Mbit/s data), starts disabled; answers 01/02/03/06/07/09/0C/0D/16 (device
  info: build 13 = 0.1.3); `SETUP_CANBUS`/`SETUP_FD` close, set rates and reopen as the firmware does; frames 00/14
  both ways (BRS in bit 7 of the length), echo 0B; no error frames;
- slcan: every command the firmware takes (S0-S9, s, Y1-Y8, O, L, C, Z, F, V, v, N, M, m, A, X, U), frames
  t/T/r/R/d/D/b/B, BEL on refusals, error lines `s`/`e`, ms timestamps with Z1;
- when the host doesn't read, rc1's buffers fill and later CAN frames are lost (`lost` in the stats). Room before
  that: `bufferBytes` (8 KB: rc1's pipe and CAN ring, the host's tty) plus the Web Serial `bufferSize` the app opened
  with (Chrome's data pipe). The page's main thread runs the fake too, so a stalled page loses frames at rc1 just as
  a real one would. GVRET has no way to tell the host about these losses: the app's dropped counter can't see them.

Traffic modes (`window.__rc1.setTraffic(mode, {seconds, rate, garbageEvery, delayMs})`):

| Mode | What |
| --- | --- |
| `idle` | nothing |
| `leaf` | the Leaf AZE0 EV-CAN as resolve-hil's `scenarios/leaf_bus.yaml` (28 IDs, DBC lengths, 10-500 ms), ~836 frames/s |
| `classic-full` | rc1's measured full classic rate: 8-byte frames at 1 Mbit/s, 8 000/s; sequence number in data[0..3]; IDs 0x100.. in turn (`ids`, default 16; `ids: 1`: one ID at the full rate) |
| `fd64-full` | rc1's measured full FD rate: 64-byte BRS frames at 500k/8M, 6 900/s; sequence number |
| `fd-mix` | every FD length (BRS on/off, 11/29-bit), classic and remote frames, 500/s |
| `re` | known patterns for the RE tools, each ID at `rate` (100/s): 0x120 byte 0 counts, byte 1 = 0x5A, bit 19 = state & 1, bits 26-27 = state & 3; 0x121 a 16-bit little-endian triangle 1000..3000 in bytes 0-1; 0x130 random bytes; 0x18FF0001 (29-bit, FD 64, BRS) byte 40 counts, byte 63 = 0xA5, bit 270 = state & 1, the triangle at bytes 50-51. The "user action": `state` (option), `__rc1.setReState(n)` |
| `errors` | `leaf` plus an error frame every 100 ms (warning, passive, bus-off, restart, ACK, RX overflow) |
| + `garbageEvery: n` | junk bytes (never 0xF1) after every n-th GVRET frame |

The device's µs clock (GVRET frame times and the time sync answer, 32 bits, wrapping at 2^32 µs = 71.6 min as rc1's):
device option `clockStartUs`, or `window.__rc1.setClock(us)` (the clock reads `us` now and runs on; set it before
connecting), `__rc1.clockUs()` reads it. With `record` each recorded frame has its GVRET time `tsUs`.

Error and disconnect cases (`fakeOptions.serial`): `device.refuseOpen` (gs_usb owns the CAN channel), `chooser:
'cancel'`, `disconnectAfterMs` (unplugged: reads fail with NetworkError "The device has been lost.", a `disconnect`
event), `__rc1.unplug()` / `replug()`, `granted` (getPorts() already lists rc1), `requireGesture` (default on, as
Chrome), `usbIntervalMs` (default 1: reads see new data at most once per USB frame).

The mock follows Chrome: `requestPort` needs a user gesture and honours filters, `open` validates its options
(bufferSize default 255), `readable` is a byte stream whose chunks are at most `bufferSize`, `close` refuses locked
streams, `setSignals` drives the fake's DTR/RTS logic, `getInfo()` gives rc1's USB IDs.

## The app's test hook (needed from the app)

The app renders into a canvas, so the tests read its state through a small hook. **The app must set
`window.webSavvyCAN`** (e.g. from `main()` with `EM_ASM`, or in the page that loads it), in every build or behind
`?test=1`:

```js
window.webSavvyCAN = {
  version: '<git sha>',
  stats() { return {
    connected,          // a connection is up (GVRET/LAWICEL over Web Serial)
    framesReceived,     // frames parsed from the device since connecting
    framesInTrace,      // frames in the trace window now
    framesEvicted,      // frames scrolled out of the bounded trace window
    framesDropped,      // frames the app dropped (the dropped-frames counter shown in the UI)
    queued,             // frames parsed but not yet in the trace (0 when settled)
    logging, logFile, logFramesWritten,
  }; },                 // invariant checked: framesReceived = framesInTrace + framesEvicted + framesDropped (+ queued)
  theme() { return 'light' | 'dark'; },
  traceTail(n) { return [{id, ext, fd, brs, esi, bus, time, data: [bytes]}]; },   // the last n frames in the trace, oldest first; time in µs
  temporalState(),   // RE Tools > Temporal Graph: ids [{id, ext, frames, fromUs, toUs}], frames, firstUs/lastUs,
                     // binUs, columns, map size, plotted, plottables/graphs/items, memoryBytes, axis ranges
};
```

Graphs (stage 2), also on `window.webSavvyCAN`: `addGraph(id, startBit, numBits, intel, sameWindow)` (a new graphing
window with that graph, or one more graph in the newest window); `graphState()` of the newest window: `graphs`,
`points`, `lastKey`, the visible `xLower/xUpper/yLower/yUpper`, `cap` (the most points a graph keeps), `heapUsed`
(bytes malloc has handed out, `mallinfo`) and `heapSize` (the wasm memory), and per graph `series[]`: `startBit`,
`numBits`, `stored` points, `bytes`, `envelope` points, `bucket` samples, `samples` taken in, `firstKey/lastKey`,
`firstValue/lastValue`, `minValue/maxValue`; `openGraphDialog()` (Graph > Add New Graph), `graphDialog()` (open,
`gridBytes`, the text fields), `setGraphDialogField('id' | 'startBit' | 'bits' | 'frameLen', n)` (as typed);
`setMaxFrames(n)` (Preferences' maximum frames in the trace; reload after).
DBC (stage 3), also on `window.webSavvyCAN`: `dbc()` (the loaded files: messages with ID, length, FD/BRS, colours and
signals with start bit, size, byte order, type, factor, offset, limits, unit, multiplexing, value descriptions;
`interpret`; the File Manager's rows), `traceDecoded(n)` (the trace's last n rows as shown: the Data column's text and
the rows' colours), `signalViewer()` (its rows: node, signal, value), `dbcEditors()` (the open editors' fields, the signal
editor's grid size and the bits it gives a signal), `graphPoints(i)` (every point of graph i of the newest graphing
window, its DBC signal); actions where Qt's combo boxes, tables and trees aren't reachable by role: `dbcSelectFile(i)`
(the File Manager's row), `dbcEdit(msg, sig?)` (the editor's tree item double-clicked), `signalViewerPick(node, msg,
sig)` / `graphDialogPick(node, msg, sig)` (the Node/Message/Signal boxes), `traceContextMenu(fromEnd, line)` (a right
click on line `line` of a row's Data cell; the test picks the menu entry by name).
`memory()`: the page's memory: `heapUsed` (malloc's bytes in use now), `maxFootprint` (the most malloc ever had: the
peak), `heapSize` (the wasm memory), `storeBytes` and `frames` (the trace's frame store).

## How the app driver works (`lib/harness.mjs` `AppDriver`)

- Qt 6.12 puts its windows in a shadow root (`#qt-shadow-container`): `document.querySelector('canvas')` sees
  nothing. Its accessibility tree starts as one visually hidden "Enable Screen Reader" button that reacts only to a
  DOM click (a Playwright click does nothing); after that, Playwright's role locators (which pierce open shadow
  roots) reach menus, buttons, checkboxes and radios by name.
- Names as the app has them (case-insensitive prefix match): Connection > Open Connection Window, Add New Device
  Connection, radio "Serial Connection (GVRET)" / "LAWICEL / SLCAN Serial", **Choose device...**, Create New
  Connection, Save Bus Settings, Disconnect Selected Device, File > Start Continuous Logging / **Cease Continuous
  Logging**, File > **Load Log File** (the browser's file picker: `loadFile`; a file under `tests/web` goes over by
  URL, so big ones load too), Help > **About**. The theme is set
  through the test hook (`setTheme`: File > Preferences > Theme is a combo box). A widget that isn't there skips the
  test with its name.
- Combo boxes are mirrored without a usable box: the driver gives a neighbouring checkbox the focus with two real
  clicks, tabs to the combo box and types the value (QComboBox's keyboard search). Not through the popup.
- Bus Details' checkboxes are all named "Bus Details:" (their labels aren't buddies): Listen Only, Enable Bus, Enable
  CAN FD in that order. They fill in from the device once the GVRET connection is up, so they are set after that.
- After connecting the driver closes the connection window (Escape): while it is open the main window's menus don't
  react.
- Unplugging with the accessibility layer on hits a Qt 6.12 bug (`setAttribute` of undefined from Qt's
  accessibility code, the window stops drawing); the app's unplug test therefore clicks the canvas at fixed positions
  without the layer (`traffic.spec.mjs`, 1280x800 viewport).

## Tests and what they check

| Spec | Checks |
| --- | --- |
| connect | chooser filtered to rc1 from a gesture; GVRET FD / classic and slcan FD set up rc1 as asked; every FD length, BRS, 29-bit, classic and remote frame arrives exactly (`traceTail`); returning visitor (granted port, no chooser); chooser cancelled; channel refused; the New Connection dialog offers only GVRET and LAWICEL (also in the accessibility tree) |
| autoconnect | the last connection remembered and made again after a reload, without the connection window or the chooser: GVRET classic 1 Mbit/s, GVRET FD 250k/4M, slcan FD 500k/4M (rc1's bus as remembered, frames arrive); File > Preferences "Connect to the last device when the page opens" off: not connected, on again: connects; a remembered connection without a granted rc1: nothing opened, no error |
| traffic | Leaf bus 5 s: every frame counted; garbled stream resyncs; slcan error lines not counted as frames; overload (5x rc1's rate): received = trace + evicted + dropped; unplug mid-stream and reconnect |
| logging | File > Start Continuous Logging / Cease: one download named `SavvyCAN_<start time>.csv`, SavvyCAN's native GVRET CSV, its rows equal to what the fake sent (FD rows, BRS), loaded by SavvyCAN's parser (when `nativecsv_tool` is built); an empty log is the header; Choose the Log's Name When Logging Stops: Keep Logging goes on, the typed name is the download's |
| theme | light first, dark visibly darker (screenshot luminance), remembered after reload; a dark system: followed on a first visit, overridden and followed again |
| sniffer | RE Tools > Sniffer on the `re` traffic: every ID in order with the newest frame's bytes, counting bytes marked, constants never, FD 64 bytes; Notch (button) for 5 s, then the state toggled: only its bytes (0x120 bytes 2 and 3, FD byte 33) shown changed; Unnotch, Fade, a filtered ID, View Bits; every frame seen once; Never Expire / expiry; pictures light and dark. Hook: `sniffer()` (rows: id, len, data, changes, notch, ...; framesSeen), `snifferDo(action, id, on)` |
| flowview | RE Tools > Flow View: the ID's frames = the trace's (bytes, times); Stop, forward, back, timeline; a seek value stops playback at its frame; trigger bit 19, and CAN FD bit 270 of a 64-byte ID, stop it where the bit changed; 64 rows/grid/graphs for FD; graphs only around the current frame; Live Mode; light and dark. Full rate with a 50 000-frame trace: one ID at 8 000 frames/s for 30 s, every frame once (consecutive sequence numbers) within the 200 000-frame bound, probe and loss bounds, then 64-byte FD frames of the same ID (`flowview-live.json`). Hook: `flowView()`, `flowViewFrames(from, n)`, `flowViewDo(action, a, b)` |
| rangestate | RE Tools > Range State on a log (classic and FD 64 patterns): the candidates = SavvyCAN's own test (`lib/retools-ref.mjs`) on the frames written, for three searches (16-bit LE; 8..16 bits at every start bit both orders and signs; FD 16-bit at every start bit), the triangles found (FD start bit 400), nothing from random bytes, a candidate's graph (line colour per theme), no event-loop gap over 1 s; live: the ID list across ring wraps, a search on the live trace (`rangestate.json`). Hook: `rangeState()`, `rangeStateRun({...})`, `rangeStateIds(ids, on)`, `rangeStateSelect(row)` |
| discretestate | RE Tools > Single/Multi State realtime: the test holds the state the window asks for (`__rc1.setReState`): 2 states x 3 iterations give exactly 0x120 bit 19, bit 26 and FD bit 270; 3 states exactly bits 26-27 (0/1/2); from logged data: the matches = the reference search on the log, FD fields, unchecked IDs left out (`discretestate.json`). Hook: `discreteState()`, `discreteStateRun({...})`, `discreteStateIds(ids, on)` |
| retools-fullrate | all four live RE tools open at 8 000 classic and 6 900 FD64 frames/s (20 s each, 50 000-frame trace): loss < 0.1 %, no stall > 1 s, timer p95 < 100 ms (the trace alone varies 32-55 ms), heap level over each second half (< 8 MB), the sniffer saw every frame, Flow View every frame of its ID, a realtime run within its budget (`retools-fullrate.json`) |
| frameinfo | RE Tools > Frame Data Analysis with CAN FD: a log of classic, 12-byte and 64-byte FD frames with known bit patterns: the heatmap's counts, the bit histogram and the heat levels of all 512 bits (64 of a classic ID) as computed from the frames, the grid's size, a byte graph per byte, the details; 30 000 frames of one FD ID (byte graphs bounded, counts exact, time); live fd-mix: the ID list fills while the window is open, each analysis = the trace's frames of the ID; Save details to file (a download); pictures light and dark. Hook: `webSavvyCAN.frameInfo()` (its ID list, the analysed ID: frames, lengths, per bit (512) changes, sets and heat level, grid size, byte graphs, the details' first level), `frameInfoSelect(id)` (as a click on the ID) |
| smoke | the page as a visitor gets it (no fake): Qt starts, the main window is drawn, the About dialog (`exec()`, Asyncify) opens and closes with Enter, no failed request; a GVRET CSV from the computer (File > Load Log File) loads every frame and graphs every point |
| graph | a live Leaf bus: a graph of 0x1DA grows and its x range follows the newest point (Follow End of Graph); two live graphs of one ID at 8 000 frames/s far past the cap (1.5x): stored points within the cap, every frame taken in, the first time and value and every extreme kept, the envelope halved, the wasm heap level after the first fold (< 4 MB), Reset View spans the session (`graph-bounded.json`, pictures light and dark); a 600 000-frame log: within the cap, the whole file, the heap in use grows by the graph's container only (`graph-bigfile.json`); CAN FD: byte 40 of 64-byte frames (start bit 320) graphs exactly, Add New Graph's bit grid shows 64 bytes for the ID and its Motorola start bit 327 graphs the same byte (pictures light and dark) |
| dbc | stage 3 with `fixtures/stage3_test.dbc` (ours: the Leaf IDs of `leaf` traffic, CAN FD messages of 12 and 64 bytes, an extended one, signals past bit 63, 64-bit, float/double, multiplexed, value descriptions, message colours): loaded through File > DBC File Manager (the file picker); Interpret Frames: every decoded value of a log of every message = a decoder in the spec; row colours light and dark; Save Decoded Frames (text, CSV): the downloads, every value; Add New Graph by signal name and the trace's "Add to a new graphing window": every point = the decoded value; Signal Viewer live (Leaf, then fd-mix): each value = the newest frame; the editors on the 64-byte message; Save (download), Remove, Load back: the same DBC; remembered after a reload; a 1 000-message DBC with 100 000 frames timed (`results/dbc-big.json`); pictures `results/dbc-*.png` |
| timewrap | logs longer than 71.6 min: the fake's clock 30 s before its 32-bit µs wrap, a Leaf bus across the wrap: every frame in the trace and the continuous log in increasing time, spaced exactly as the device stamped them, log = trace, the graph goes on past the wrap; a GVRET CSV with times "0", "523", "999", ... loads with exactly those times |
| bisect | RE Tools > Capture Bisector on a log of 10 000 frames over 100 s with 0x7E8 (64-byte FD, BRS) only from 62 to 63 s, narrowed down as a user would: halves, Replace main list with split frames (the trace holds the split), a percentage, the ID range, the bus; each split = the frames it should hold (count, frames per ID, first/last time), the split saved (a download of SavvyCAN's GVRET CSV, all 64 bytes of each row read back); pictures light and dark. Hook: `bisect()` (main list, ID lists, the split: frames, per ID, first/last time, bytes, time taken), `bisectSet({mode, lower, frame, percent, lowId, highId, bus})` (the window's controls as typed) |
| filecompare | RE Tools > File Comparison, both sides through the window's own buttons and the file picker (`lib/retools.mjs` `nextPick`): two GVRET CSVs that differ in known IDs, bytes and bits (byte 40 and bit 511 of 64-byte FD frames), a candump log added to side 2: the tree's IDs on one side only, per shared ID its bits and per-byte values on one side only (64 byte nodes for FD), "Show only data unique to Side 1", Save Details to File, Clear Frames; the summaries' memory; pictures light and dark. Hook: `fileCompare()` (each side's frames, IDs, memory; the tree as `{t, c}`; load and compare times) |
| dbccompare | RE Tools > DBC Comparison, both DBCs through its own buttons and the picker: nodes and messages on one side only, signals missing on either side (one at byte 63 of a 64-byte message), moved / resized / rescaled signals, Save Details to File; 1000 messages each with a signal only in the second file (SavvyCAN's loop on the wrong index): all listed, < 1 s; pictures light and dark. Hook: `dbcCompare()` |
| search | RE Tools > Frame Search on a log of classic and 64-byte FD frames with known content: a value at byte 40 (hex), a Motorola 12-bit pattern ending at bit 511 (binary), bytes 6-7 = 0 (short frames don't match), scale and bias, any change of byte 50, Tx / length filters, an invalid value; each result row for row; the first match selected in the trace, Next / Previous; the bit grid 64 bytes; pictures light and dark. Hook: `search()` (matching rows, count, current, label, time, grid bytes, the trace's selected row), `searchSet({id, len, dir, mode, startBit, bits, intel, signed, scale, bias, value})` |
| retools-big | the four tools on a 1-hour log of 600 000 frames (30 IDs, 29-bit, FD 64): bisector open and split (percentage, ID) exact, split memory < 100 bytes a frame; frame search exact; file comparison of the log and a variant (2 x 600 000 frames through the picker) exact, < 5 MB kept after; every step's longest event-loop pause < 1 s (`results/retools-big.json`) |
| loader | the leaner log loader: GVRET CSVs of 600 000 and 3 000 000 frames (11- and 29-bit, classic, CAN FD 12 and 64 bytes with BRS, Tx) through File > Load Log File: every frame in the trace, the newest 40 exactly as written, the load time (< 5 s, < 15 s) and the heap (< 150 bytes a frame after, the peak in use and malloc's footprint bounded, polled while loading), then Temporal Graph, a graph and Frame Data Analysis on the whole log (`results/loader-600k.json`, `loader-3M.json`); `@large`: 14 M frames (~1 GB, the load limit; `WSC_LOADER_LIMIT_FRAMES`); candump (classic, FD) and CRTD still load by autodetect; a broken CSV asks to salvage (No keeps the trace, Yes loads the rows that could be read); another format over 200 MB is refused with a message |
| temporal | RE Tools > Temporal Graph on a 1-hour log of 600 000 frames (30 IDs, 29-bit, CAN FD 64): every ID and its frames, the whole hour, an ID that starts half way, one colour map under 1 MB, open in seconds with no event-loop gap over 1 s; live (Leaf, then 20 s of FD64 at 16 000/s with a 50 000-frame trace ring): grows, follows the end, every frame counted once across the ring's wraps, memory flat; light and dark pictures |
| download (`download`, `firefox`) | the logging module alone: rows written in batches, one download named by the start time, byte for byte SavvyCAN's own rows (`nativecsv_tool gen`) and loaded by SavvyCAN's parser; `_blob()` while logging; the name given at stop; an empty log; a second log; "leave site?" while logging |
| fullload `@long` | 60 s at 8 000 classic / 6 900 FD64 frames/s: accounting, rc1-side loss < 0.1 %, 10 ms timer lateness p95 < 50 ms, no stall > 1 s; `results/fullload-*.json` |
| large `@large` | gigabytes of rows through the logging module, one download: size and SHA-256, JS heap, browser PSS and profile size every 256 MB; `results/large-*.json` |
| screenshots `@screenshots` | pictures only: main window with traffic, RE Tools menu, About, Preferences, light and dark |
| every test | no console error, no uncaught exception |

Without `dist/SavvyCAN.html` every `app` test is skipped with that reason (the smoke spec fails). `run.sh` uses port 8751 for its server and
never reuses one already there (another worktree's server would serve its own harness and build): set `WSC_PORT`
when two run at once.

## One place for the tests (simplification review, docs/SIMPLIFY.md E1-E6)

Everything is here and in `tests/desktop`: one `package.json`, one Playwright config, one server, one fake rc1, one
`navigator.serial` mock, one app driver. Gone: the reference client (`selftest/`; the specs run against the app
only), `test/web` (its download module spec and the multi-GB spec moved here, the app download spec merged into
`logging`, the live-to-disk specs went with that code to branch `disklog-live`), `wasm/webserial/tests/e2e.mjs`
(`connect` and `traffic` cover it), the C++ fake rc1 (`fakerc1.h`, `tst_webserial_connections`), the smoke test's
own server and Playwright install (`scripts/smoke-wasm.cjs`), the screenshot script's own driver and fake GVRET
device. The desktop C++ tests (`test/frames`, `test/nativecsv`, `tst_webserialport`) are one CMake project in
`tests/desktop` with one `run.sh` (plain, address, thread).

## The desktop fake

`tests/desktop/rc1_pty.py --link /tmp/rc1-fake --traffic leaf` gives a pty that is rc1's serial port (same fake, run by
node). Notes for the desktop build:
- `QSerialPortInfo::availablePorts()` does not list ptys, and `GVRetSerial` opens `QSerialPort(QSerialPortInfo(name))`,
  which is empty for an unlisted name: to test the desktop build on the pty, the connection must open a typed-in path
  (`QSerialPort::setPortName("/tmp/rc1-fake")`);
- DTR/RTS ioctls fail on a pty: SavvyCAN's "ESP32 serial mode" would report an unsupported operation (its standard
  mode works); the session ends when the app closes the pty, as rc1's ends on DTR falling;
- `--tcp 2323` serves the same device for SavvyCAN's "Network connection (GVRET)" (which connects to port 23: forward
  it, or run in a network namespace).
