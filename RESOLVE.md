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
  the trace and in the log file.
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
| A log file from the computer | File > Load Log File opens the browser's own file picker (Chrome: `showOpenFilePicker`); the file type is detected from the contents (SavvyCAN's autodetect: GVRET CSV, candump, PCAN, Vector ASC/BLF, ...). The bytes go to the in-memory file system for SavvyCAN's loaders and are deleted after. **Up to 500 MB**: everything lives in the page's memory (wasm32, 4 GB): the bytes, their copy and about 150 bytes a frame twice while they go into the trace. A 1-hour Leaf log (~3 M frames, ~200 MB as CSV) fits; an 8-hour one doesn't |
| Time axis | In the app's time style (File > Preferences: microseconds by default, or seconds, or clock) |
| Colours | Default line light grey in dark; the Add New Graph dialog's random colour from the light half in dark |
| Owner's other changes | No View menu (the theme is in File > Preferences > Theme); no "Press F1 on any screen for help" (the help pages aren't in this build) |

Left for stage 2 (fork plan): the min/max envelope and **bounded graph storage** (G5–G8: a graph keeps every point today, ~32 bytes each, so a graph of a 100 Hz signal grows ~11 MB an hour), the FD bit picker in Add New Graph (its bit grid has 64 bits; FD signals past byte 8 by typed start bit only), the remaining hard-coded colours. Then files larger than memory: graphing straight from the file (only the graphed signals kept), not loading every frame.

Tests: `tests/web` smoke spec (in CI) loads a GVRET CSV through the file picker and graphs it (every frame, every point); `graph.spec` graphs a live Leaf bus (grows, follows, value axis takes in 0..255).

## Next (owner 2026-10-29)

1. **Logs longer than 71.6 minutes.** GVRET sends a frame's time as 32-bit microseconds and SavvyCAN's reader keeps
   it in 32 bits (`GVRetSerial`'s `buildTimestamp`, `uint`): 2^32 µs after the session's start (71.6 min) the times
   start again near 0. Frames keep coming and are logged, but their times jump back (long logs and graphs out of
   order). Same in desktop SavvyCAN with any GVRET device. Fix in the app only: count the wraps in the reader
   (a time smaller than the last one by more than half the range = one wrap) and keep 64-bit µs. **No change to the
   GVRET protocol or rc1's firmware**, so rc1 stays usable with standard SavvyCAN. Test: the fake rc1 with its clock
   started near 2^32 µs (a wrap within seconds), frames in order across it in the trace, the log and a graph; on the
   rig, a log of more than 72 min. Goal: a whole CCS charging session (hours: ~0.25 GB of CSV an hour on the Leaf
   bus; Chrome keeps logs of 8 GB, Firefox about half the PC's free memory).

Done: **remember the connection and connect by itself** (branch `autoconnect`, owner's item 2 of 2026-10-29). The
browser saves the connections as soon as one is made or its bus is set (SavvyCAN's own `connections/*` settings,
`ConnectionWindow::saveConnections`, in the page's `localStorage`; on the desktop they are still saved at exit); the
last one stays remembered after Disconnect. As the page opens, once the browser has listed the allowed ports
(`WebSerialHub::listPorts()`, `navigator.serial.getPorts()`), the remembered connection on one of them is made again
without the connection window: LAWICEL with its bus speed, CAN FD and data rate; GVRET with its bus (speed, enable,
listen-only, CAN FD, data rate) set as Save Bus Settings sets it once rc1 has answered. No allowed rc1: nothing
happens (no chooser, no error). File > Preferences > **Connect to the last device when the page opens**
(`Main/SaveRestoreConnections`, upstream's "Save/Restore CAN bus connections"; on by default in the browser).
Tests: `tests/web` autoconnect spec. Not tried with a real rc1 in Chrome yet.

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
- **Hard-coded colours left for later stages** (fix when the window is switched on):
  - graphs (stage 2): `GraphParams` line colour black (`re/graphingwindow.cpp` l. ~1676), random dark line colours
    from the trace's context menu (`mainwindow.cpp` "Add to graph") and `re/newgraphdialog.cpp` (button palette);
  - DBC (stage 3): message colours from the DBC (`GenMsgBackgroundColor`/`Foreground`, `dbc/dbchandler.cpp`), shown
    as row colours in the trace when "Interpret Frames" is on (white background in dark); DBC editors' colour
    pickers;
  - Sniffer: `re/sniffer/sniffermodel.cpp` (grey levels and dark red/green, palette read once) and
    `SnifferDelegate.cpp`;
  - Frame Info: `graphColors` with `Qt::black` (`re/frameinfowindow.cpp`); Flow view: `graphColors[8]` with
    `Qt::black`, black text pen (`re/flowviewwindow.cpp`); Range state: black graph pen
    (`re/rangestatewindow.cpp`); Discrete state: fixed label palettes (`re/discretestatewindow.cpp`);
  - `candatagrid.cpp` (bit grid colours, used by the DBC editor, New graph, sender);
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
- **Don't test on the real rc1** (the rig runs scheduled tests); use the fake GVRET device and the browser mock.
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
