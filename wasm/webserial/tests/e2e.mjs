// End-to-end in headless Chromium: the wasm SavvyCAN (a dist/ from scripts/build-wasm.sh) with the test harness's fake
// rc1 and navigator.serial mock (tests/web/fake/rc1.js) in place of a real rc1. Drives the app's own UI through Qt's accessibility tree:
// Connection window -> Add New Device Connection -> GVRET or LAWICEL -> Choose device -> Create New Connection, then
// checks on the fake that the session runs (GVRET mode and its validation polls / slcan open), Leaf traffic arrives
// with nothing lost in rc1, unplugs and replugs. Screenshots in the output directory.
//
//   node wasm/webserial/tests/e2e.mjs --dist dist [--out /tmp/wsc-webserial-e2e] [--protocol gvret|lawicel|both]
//        [--granted] (a returning visitor: the port is listed without "Choose device")
// Playwright and the fake: from PLAYWRIGHT_DIR / WSC_RC1_FAKE, else tests/web in this tree.
import { createServer } from 'node:http';
import { createRequire } from 'node:module';
import { existsSync, mkdirSync, readFileSync, statSync } from 'node:fs';
import { dirname, extname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

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
const webDirs = [join(here, '../../../tests/web')];
const pwDir = process.env.PLAYWRIGHT_DIR || webDirs.find((d) => existsSync(join(d, 'node_modules/playwright')));
const { chromium } = createRequire(join(pwDir, 'package.json'))('playwright');
const FAKE = process.env.WSC_RC1_FAKE || webDirs.map((d) => join(d, 'fake/rc1.js')).find((p) => existsSync(p));
const FAKE_SRC = readFileSync(FAKE, 'utf8');
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
  await page.addInitScript({ content: `${FAKE_SRC}\n;RC1Fake.installWebSerial({ granted: ${GRANTED} });` });
  await page.goto(`${base}/SavvyCAN.html`);
  // Qt 6.12 puts its windows in a shadow root (#qt-shadow-container): document.querySelector can't see them.
  // Its accessibility tree starts empty but for a visually hidden "Enable Screen Reader" button, which only reacts to a
  // DOM click (Playwright's click on it does nothing); after that, menus and widgets are reachable by role.
  const shadowButton = () => {
    const r = document.querySelector('#qt-shadow-container');
    return r && r.shadowRoot && r.shadowRoot.querySelector('button.hidden-visually-read-by-screen-reader');
  };
  const started = await waitFor(page, `!!(${shadowButton})()`, null, 60000);
  check(started, `${protocol}: app started`);
  await page.waitForTimeout(1000);
  await page.evaluate(`(${shadowButton})().click()`);

  try {
    await click(page, 'menuitem', 'Connection');
    await click(page, 'menuitem', 'Open Connection Window');
    await click(page, 'button', 'Add New Device Connection');
    await page.screenshot({ path: join(OUT, `${protocol}-1-dialog.png`) });
    await click(page, 'radio', protocol === 'gvret' ? 'GVRET' : 'LAWICEL');
    if (!GRANTED) {
      await click(page, 'button', 'Choose device');
      const asked = () => window.__rc1.events.filter((e) => e.type === 'requestPort');
      check(await waitFor(page, `(${asked})().length > 0`), `${protocol}: requestPort called (from the click: Chrome's gesture rule)`);
      const filters = await page.evaluate(`(${asked})()[0].filters`);
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

  check(await waitFor(page, () => window.__rc1.stats().sessions >= 1), `${protocol}: port opened`);
  if (protocol === 'gvret') {
    // ESP32 mode first: DTR/RTS low, 3 s, handshake; once connected SavvyCAN polls every 250 ms.
    check(await waitFor(page, () => window.__rc1.stats().mode === 'gvret', null, 10000), 'gvret: device switched to GVRET');
    await page.waitForTimeout(6000);
    const b0 = await page.evaluate(() => window.__rc1.stats().bytesFromHost);
    await page.waitForTimeout(1500);
    const b1 = await page.evaluate(() => window.__rc1.stats().bytesFromHost);
    check(b1 - b0 >= 8, `gvret: connected (validation polls: ${b1 - b0} bytes in 1.5 s)`);
    const sig = await page.evaluate(() => window.__rc1.ports[0]._signals);
    check(sig && !sig.dataTerminalReady && !sig.requestToSend, `gvret: DTR/RTS low (${JSON.stringify(sig)})`);
    // The bus: SavvyCAN's own bus settings in the connection window.
    try {
      // Its checkboxes have no names of their own (their labels aren't buddies): all read "Bus Details:", in the .ui's
      // order Listen only, Enable, CAN FD.
      let enable = page.getByRole('checkbox', { name: 'Enable Bus', exact: false });
      if (!(await enable.count())) enable = page.getByRole('checkbox', { name: 'Bus Details:' }).nth(1);
      await enable.first().click({ force: true, timeout: 5000 });
      await click(page, 'button', 'Save Bus Settings');
    } catch (e) {
      const snap = await page.accessibility.snapshot().catch(() => null);
      const flat = (n) => (n ? [`${n.role} "${n.name}"`].concat(...(n.children || []).map(flat)) : []);
      const names = flat(snap).filter((l) => /^(checkbox|button|combobox|spinbutton|row|cell)/.test(l));
      console.log(`note gvret: bus settings UI not reached (${e.message.split('\n')[0]}); controls: ${names.slice(0, 60).join(', ')}`);
    }
  }
  const open = await waitFor(page, () => window.__rc1.stats().channel.open, null, 10000);
  check(open, `${protocol}: CAN channel open (${await page.evaluate(() => JSON.stringify(window.__rc1.requests()))})`);
  if (open) {
    await page.evaluate(() => window.__rc1.setTraffic('leaf', { seconds: 3 }));
    await page.waitForTimeout(4000);
    const st = await page.evaluate(() => window.__rc1.stats());
    check(st.encoded > 1000 && st.lost === 0 && st.queuedBytes < 1024,
      `${protocol}: Leaf traffic read (${st.encoded} frames sent to the app, ${st.lost} lost in rc1, ${st.queuedBytes} B unread)`);
    await page.screenshot({ path: join(OUT, `${protocol}-3-traffic.png`) });
  }

  // Unplug: SavvyCAN gets ResourceError and closes; replug: listed again.
  await page.evaluate(() => window.__rc1.unplug());
  check(await waitFor(page, () => window.resolveWebSerial.sessions() === 0, null, 5000), `${protocol}: closed after unplug`);
  await page.evaluate(() => window.__rc1.replug());
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
