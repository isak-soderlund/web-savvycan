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
scripts/build-wasm.sh --jspi            # JSPI instead of Asyncify
scripts/build-wasm.sh -- -DWEB_SAVVYCAN_SCRIPTING=OFF   # without the scripting window (no Qt Qml)
scripts/smoke-wasm.sh                   # serve dist/, open it in headless Chromium, dist/smoke.png
```

`dist/` gets `SavvyCAN.html`, `SavvyCAN.js`, `SavvyCAN.wasm`, `qtloader.js`, `qtlogo.svg`, each also
as `.gz` (gzip -9) and `.br` (brotli 11), and `SOURCE_COMMIT` (the commit, plus `dirty`). The script
prints the sizes. To try it by hand: `python3 -m http.server -d dist 8000`, then
`http://localhost:8000/SavvyCAN.html` in Chrome.

What the wasm build changes (all in `wasm/` and behind `EMSCRIPTEN` / `Q_OS_WASM`):

- **Qt Serial Port / Serial Bus** do not exist for wasm. `wasm/include/` has stand-in headers:
  `QSerialPort`/`QSerialPortInfo` (`wasm/qserialport_wasm.{h,cpp}`, a `QIODevice` with Qt's API; the
  Web Serial backing goes there) and `QCanBus*` (empty plugin list, `QCanBusFrame`'s enum). The
  QtSerialBus connection (`serialbusconnection.cpp`) is not compiled.
- **No threads:** connections, the frame sender and playback run on the main thread
  (`canconnection.cpp`, `framesenderobject.cpp`, `frameplaybackobject.cpp`).
- **No TLS:** MQTT over TLS is left out (`QT_NO_SSL`).
- **No OpenGL** for QCustomPlot, no Qt Help / OpenGL modules linked.
- **`QDialog::exec()`:** Asyncify (`-sASYNCIFY`), or JSPI with `--jspi`.
- Optimised for size (`-Oz`, `WEB_SAVVYCAN_WASM_LINK_OPT`).
- `WEB_SAVVYCAN_SCRIPTING=OFF` (any build) leaves the scripting window and Qt Qml out (`SAVVYCAN_NO_SCRIPTING`).

### On the rig

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
