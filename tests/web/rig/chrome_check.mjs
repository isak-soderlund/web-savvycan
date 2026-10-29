#!/usr/bin/env node
// The app in Chromium with its REAL Web Serial and the real rc1 (no mock, no bridge): what the owner's check in Chrome
// does (docs/TESTING.md), automated on the rig. rc1 must be allowed for the page's origin without the chooser (nobody
// to click it): Chromium policy SerialAllowUsbDevicesForUrls, 1209:0001 for http://127.0.0.1:8752 (resolve-hil
// host/chromium/resolve-hil-serial.json, installed by its bootstrap.sh). Headed under Xvfb by default (xvfb-run);
// --headless tries new-headless (which lost the display's port on this host: resolve-hil docs/findings.md 2026-10-07).
//
//   xvfb-run -a node tests/web/rig/chrome_check.mjs [--headless] [--dist DIR] [--no-unplug]
//
// Steps: connect GVRET with CAN FD 500k/2M through the UI (rc1 listed by getPorts(): no Choose device...), the peer's
// fd-mix frames in the trace exactly, a continuous log downloaded with every row, a reload connects by itself
// (auto-connect), rc1 unplugged for real (hub port off) while frames arrive: disconnected, the page alive; plugged back
// in: connected again (auto-connect on the `connect` event, else through the connection window), frames again.
// Prints each step, writes results/rig-chrome.json and pictures results/rig-chrome-*.png; exit 1 on a failure.
import { spawn } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { chromium } from '@playwright/test';
import { AppDriver, DIST, RESULTS, WEB, saveResult } from '../lib/harness.mjs';
import { peer, usb } from './peer.mjs';

