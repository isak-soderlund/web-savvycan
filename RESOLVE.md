# web-savvycan

The Resolve fork of SavvyCAN (collin80/SavvyCAN, branch `QT6WIP`, forked at 9c3b0af on 2026-10-28) for
**resolve-can**: SavvyCAN in the browser (Qt for WebAssembly, Web Serial) and on the desktop.

- **Licence:** GPL, as SavvyCAN with QCustomPlot.
- **Public** GitHub fork `isak-soderlund/web-savvycan` (owner 2026-10-28), so every build we offer has its GPL source.
- **Upstream:** worked on completely separately. No pull requests, issues or contact upstream (owner).
- `main` is ours (started from upstream `QT6WIP` 9c3b0af); upstream's own branches are kept as they came with the fork.

## Where the requirements and plans are (repo `isak-soderlund/resolve-can`)

| Document | What |
| --- | --- |
| `docs/requirements.md` SW-10 | Exactly SavvyCAN's interface, exceptions only for CAN FD and browser limits; CAN FD out of the box; light/dark switch; logging straight to disk; no crash on a busy bus |
| `docs/savvycan-fork-plan.md` | Decisions and the staged rollout (connection + logging, graphs, DBC, RE tools, ...) |
| `docs/research/savvycan-improve.md` | Root causes of the crashes and slow graphs (G, M, W ids), FD gaps, theming |
| `docs/research/savvycan-web.md` | WebAssembly spike (patch + screenshot), browser limits, logging, hosting |

## MVP: log from rc1 in the browser (stage 1)

The smallest version that is useful: connect to rc1 from Chrome, watch the frames, log them to disk.

