// Smoke test of the wasm build: open dist/SavvyCAN.html in headless Chromium, wait for Qt to
// start, check that the main window is drawn and nothing went to the console as an error.
// Run through scripts/smoke-wasm.sh (it serves dist/). Exit 1 on a failure.
// Also checks a modal QDialog::exec() (Asyncify/JSPI): File > Crop Log with no frames opens a
// QMessageBox; it must appear, and close again with Enter.
// Usage: node scripts/smoke-wasm.cjs URL SCREENSHOT [WAIT_MS]
const { chromium } = require('playwright');

(async () => {
  const [url, shot, waitArg] = process.argv.slice(2);
  const waitMs = Number(waitArg || 8000);
  const problems = [];
  const browser = await chromium.launch({ headless: true });
  const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
  page.on('console', m => {
    if (m.type() === 'error') problems.push(`console.error: ${m.text()}`);
  });
  page.on('pageerror', e => problems.push(`page error: ${e.message}`));
  page.on('requestfailed', r => problems.push(`request failed: ${r.url()} ${r.failure()?.errorText}`));
  page.on('response', r => {
    if (r.status() >= 400 && !r.url().endsWith('/favicon.ico')) problems.push(`HTTP ${r.status()}: ${r.url()}`);
  });

  const t0 = Date.now();
  await page.goto(url, { waitUntil: 'load' });
  let loaded = true;
  try {
    // qtloader's onLoaded shows #screen; Qt then adds its canvas/window elements into it.
    await page.waitForFunction(() => {
      const s = document.querySelector('#screen');
      return s && s.style.display === 'block' && s.querySelector('canvas, div');
    }, null, { timeout: 60000 });
  } catch (e) {
    loaded = false;
    problems.push('Qt did not start within 60 s: ' + (await page.textContent('#qtstatus').catch(() => '?')));
  }
  const tLoaded = Date.now() - t0;
  await page.waitForTimeout(waitMs);
  const status = (await page.textContent('#qtstatus').catch(() => '')) || '';
  if (/exit/i.test(status)) problems.push(`application exited: ${status}`);

  const png = await page.screenshot({ path: shot });
  // Is something drawn? Count the distinct colours of the screenshot (a blank page has ~1).
  const colours = await page.evaluate(async b64 => {
    const img = new Image();
    img.src = 'data:image/png;base64,' + b64;
    await img.decode();
    const c = document.createElement('canvas');
    c.width = img.width; c.height = img.height;
    const ctx = c.getContext('2d');
    ctx.drawImage(img, 0, 0);
    const d = ctx.getImageData(0, 0, c.width, c.height).data;
    const set = new Set();
    for (let i = 0; i < d.length; i += 16) set.add((d[i] << 16) | (d[i + 1] << 8) | d[i + 2]);
    return set.size;
  }, png.toString('base64'));
  if (loaded && colours < 20) problems.push(`main window not drawn (${colours} colours on screen)`);

  // Modal dialog through exec(): File menu (top left), "Crop Log to Frame Range" (14th row).
  if (loaded && colours >= 20) {
    const before = await page.screenshot();
    await page.mouse.click(23, 11);
    await page.waitForTimeout(800);
    await page.mouse.click(135, 313);
    await page.waitForTimeout(1500);
    const dialog = await page.screenshot({ path: shot.replace(/\.png$/, '-dialog.png') });
    await page.keyboard.press('Enter');
    await page.waitForTimeout(1500);
    const after = await page.screenshot();
    const diff = await page.evaluate(async ([a, b, c]) => {
      const load = async b64 => {
        const img = new Image(); img.src = 'data:image/png;base64,' + b64; await img.decode();
        const cv = document.createElement('canvas'); cv.width = img.width; cv.height = img.height;
        const ctx = cv.getContext('2d'); ctx.drawImage(img, 0, 0);
        return ctx.getImageData(0, 0, cv.width, cv.height).data;
      };
      const [da, db, dc] = await Promise.all([load(a), load(b), load(c)]);
      const count = (x, y) => { let n = 0; for (let i = 0; i < x.length; i += 4) if (x[i] !== y[i] || x[i+1] !== y[i+1] || x[i+2] !== y[i+2]) n++; return n; };
      return { shown: count(da, db), left: count(da, dc) };
    }, [before.toString('base64'), dialog.toString('base64'), after.toString('base64')]);
    if (diff.shown < 5000) problems.push(`exec(): the Crop Log message box did not appear (${diff.shown} pixels changed)`);
    else if (diff.left > 2000) problems.push(`exec(): the message box did not close with Enter (${diff.left} pixels still differ)`);
    else console.log(`exec(): message box shown (${diff.shown} px) and closed (${diff.left} px left)`);
  }

  const mem = await page.evaluate(() => (performance.memory ? performance.memory.usedJSHeapSize : 0));
  console.log(`Qt loaded after ${(tLoaded / 1000).toFixed(1)} s; ${colours} colours on screen; JS heap ${(mem / 1e6).toFixed(0)} MB`);
  console.log(`screenshot: ${shot}`);
  await browser.close();
  if (problems.length) {
    console.log('FAIL');
    for (const p of problems) console.log('  ' + p);
    process.exit(1);
  }
  console.log('OK: main window drawn, modal exec() dialog works, no console errors');
})().catch(e => { console.error(e); process.exit(1); });