const args = process.argv.slice(2);
const headless = args.includes('--headless');
const dist = args.includes('--dist') ? args[args.indexOf('--dist') + 1] : DIST;
const PORT = 8752;
const steps = [];
let failed = false;
const step = (name, ok, detail = '') => {
  steps.push({ name, ok, detail });
  if (!ok) failed = true;
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${detail ? ': ' + detail : ''}`);
};
const norm = (f) => ({ id: f.id, ext: !!f.ext, fd: !!f.fd, brs: !!f.fd && !!f.brs, data: f.rtr ? [] : Array.from(f.data) });
const sameFrames = (a, b) => JSON.stringify(a.map(norm)) === JSON.stringify(b.map(norm));

const server = spawn('node', [join(WEB, 'server.mjs'), '--port', String(PORT), '--dist', dist], { stdio: 'ignore' });
await new Promise((r) => setTimeout(r, 800));
const released = peer.release();
peer.setup({ bitrate: 500000, dbitrate: 2000000 });
const browser = await chromium.launch({ channel: 'chromium', headless,
  args: ['--enable-experimental-web-platform-features'] });
const context = await browser.newContext({ viewport: { width: 1280, height: 800 }, acceptDownloads: true });
const page = await context.newPage();
const errors = [];
page.on('console', (m) => m.type() === 'error' && errors.push(m.text()));
page.on('pageerror', (e) => errors.push(String(e)));
const app = new AppDriver(page);
app.open = async () => { await page.goto(`http://127.0.0.1:${PORT}/SavvyCAN.html`); await app.enableA11y(); };
const stats = () => page.evaluate(() => window.webSavvyCAN.stats());
const waitFrames = async (n, ms = 15000) => {
  const end = Date.now() + ms;
  while (Date.now() < end) {
    const s = await stats();
    if (s.framesReceived >= n && s.queued === 0) return s;
    await page.waitForTimeout(100);
  }
  return stats();
};

/** GVRET through the UI as the owner does it (TESTING.md step 2), rc1 already allowed. */
async function connect() {
  await app.menu('Connection', 'Open Connection Window');
  await app.click('button', 'Add New Device Connection');
  await app.click('radio', 'Serial Connection (GVRET)');
  await app.click('button', 'Create New Connection');
  await page.waitForFunction(() => window.webSavvyCAN.stats().connected, null, { timeout: 20000 });
  const group = app.role('group', 'Bus Details:');
  await page.waitForFunction(() => true);
  await page.waitForTimeout(4000);
  const boxes = group.getByRole('checkbox');
  const [listen, enable, canFd] = [boxes.nth(0), boxes.nth(1), boxes.nth(2)];
  await app.pickCombo(listen, 'Shift+Tab', 500000);
  await app.setChecked(listen, false);
  await app.setChecked(enable, true);
  await app.setChecked(canFd, true);
  await app.pickCombo(canFd, 'Tab', 2000000);
  await app.click('button', 'Save Bus Settings');
  await page.waitForTimeout(1500);
  await app.closeConnectionWindow();
}

try {
  console.log(`Chromium ${browser.version()}, ${headless ? 'new headless' : 'headed (Xvfb)'}, dist ${readFileSync(join(dist, 'SOURCE_COMMIT'), 'utf8').trim().split('\n')[0]}`);
  await app.open();
  const ports = await page.evaluate(async () => (await navigator.serial.getPorts()).map((p) => p.getInfo()));
  step('rc1 allowed (getPorts, no chooser)', ports.some((p) => p.usbVendorId === 0x1209 && p.usbProductId === 1), JSON.stringify(ports));
  await connect();
  step('connected over GVRET', (await stats()).connected);

  let base = (await stats()).framesReceived;
  let sent = await peer.send('fd-mix', { count: 800, rate: 800 });
  let s = await waitFrames(base + 800);
  let tail = await page.evaluate((n) => window.webSavvyCAN.traceTail(n), 800);
  step('800 fd-mix frames (every FD length, BRS, 29-bit, remote) exactly in the trace',
    sent.acked === 800 && s.framesReceived - base === 800 && sameFrames(tail, sent.frames),
    `acked ${sent.acked}, received ${s.framesReceived - base}, dropped ${s.framesDropped}`);
  await page.screenshot({ path: join(RESULTS, 'rig-chrome-trace.png') });

  await app.startLog();
  base = (await stats()).framesReceived;
  sent = await peer.send('fd-mix', { count: 2000, rate: 1000 });
  await waitFrames(base + 2000);
  const dl = await app.stopLog();
  const file = join(RESULTS, 'rig-chrome-log.csv');
  await dl.saveAs(file);
  // SavvyCAN's native CSV: time, ID (hex), extended, dir, bus, length, data bytes..., FD flags.
  const rows = readFileSync(file, 'latin1').split('\n').slice(1).filter(Boolean).map((r) => {
    const c = r.split(',');
    const len = Number(c[5]);
    return { id: parseInt(c[1], 16), data: c.slice(6, 6 + len).map((x) => parseInt(x, 16)) };
  });
  const want = sent.frames.map((f) => ({ id: f.id, data: f.rtr ? [] : f.data }));
  step('continuous log: one download, every frame as a row, as sent', JSON.stringify(rows) === JSON.stringify(want),
    `${dl.suggestedFilename()}, ${rows.length} rows`);

  await app.reload();
  const autoOk = await page.waitForFunction(() => window.webSavvyCAN.stats().connected, null, { timeout: 20000 }).then(() => true, () => false);
  await page.waitForTimeout(4000);
  base = (await stats()).framesReceived;
  sent = await peer.send('fd-mix', { count: 400, rate: 400 });
  s = await waitFrames(base + 400);
  step('after a reload: connected by itself, frames arrive', autoOk && sent.acked === 400 && s.framesReceived - base === 400,
    `acked ${sent.acked}, received ${s.framesReceived - base}`);

  if (!args.includes('--no-unplug')) {
    const streaming = peer.send('classic', { count: 3000, rate: 1000, list: false });
    await page.waitForTimeout(1000);
    usb('off');
    const lost = await page.waitForFunction(() => !window.webSavvyCAN.stats().connected, null, { timeout: 10000 }).then(() => true, () => false);
    await streaming;
    step('unplugged: shown disconnected, the page alive', lost && (await page.evaluate(() => 1 + 1)) === 2);
    await page.screenshot({ path: join(RESULTS, 'rig-chrome-unplugged.png') });
    usb('on');
    let back = await page.waitForFunction(() => window.webSavvyCAN.stats().connected, null, { timeout: 20000 }).then(() => true, () => false);
    let how = 'by itself';
    if (!back) {
      how = 'through the connection window';
      // Qt 6.12's accessibility code breaks on unplug (README): reload, then the remembered connection comes back.
      await app.reload();
      back = await page.waitForFunction(() => window.webSavvyCAN.stats().connected, null, { timeout: 20000 }).then(() => true, () => false);
      how = 'after a reload (auto-connect)';
    }
    await page.waitForTimeout(4000);
    base = (await stats()).framesReceived;
    sent = await peer.send('classic', { count: 500, rate: 500 });
    s = await waitFrames(base + 500);
    step('plugged back in: connected again, frames arrive', back && sent.acked === 500 && s.framesReceived - base === 500,
      `${how}; acked ${sent.acked}, received ${s.framesReceived - base}`);
  }
  step('no console errors', errors.length === 0, errors.slice(0, 5).join(' | '));
} catch (e) {
  step('ran to the end', false, String(e).slice(0, 500));
} finally {
  await page.screenshot({ path: join(RESULTS, 'rig-chrome-end.png') }).catch(() => {});
  await browser.close();
  server.kill();
  try { usb('on'); } catch {}
  peer.restore();
  peer.reclaim(released);
}
saveResult('rig-chrome', { headless, steps, errors });
console.log(failed ? 'FAILED' : 'all steps passed');
process.exit(failed ? 1 : 0);
