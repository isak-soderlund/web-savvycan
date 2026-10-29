# Real-rc1 tests on the rig

The app (dist/) in headless Chromium against the **real** resolve-can-1 and a real CAN bus, instead of the fake rc1.
Only on the Resolve HIL rig (or a PC wired the same way): rc1 on USB, and a second CAN node on rc1's bus (the rig's
Kvaser Leaf v3, SocketCAN `hilcan1`) that sends known frames.

```
tests/web/run.sh --rig                                   # every rig spec but @long (~3 min)
tests/web/run.sh --rig -- rig/fd.spec.mjs                # one spec
tests/web/run.sh --rig --long -- rig/timewrap.spec.mjs   # 75 min past rc1's 32-bit µs clock wrap
xvfb-run -a node tests/web/rig/chrome_check.mjs          # Chromium's OWN Web Serial (no bridge): see below
```

One rig user at a time (resolve-hil AGENTS.md): nothing else may use rc1, its gs_usb interface or `hilcan1` meanwhile.
Needs `python3` with pyserial, `sudo` without a password (for `ip link`), and resolve-hil's `tools/usb_port.py` for
the unplug test.

## How it works

| File | What |
| --- | --- |
| `bridge.mjs` | The bridge: the suite's `navigator.serial` mock (`fake/rc1.js`) keeps every Chrome rule it has (user gesture, filters, `bufferSize` chunks, close rules, `disconnect`/`connect`), but its device is `RemoteDevice`: bytes and DTR/RTS go through the test's Node process to `serial_pipe.py`, which has rc1's tty open with pyserial. A shadow of the fake's protocol engine follows what the app asked rc1 for (mode, bus rates, `log()`), so the specs' checks read as with the fake; `encoded` counts the frames rc1 really sent. rc1 unplugged (the pipe loses the port): the page gets `disconnect`; the tty back: `connect` |
| `serial_pipe.py` | pyserial side (from resolve-hil's `tools/web/serial_pipe.py`, the display's bridge), reads batched |
| `can_peer.py` | The other node on rc1's bus, stdlib SocketCAN only: `setup` (classic or FD rates, sudo), `send` a pattern (`classic`, `fd64`: sequence numbers; `leaf`: the Leaf IDs; `fd-mix`: every FD length with BRS on/off, 11/29-bit, classic and remote) and print the frames sent; every frame must be **ACKed** (the interface's tx counter), else the queue is dropped and it exits 1 (rc1's channel isn't open); `release`/`reclaim` rc1's own SocketCAN interface (gs_usb, `can0`); `restore` the rig's 500 kbit/s classic |
| `peer.mjs` | The specs' side: `peer.setup/send/restore`, `rigBus(test, bus)` (rc1 released from gs_usb and the peer set up for a spec file, both put back after), `arrived(page, n)` (waits for n frames: on the real rc1 the fake's `settle()` can't know what is still on its way), `usb('off' | 'on')` (rc1's hub port, data and 5 V) |
| `*.spec.mjs` | The rig specs (below), Playwright project `rig` (`WSC_RIG=1`; then only this project runs) |
| `chrome_check.mjs` | The owner's check (docs/TESTING.md) with Chromium's own Web Serial, headed under Xvfb; rc1 allowed for http://127.0.0.1:8752 by Chromium policy (resolve-hil `host/chromium/resolve-hil-serial.json`) |

**rc1's gs_usb interface must be down**: while Linux has rc1's SocketCAN interface (gs_usb, `can0` on the rig) up, the
firmware gives the CAN channel to it and the serial port gets no frames (no error either). `rigBus` takes it down and
puts it back up after if it was up.

Settings (env): `WSC_RC1_TTY` (default: the first `/dev/serial/by-id/usb-resolve-can_resolve-can-1_*`),
`WSC_RC1_PEER` (`hilcan1`), `WSC_RC1_USB` (a command taking `off`/`on`; default resolve-hil's `usb_port.py` on
1-4.3 port 4), `WSC_HIL` (resolve-hil checkout), `WSC_RC1_TRACE=<file>` (every byte from rc1, as the page gets it),
`WSC_RC1_DEBUG=1` (device and bridge counters printed), `WSC_RIG_WRAP_MINUTES` (timewrap's length, default 75).

## Specs

| Spec | Checks |
| --- | --- |
| connect | classic 500 kbit/s, GVRET and LAWICEL through the UI: 2000 frames from the peer in the trace exactly (order, IDs, data), none dropped |
| fd | CAN FD 500k/2M, GVRET and LAWICEL: 800 fd-mix frames exactly; a continuous log of 2000: the download's rows as sent, rc1's µs times spanning the 2 s |
| autoconnect | GVRET and LAWICEL with FD: after a reload connected by itself (same bus, no chooser, no connection window), frames arrive |
| unplug | rc1's hub port off (data and 5 V) mid-stream: disconnected, no page error; on again: reconnected (canvas clicks, no a11y layer, as traffic.spec), 500 frames exactly |
| rate | back to back at the bus's limit: 40 000 classic frames at 1 Mbit/s, 20 000 64-byte BRS frames at 500k/4M: every frame, the newest 2000 in order, none dropped, no stall over 1 s (`results/rig-rate-*.json`) |
| tools | a live graph of 0x1DA (every frame of it, last value, follows the end), Temporal Graph (every frame of every ID, rc1's time span), Frame Data Analysis of 64-byte FD IDs (every bit's changes and sets as the peer sent them); pictures `results/rig-*.png` |
| timewrap `@long` | 75 min of logging at 100 frames/s, past rc1's clock wrap at 71.6 min: every frame in the download, in order, times increasing and 10 ms apart, memory each minute (`results/rig-timewrap.json`) |

## Results

2026-10-29, main 8a5145d (dist from CI), rc1 firmware 0.1.3: connect, fd, autoconnect, unplug, rate and tools passed
(12 tests). Full rate measured: classic 1 Mbit/s 7 512 frames/s, FD64 BRS 500k/4M 4 577 frames/s, nothing dropped;
the page's 10 ms timer p95 2.3 / 34.5 ms. timewrap: see RESOLVE.md.

`chrome_check.mjs` (Chromium 141, its own Web Serial, headed and headless): `getPorts()` lists rc1 and `open()`
succeeds, but the first read fails "The device has been lost". strace: Chromium's first `read()` on the tty returns 0
(no data yet, non-blocking, VMIN = VTIME = 0), which its Linux serial code takes as the device gone. Same as the
display's port (resolve-hil docs/findings.md 2026-10-07, now with this cause), on this host's kernel 7.0 / Ubuntu
26.04. So on the rig the bridge is the way; the owner's Chrome on macOS/Windows is not affected by this.
