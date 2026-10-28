// End-to-end in headless Chromium: the wasm SavvyCAN (a dist/ from scripts/build-wasm.sh) with the navigator.serial
// mock (mock/mock-serial.js) in place of a real rc1. Drives the app's own UI through Qt's accessibility tree:
// Connection window -> Add New Device Connection -> GVRET or LAWICEL -> Choose device -> Create New Connection, then
// checks on the mock that the session runs (GVRET handshake, validation every 250 ms / slcan open), sends bus frames,
// unplugs and replugs. Screenshots in the output directory.
//
//   node wasm/webserial/tests/e2e.mjs --dist dist [--out /tmp/wsc-webserial-e2e] [--protocol gvret|lawicel|both]
//        [--granted] (a returning visitor: the port is listed without "Choose device")
// Playwright: from PLAYWRIGHT_DIR (a node_modules that has `playwright`), else ~/src/web-savvycan-tests/tests/web.
import { createServer } from 'node:http';
import { createRequire } from 'node:module';
import { existsSync, mkdirSync, readFileSync, statSync } from 'node:fs';
import { dirname, extname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { homedir } from 'node:os';

const here = dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const opt = (name, dflt) => {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : dflt;
};
const DIST = resolve(opt('--dist', join(here, '../../../dist')));
const OUT = resolve(opt('--out', '/tmp/wsc-webserial-e2e'));
const PROTOCOLS = opt('--protocol', 'both') === 'both' ? ['gvret', 'lawicel'] : [opt('--protocol')];
const GRANTED = args.includes('--granted');
const pwDir = process.env.PLAYWRIGHT_DIR || join(homedir(), 'src/web-savvycan-tests/tests/web');
const { chromium } = createRequire(join(pwDir, 'package.json'))('playwright');
const MOCK = readFileSync(join(here, '../mock/mock-serial.js'), 'utf8');
mkdirSync(OUT, { recursive: true });

const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.svg': 'image/svg+xml' };
function serve() {
  const server = createServer((req, res) => {
    const path = join(DIST, decodeURIComponent(new URL(req.url, 'http://x').pathname));
    if (!path.startsWith(DIST) || !existsSync(path) || statSync(path).isDirectory()) {
      res.writeHead(404).end();
      return;
    }
    res.writeHead(200, { 'Content-Type': TYPES[extname(path)] || 'application/octet-stream' });
    res.end(readFileSync(path));
  });
  return new Promise((ok) => server.listen(0, '127.0.0.1', () => ok(server)));
}

let failures = 0;
const check = (ok, what) => {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}`);
  if (!ok) failures++;
};

async function waitFor(page, fn, arg, ms = 15000) {
  try {
    await page.waitForFunction(fn, arg, { timeout: ms, polling: 100 });
    return true;
  } catch {
    return false;
  }
}

async function click(page, role, name) {
  const loc = page.getByRole(role, { name, exact: false }).first();
  await loc.waitFor({ timeout: 15000 });
  await loc.click({ force: true });
}

async function run(browser, base, protocol) {
  const page = await browser.newPage({ viewport: { width: 1400, height: 900 } });
  const logs = [];
  page.on('console', (m) => logs.push(`[${m.type()}] ${m.text()}`));
  page.on('pageerror', (e) => logs.push(`[pageerror] ${e.message}`));
  await page.addInitScript({ content: `${MOCK}\n;installMockSerial({ granted: ${GRANTED} });` });
  await page.goto(`${base}/SavvyCAN.html`);
  const started = await waitFor(page, () => document.querySelector('canvas, [role=application], .qt-window'), null, 60000);
  check(started, `${protocol}: app started`);
  await page.waitForTimeout(1500);
  const a11y = page.getByRole('button', { name: /enable screen reader/i });
  if (await a11y.count()) await a11y.first().click({ force: true });

  try {
    await click(page, 'menuitem', 'Connection');
    await click(page, 'menuitem', 'Open Connection Window');
    await click(page, 'button', 'Add New Device Connection');
    await page.screenshot({ path: join(OUT, `${protocol}-1-dialog.png`) });
    await click(page, 'radio', protocol === 'gvret' ? 'GVRET' : 'LAWICEL');
    if (!GRANTED) {
      await click(page, 'button', 'Choose device');
      check(await waitFor(page, () => window.__mockSerial.serial.requests.length > 0), `${protocol}: requestPort called`);
      const filters = await page.evaluate(() => window.__mockSerial.serial.requests[0].filters);
      check(JSON.stringify(filters) === JSON.stringify([{ usbVendorId: 0x1209, usbProductId: 1 }]),
        `${protocol}: chooser filtered to rc1 (${JSON.stringify(filters)})`);
    }
    check(await waitFor(page, () => window.resolveWebSerial.ports().length === 1), `${protocol}: port listed`);
    await page.waitForTimeout(500);
    await page.screenshot({ path: join(OUT, `${protocol}-2-chosen.png`) });
    await click(page, 'button', 'Create New Connection');
  } catch (e) {
    check(false, `${protocol}: UI step: ${e.message.split('\n')[0]}`);
    await page.screenshot({ path: join(OUT, `${protocol}-ui-failed.png`) });
    await page.close();
    return logs;
  }

  check(await waitFor(page, () => window.__mockSerial.port.openCount >= 1), `${protocol}: port opened`);
  if (protocol === 'gvret') {
    // ESP32 mode first: DTR/RTS low, 3 s, handshake; connected once the validation ticks run (every 250 ms).
    check(await waitFor(page, () => window.__mockSerial.device.gvret, null, 10000), 'gvret: device switched to GVRET');
    check(await waitFor(page, () => (window.__mockSerial.device.ops[9] || 0) >= 5, null, 15000),
      'gvret: connected (comm validation running)');
    const sig = await page.evaluate(() => window.__mockSerial.port.signals[0]);
    check(sig && !sig.dataTerminalReady && !sig.requestToSend, `gvret: DTR/RTS low (${JSON.stringify(sig)})`);
  } else {
    check(await waitFor(page, () => window.__mockSerial.device.log.includes('slcan O'), null, 10000),
      `lawicel: bus opened (${await page.evaluate(() => window.__mockSerial.device.log.join(', '))})`);
  }
  // Open the bus for GVRET as SavvyCAN would after "Save Bus Settings" (the mock's state), then traffic.
  await page.evaluate(() => { window.__mockSerial.device.enabled = true; });
  const sent = await page.evaluate(() => {
    let n = 0;
    for (let i = 0; i < 200; i++) n += window.__mockSerial.device.busFrame({ id: 0x100 + (i % 8), data: [i & 255, 1, 2, 3, 4, 5, 6, 7] }) ? 1 : 0;
    return n;
  });
  check(sent === 200, `${protocol}: 200 frames on the bus`);
  await page.waitForTimeout(1500);
  await page.screenshot({ path: join(OUT, `${protocol}-3-traffic.png`) });

  // Unplug: SavvyCAN gets ResourceError and closes; replug: listed again.
  await page.evaluate(() => window.__mockSerial.port.unplug());
  check(await waitFor(page, () => window.resolveWebSerial.sessions() === 0, null, 5000), `${protocol}: closed after unplug`);
  await page.evaluate(() => window.__mockSerial.port.replug());
  check(await waitFor(page, () => window.resolveWebSerial.ports().length === 1, null, 5000), `${protocol}: listed after replug`);
  await page.screenshot({ path: join(OUT, `${protocol}-4-unplugged.png`) });
  const errors = logs.filter((l) => l.startsWith('[pageerror]') || /Aborted|RuntimeError|unreachable/.test(l));
  check(errors.length === 0, `${protocol}: no page errors ${errors.slice(0, 3).join(' | ')}`);
  await page.close();
  return logs;
}

const server = await serve();
const base = `http://127.0.0.1:${server.address().port}`;
const browser = await chromium.launch();
try {
  for (const p of PROTOCOLS) {
    const logs = await run(browser, base, p);
    const { writeFileSync } = await import('node:fs');
    writeFileSync(join(OUT, `${p}-console.log`), logs.join('\n'));
  }
} finally {
  await browser.close();
  server.close();
}
console.log(`${failures ? 'FAILED' : 'passed'}; screenshots and console logs in ${OUT}`);
process.exit(failures ? 1 : 0);