| # | Feature | Notes |
| --- | --- | --- |
| 1 | **WebAssembly build** | Qt 6.12 `wasm_singlethread` + Emscripten 5.0.5 (the spike's toolchain); Qt SerialBus and MQTT left out of the wasm build; Asyncify for the dialogs that use `exec()`; the desktop build of the same tree keeps working |
| 2 | **Web Serial port** | A `QSerialPort` stand-in backed by Web Serial (SavvyCAN uses ~15 of its calls: open, baud, DTR, readAll, write, readyRead, errorOccurred, ...), so the GVRET/LAWICEL code stays as it is. A "Choose device" button calls `requestPort()` (needs a user gesture), filtered to rc1's USB ID |
| 3 | **Connection window** | SavvyCAN's own dialog, offering GVRET and LAWICEL (slcan) on the Web Serial port; the other connection types hidden in the browser |
| 4 | **Single-threaded GVRET** | No `QThread` in the wasm build (the spike's three changes); incoming bytes parsed in batches from the JS read loop; no per-byte debug string (fix M-series) |
| 5 | **Bus setup** | Nominal bit rate, CAN FD on/off, data bit rate, listen-only, through GVRET's own commands (rc1 firmware 0.1.3 implements them, classic + FD verified with desktop SavvyCAN on 2026-10-28) |
| 6 | **Main window trace** | Live frame list with CAN FD (64-byte data, BRS/ESI columns), frame counter, **dropped-frames counter**; ring-buffer frame store and an index list for the filtered view (fixes M-series), so it never stalls on a full list |
| 7 | **Logging (download)** | Owner's MVP choice 2026-10-28: File > Start Continuous Logging starts at once; the rows are collected in the page as Blobs, outside the wasm heap (the browser keeps them; Chrome pages big totals to disk); Cease Continuous Logging starts **one normal download** of the CSV, named by the start time (`SavvyCAN_YYYY-MM-DD_HH-MM-SS.csv`), or by a name asked at stop (File > Choose the Log's Name When Logging Stops...). The LOGGING label shows size and frames and that a crash loses the log (accepted); closing the tab while logging asks first. Same in every browser. Format: SavvyCAN's native GVRET CSV, FD rows included. Writing to disk while logging (File System Access, commits every 3 s, a remembered folder; Firefox streaming download through a service worker) is on branch **`disklog-live`** (complete and tested there, = `main` 2d52b34), not in `main` |
| 8 | **Theme switch** | Light (SavvyCAN's normal) / dark, remembered between visits |
| 9 | **Only stage-1 windows** | Every menu and window not in stage 1 hidden (not broken); they come back stage by stage |
| 10 | **Settings kept** | Connection, theme and log settings remembered in the browser (whether `QSettings` persists in wasm: to check) |
| 11 | **About / licence** | GPL notice, SavvyCAN's and QCustomPlot's credits, the source link once published |

**Tests:**
- **Without hardware:** a fake GVRET device (resolve-can's `test/hil/gvret_fake.py`) and unit tests for the
  Web Serial stand-in and the parsers.
- **On the rig:** rc1 with the Kvaser as the peer, classic and CAN FD. Every frame the Kvaser sends must appear in
  the trace and in the log file. Done: `tests/web/run.sh --rig` (`tests/web/rig/README.md`): the real rc1 behind the
  suite's Web Serial mock through a bridge to its tty, the Kvaser Leaf v3 sending known frames; connect, CAN FD, log,
  auto-connect, a real unplug, full rate, graphs and RE tools passed 2026-10-29 (8a5145d).
- **Where:** browser tests in `tests/web` (`run.sh`: the fake rc1 (`fake/rc1.js`), the app in headless Chromium,
  the logging module alone in Chromium and Firefox; `--long`, `--large`, `--screenshots` opt-in), desktop C++ tests in
  `tests/desktop` (`run.sh plain|address|thread`, through `heavy.sh`); the smoke test is `scripts/smoke-wasm.sh`
  (`tests/web`'s smoke spec, also in CI). Builds: CMake only (`scripts/build-*.sh`; `SavvyCAN.pro` is upstream's, unused).
- **Logging:** `tests/web` download and logging specs: the download's bytes equal SavvyCAN's own rows and load in the
  desktop SavvyCAN's parser (`nativecsv_tool`, `tests/desktop`), Chromium and Firefox; the app with the fake rc1 (every
  frame sent is in the download); a multi-GB log (`run.sh --large`, through `heavy.sh`). Writing to disk while logging,
  and its tests, are on branch `disklog-live`.
  Measured 2026-10-28 (headless, rows copied out of a fixed "wasm heap" buffer, 3G `heavy.sh` slice): page JS heap
  1 MB and wasm heap flat throughout; collecting at ~520-590 MB/s.
  - **Chromium:** 2, 3, 4, 6 and 8 GB logs downloaded byte-exact (SHA-256); its blob storage holds ~2 GB in RAM
    (browser PSS peak 2.2-2.6 GB) and pages the rest into the profile's `blob_storage` on disk; 8 GB downloaded in
    53 s. One of three 4 GB runs ended with the download "canceled" (not seen again in 7 later runs up to 8 GB).
  - **Firefox:** the log stays in RAM (~1.1x its size while logging) and the download roughly doubles it: 0.5 GB
    fine (peak 1.4 GB); 1 GB was killed by the 3G slice at the download. Firefox's limit is the PC's free RAM / 2.
- **Sanitizers:** AddressSanitizer and ThreadSanitizer runs of the desktop build of the same tree.

## Stage 2: graphs (started 2026-10-29, owner)

Switched on so far (`enabledfeatures.h`): **Graphs** and, brought forward from stage 7, **LoadLog**.

| What | How |
| --- | --- |
| RE Tools > New Graphing Window | SavvyCAN's graphing window. In the browser a framed dialog (title bar, close button), as the connection window: a `Qt::Window` would be frameless over the main window's menus |
| Live graphs | A graph of a frame ID grows as frames arrive and **follows the newest data** by default (View > Follow End of Graph in the graph window, as SavvyCAN has it, now on from the start). While following, the value axis grows to take in new values (never shrinks) |
| A log file from the computer | File > Load Log File opens the browser's own file picker (Chrome: `showOpenFilePicker`); the file type is detected from the contents (SavvyCAN's autodetect: GVRET CSV, candump, PCAN, Vector ASC/BLF, ...). SavvyCAN's own GVRET CSV is parsed straight from the bytes the browser hands over (the leaner loader, below); other formats go to the in-memory file system for SavvyCAN's loaders and are deleted after. **GVRET CSV up to 1000 MB** (~14 M frames: 27-36 s, 0.8 GB of the page's memory after, 2.1 GB at the peak); **other formats up to 200 MB** (SavvyCAN's loaders: a 1 M-frame candump log, 46 MB, 18 s and 254 MB at the peak) |
| Time axis | In the app's time style (File > Preferences: microseconds by default, or seconds, or clock) |
| **Bounded graph storage** (G6) | A graph's points live once, in the plot's own container (no `GraphParams` x/y copy, no copy of the matching frames). **Cap: 400 000 points a graph, 6.4 MB** (16 bytes a point; the container is allocated once at 412 500 points, 6.6 MB, so it never grows after the first fold). The newest **200 000 samples are kept exactly** (a 100 Hz signal: 33 min; rc1's full 8 000 frames/s on one ID: 25 s); older ones are folded, 50 000 at a time, into a **min/max envelope** of at most 150 000 points: two per bucket, at the bucket's first and last time, carrying its smallest and largest value in the order they came, so the buckets tile the time axis and the session's first and last time and every extreme stay. When the envelope is full, neighbouring buckets merge (buckets of 4, 8, 16, ... samples). A 10-hour session of a 100 Hz signal: buckets of 64 samples (0.64 s), finer than a pixel when the whole session is on screen. Live updates stay O(new points): a fold copies at most the cap once per 50 000 new samples. `re/graphingwindow.cpp` `boundGraph()` |
| Min/max envelope on screen (G8) | QCustomPlot's adaptive sampling (on, as QCustomPlot's default; now set explicitly) draws each pixel column's min and max once a graph has 2 or more points per pixel (`QCPGraph::getOptimizedLineData`); with the stored envelope that picture is exact for the whole session. Zoomed into a folded part you see its envelope (extremes at most one bucket off in time), not the samples; Save Spreadsheet writes what is stored (envelope rows there) |
| Other graph fixes | Value-table brackets bounded to the newest 500 a graph (G5); Follow End of Graph follows the newest point of any graph, not the last graph's (G9); frames not yet announced to the windows are left to the next tick (a graph made on a live bus had its newest frames twice, and every open graph took a loaded file twice); remote/error frames not graphed live (as from the trace); a window made before its frames came scales to them; Reset View from the plot's data; the graph window's menu bar shows its menus in the browser (at its 20 px everything went into "»") |
| CAN FD bit picker | Add New Graph's bit grid covers the frame: 8, 16, 32 or 64 bytes (CANDataGrid's layouts), from the ID's newest frame in the trace (the last 100 000 frames), the Frame Length field, the ID's DBC message and the bits picked, whichever is longest; never less than SavvyCAN's 8. Start bit 0..511, length 1..64 (as before; `processIntegerSignal` reads up to bit 511). The grid's text is sized by its cells (unchanged at 8 bytes) |
| Colours | Default line light grey in dark; random line colours (Add New Graph, the trace's "Add to graph") from the light half in dark; the colour swatches' "CHANGE" black or white by the colour; the selected graph's pen, the position text "X: Y:" and the tracer themed; the bit grid in dark (dark empty cells, light set bits, grey outlines, text by cell colour) |
| Owner's other changes | No View menu (the theme is in File > Preferences > Theme); no "Press F1 on any screen for help" (the help pages aren't in this build) |

