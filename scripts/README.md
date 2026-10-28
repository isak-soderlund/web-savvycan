# Scripts

## Building web-savvycan (desktop and WebAssembly)

The same tree builds for the desktop (Qt 6.12, `gcc_64`) and for the browser (Qt 6.12
`wasm_singlethread` + Emscripten 5.0.5). The wasm build is selected by Qt's toolchain (`EMSCRIPTEN`
in CMake, `Q_OS_WASM` in C++); the desktop build is not changed by it.

### 1. Toolchain (once)

```bash
scripts/setup-toolchain.sh
```

Installs in user space, no sudo: Qt 6.12.0 desktop (+ Serial Port, Serial Bus, WebSockets) and
`wasm_singlethread` into `~/Qt` with aqtinstall (venv in `~/.local/share/web-savvycan/aqt-venv`),
Emscripten 5.0.5 into `~/emsdk`, then writes `~/.local/share/web-savvycan/toolchain.env`
(`QT_DESKTOP`, `QT_WASM`, `EMSDK`, emcc and wasm-opt on `PATH`). The build scripts source it.
System packages it expects (Ubuntu): `git python3-venv cmake ninja-build g++ libgl-dev libegl-dev
libopengl-dev libxkbcommon-dev`.

### 2. Desktop

```bash
scripts/build-desktop.sh                # Release            -> build/desktop-release/SavvyCAN
scripts/build-desktop.sh asan           # Debug + ASan/UBSan -> build/desktop-asan/SavvyCAN
scripts/build-desktop.sh tsan           # Debug + TSan       -> build/desktop-tsan/SavvyCAN
scripts/build-desktop.sh all --clean
```

CMake options behind them: `USE_SANITIZER` (ASan + UBSan, Debug), `USE_TSAN` (ThreadSanitizer, Debug).
Upstream's `BUILD_TESTS` target (`test/`) does not compile on QT6WIP (it still uses `CANFrame`,
`SOCKETCAN`); `--tests` turns it on anyway.

### 3. WebAssembly

```bash
scripts/build-wasm.sh                   # -> dist/
scripts/build-wasm.sh --wasm-opt        # plus an extra wasm-opt -Oz pass
scripts/build-wasm.sh --opt -Os         # another optimisation level (default -Oz)
scripts/build-wasm.sh --gzip-loader     # wasm only as SavvyCAN.wasm.gz, decompressed in the page (20 MB file limits)
scripts/build-wasm.sh --jspi            # JSPI instead of Asyncify
WASM_BUILD_DIR=build/wasm-noscript scripts/build-wasm.sh -- -DWEB_SAVVYCAN_SCRIPTING=OFF  # no scripting window/Qt Qml (own build dir: toggling it in one leaves stale moc output)
scripts/smoke-wasm.sh                   # serve dist/, open it in headless Chromium, dist/smoke.png
```

`dist/` gets `SavvyCAN.html`, `SavvyCAN.js`, `SavvyCAN.wasm`, `qtloader.js`, `qtlogo.svg`, each also
as `.gz` (gzip -9) and `.br` (brotli 11), and `SOURCE_COMMIT` (the commit, plus `dirty`). The script
prints the sizes. To try it by hand: `python3 -m http.server -d dist 8000`, then
`http://localhost:8000/SavvyCAN.html` in Chrome.

What the wasm build changes (all in `wasm/` and behind `EMSCRIPTEN` / `Q_OS_WASM`):

