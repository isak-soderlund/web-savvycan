# Browser tests: a fake resolve-can-1 and the Playwright suite

No rc1 and no rig hardware: the tests run against a fake rc1 that speaks GVRET and LAWICEL/slcan exactly as rc1's
firmware does, behind a `navigator.serial` (Web Serial) mock injected before the page loads.

```
tests/web/run.sh                      # unit tests + the Playwright suite (projects app, download, firefox)
tests/web/run.sh --dist path/to/dist  # the app build to test (default <repo>/dist, env WSC_DIST)
tests/web/run.sh --long               # also the 60 s full-load replays (through ~/.local/share/web-savvycan/heavy.sh)
tests/web/run.sh --large              # also the multi-GB log (WSC_LARGE_GB=2,4,8; through heavy.sh)
tests/web/run.sh --screenshots        # also pictures of the app in light and dark (results/screenshots/)
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
| `classic-full` | rc1's measured full classic rate: 8-byte frames at 1 Mbit/s, 8 000/s; sequence number in data[0..3] |
| `fd64-full` | rc1's measured full FD rate: 64-byte BRS frames at 500k/8M, 6 900/s; sequence number |
| `fd-mix` | every FD length (BRS on/off, 11/29-bit), classic and remote frames, 500/s |
| `errors` | `leaf` plus an error frame every 100 ms (warning, passive, bus-off, restart, ACK, RX overflow) |
| + `garbageEvery: n` | junk bytes (never 0xF1) after every n-th GVRET frame |

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
  traceTail(n) { return [{id, ext, fd, brs, data: [bytes]}]; },   // the last n frames in the trace, oldest first
};
```

## How the app driver works (`lib/harness.mjs` `AppDriver`)

- Qt 6.12 puts its windows in a shadow root (`#qt-shadow-container`): `document.querySelector('canvas')` sees
  nothing. Its accessibility tree starts as one visually hidden "Enable Screen Reader" button that reacts only to a
  DOM click (a Playwright click does nothing); after that, Playwright's role locators (which pierce open shadow
  roots) reach menus, buttons, checkboxes and radios by name.
- Names as the app has them (case-insensitive prefix match): Connection > Open Connection Window, Add New Device
  Connection, radio "Serial Connection (GVRET)" / "LAWICEL / SLCAN Serial", **Choose device...**, Create New
  Connection, Save Bus Settings, Disconnect Selected Device, File > Start Continuous Logging / **Cease Continuous
  Logging**, View > **Light Theme** / **Dark Theme** / **Follow System Theme**. A widget that isn't there skips the
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
| traffic | Leaf bus 5 s: every frame counted; garbled stream resyncs; slcan error lines not counted as frames; overload (5x rc1's rate): received = trace + evicted + dropped; unplug mid-stream and reconnect |
| logging | File > Start Continuous Logging / Cease: one download named `SavvyCAN_<start time>.csv`, SavvyCAN's native GVRET CSV, its rows equal to what the fake sent (FD rows, BRS), loaded by SavvyCAN's parser (when `nativecsv_tool` is built); an empty log is the header; Choose the Log's Name When Logging Stops: Keep Logging goes on, the typed name is the download's |
| theme | light first, dark visibly darker (screenshot luminance), remembered after reload; a dark system: followed on a first visit, overridden and followed again |
| smoke | the page as a visitor gets it (no fake): Qt starts, the main window is drawn, the About dialog (`exec()`, Asyncify) opens and closes with Enter, no failed request |
| download (`download`, `firefox`) | the logging module alone: rows written in batches, one download named by the start time, byte for byte SavvyCAN's own rows (`nativecsv_tool gen`) and loaded by SavvyCAN's parser; `_blob()` while logging; the name given at stop; an empty log; a second log; "leave site?" while logging |
| fullload `@long` | 60 s at 8 000 classic / 6 900 FD64 frames/s: accounting, rc1-side loss < 0.1 %, 10 ms timer lateness p95 < 50 ms, no stall > 1 s; `results/fullload-*.json` |
| large `@large` | gigabytes of rows through the logging module, one download: size and SHA-256, JS heap, browser PSS and profile size every 256 MB; `results/large-*.json` |
| screenshots `@screenshots` | pictures only: main window with traffic, View menu, About, Preferences, light and dark |
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
