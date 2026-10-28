# Testing stage 1 with a real rc1 (owner)

Stage 1 (RESOLVE.md "MVP"): connect to rc1 from Chrome, watch the frames, log them to a file, light/dark theme.
Everything below has passed in headless Chromium against a **fake** rc1 (see "What only the fake has checked"); this
is the check with the real device, the real Chrome and a real bus.

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

From the rig, without copying anything (the page runs in your Chrome, rc1 is plugged into your PC):

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

1. **Start.** The page loads (a few seconds: ~9 MB). SavvyCAN's main window: File, View, Connection, Help;
   only stage-1 entries are enabled.
2. **Connect over GVRET.** Connection > Open Connection Window > Add New Device Connection > **Serial Connection
   (GVRET)** > **Choose device...** > pick *resolve-can-1* in Chrome's list > Connect > **Create New Connection**.
   - The list shows GVRET / WebSerial ... / Connected after ~3-4 s (SavvyCAN's ESP32 start: DTR/RTS low, 3 s wait,
     then the GVRET handshake and its validation polls).
   - **Bus Details**: Speed **500000**, tick **Enable Bus**, **Enable CAN FD** as your bus is (with CAN FD:
     **CAN FD Data Rate**, e.g. 2000000), then **Save Bus Settings**.
   - Close the connection window (its close button, or Escape); the connection stays up.
   - Don't press **Enter** inside the connection window: it presses Disconnect Selected Device (known issue below).
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
5. **Theme.** View > **Dark Theme**: the whole window goes dark (trace rows too). Reload the page (Cmd-R / F5):
   still dark. View > **Light Theme**, View > **Follow System Theme** (then it follows the OS setting).
6. **Unplug.** While connected and receiving, pull rc1's USB cable: the connection shows as disconnected, the page
   keeps working (no error page, no freeze). Plug it back in, Add New Device Connection again (rc1 is listed without
   the chooser now) and Create New Connection: frames come again.
7. **Returning visit.** Close the tab, open the page again: the theme is kept, and rc1 is listed in the New
   Connection dialog without Choose device... (Chrome remembers the permission).
8. **Optional: LAWICEL.** Same as 2 with **LAWICEL / SLCAN Serial**: set CAN Bus Speed (and CAN-FD, its data rate)
   in the New Connection dialog before Create New Connection; there are no Bus Details to save.

Please note for each step: works / doesn't (what you saw, a screenshot), Chrome's version and the OS, and for any
problem the browser console (View > Developer > JavaScript Console): copy its red lines.

## What only the fake has checked

The automated suite (`tests/web/run.sh`) runs the real app build in headless Chromium, but the device, the USB link
and the bus are simulated (`tests/web/fake/rc1.js`, a port of the firmware's serial code; RESOLVE.md "Tests"). Not
checked by it, so checked only by this test:

- **Chrome's real Web Serial**: the chooser dialog, the permission remembered across visits, real USB timing and
  buffering, real unplug events (the suite uses a `navigator.serial` mock that follows Chrome's documented behaviour).
- **rc1's real firmware**: the fake is ported from firmware 0.1.3; differences in the real device (GVRET handshake
  timing, DTR/RTS handling, `SETUP_CANBUS`/`SETUP_FD`, slcan commands) only show here.
- **A real CAN bus**: bit rates, FD data phase, ACKs, error frames and bus-off are only "set up" in the fake (it
  records the commands); frames come from a generator, not a bus.
- **Throughput**: the suite sends rc1's measured full rates (8 000 classic / 6 900 FD64 frames/s) through the mock;
  real USB full-speed transfers and a real busy car bus are not covered.
- **The download**: headless Chromium's download event; Chrome's own download UI ("Ask where to save each file"),
  your downloads folder and multi-GB logs on your PC are not (multi-GB was measured on the rig: RESOLVE.md "Tests").
- **The theme on a real OS**: the suite emulates `prefers-color-scheme`; switching the OS theme while the page is
  open is not tested.
- **Settings across browser restarts**: the suite reloads the page; quitting Chrome is not tested.
- **Other browsers**: the app was only run in Chromium.

## Known issues (2026-10-28, branch `integration`)

- Pressing **Enter** in the connection window presses *Disconnect Selected Device* (also when Enter picks an item in
  an open drop-down list): the window's default button. Use the mouse in the drop-downs.
- With a **screen reader** (Qt's accessibility layer switched on), unplugging rc1 makes Qt 6.12 throw an error in its
  accessibility code and the window stops drawing; reload the page. Without a screen reader unplugging is fine.