- **Qt Serial Port / Serial Bus** do not exist for wasm. `QSerialPort`/`QSerialPortInfo` come from
  `wasm/webserial/` (a `QIODevice` with Qt's API backed by Web Serial: `webserialport.*`, the bridge
  `webserial.js` linked with `--pre-js`, tests in `wasm/webserial/tests`); `wasm/include/` has the
  `QCanBus*` stand-ins (empty plugin list, `QCanBusFrame`'s enum). The QtSerialBus connection
  (`serialbusconnection.cpp`) is not compiled. In the browser the connection window offers GVRET and
  LAWICEL only, with "Choose device..." for the browser's port chooser.
- **No threads:** connections, the frame sender and playback run on the main thread
  (`canconnection.cpp`, `framesenderobject.cpp`, `frameplaybackobject.cpp`).
- **No TLS:** MQTT over TLS is left out (`QT_NO_SSL`).
- **No OpenGL** for QCustomPlot, no Qt Help / OpenGL modules linked.
- **`QDialog::exec()`:** Asyncify (`-sASYNCIFY`), or JSPI with `--jspi`.
- Optimised for size (`-Oz`, `WEB_SAVVYCAN_WASM_LINK_OPT`).
- `WEB_SAVVYCAN_SCRIPTING=OFF` (any build) leaves the scripting window and Qt Qml out (`SAVVYCAN_NO_SCRIPTING`).

### Sizes and JSPI (measured 2026-10-28, Qt 6.12.0, Emscripten 5.0.5)

`scripts/wasm-size-experiments.sh` rebuilds these; every variant except JSPI passes the smoke test.

| Variant | .wasm | gzip -9 | brotli 11 |
| --- | --- | --- | --- |
| default: `-Oz`, Asyncify | 35.06 MB | 11.13 MB | 7.86 MB |
| + `--wasm-opt` (extra `wasm-opt -Oz`) | 33.94 MB | 11.00 MB | 7.83 MB |
| `-Os` instead of `-Oz` | 36.25 MB | 11.33 MB | 7.94 MB |
| no scripting window (`WEB_SAVVYCAN_SCRIPTING=OFF`, no Qt Qml) | 29.34 MB | 9.47 MB | 6.73 MB |
| no scripting + `--wasm-opt` | 28.45 MB | 9.36 MB | 6.72 MB |
| JSPI instead of Asyncify (does not start, below) | 23.49 MB | 8.35 MB | 6.13 MB |

SavvyCAN.js is 0.25 MB (0.05 MB brotli); qtloader.js and the html are a few kB.

- **20 MB per file (Shopify Oxygen):** no raw .wasm fits. `--gzip-loader` ships only
  `SavvyCAN.wasm.gz` (11.1 MB) and the page decompresses it (`DecompressionStream('gzip')`; Chromium
  has no `'brotli'` there): every file is under 20 MB, smoke test passes.
- **JSPI:** links (Asyncify costs ~11.5 MB of the wasm), but fails at start with "trying to suspend
  without WebAssembly.promising": the prebuilt Qt is configured without `wasm-jspi`
  (`QT_FEATURE_wasm_jspi -1`), so its JS-to-wasm callbacks are not promising-wrapped. JSPI needs Qt
  6.12 built from source with `-feature-wasm-jspi`. Asyncify works (the smoke test's modal
  `QMessageBox` through `exec()`).



Heavy jobs share the PC with the rig's test jobs and a Windows VM: run builds through the wrapper,
which takes the build lock (one job at a time), runs the job in its own 3G memory slice
(`wscjobs.slice`) and sets `WSC_JOBS` (the scripts' default `-j`, 2). It may wait for the lock:
start it in the background.

```bash
~/.local/share/web-savvycan/heavy.sh scripts/build-desktop.sh
~/.local/share/web-savvycan/heavy.sh scripts/build-wasm.sh
```

Exit 137 means the job hit 3G: lower `-j`, never raise the cap.

### CI

`.github/workflows/web-savvycan.yml` runs the same scripts on GitHub's `ubuntu-24.04` runner:
desktop Release, wasm, the smoke test; `dist/` as an artifact.

## Running scripts

To run this scripts, first you must authorize your terminal to run:

```bash
chmod +x script/*.sh
```

Then you can run by using this command:

```bash
./scripts/the/script/you/want/to/run
```
