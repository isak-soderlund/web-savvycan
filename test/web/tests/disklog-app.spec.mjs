// The wasm app itself (dist/SavvyCAN.html, scripts/build-wasm.sh): SavvyCAN's own File menu starts and
// stops continuous logging through WebDiskLog -> SavvyDiskLog, into an origin-private folder that the
// test remembers in place of the one the user picks. Skipped when there is no dist/.
// The Qt UI is a canvas: menus are clicked by position (1280x800 window, SavvyCAN's default layout).
import { test, expect } from '@playwright/test';
import fs from 'node:fs';
import path from 'node:path';
import { REPO, HEADER, note, saveBytes } from './common.mjs';

const DIST = path.join(REPO, 'dist/SavvyCAN.html');
test.skip(!fs.existsSync(DIST), 'no wasm build in dist/ (scripts/build-wasm.sh)');
test.use({ viewport: { width: 1280, height: 800 } });

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function openApp(page) {
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  await page.goto('/dist/SavvyCAN.html');
  await page.waitForFunction(() => {
    const s = document.querySelector('#screen');
    return s && s.style.display === 'block' && s.querySelector('canvas, div');
  }, null, { timeout: 60000 });
  await sleep(3000);
  return errors;
}

// By position (the menus are drawn in the canvas): File, and its "Start/Cease Continuous Logging" item.
const POS = JSON.parse(process.env.WSC_MENU_POS || '{"file":[22,12],"start":[135,118]}');

test('app: File > Start Continuous Logging, "Name it by the start time" -> a log file with the header; Cease closes it', async ({ page }) => {
  const errors = await openApp(page);
  const sup = await page.evaluate(() => ({ api: typeof SavvyDiskLog, backend: SavvyDiskLog.support().backend }));
  expect(sup).toEqual({ api: 'object', backend: 'fsa' });
  await page.evaluate(async () => {
    window.events = [];
    const root = await navigator.storage.getDirectory();
    const dir = await root.getDirectoryHandle('applogs', { create: true });
    await SavvyDiskLog._rememberDirectory(dir);
  });
  await page.mouse.click(...POS.file);
  await sleep(500);
  await page.mouse.click(...POS.start);
  await sleep(1500);                             // the dialog asks for the remembered folder first
  await page.screenshot({ path: path.join(REPO, 'test/web/test-results/disklog/app-dialog.png') });
  await (process.env.WSC_CLICK_DIRECT ? page.mouse.click(...JSON.parse(process.env.WSC_CLICK_DIRECT)) : page.keyboard.press('Enter')); // Enter: the default button, "Name it by the start time" (a user gesture too)
  await page.waitForFunction(() => SavvyDiskLog.status().state === 'open', null, { timeout: 10000 });
  const st = await page.evaluate(() => SavvyDiskLog.status());
  await sleep(500);
  await page.mouse.click(...POS.file);
  await sleep(500);
  await page.screenshot({ path: path.join(REPO, 'test/web/test-results/disklog/app-menu-logging.png') });
  await page.mouse.click(...POS.start);          // now "Cease Continuous Logging"
  await page.waitForFunction(() => SavvyDiskLog.status().state === 'idle', null, { timeout: 10000 });
  const text = await page.evaluate(async (name) => {
    const root = await navigator.storage.getDirectory();
    const dir = await root.getDirectoryHandle('applogs');
    return (await (await dir.getFileHandle(name)).getFile()).text();
  }, st.name);
  expect(st.name).toMatch(/^SavvyCAN_\d{4}-\d\d-\d\d_\d\d-\d\d-\d\d\.csv$/);
  expect(text).toBe(HEADER);
  saveBytes('app-empty-log.csv', text);
  note(`${st.name}: ${text.length} B (the header; no device connected); page errors: ${errors.length}`);
  expect(errors).toEqual([]);
});
