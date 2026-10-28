# Simplification review of the Resolve additions (stage 1)

The owner's "Karpathy-style" review (RESOLVE.md "After the MVP"): less code, fewer layers, delete what stage 1
doesn't need, plain code over clever code, one way to do each thing. Scope: everything we added or changed since
upstream QT6WIP 9c3b0af (`git diff 9c3b0af...main`); upstream code only where our changes touch it.
Branch `simplify`, from `main` 2d52b34 (the frame store and the integration merge).

Each finding: **where**, what and why, **change**. Status at the end of each: *done*, *kept* (with the reason) or
*open*.

## Line counts (ours only)

Counted with the script in "How the lines are counted" below: new files by area, and the lines we added/removed in
upstream files.

| | before (2d52b34) | after |
| --- | ---: | ---: |
| app (C++, JS, CMake) | 3 859 | |
| tests | 7 282 | |
| scripts, CI | 963 | |
| docs | 393 | |
| **new files, total** | **12 497** | |
| upstream files changed | +2 113 / -721 | |

## A. Logging (`wasm/disklog/`, `mainwindow.cpp`)

A1. **Disk-live logging is switched off but is half of the logging code.** `LOG_TO_DISK_LIVE=false`
(`enabledfeatures.h`), yet `main` carries the File System Access writer, the remembered folder (IndexedDB), the
Firefox streaming download and its service worker (`disklog-sw.js`), split files, the start dialog
(`weblogstartdialog.*`, 184 lines), the commit/part/crash-safe state in `WebDiskLog`, the matching branches in
`MainWindow::web*` and four test specs (`test/web/tests/disklog*.spec.mjs`, real pickers under Xvfb).
Stage 1 ships one download on stop; the live path is a later, owner-decided feature.
**Change:** keep the live code on branch `disklog-live` (= `main` 2d52b34, where it is complete and tested), and
cut `main` down to the download: `disklog.js` keeps only the Blob collector, `WebDiskLog` becomes
start/write/stop + size/frames, `weblogstartdialog.*`, `disklog-sw.js`, `LOG_TO_DISK_LIVE`, the IndexedDB folder and
the live specs go. RESOLVE.md names the branch. *Recommended: the live path can be merged back from the branch
when the owner wants it; nothing in stage 1 uses it.*
*Done:* `disklog.js` is the Blob collector only (752 -> ~110 lines, all synchronous: no events, no promises,
no listener); `WebDiskLog` is a namespace of start/write/setFileName/stop + bytes/frames/fileName (no QObject, no
embind callback); `FrameFileIO::openContinuousNative()`/`closeContinuousNative()` start and stop it, so the
desktop's two logging handlers run unchanged in the browser too (plus the name question at stop);
`weblogstartdialog.*`, `disklog-sw.js`, `disklog.cmake`, `LOG_TO_DISK_LIVE` deleted. The name asked at stop is a
modal `QInputDialog` (`exec()`, as Help > About). RESOLVE.md MVP #7 and Risks name the branch.

A2. **`MainWindow`'s browser logging has three ways of saying the same state** (label text, tooltip, blink, and
per-backend wording for "fsa"/"download"/"blob"). **Change:** with one backend left, one label function and one
tooltip.
*Done:* one label function (`MainWindow::webLogLabel`), no tooltip (the label says it all), the blink as on the
desktop (from the label's own text, no extra flag).

## B. Web Serial (`wasm/webserial/`)

B1. **Thread safety nobody needs.** `WebSerialHub` has a mutex, `QPointer` + `shared_ptr` batches and
cross-thread queuing because `tst_webserial_connections` runs GVRET/LAWICEL in `QThread`s. In the browser
everything is one thread (wasm_singlethread, `CANConnection` forces `pUseThread=false`); the desktop build uses
Qt's real QSerialPort. **Change:** single-threaded hub: reports queued to the event loop (still never run app code
inside a JS callback), data appended to the port with one pending `readyRead`.
*Done:* no mutex, `QPointer` or `shared_ptr`; reports are queued with the port as context object (Qt drops them
when it is deleted), data goes into the port's buffer at once with one queued `readyRead`. `WebSerialHub` moved into
`webserialport.{h,cpp}` (with `WebSerialBackend`, `QSerialPortInfo`); `webserialhub.*` deleted.

