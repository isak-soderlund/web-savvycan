// Smoke test of the wasm build (scripts/smoke-wasm.sh, CI): Qt starts, the main window is drawn, a modal dialog run
// with QDialog::exec() (Asyncify) opens and closes (its Close button), nothing fails to load, no console error. No fake rc1 here: the
// page as a visitor gets it. Pictures: results/smoke.png, results/smoke-dialog.png.
// Also: a log file from the visitor's computer (File > Load Log File, the browser's file picker) loads and graphs
// (results/smoke-graph.png).
import { join } from 'node:path';
import { mkdirSync, writeFileSync } from 'node:fs';
import { test, expect, RESULTS, AppDriver } from '../lib/harness.mjs';

// Distinct colours in the first screenshot, and pixels that differ from it in each of the others (decoded in the page).
const measure = (page, pngs) => page.evaluate(async (b64s) => {
  const decode = async (b64) => {
    const img = await createImageBitmap(await (await fetch('data:image/png;base64,' + b64)).blob());
    const g = new OffscreenCanvas(img.width, img.height).getContext('2d');
    g.drawImage(img, 0, 0);
    return g.getImageData(0, 0, img.width, img.height).data;
  };
  const [first, ...rest] = await Promise.all(b64s.map(decode));
  const colours = new Set();
  for (let i = 0; i < first.length; i += 16) colours.add((first[i] << 16) | (first[i + 1] << 8) | first[i + 2]);
  const changed = rest.map((d) => {
    let n = 0;
    for (let i = 0; i < d.length; i += 4) if (d[i] !== first[i] || d[i + 1] !== first[i + 1] || d[i + 2] !== first[i + 2]) n++;
    return n;
  });
  return { colours: colours.size, changed };
}, pngs.map((p) => p.toString('base64')));

test('the app starts, draws its main window, and a modal exec() dialog opens and closes', async ({ page, consoleErrors }) => {
  page.on('requestfailed', (r) => consoleErrors.push(`request failed: ${r.url()} ${r.failure()?.errorText}`));
  page.on('response', (r) => r.status() >= 400 && !r.url().endsWith('/favicon.ico') && consoleErrors.push(`HTTP ${r.status()}: ${r.url()}`));
  const t0 = Date.now();
  await page.goto('/SavvyCAN.html');
  await page.waitForFunction(() => window.webSavvyCAN, null, { timeout: 60000 });
  const loaded = (Date.now() - t0) / 1000;
  const app = new AppDriver(page);
  await app.enableA11y(); // menus by name: their places change as stages add menus
  await page.waitForTimeout(3000);
  expect(await page.textContent('#qtstatus').catch(() => '')).not.toMatch(/exit/i);
  const main = await page.screenshot({ path: join(RESULTS, 'smoke.png') });
  expect((await measure(page, [main])).colours, 'main window drawn (distinct colours on screen)').toBeGreaterThanOrEqual(20);
  // Help > About web-savvycan: a dialog run with exec(), closed with its Close button.
  await app.menu('Help', 'About');
  await page.waitForTimeout(1500);
  const dialog = await page.screenshot({ path: join(RESULTS, 'smoke-dialog.png') });
  await app.click('button', /^Close$/i, 'button "Close"'); // the accessibility layer has the keyboard focus
  await page.waitForTimeout(1500);
  const { changed: [shown, left] } = await measure(page, [main, dialog, await page.screenshot()]);
  expect(shown, 'the About dialog appeared (pixels changed)').toBeGreaterThan(5000);
  expect(left, 'the About dialog closed (pixels still different)').toBeLessThan(2000);
  test.info().annotations.push({ type: 'result', description: `Qt loaded after ${loaded.toFixed(1)} s` });
  expect(consoleErrors, 'console errors, failed requests').toEqual([]);
});

test('a log file from the computer loads (File > Load Log File) and graphs', async ({ page, consoleErrors }) => {
  // A GVRET CSV as SavvyCAN saves it: 2000 frames of 0x1DA every 10 ms, byte 0 a sawtooth 0..199.
  const frames = 2000;
  const rows = ['Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8'];
  for (let i = 0; i < frames; i++) rows.push(`${i * 10000},000001DA,false,Rx,0,8,${(i % 200).toString(16).toUpperCase().padStart(2, '0')},00,00,00,00,00,00,00`);
  mkdirSync(RESULTS, { recursive: true });
  const file = join(RESULTS, 'smoke-load.csv');
  writeFileSync(file, rows.join('\n') + '\n');

  await page.goto('/SavvyCAN.html');
  const app = new AppDriver(page);
  await app.enableA11y();
  await app.loadFile(file, frames);
  const stats = await page.evaluate(() => window.webSavvyCAN.stats());
  expect(stats.framesReceived, 'every frame of the file in the trace').toBe(frames);
  expect((await page.evaluate(() => window.webSavvyCAN.traceTail(1)))[0].data[0], 'the last frame as in the file').toBe((frames - 1) % 200);

  // RE Tools > New Graphing Window with byte 0 of 0x1DA (Intel, 8 bits), as the Add New Graph dialog makes it.
  expect(await page.evaluate(() => window.webSavvyCAN.addGraph(0x1da, 0, 8, true)), 'graph created').toBe(true);
  await expect.poll(() => page.evaluate(() => window.webSavvyCAN.graphState().points), { message: 'every frame plotted' }).toBe(frames);
  const g = await page.evaluate(() => window.webSavvyCAN.graphState());
  // x is time in the app's time style (File > Preferences; microseconds unless set to seconds or clock).
  expect(g.lastKey, 'the last point at the file\'s last time stamp (µs)').toBe(19990000);
  await page.waitForTimeout(1000);
  await page.screenshot({ path: join(RESULTS, 'smoke-graph.png') });
  expect(consoleErrors, 'console errors').toEqual([]);
});
