// The rig's side of the real-rc1 tests: the CAN peer (can_peer.py on the Kvaser channel wired to rc1, WSC_RC1_PEER,
// default hilcan1) and rc1's own SocketCAN interface, which must be down while the browser has rc1's channel.
import { execFile, execFileSync } from 'node:child_process';
import { existsSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const PEER = join(dirname(fileURLToPath(import.meta.url)), 'can_peer.py');
const last = (out) => JSON.parse(out.trim().split('\n').pop());
// Async where the page must keep running meanwhile (the bridge lives in this process's event loop).
const runAsync = (...args) => new Promise((resolve, reject) => execFile('python3', [PEER, ...args],
  { encoding: 'utf8', maxBuffer: 1 << 28 }, (err, out) => (err && !out ? reject(err) : resolve(last(out)))));
const run = (...args) => JSON.parse(execFileSync('python3', [PEER, ...args], { encoding: 'utf8', maxBuffer: 1 << 28 }).trim().split('\n').pop());

export const peer = {
  /** The peer's bus: classic at `bitrate`, or CAN FD with `dbitrate`. */
  setup: ({ bitrate = 500000, dbitrate = 0 } = {}) => run('setup', '--bitrate', String(bitrate), ...(dbitrate ? ['--dbitrate', String(dbitrate)] : [])),
  /** The rig's own setting (500 kbit/s classic). */
  restore: () => run('restore'),
  state: () => run('state'),
  /** Sends `pattern` frames; resolves to {sent, seconds, frames: [{id, ext, fd, brs, rtr, data}]}. `re`: `state` (the
   * state bits' value) or `stateFile` (stateFile().file: followed while sending). */
  send: (pattern, { count, rate = 1000, seconds, list = true, state, stateFile } = {}) => runAsync('send', '--pattern', pattern,
    ...(count ? ['--count', String(count)] : []), '--rate', String(rate), ...(seconds ? ['--seconds', String(seconds)] : []),
    ...(list ? [] : ['--no-list']), ...(state !== undefined ? ['--state', String(state)] : []),
    ...(stateFile ? ['--state-file', stateFile] : [])),
  /** rc1's gs_usb interfaces down; returns the names that were up (give them to reclaim()). */
  release: () => run('release'),
  reclaim: (names) => (names && names.length ? run('reclaim', ...names) : []),
};

/** The `re` pattern's state on cue: a file the running peer follows (send(..., {stateFile})); set(n) writes it. */
export function stateFile(name = 'rig-re-state') {
  const file = join(tmpdir(), `${name}-${process.pid}`);
  writeFileSync(file, '0');
  return { file, set: (n) => writeFileSync(file, String(n)), remove: () => rmSync(file, { force: true }) };
}

/**
 * rc1's USB port off (data and 5 V) or on: WSC_RC1_USB is the command (gets `off` / `on`), default resolve-hil's
 * usb_port.py on rc1's hub port (1-4.3 port 4 on this rig, `resolve_can.usb_port` in its rig.yaml).
 */
export function usb(state) {
  const cmd = process.env.WSC_RC1_USB;
  if (cmd) return execFileSync('sh', ['-c', `${cmd} ${state}`], { encoding: 'utf8' });
  const hil = process.env.WSC_HIL || `${process.env.HOME}/src/resolve-hil`;
  const tool = [`${hil}/tools/usb_port.py`, `${process.env.HOME}/src/resolve-hil-rc1/tools/usb_port.py`].find((f) => existsSync(f));
  if (!tool) throw new Error('no usb_port.py: set WSC_RC1_USB to a command that switches rc1\'s USB port (off/on)');
  return execFileSync(`${hil}/.venv/bin/python`, [tool, state, '1-4.3', '4', '--both'], { encoding: 'utf8' });
}

/** For a spec file: rc1 released from gs_usb and the peer set up before, both put back after. */
export function rigBus(test, bus = {}) {
  let released = [];
  test.beforeAll(() => {
    released = peer.release();
    peer.setup(bus);
  });
  test.afterAll(() => {
    peer.restore();
    peer.reclaim(released);
  });
}

/** Waits until the app has parsed `frames` frames and has nothing queued (or `ms` passed) -> [device, app] stats. On
 * the real rc1 the fake's settle() can't know what is still on its way: the test says how many to expect. */
export async function arrived(page, frames, ms = 10000) {
  const end = Date.now() + ms;
  for (;;) {
    const [s, a] = await page.evaluate(() => [window.__rc1.stats(), window.webSavvyCAN.stats()]);
    if ((a.framesReceived >= frames && a.queued === 0 && s.queuedBytes === 0) || Date.now() > end) {
      if (process.env.WSC_RC1_DEBUG) console.log('device', JSON.stringify(s), 'app', JSON.stringify(a));
      return [s, a];
    }
    await page.waitForTimeout(100);
  }
}
