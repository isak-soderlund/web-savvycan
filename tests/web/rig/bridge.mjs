// The rig's Web Serial bridge: the real rc1 behind the suite's navigator.serial mock (fake/rc1.js RemoteDevice).
// Headless Chromium can't be given a real serial port without its chooser (and on the rig loses USB serial ports:
// resolve-hil docs/findings.md 2026-10-07), so the bytes go through this Node process and serial_pipe.py (pyserial)
// to rc1's tty. The app's own Web Serial stand-in, GVRET and LAWICEL code runs unchanged against rc1's firmware;
// only Chrome's serial transport is replaced (the mock keeps its gesture, filter, bufferSize and close rules).
//
// rc1's tty: WSC_RC1_TTY, else the first /dev/serial/by-id/usb-resolve-can_resolve-can-1_*. Unplugged (the pipe
// reports the port lost): the page gets `disconnect`; when the tty is back, `connect` (the mock's replug).
// WSC_RC1_TRACE=<file>: every byte from rc1, as the page gets it, appended to the file.
import { spawn } from 'node:child_process';
import { existsSync, openSync, readdirSync, writeSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));

/** rc1's serial port on this machine, or null. */
export function rc1Tty() {
  if (process.env.WSC_RC1_TTY) return process.env.WSC_RC1_TTY;
  const dir = '/dev/serial/by-id';
  if (!existsSync(dir)) return null;
  const name = readdirSync(dir).find((n) => n.startsWith('usb-resolve-can_resolve-can-1_'));
  return name ? join(dir, name) : null;
}

/** serial_pipe.py's frames: 1 byte type, u32 LE length, payload. */
function startPipe(onData, onLost) {
  const child = spawn('python3', ['-u', join(here, 'serial_pipe.py')], { stdio: ['pipe', 'pipe', 'inherit'] });
  let buffer = Buffer.alloc(0);
  const waiting = { o: [], c: [], s: [] };
  child.stdout.on('data', (chunk) => {
    buffer = buffer.length ? Buffer.concat([buffer, chunk]) : chunk;
    while (buffer.length >= 5) {
      const length = buffer.readUInt32LE(1);
      if (buffer.length < 5 + length) break;
      const kind = String.fromCharCode(buffer[0]);
      const payload = buffer.subarray(5, 5 + length);
      buffer = buffer.subarray(5 + length);
      if (kind === 'R') onData(payload);
      else if (kind === 'E') onLost(payload.toString());
      else {
        const done = waiting[kind]?.shift();
        if (done) done(payload.toString());
        if (payload.length && kind === 'o') onLost(payload.toString());
      }
    }
  });
  const send = (kind, payload = Buffer.alloc(0)) => {
    const head = Buffer.alloc(5);
    head[0] = kind.charCodeAt(0);
    head.writeUInt32LE(payload.length, 1);
    child.stdin.write(Buffer.concat([head, payload]));
  };
  const call = (kind, answer, payload) => new Promise((resolve) => {
    waiting[answer].push(resolve);
    send(kind, payload);
  });
  return {
    open: (tty) => call('O', 'o', Buffer.from(tty)),
    close: () => call('C', 'c'),
    write: (bytes) => send('W', Buffer.from(bytes)),
    signals: (dtr, rts) => call('S', 's', Buffer.from([dtr ? 1 : 0, rts ? 1 : 0])),
    quit: () => { try { send('Q'); child.stdin.end(); } catch {} },
    child,
  };
}

/**
 * Connects the page's mock (installed with `serial: {remote: true}`) to the real rc1. Call before the page loads.
 * Returns {stop(), stats()}: stop() closes the port and the pipe (the fixture's teardown).
 */
export async function installBridge(page, { tty = rc1Tty() } = {}) {
  if (!tty || !existsSync(tty)) throw new Error(`rc1's serial port not found (${tty || 'no /dev/serial/by-id/usb-resolve-can_resolve-can-1_*'}): set WSC_RC1_TTY`);
  const trace = process.env.WSC_RC1_TRACE ? openSync(process.env.WSC_RC1_TRACE, 'a') : null;
  const st = { bytesIn: 0, bytesOut: 0, pushes: 0, lost: 0, opens: 0 };
  // One page.evaluate in flight at a time, the rest gathered: in order, batched under load.
  let pending = [];
  let flying = false;
  let isOpen = false;
  let lostTimer = null;
  const flush = async () => {
    if (flying || !pending.length) return;
    flying = true;
    const data = Buffer.concat(pending);
    pending = [];
    st.pushes++;
    try {
      await page.evaluate((b64) => window.__rc1Remote && window.__rc1Remote.push(b64), data.toString('base64'));
    } catch {}
    flying = false;
    flush();
  };
  const onData = (bytes) => {
    st.bytesIn += bytes.length;
    if (trace !== null) writeSync(trace, bytes);
    pending.push(Buffer.from(bytes));
    flush();
  };
  const onLost = (why) => {
    if (!isOpen) return;
    isOpen = false;
    st.lost++;
    pending = [];
    page.evaluate(() => window.__rc1Remote && window.__rc1Remote.lost()).catch(() => {});
    // Back when the tty is there again (rc1 replugged): the mock's replug, a `connect` event.
    clearInterval(lostTimer);
    lostTimer = setInterval(() => {
      if (!existsSync(tty)) return;
      clearInterval(lostTimer);
      lostTimer = null;
      page.evaluate(() => window.__rc1Remote && window.__rc1Remote.back()).catch(() => {});
    }, 250);
    if (trace !== null) writeSync(trace, `\n#WSC_RC1_LOST# ${why}\n`);
  };
  const pipe = startPipe(onData, onLost);
  await page.exposeFunction('__rc1Call', (kind, arg) => {
    if (kind === 'open') {
      st.opens++;
      isOpen = true;
      pipe.open(tty);
    } else if (kind === 'close') {
      isOpen = false;
      pending = [];
      pipe.close();
    } else if (kind === 'write') {
      st.bytesOut += arg.length;
      pipe.write(arg);
    } else if (kind === 'signals') {
      pipe.signals(arg.dtr, arg.rts);
    }
  });
  return {
    stats: () => ({ ...st, tty }),
    async stop() {
      clearInterval(lostTimer);
      await Promise.race([pipe.close(), new Promise((r) => setTimeout(r, 2000))]);
      pipe.quit();
    },
  };
}
