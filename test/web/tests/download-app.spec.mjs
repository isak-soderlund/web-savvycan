// The wasm app itself (dist/SavvyCAN.html, scripts/build-wasm.sh) with the MVP's logging (LOG_TO_DISK_LIVE
// false): a fake rc1 behind a navigator.serial mock (repo branch "tests": tests/web/fake/rc1.js; WSC_RC1_FAKE
// or ~/src/web-savvycan-tests/tests/web/fake/rc1.js) sends Leaf traffic over GVRET. SavvyCAN's own File menu
// starts logging at once (no dialog), the LOGGING label shows size and frames, Cease starts one download:
// every frame the fake sent is in it, and it loads in the desktop SavvyCAN. Then the same with
// "Choose the Log's Name When Logging Stops...": the name asked at Cease is the download's.
// Driven through Qt's accessibility tree (as wasm/webserial/tests/e2e.mjs). Skipped without dist/ or the fake.
import { test, expect } from '@playwright/test';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { REPO, OUT, note, savvycanCheck } from './common.mjs';

const DIST = path.join(REPO, 'dist/SavvyCAN.html');
const FAKE = process.env.WSC_RC1_FAKE || path.join(os.homedir(), 'src/web-savvycan-tests/tests/web/fake/rc1.js');
test.skip(!fs.existsSync(DIST), 'no wasm build in dist/ (scripts/build-wasm.sh)');
test.skip(!fs.existsSync(FAKE), `no fake rc1 (${FAKE})`);
test.use({ viewport: { width: 1400, height: 900 } });
test.setTimeout(180_000);

const SHOTS = path.join(OUT, 'app');
const shadowButton = () => {
  const r = document.querySelector('#qt-shadow-container');
  return r && r.shadowRoot && r.shadowRoot.querySelector('button.hidden-visually-read-by-screen-reader');
};

async function click(page, role, name, exact = false) {
  const loc = page.getByRole(role, { name, exact }).first();
  await loc.waitFor({ timeout: 15000 });
  await loc.click({ force: true });
}

async function menu(page, item) {
  await click(page, 'menuitem', 'File', true);
  await click(page, 'menuitem', item);
}

// The app with the fake rc1 connected over GVRET and its bus open.
async function openConnected(page) {
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  await page.addInitScript({ content: `${fs.readFileSync(FAKE, 'utf8')}\n;RC1Fake.installWebSerial({ granted: false });` });
  await page.goto('/dist/SavvyCAN.html');
  await page.waitForFunction(`!!(${shadowButton})()`, null, { timeout: 60000 });
  await page.waitForTimeout(1000);
  await page.evaluate(`(${shadowButton})().click()`);
  await click(page, 'menuitem', 'Connection');
  await click(page, 'menuitem', 'Open Connection Window');
  await click(page, 'button', 'Add New Device Connection');
  await click(page, 'radio', 'GVRET');
  await click(page, 'button', 'Choose device');
  await page.waitForFunction(() => window.resolveWebSerial.ports().length === 1, null, { timeout: 15000 });
  await click(page, 'button', 'Create New Connection');
  await page.waitForFunction(() => window.__rc1.stats().mode === 'gvret', null, { timeout: 15000 });
  await page.waitForTimeout(6000);
  let enable = page.getByRole('checkbox', { name: 'Enable Bus', exact: false });
  if (!(await enable.count())) enable = page.getByRole('checkbox', { name: 'Bus Details:' }).nth(1);
  await enable.first().click({ force: true, timeout: 5000 });
  await click(page, 'button', 'Save Bus Settings');
  await page.waitForFunction(() => window.__rc1.stats().channel.open, null, { timeout: 10000 });
  await page.keyboard.press('Escape');            // the connection window
  await page.waitForTimeout(500);
  return errors;
}

// Leaf traffic for `seconds` while logging; returns the frames the fake sent.
async function traffic(page, seconds) {
  const before = await page.evaluate(() => window.__rc1.stats().encoded);
  await page.evaluate((s) => window.__rc1.setTraffic('leaf', { seconds: s }), seconds);
  await page.waitForTimeout(seconds * 1000 + 1500);
  const st = await page.evaluate(() => window.__rc1.stats());
  expect(st.lost).toBe(0);
  return st.encoded - before;
}