Measured (headless Chromium, `graph.spec`, 2026-10-29): two live graphs of one ID at 8 000 frames/s for 75 s (614 000 samples each, 1.5x the cap, the envelope halved once to buckets of 8): at most 370 000 points stored each; the wasm heap in use level from the first fold on (47.2 -> at most 47.4 MB over the last 40 s, with the trace set to 100 000 frames), memory 86 MB. A 600 000-frame log (600 s at 1 ms) graphed: 300 000 points (100 000 envelope, buckets of 8), the heap in use +6.3 MB for the graph (its container; before: 32 bytes a point plus a copy of every matching frame, ~40 MB here). Zoomed out over the whole session a sawtooth is the full band; zoomed into the folded part it is still the sawtooth at 1 ms buckets (`results/graph-bounded-*.png`).

Left for later: files larger than memory (over ~1 GB of CSV): graphing straight from the file (only the graphed signals kept), not loading every frame. Decoding off the UI thread and replotting at most at the screen rate (G7).

Tests: `tests/web` smoke spec (in CI) loads a GVRET CSV through the file picker and graphs it (every frame, every point); `graph.spec` graphs a live Leaf bus (grows, follows, value axis takes in 0..255), a live graph past its cap (bounded, whole session kept, heap level), a 600 000-frame log (bounded, whole file, heap), and CAN FD signals past byte 8 (the test hook and the Add New Graph dialog with its 64-byte grid, light and dark).

## Frame Data Analysis (stage 4, switched on 2026-10-29)

RE Tools > Frame Data Analysis ("the heatmap tool", owner 2026-10-29: brought forward from stage 4), working with CAN FD.

