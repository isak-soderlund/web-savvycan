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
| 1 | **WebAssembly build** | Qt 6.12 `wasm_singlethread` + Emscripten 5.0.5 (the spike's toolchain); Qt SerialBus and MQTT left out of the wasm build; Asyncify (or JSPI) for the dialogs that use `exec()`; the desktop build of the same tree keeps working |
| 2 | **Web Serial port** | A `QSerialPort` stand-in backed by Web Serial (SavvyCAN uses ~15 of its calls: open, baud, DTR, readAll, write, readyRead, errorOccurred, ...), so the GVRET/LAWICEL code stays as it is. A "Choose device" button calls `requestPort()` (needs a user gesture), filtered to rc1's USB ID |
| 3 | **Connection window** | SavvyCAN's own dialog, offering GVRET and LAWICEL (slcan) on the Web Serial port; the other connection types hidden in the browser |
| 4 | **Single-threaded GVRET** | No `QThread` in the wasm build (the spike's three changes); incoming bytes parsed in batches from the JS read loop; no per-byte debug string (fix M-series) |
| 5 | **Bus setup** | Nominal bit rate, CAN FD on/off, data bit rate, listen-only, through GVRET's own commands (rc1 firmware 0.1.3 implements them, classic + FD verified with desktop SavvyCAN on 2026-10-28) |
| 6 | **Main window trace** | Live frame list with CAN FD (64-byte data, BRS/ESI columns), frame counter, **dropped-frames counter**; ring-buffer frame store and an index list for the filtered view (fixes M-series), so it never stalls on a full list |
| 7 | **Logging to disk** | File System Access API: name the file in a prompt, or create it directly with the start time as its name; frames written as they arrive, committed every few seconds (close and reopen in append mode), so a crash loses at most the last seconds. Format: SavvyCAN's native GVRET CSV, FD rows included. Firefox: streaming-download fallback |
| 8 | **Theme switch** | Light (SavvyCAN's normal) / dark, remembered between visits |
| 9 | **Only stage-1 windows** | Every menu and window not in stage 1 hidden (not broken); they come back stage by stage |
| 10 | **Settings kept** | Connection, theme and log settings remembered in the browser (whether `QSettings` persists in wasm: to check) |
| 11 | **About / licence** | GPL notice, SavvyCAN's and QCustomPlot's credits, the source link once published |

**Tests:**
- **Without hardware:** a fake GVRET device (resolve-can's `test/hil/gvret_fake.py`) and unit tests for the
  Web Serial stand-in and the parsers.
- **On the rig:** rc1 with the Kvaser as the peer, classic and CAN FD. Every frame the Kvaser sends must appear in
  the trace and in the log file.
- **Crash test:** close the tab mid-log; the file must hold everything up to the last commit.
- **Sanitizers:** AddressSanitizer and ThreadSanitizer runs of the desktop build of the same tree.

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
