// Smoke test of the wasm build (scripts/smoke-wasm.sh, CI): Qt starts, the main window is drawn, a modal dialog run
// with QDialog::exec() (Asyncify) opens and closes, nothing fails to load, no console error. No fake rc1 here: the
// page as a visitor gets it. Pictures: results/smoke.png, results/smoke-dialog.png.
import { join } from 'node:path';
import { test, expect, RESULTS } from '../lib/harness.mjs';

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
  await page.waitForTimeout(3000);
  expect(await page.textContent('#qtstatus').catch(() => '')).not.toMatch(/exit/i);
  const main = await page.screenshot({ path: join(RESULTS, 'smoke.png') });
  expect((await measure(page, [main])).colours, 'main window drawn (distinct colours on screen)').toBeGreaterThanOrEqual(20);
  // Help (4th in the menu bar) > About web-savvycan: a dialog run with exec(); Enter closes it.
  await page.mouse.click(226, 11);
  await page.waitForTimeout(800);
  await page.keyboard.press('ArrowDown');
  await page.keyboard.press('Enter');
  await page.waitForTimeout(1500);
  const dialog = await page.screenshot({ path: join(RESULTS, 'smoke-dialog.png') });
  await page.keyboard.press('Enter');
  await page.waitForTimeout(1500);
  const { changed: [shown, left] } = await measure(page, [main, dialog, await page.screenshot()]);
  expect(shown, 'the About dialog appeared (pixels changed)').toBeGreaterThan(5000);
  expect(left, 'the About dialog closed with Enter (pixels still different)').toBeLessThan(2000);
  test.info().annotations.push({ type: 'result', description: `Qt loaded after ${loaded.toFixed(1)} s` });
  expect(consoleErrors, 'console errors, failed requests').toEqual([]);
});