const rowsIn = (file) => fs.readFileSync(file, 'latin1').split('\n').filter((l) => l.length).length - 1;

test('app: Start Continuous Logging starts at once; Cease -> one download with every frame; loads in SavvyCAN', async ({ page }) => {
  fs.mkdirSync(SHOTS, { recursive: true });
  const errors = await openConnected(page);
  await menu(page, 'Start Continuous Logging');
  await page.waitForFunction(() => SavvyDiskLog.status().state === 'open', null, { timeout: 5000 });
  const st = await page.evaluate(() => SavvyDiskLog.status());
  expect(st.backend).toBe('blob');
  expect(st.name).toMatch(/^SavvyCAN_\d{4}-\d\d-\d\d_\d\d-\d\d-\d\d\.csv$/);
  const sent = await traffic(page, 3);
  await page.waitForTimeout(1500);                 // the label is refreshed on the GUI tick
  await page.screenshot({ path: path.join(SHOTS, 'logging-label.png') });
  const dl = page.waitForEvent('download');
  await menu(page, 'Cease Continuous Logging');
  const d = await dl;
  expect(d.suggestedFilename()).toBe(st.name);
  const file = path.join(OUT, 'app-log.csv');
  await d.saveAs(file);
  const rows = rowsIn(file);
  expect(rows).toBe(sent);
  note(`${st.name}: ${fs.statSync(file).size} B, ${rows} rows = ${sent} frames sent by the fake; `
       + `${savvycanCheck(file, '--exact', String(rows), '--first', '-1')}; page errors: ${errors.length}`);
  expect(errors).toEqual([]);
  expect(await page.evaluate(() => SavvyDiskLog.status().state)).toBe('idle');
});

test('app: "Choose the Log\'s Name When Logging Stops" -> the name asked at Cease; Keep Logging keeps it running', async ({ page }) => {
  fs.mkdirSync(SHOTS, { recursive: true });
  const errors = await openConnected(page);
  await menu(page, 'Choose the Log');               // the checkable item (remembered in QSettings)
  await menu(page, 'Start Continuous Logging');
  await page.waitForFunction(() => SavvyDiskLog.status().state === 'open', null, { timeout: 5000 });
  const sent = await traffic(page, 2);
  // Cease asks for the name; "Keep Logging" (cancel) leaves the log running
  await menu(page, 'Cease Continuous Logging');
  await click(page, 'button', 'Keep Logging');
  await page.waitForTimeout(500);
  expect(await page.evaluate(() => SavvyDiskLog.status().state)).toBe('open');
  await menu(page, 'Cease Continuous Logging');
  const box = page.getByRole('textbox').first();
  await box.waitFor({ timeout: 10000 });
  await page.screenshot({ path: path.join(SHOTS, 'choose-name.png') });
  // a real click into the field (Qt's window then gets the keys; the accessibility element's box is not
  // on screen): the dialog sits in the middle of the 1400x900 window, its field at (690, 405)
  await page.mouse.click(...JSON.parse(process.env.WSC_NAME_FIELD_POS || '[690,405]'));
  await page.keyboard.press('Control+A');
  await page.keyboard.type('leaf-drive-1');
  await page.waitForTimeout(300);
  await page.screenshot({ path: path.join(SHOTS, 'choose-name-typed.png') });
  const dl = page.waitForEvent('download');
  await click(page, 'button', 'Download');
  const d = await dl;
  expect(d.suggestedFilename()).toBe('leaf-drive-1.csv');
  const file = path.join(OUT, 'app-log-named.csv');
  await d.saveAs(file);
  expect(rowsIn(file)).toBe(sent);
  note(`${d.suggestedFilename()}: ${rowsIn(file)} rows = ${sent} frames sent; page errors: ${errors.length}`);
  await menu(page, 'Choose the Log');               // back off for the next run
  expect(errors).toEqual([]);
});