B2. **A second fake rc1 in C++** (`wasm/webserial/tests/fakerc1.h`, 226 lines) for `tst_webserial_connections`
(219 lines). The browser suite runs the same GVRET/LAWICEL code in the real wasm app against the one fake
(`tests/web/fake/rc1.js`) — connect, traffic, unplug, refused channel. **Change:** delete both; the stand-in's own
unit test (`tst_webserialport`, fake backend) stays.

B3. **API nobody calls.** The stand-in implements Qt API SavvyCAN never uses (parity/stop-bit/flow setters'
change signals, `setBreakEnabled`, `pinoutSignals`, `readBufferSize`, `canReadLine`/`readLineData`,
`waitFor*`, `standardBaudRates`, `swap`, `serialNumber`, `manufacturer`, `isOpening`, `handle`...).
**Change:** keep what GVRetSerial, LAWICELSerial, NewConnectionDialog and ConnectionWindow use (the compiler is the
judge); Qt's names and enum values stay.
*Done:* `QSerialPort` keeps open/close, baud rate, data bits (always 8), flow control, DTR/RTS, error/clearError,
readyRead/errorOccurred, `handle()` (tests); gone: Directions, BaudRate/Parity/StopBits/PinoutSignal enums, break,
`*Changed` signals, flush/clear, bytesToWrite and the "written" reports (nobody listens to `bytesWritten`),
readBufferSize, canReadLine/readLineData, waitFor*, read-error counting (the JS side logs those). `QSerialPortInfo`
is name + description (no USB ids, `swap`, `standardBaudRates`); the backend's port list is `QList<QSerialPortInfo>`
(no `WebSerialPortDescription`, no `WebSerialOpenOptions`). The unused `wasm/include/QtSerialBus/*` and
`QtSerialPort/QSerialPort` forwarding headers and unused `QCanBusDeviceInfo` members went too.

B4. **Two names for the JS bridge.** `webserialbackend_wasm.cpp` calls `webSerialBridge` (EM_JS) and
`globalThis.resolveWebSerial` (embind `val::global`) for the same object. **Change:** one name.
*Done:* `globalThis.resolveWebSerial` only (EM_JS and `val::global` both use it).

## C. Frame store and model (`framestore.*`, `canframemodel.*`)

C1. **`FrameListView` imitates all of QVector** (`count/length/size`, `empty/isEmpty`, `value/[]/at`,
`first/constFirst/last/constLast`, `cbegin/cend`, a full random-access iterator) and has a third implementation,
`FrameVectorView`, used by nobody but tests. **Change:** keep the members the windows use; drop `FrameVectorView`.
*Done:* `size/count/length`, `isEmpty`, `at`, `first`, `last`, `mid`, `tail`, `toVector` kept (all called by the
windows); the iterator, `[]`, `value`, `empty`, `constFirst/constLast`, `cbegin/cend`, `FrameVectorView` and the
test-only `FrameIndexView` template deleted (their tests in `tst_framestore` with them).