| What | How |
| --- | --- |
| The window | SavvyCAN's: the ID list, the details tree (lengths, intervals, per byte changed bits, range, value histogram; Bitfield Histogram; Bitchange Heatmap; J1939/GMLAN for 29-bit IDs), the bit-change **heatmap**, the bit histogram, the interval histogram and a graph per byte. In the browser a framed dialog, as the graphing window, first opened at the .ui's 1142 x 651 (SavvyCAN's remembered-size default 794 x 694 is too narrow for a 64-byte heatmap) |
| CAN FD (fix W1) | Every per-byte and per-bit table holds 64 bytes / 512 bits (SavvyCAN's `minData[8]`, `dataHistogram[256][8]`, `bitfieldHistogram[64]`, `bitFlipHeat[64]`, ... overflowed on frames over 8 bytes); the heatmap grid as large as the longest frame (`CANDataGrid` 8x8, 16x8, 16x16 or 32x16 bits); a byte graph for every byte (8 for classic as SavvyCAN, up to 64 in a scroll area); the bit histogram over every bit. Also fixed: a byte's reference is the first frame that has it (an ID with longer and shorter frames read uninitialised bytes), "Data Length" no longer "8 to 64", the bit histogram drew every bit twice |
| Live | As SavvyCAN: an ID is analysed when it is selected (and the first one when the window opens); new IDs join the list as frames come (from the trace's newest frames, so the ring-buffer trace works; every new ID of a tick, SavvyCAN stopped at the selected one). Select the ID again for the newer frames. Memory: the window keeps the ID set and the last analysis only |
| Large captures | No copy of the ID's frames (their rows, 8 bytes each, during the analysis); byte graphs of at most 8 000 points (past 4 000 frames each pair is a run's min and max); one analysis as the window opens (was two). 30 000 64-byte frames of one ID: 0.56 s in headless Chromium (2026-10-29), the UI waits that long |
| Save details to file | In the browser the text goes to the browser as a download (`FrameDetails.txt`) |
| Colours | Byte graph colours per theme (light: SavvyCAN's; dark: lighter, light grey for black); plots themed; heatmap bit numbers dark or light by the cell's colour (black text on the black "no change" cells could not be read) |
| Bit grid | `candatagrid.cpp/.h` taken unchanged from branch `graph-bounds` (c15934c: FD text sizes, dark theme, buffers cleared past the frame), plus the heatmap's text colour (only in `HEAT_VIEW`) |

Limits: like SavvyCAN, an ID is one number (an 11-bit and a 29-bit ID of the same value are one entry); the
analysis is a pass over the whole trace on the UI thread (a few 100 ms per million frames, more for big FD IDs);
Save details re-analyses every ID; the heatmap grid of a 12/20/24/48-byte ID shows the unused bits of the next size.

Tests: `tests/web` `frameinfo.spec`: a log of classic, 12-byte FD and 64-byte FD (29-bit, BRS) frames with known bit
patterns (byte 40 bit 3 toggling every frame, byte 63 constant, ...): the heatmap counts, bit histogram and heat
levels of all 512 bits (64 of the classic ID) equal those computed from the frames, grid size, byte graphs, details;
30 000 frames of one FD ID (bounded byte graphs, exact counts, time); live fd-mix traffic (the list fills while the
window is open, analyses equal the trace's frames, twice); Save details to file. Pictures in light and dark.

## Stage 4: live RE tools (switched on 2026-10-30)

RE Tools > **Sniffer**, **Flow View**, **Range State** and **Single/Multi State**: SavvyCAN's windows and controls,
with CAN FD, the dark theme, bounded memory and live updates on the ring-buffer trace. What they share (no UI, tested
natively): `re/retools.{h,cpp}`.

| Tool | What it does | What changed |
| --- | --- | --- |
| Sniffer (`re/sniffer/`) | Every live ID with its newest bytes; bytes going up green, down red; Notch remembers the bits that changed in the last notch period and no longer colours them; Mute, Fade, Never Expire, View Bits; ID filters; IDs vanish after the expire interval | Frames straight from the connection (`CANConManager::framesReceived`: the trace's ring doesn't matter). CAN FD: every per-ID buffer initialised for 64 bytes (only the first frame's length was: a longer frame later compared uninitialised bytes); times with their seconds. Colours chosen by the theme when drawn (it read the palette once); bit cells in dark: light set, dark clear, grey notched. Rows looked up in a vector (`index()` walked the map per cell). Error frames not listed as IDs, remote frames don't empty an ID's bytes; at most 4096 IDs (more are counted, not shown); a new ID shown while filtering (it was announced in the wrong row order); no null item when filtering an expired ID |
| Flow View | One ID's frames one by one: playback (back, stop, forward, reverse, pause, play, speed, loop), timeline, the byte table (value, reference, seek value, graph on/off, colour), the bit grid (changes against the reference), byte graphs around the current frame, trigger bits (click the grid), Live Mode, Auto Reference, graph by time | The ID's frames kept compactly (`IdFrames`: time, length, payload in rows of the longest frame) and **bounded to the newest 200 000** (was a `QList<CommFrame>` of every frame forever). The graphs hold only the frames around the current one (+-2x the chart range; the view is always +-1x, no zoom), rebuilt when the view leaves them (were all frames in up to 64 graphs, twice; live frames all at one x). Live: new frames by the newest frame taken (not rows count-n). FD: trigger bits past byte 7 (the 64-bit chunks were indexed by byte*8: read past the frame), a longer frame later adds its rows/graphs; the Ref column sets the reference (it set the seek value). No crash with no frames (Stop, Live Mode, the timeline read frame 0 or -1). Byte colours per theme (shared with Frame Data Analysis), the "+" in the theme's colour. Browser: "Save image to file" downloads a PNG |
| Range State | Candidate signals that range smoothly (sensitivity, min/max size, granularity, byte order, signedness, ID filter); a candidate's graph | SavvyCAN's test unchanged (`rangeSignalLooksGood`, line for line, checked against the original natively), 64-bit (`1 << 32` and the values overflowed). One pass over the trace into compact rows per ID (was a full scan and a `reserve()` of the whole trace per ID). FD: start bits up to 511 (packed into 8 bits, they ran into the length). No crash for an ID whose frames left the trace. The event loop every 100 ms while it searches. Line in the theme's colour |
| Single/Multi State | Bits that follow a user action. **Realtime:** "Go to state N" for each state in turn (countdown, hold, wait), Iterations times; **From logged data:** fields with exactly Number of States values | SavvyCAN's window had **no search** (`calculateResults()` empty in both modes) and its state counter ran one past. Now: realtime records each ID's frames per state (after the first quarter of each hold, the time to get there; `StateSampler`: an even spread of at most 2048 frames per ID and state, all within 32 MB); a field of Min..Max bits (Intel numbering) matches when in every state >= 90 % of its frames have one value and the values differ between states. Logged: fields with exactly N values over the trace. Both: only fields whose first and last bits change, smallest first, none containing a match (a toggling bit is reported once). Up to 64 bytes. Status colours lighter in dark |

All four: a framed dialog in the browser; -1/-2 (Clear, a file loaded) rebuild from the list (W2); the ID lists fill
from the trace's newest frames. `testhook_retools.cpp` (own file): `sniffer()`, `flowView()`, `rangeState()`,
`discreteState()` and their actions.

Measured (headless Chromium, the fake rc1, 2026-10-30; `results/flowview-live.json`, `retools-fullrate.json`,
`rangestate.json`, `discretestate.json`):
- **Flow View at full rate:** one ID at 8 000 frames/s for 30 s with a 50 000-frame trace (190 625 frames evicted):
  its cache kept the newest 190 000 (50 000 dropped from its front in eighths), every frame once (consecutive sequence
  numbers), 3.4 MB; timer p95 21-23 ms, max 67-74 ms, nothing lost at rc1. Then 64-byte FD frames of the same ID:
  184 000 kept, 14.6 MB (the bound is 200 000 x (64 + 9) bytes).
- **All four open at full rate** (Sniffer, Flow View in Live Mode, Range State, a realtime Single/Multi State run;
  50 000-frame trace): classic 8 000/s for 20 s: nothing lost, timer p95 28 ms, max 88 ms, heap in use 33.4 ->
  36.5 MB; FD64 6 900/s for 20 s: nothing lost, p95 48-54 ms (34 ms with no tool open; Flow View on a 64-byte ID adds
  ~16-20 ms a GUI tick: 64 graphs and the 512-bit grid redrawn 4 times a second), max 106 ms, heap 41.9 -> 51.0 MB
  (the realtime recording 3.9 MB, Flow View's cache 2.4 MB). The sniffer saw every frame once.
- **Range State** (1 000 frames per ID): 16-bit at every start bit of a 64-byte ID, 4 kinds (2 048 tests): 92 ms;
  3 classic IDs, 8..16 bits at every start bit, 4 kinds (20 736 tests): 315 ms; no event-loop gap over 200 ms.
- **Single/Multi State:** realtime, 2 states x 3 iterations of 1 s on the `re` traffic: exactly the three planted
  fields (0x120 bits 19 and 26, FD bit 270), 1 684 frames recorded (62 KB), the search 3 ms; 3 states: exactly bits
  26-27.

Tests: `tests/web` `sniffer`, `flowview`, `rangestate`, `discretestate`, `retools-fullrate` specs (the fake's new `re`
traffic: a counter, a constant, a toggled "state" bit and a 2-bit state field, a triangle signal, random bytes, the same
in a 29-bit 64-byte FD frame); each tool's results against what was sent (Range State and logged Single/Multi State
against reference implementations in `lib/retools-ref.mjs`, SavvyCAN's processSignal written out); pictures light and
dark. `tests/desktop` `tst_retools` (11 tests, also ASan/UBSan): new frames across ring wraps, the bounded rows, Range
State's test equal to SavvyCAN's original on random walks, the discrete searches, the sampler's budget, the sniffer
item with lengths 8 <-> 64.

Limits: like SavvyCAN, an ID is one number in Sniffer, Flow View and Range State (an 11-bit and a 29-bit ID of the
same value are one entry; Single/Multi State keeps them apart). Flow View keeps the newest 200 000 frames of its ID
(25 s of one ID at rc1's full rate; 33 min at 100 Hz). Range State's search is SavvyCAN's: O(sizes x start bits x
kinds x frames) on the UI thread, with the event loop every 100 ms (the window waits; a 64-byte ID at granularity 1
takes seconds). Single/Multi State realtime uses Min/Max Bits too (SavvyCAN greys them out there; they keep their
values, 1..8); a field can't be over 8 bits (the spin boxes' limit). The sniffer keeps at most 4096 IDs.

## Done: logs longer than 71.6 minutes (2026-10-29, Next item 1)

GVRET sends a frame's time as 32-bit µs, so 2^32 µs (71.6 min) after the device's clock started the times began
again near 0 (upstream SavvyCAN with any GVRET device). Fixed in the app only; the GVRET protocol and rc1's firmware
are unchanged, so rc1 stays usable with standard SavvyCAN. Goal: whole CCS charging sessions (hours).

| What | How |
| --- | --- |
| 64-bit device time | `GVRetSerial::unwrapDeviceTime()`, one per connection (one device clock for all its buses), classic and FD frames and the time sync answer: the session's first device time is the anchor; with the host's monotonic clock since then the app knows roughly where the device clock is now and takes the raw time as the 64-bit time nearest to that (within half the 32-bit range, ±35.8 min). So a time far below the expected one is one more wrap, also after a silent bus of any length; a frame a little older stays a little older. Reset when a session starts (`deviceConnected`) |
| Time sync in 64 bits | `rebuildLocalTimeBasis()` and `timeBasis` were int32: a device time past 2^31 µs at the sync (36 min) gave times 2^32 µs off. Now 64-bit; without a time sync answer the frames keep the device's own (unwrapped) times |
| Log loader | `loadNativeCSVFile` took a time shorter than 4 characters as "no time" and made one up: frames in a log's first millisecond ("0".."999") moved. Any number is now the time; made-up times only for an empty or non-numeric field |
| Tests | `tests/web` `timewrap` spec: the fake's clock 30 s before the wrap (`__rc1.setClock`), a Leaf bus across it: trace (`traceTail` now has `time`), continuous log and a graph in increasing time, spaced exactly as the device stamped them; a CSV with times 0, 523, 999, ... loads with exactly those. `tests/desktop`: `tst_parsers` `gvret_timeWrap`, `gvret_timeSyncBeforeWrap`; `nativecsv_tool shorttimes` |

Seen with the real rc1 on the rig (2026-10-29, 8a5145d, `tests/web/rig` timewrap): 75 min of logging at 100 frames/s
from the Kvaser, 450 000 rows in the download, all in order, none missing; times from 11.7 s to 4 511.6 s (past 2^32 µs
= 4 295.0 s), 10 000 µs apart across the wrap as before it, never going back (steps 0.2-19.9 ms: the peer's timing).
`MQTT_BUS` (not in the browser build) has the same
int32 time basis as upstream's GVRET had.

## Temporal Graph (stage 4, switched on 2026-10-29)

Brought forward from stage 4 (owner 2026-10-29): RE Tools > Temporal Graph, which IDs appear when.

| What | How |
| --- | --- |
| The crash | SavvyCAN's map was ID range / 30 x seconds x 4 cells, sized in `int` in QCustomPlot (G1, G2): 20 000 frames of J1939-style 29-bit IDs over 60 s ask for 1.6 G cells and the page aborts ("Aborted()", measured on the unchanged window 2026-10-29); plus a circle per frame redrawn on every pan and zoom (G3), and no live update (G4) |
| The map | `TemporalDensity` (`re/temporalgraphwindow.h`): frame counts per (time column, ID). At most **512 columns**, each a power of two µs wide; the width doubles as the capture grows (columns added in pairs), so 10 s and 10 h both fit. **One row per distinct ID** (11-bit and 29-bit apart, in ID order, labelled in hex), at most 2048 rows; frames of further IDs are counted and named in the summary line, not drawn. About 2 KB of counts per ID plus the map's cells (12 bytes each): ~0.25 MB for 30 IDs, whatever the number of frames |
| Drawing | One `QCPColorMap` of the counts (log colour scale, empty cells transparent), a summary line (IDs, frames, span, column width); no point per frame. One gradient (blue to red) for light and dark; the plot is themed as every plot |
| Live | `framesUpdated(n)` adds the new frames: O(new frames) per GUI tick. New frames are found by the newest frame counted, not by an index (the trace is a ring; a filtered list's tail isn't counted twice). The counts keep the **whole session**, also after the trace's ring has dropped the oldest frames. -1/-2 (cleared, a file loaded, a new set) count the list again, in slices of ~25 ms, so the page keeps running on a big capture; the ">2000 frames, may crash" question is gone. The view shows the whole capture and follows it until the user zooms or pans (R: whole capture again) |
| Browser | A framed dialog (title bar, close button), as the graphing window. Time axis in the app's time style |
| QCustomPlot | `QCPColorMapData`: cell count in `size_t` and capped at 2^31, no cell access without an array, `fill()` of any value (it was a `memset` of a double) |
| Limits | The map's resolution is the column width (span / 256..512): zooming in shows wider cells, not finer ones. Over 2048 distinct IDs, the rest are counted, not shown. In overwrite mode with the filtered list (Main/UseFiltered) live counts follow the trace's rows, not the bus |

Tests: `tests/web` `temporal.spec`: a 1-hour log of 600 000 frames (30 IDs: Leaf, J1939-style 29-bit, 11- and
29-bit CAN FD 64 bytes with BRS; one ID only in the second half hour) loaded and the window opened: every ID with its
frame count, the whole hour on the time axis, 0x7E8 from the second half, one plottable, < 1 MB; it opens in
0.4 s with no event-loop gap over 253 ms (the 40 MB file itself loads in ~1.5 s through File > Load Log File since the leaner loader; ~70 s before). Live: a Leaf bus, then 20 s of 64-byte FD at
16 000 frames/s with a 50 000-frame trace ring: every frame counted exactly once across the ring's wraps (323 344
frames, 275 000 of them evicted from the trace), 45 IDs, memory flat (0.35-0.40 MB for counts and map). Pictures in light and dark.
A native harness of the same code (Qt 6.4, offscreen) built the 600 000 frames in 77 ms and counted 1.19 M live frames
across 1.14 M ring evictions exactly.

## Next (owner 2026-10-29)

Done: **a faster, leaner log loader** (branch `fast-loader`, owner's item 1 of 2026-10-29). Measured in headless
Chromium (tests/web `loader.spec`, `results/loader-*.json`), before (main a9c210a) -> after:

| GVRET CSV | Load time | Heap in use after, a frame | Peak while loading (in use / wasm memory) |
| --- | --- | --- | --- |
| 600 000 frames, 41-45 MB | 54 s (18 s without the screen reader layer) -> **1.1-1.3 s** | 1 470 B (130 B without) -> **66 B** | >= 0.89 / 0.97 GB (without: >= 85 MB / 0.27 GB) -> **75 MB / 155 MB** |
| 3 000 000 frames, 207 MB | refused (over 100 MB; would not have fit) -> **5.9-6.3 s** | -> **59 B** (185 MB) | -> **0.34-0.39 GB / 0.5 GB** |
| 14 000 000 frames, 976 MB (`@large`) | -> **27-36 s** | -> **57 B** (0.81 GB) | -> **1.6 GB / 2.1 GB** |

On the 3 M log, Temporal Graph opens in 0.7-1.0 s, a graph of one ID takes 0.5-0.6 s, Frame Data Analysis of the 64-byte
FD ID 2.6-3.1 s (14 M: 2.6-3.2 s, 1.3-2.3 s, 6.4-8.4 s; the heap 0.86 GB with all three). Where it went, and the fixes:
- **The screen reader layer** (on in every browser test; for a real screen reader user too): when a child of the trace's
  table (its scroll bar) is shown, Qt's browser accessibility goes over every child of the table, and Qt's table interface
  makes and keeps an interface for each cell: ~1.3 KB and ~60 µs a frame. The trace, its headers and viewport are plain
  accessible widgets now (`traceAccessible`, `mainwindow.cpp`, browser only): one "Frames" element, no cells.
- **The CSV parser:** `readLine`, `simplified`, `split(',')` and `toInt` per field, ~75 allocations a row (malloc was a
  third of the time natively), and `processEvents` every 100 rows, which in the browser suspends to the page each time
  (Asyncify). Now parsed in place from the bytes (`FrameFileIO::loadNativeCSVData`, the same frames bit for bit as the old
  loader, checked on fuzzed files natively: 2.1 s -> 0.12 s for 600 000 rows), and every loader's `processEvents` at
  most every 100 ms (`FrameFileIO::keepUiAlive`, which also moves the progress dialog, shown after a second).
- **Copies:** the file in the in-memory file system as well as in the heap, a `QVector<CommFrame>` of every frame
  (growing by copying), then the trace's copy. Now the bytes the browser handed over are parsed into a compact staging
  store, which goes into the trace chunk by chunk, each chunk freed as it goes in. Detection reads the first two lines.
- **The frame store:** a `CommFrame` (64 bytes in wasm32) plus its own `QByteArray` payload (32-96 bytes with malloc's
  overhead) a frame. `FrameStore` now keeps compact records (time, ID/type/flags, bus, length, payload inline; 36 bytes a
  classic frame, 92 a 64-byte FD one, `framestore.h`); every reader still gets `CommFrame`s, made on access.
Other formats keep SavvyCAN's loaders (a vector of whole frames, the file system copy), hence their 200 MB limit.

Done: **remember the connection and connect by itself** (branch `autoconnect`, owner's item 2 of 2026-10-29). The
browser saves the connections as soon as one is made or its bus is set (SavvyCAN's own `connections/*` settings,
`ConnectionWindow::saveConnections`, in the page's `localStorage`; on the desktop they are still saved at exit); the
last one stays remembered after Disconnect. As the page opens, once the browser has listed the allowed ports
(`WebSerialHub::listPorts()`, `navigator.serial.getPorts()`), the remembered connection on one of them is made again
without the connection window: LAWICEL with its bus speed, CAN FD and data rate; GVRET with its bus (speed, enable,
listen-only, CAN FD, data rate) set as Save Bus Settings sets it once rc1 has answered. No allowed rc1: nothing
happens (no chooser, no error). File > Preferences > **Connect to the last device when the page opens**
(`Main/SaveRestoreConnections`, upstream's "Save/Restore CAN bus connections"; on by default in the browser).
Tests: `tests/web` autoconnect spec; with the real rc1 on the rig (`rig/autoconnect.spec`, GVRET and LAWICEL with CAN FD,
2026-10-29), through the bridge, not Chrome's own Web Serial (which loses USB serial ports on the rig's Linux).

## Stages switched on: `enabledfeatures.h`

- **One list** of what is switched on: `enabledfeatures.h` (`Features::enabledFeature()`, one line per feature,
  with its stage). Stage 1: connection, trace, logging, Preferences, About, theme switch.
- `MainWindow::applyFeatures()` hides and disables every menu entry, panel and button of a feature that is off (a
  disabled action's shortcut is dead too); menus left empty disappear; the `show...()` slots return at once for a
  feature that is off (drag-and-drop, context menu "Add to graph", Ctrl+O included); Preferences hides the groups
  of windows that are off. Same on the desktop and in the browser.
- **To open a stage:** set its lines to `true`, check the window in light and dark and with CAN FD.

## Theme

- `theme.h/.cpp`: Light (SavvyCAN's normal look) / Dark (Fusion + dark palette) / Follow system (default, the
  OS's or the browser's `prefers-color-scheme`); Preferences > Theme and View > Dark Theme; `QSettings`
  `Main/Theme`. In the browser QSettings is the page's `localStorage` (`main.cpp` sets `WebLocalStorageFormat`:
  Qt's wasm `IniFormat` is a temporary file, lost on reload).
- Dark covers the palette, the trace's per-ID row colours (`kRowPaletteDark` in `utility.h`) and every
  `QCustomPlot` (themed when shown and on every switch: background, axes, ticks, grid, legend, titles).
- Graphs (stage 2): every colour above themed (`GraphParams::randomLineColor()`, the swatches, the selected pen; the
  plot's items in `Theme::applyToPlot`: text, tracer and brackets left in the default black follow the theme);
  `candatagrid.cpp`'s bit grid (New graph, and the DBC editor and sender once they are on).
- **Hard-coded colours left for later stages** (fix when the window is switched on):
  - DBC (stage 3): message colours from the DBC (`GenMsgBackgroundColor`/`Foreground`, `dbc/dbchandler.cpp`), shown
    as row colours in the trace when "Interpret Frames" is on (white background in dark); DBC editors' colour
    pickers;
  - Sniffer, Flow view, Range state, Discrete state: done 2026-10-30 (stage 4 live RE tools); Frame Info: done
    2026-10-29 (byte graph colours per theme);
  - `candatagrid.cpp`: the DBC editor's signal colours (`signalColors`) are bright in both themes (names in black);
  - Scripting: `jsedit.cpp` syntax colours, line numbers, cursor and bracket colours.

## Hosting (decide before the first public build)

- The app is static (no server side). CAN data never leaves the browser; the app itself is a download of a few to
  tens of MB.
- **resolve-ev.com/resolve-can/savvycan on the shop (Shopify Oxygen)** is possible only if:
  - the wasm file fits Oxygen's **≤ 20 MB per file** (the full spike was 38.7 MB uncompressed; a stage-1 build
    should be smaller, to measure);
  - that route's Content-Security-Policy gains `'wasm-unsafe-eval'` (today the site's CSP blocks WebAssembly). This
    is a website PR.
- **Otherwise:**
  - a Cloudflare Worker + R2 on that same path, if resolve-ev.com's DNS is proxied through Cloudflare (to check);
  - or a subdomain such as `tools.resolve-ev.com` as a static container next to the forum on Sliplane.
- **GPL:** met by this public repo; every hosted build names its commit and links here.

## Rules for working here

- **Never open a pull request, issue or comment on collin80/SavvyCAN** (owner). This repo is a GitHub fork, and `gh pr
  create` in a fork defaults to the upstream repo: the default here is set to `isak-soderlund/web-savvycan`
  (`gh repo set-default`); always pass `--repo isak-soderlund/web-savvycan` anyway.
- Work on branches, merge into `main` here. Commit messages end with the repo's Co-Authored-By line.
- **The real rc1 only through `tests/web/run.sh --rig`** (owner 2026-10-29: real-rc1 tests on the rig), one rig user at a
  time (resolve-hil AGENTS.md); everything else against the fake rc1 and the browser mock.
- **Heavy jobs** (Qt/WebAssembly and desktop builds, sanitizer runs, Lighthouse) only through
  `scripts/heavy.sh <cmd>` (`~/.local/share/web-savvycan/heavy.sh` links to it): one at a time, in web-savvycan's own memory slice (`wscjobs.slice`,
  3G), separate from resolve-can's `rcanjobs.slice` where the rig's Windows VM runs (on 2026-10-28 a build in that
  shared slice got the VM killed). `-j$WSC_JOBS`; exit 137 = lower `-j`, never raise the cap.

## Risks

| Risk | What could go wrong | How it's handled |
| --- | --- | --- |
| **Shop gets slower** | The WebAssembly app ends up in the website's shared JS bundle, or is prefetched from other pages | Loaded only by its own route at runtime (script tag), never imported into shared code; links to it don't prefetch; `resolve-hil/tools/web/site_baseline.py compare` on the PR preview must show no other page slower or heavier |
| **Shop security** | Allowing WebAssembly (`'wasm-unsafe-eval'`) site-wide weakens the shop's CSP | Added for the tool's route only |
| **File size on Oxygen** | Oxygen's static files are ≤ 20 MB each; the full SavvyCAN wasm was 38.7 MB | Stage-1 build trimmed and measured; if over, `tools.resolve-ev.com` (static container next to the forum) or a Cloudflare Worker + R2 on the same path |
| **Browser support** | Web Serial is Chromium-only (and Firefox 151+); no iPhone | Stated on the page; desktop build for others |
| **Data loss while logging** | Tab crash or close | MVP: accepted by the owner (the log is lost); the label says so, closing the tab asks first. Later: branch `disklog-live` (written to disk as it runs) |
| **Frame loss in the browser** | Single-threaded wasm stalls on a busy bus | Ring buffer, batched parsing, visible dropped-frames counter; a full-load replay test |
| **Memory cap** | wasm32's 4 GB limits the in-memory window | A bounded window (scrolls), everything on disk |
| **Fork drift** | Upstream QT6WIP moves fast | Upstream changes taken in deliberately (merge from `upstream`), never automatically |
| **GPL** | Hosting a build without its source | This public repo; each hosted build names its commit |
| **Rig** | Heavy builds starve the rig's night tests | memcap + one build at a time (the lock above) |

## After the MVP (owner 2026-10-28)

A **"Karpathy-style" simplification review** of everything we added (Web Serial, frame store, logging, theme/UI, build
scripts, tests): less code, fewer abstractions and layers, delete what stage 1 doesn't need, plain readable code over
cleverness, one way to do each thing (e.g. one serial mock, one build path). Findings fixed on a branch, tests kept green.

## Decisions on the build (2026-10-28)

- Stage 1's web build leaves the scripting window (Qt Qml) out (`WEB_SAVVYCAN_SCRIPTING=OFF` for wasm): 5.7 MB less;
  scripting returns with its stage.
- Ship with the gzip loader (`build-wasm.sh`'s default: the page fetches `SavvyCAN.wasm.gz` and unpacks it; every file under Oxygen's 20 MB).
  Building Qt from source for JSPI (smaller still) is deferred.

## Found by the sanitizer builds (2026-10-28, for the simplification/fix pass)

- ASan/UBSan: `canframemodel.cpp:171`: load of value 190 for a `bool` (an uninitialised `bool` member, upstream).
  Fixed (the frame store's constructor); the simplified tree starts clean.
- TSan: data-race warnings at start-up, first in `QEvent::type()`. On the simplified tree 5, all events handed to
  another thread through Qt's posted-event queue (Qt's QMutex isn't visible to TSan in an uninstrumented Qt): Qt's
  D-Bus thread and the frame sender's thread; none in our code (docs/SIMPLIFY.md C2).
- clang: `canframemodel.cpp:689` compares two different enum types (upstream): a real bug (remote frames showed
  data in the decimal column), fixed.