C2. **Sanitizer findings** (RESOLVE.md): `canframemodel.cpp:171` uninitialised `ignoreDBCColors` (upstream) is
already initialised by the frame store's constructor (*done* in e1ab25a); **`canframemodel.cpp` compares
`CommFrame::frameType()` with `QCanBusFrame::RemoteRequestFrame`** (the 9c3b0af line 689, now ~745): a real bug,
`QCanBusFrame::RemoteRequestFrame` is 3, which is `CommFrame::LINDataFrame`; remote frames showed data bytes.
**Change:** `CommFrame::RemoteRequestFrame`. TSan start-up reports: rerun on the simplified tree, fix ours,
suppress or explain Qt's.
*Done (the enum):* the only `QCanBusFrame`-vs-`CommFrame` comparison in the tree (the others compare a real
`QCanBusFrame` in `serialbusconnection.cpp`). Sanitizer start-up runs of the simplified tree (desktop,
Xvfb, 15 s): ASan/UBSan none. TSan 5, none in our code: objects handed to another thread through Qt's posted-event
queue (its QMutex is a futex inside the uninstrumented libQt6Core, invisible to TSan): 3 with Qt's own
`QDBusConnection` thread (from `setupUi`), 2 with the frame sender's `QThread` (`FrameSenderObject::startSending`'s
queued `invokeMethod`, read in `QEvent::type()`/deleted in `sendPostedEvents` there). *Kept* as explained (Qt
isn't TSan-built); no suppression file for the app. `test/frames` under ASan and TSan: clean.

## D. App UI

D1. **Enter in the connection window presses "Disconnect Selected Device".** `ConnectionWindow` is a `QDialog`;
its push buttons are auto-default, so Enter (also in an open drop-down list) presses the first one.
**Change:** no auto-default buttons in that window.
*Done:* `ConnectionWindow` sets `autoDefault` off on all its push buttons (Disconnect, Reset, Save Bus Settings, Move
Up/Down, Add New Device Connection, the console's Send/Clear). *Kept:* `NewConnectionDialog`: Enter presses Create New
Connection, its only OK button: a dialog's normal default.

D2. **Hidden connection types are still in the dialog's accessibility tree** (wasm): `NewConnectionDialog` only
hides the SocketCAN/Remote/Kayak/MQTT/CANserver/Canlogserver radio buttons; a screen reader (and Playwright's
role locators) still finds them, and a restored connection of such a type can still check one. **Change:** take
them out of the dialog in the browser (not just hidden), and a test that they aren't offered.
*Done:* in the browser the six radios are moved into a never-shown window owned by the dialog (a separate window:
not in the dialog's layout, focus chain or accessibility tree); `CanConFactory::create` makes every other type a
GVRET connection on the same port in the browser (for SocketCAN it returned no connection there). The spec (only GVRET and LAWICEL radios) is for `tests/web/specs/connect.spec.mjs` (with the test move).

D3. **`SavvyCAN.pro` half-updated** (qmake): our sources were added to it partly (no `framestore`, no wasm), so it
builds neither tree. CMake is the one build path (scripts, CI). **Change:** our edits to `SavvyCAN.pro` reverted;
RESOLVE.md says CMake only.

## E. Tests: one location, one harness, one fake, one mock, one server

Today: `tests/web` (Playwright suite, fake rc1, Web Serial mock, server, selftest page), `test/web` (logging specs,
own server, own `common.mjs`, own `package.json`, own harness page, Firefox and Xvfb projects),
`wasm/webserial/tests` (`e2e.mjs`: a third Playwright driver with its own server; `bridge.test.mjs`; C++ tests),
`test/frames`, `test/nativecsv`, `tests/desktop` (pty fake), `scripts/smoke-wasm.*` (own server, own
Playwright install in `scripts/package.json`), `scripts/screenshots/` (`wasm.cjs`: another driver;
`fake_gvret_pty.py`: another fake GVRET device).

E1. **The selftest client** (`tests/web/selftest/index.html`, 258 lines, and the `selftest` project that runs every
spec twice) was the reference before the app existed. The app now passes the same specs. **Change:** delete it and
its driver; the specs run against the app only.

E2. **`test/web` moves into `tests/web`** (one `package.json`, one config, one server, one harness): the download
module spec and the multi-GB spec (`large` project, through `heavy.sh`); `download-app.spec` merged into
`specs/logging.spec` (same checks, one driver); live-logging specs go with A1 to `disklog-live`.

E3. **`wasm/webserial/tests/e2e.mjs`** (175 lines) duplicates `connect`/`traffic` with its own driver and server.
**Change:** delete. `bridge.test.mjs` moves to `tests/web/unit/`.

E4. **Desktop C++ tests in three places** (`test/frames`, `test/nativecsv`, `wasm/webserial/tests`), each with
its own CMake project and runner. **Change:** one `tests/desktop` CMake project and one `run.sh` (plain,
`--asan`, `--tsan`).

E5. **Smoke test with its own server and Playwright install** (`scripts/smoke-wasm.cjs`, `scripts/package.json`).
**Change:** a `smoke` spec in `tests/web`; `scripts/smoke-wasm.sh` runs it; CI installs `tests/web`.

E6. **A second fake GVRET device for screenshots** (`scripts/screenshots/fake_gvret_pty.py`). **Change:** use
`tests/desktop/rc1_pty.py --tcp` (the one fake). `scripts/screenshots/wasm.cjs` uses the suite's driver or goes.

## F. Build scripts

F1. **`scripts/wasm-size-experiments.sh`**: the experiment is done and its decisions are in RESOLVE.md.
**Change:** delete (git history keeps it).

F2. One build path: `scripts/build-wasm.sh` and `scripts/build-desktop.sh` (CMake); options nobody uses get cut
where found.

## G. Found while simplifying (app)

G1. **The receive slicing was written twice** (`GVRetSerial` and `LAWICELSerial`: `processRxBytes`,
`processPendingRx`, the pending buffer, the slice budget, ~45 lines each). **Change:** once in `CANConnection`
(`processRxBytes` feeds a virtual `processRxByte`); GVRET's is `procRXChar`, LAWICEL's builds the slcan line.
*Done.*

G2. **The test hook had a registry** (`TestHook::registerStat`, `std::function` map, "keep an entry already
registered") for nine fixed numbers, and was compiled into the desktop build where it did nothing. **Change:**
`stats()` reads the model, `CANConManager` and `WebDiskLog` directly; `testhook.*` is in the wasm build only.
*Done.* (`window.webSavvyCAN` is unchanged.)

G3. **Build options nobody uses:** `WEB_SAVVYCAN_JSPI` (Qt isn't built for JSPI; deferred in RESOLVE.md) and
`WEB_SAVVYCAN_SCRIPTING` (always off in the browser, on on the desktop), `WEB_SAVVYCAN_WASM` (= `EMSCRIPTEN`),
`SAVVYCAN_NO_SCRIPTING` (= `Q_OS_WASM`), and the wasm build spread over three CMake files (`wasm.cmake`,
`webserial.cmake`, `disklog.cmake`). **Change:** one `wasm/wasm.cmake`; scripting = desktop only. *Done.*
(`scripts/build-wasm.sh --jspi/--scripting` now do nothing: to remove there.)

G4. **Two names for one function:** `Features::enabledFeature()` and `Features::on()`. **Change:** `on()` only.
*Done.*

G5. **Small dead API:** `Theme::plotBackground/plotForeground/plotGrid` (public, used only inside `applyToPlot`)
and its `m_initialised` guard; `AboutDialog::sourceUrl()` (public, used only inside the dialog). **Change:**
file-local. *Done.*

## How the lines are counted

```bash
BASE=9c3b0af REV=HEAD
git diff --diff-filter=AR --name-only $BASE $REV | grep -v -e '\.png$' -e 'package-lock.json' \
  | while read -r f; do echo "$(git show $REV:"$f" | wc -l) $f"; done      # new files (sum by area)
git diff --diff-filter=M --numstat $BASE $REV | awk '{a+=$1; d+=$2} END {print "+" a, "-" d}'  # upstream files
```

Areas: `tests` = `tests/`, `test/`, `*/tests/`; `scripts` = `scripts/`, `.github/`; `docs` = `*.md`; `app` = the rest.
