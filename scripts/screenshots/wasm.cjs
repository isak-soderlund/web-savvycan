// Screenshots of the wasm build in headless Chromium (Playwright), light and dark, and a check that the
// theme choice persists in the browser (localStorage) across a reload.
//
//   node scripts/screenshots/wasm.cjs URL OUT_DIR [WAIT_MS]
//
// 1. A fresh profile with the browser in light, then dark (prefers-color-scheme): the app follows it.
// 2. In the light profile: View > Dark Theme picked, page reloaded: still dark (window.webSavvyCAN.theme()),
//    and the setting is in localStorage. About, Preferences and View menu pictures in both themes.
// Playwright from NODE_PATH (scripts/smoke-wasm.sh sets it up the same way).
const { chromium } = require('playwright');

const [url, outDir, waitArg] = process.argv.slice(2);
const WAIT = Number(waitArg || 9000);
if (!url || !outDir) { console.error('usage: wasm.cjs URL OUT_DIR [WAIT_MS]'); process.exit(2); }

// Menu positions in the 1400x860 page (Qt draws the menu bar into its canvas; no DOM to query).
const MENU = { file: [22, 11], view: [69, 11], connection: [148, 11], help: [226, 11] };

async function open(browser, scheme) {
  const ctx = await browser.newContext({ colorScheme: scheme, viewport: { width: 1400, height: 860 } });
  const page = await ctx.newPage();
  const errors = [];
  page.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });
  page.on('pageerror', e => errors.push(String(e)));
  await page.goto(url);
  await page.waitForTimeout(WAIT);
  return { ctx, page, errors };
}

// Opens a menu and picks an item with the keyboard (Down n times, or Up n times when n < 0).
async function menuItem(page, menu, n) {
  await page.mouse.click(...MENU[menu]);
  await page.waitForTimeout(600);
  for (let i = 0; i < Math.abs(n); i++) await page.keyboard.press(n > 0 ? 'ArrowDown' : 'ArrowUp');
  await page.keyboard.press('Enter');
  await page.waitForTimeout(1500);
}

async function hook(page) {
  return page.evaluate(() => window.webSavvyCAN
    ? { version: window.webSavvyCAN.version, theme: window.webSavvyCAN.theme(),
        stats: window.webSavvyCAN.stats(), tail: window.webSavvyCAN.traceTail(3) }
    : null);
}

(async () => {
  const browser = await chromium.launch();
  let failed = false;
  for (const scheme of ['light', 'dark']) {
    const { ctx, page, errors } = await open(browser, scheme);
    await page.screenshot({ path: `${outDir}/wasm-${scheme}-follow-browser.png` });
    console.log(`${scheme}: follows the browser -> wasm-${scheme}-follow-browser.png; console errors: ${errors.length}`);
    const h = await hook(page);
    console.log(`${scheme}: window.webSavvyCAN =`, JSON.stringify(h));
    if (!h || h.theme !== scheme) { console.log(`FAIL: hook theme ${h && h.theme}, browser ${scheme}`); failed = true; }
    // Help > About, File > Preferences (Exit is hidden in the browser: Preferences is the last item)
    await menuItem(page, 'help', 1);
    await page.screenshot({ path: `${outDir}/wasm-${scheme}-about.png` });
    await page.keyboard.press('Escape'); await page.waitForTimeout(500);
    await menuItem(page, 'file', -1);
    await page.screenshot({ path: `${outDir}/wasm-${scheme}-preferences.png` });
    await page.keyboard.press('Escape'); await page.waitForTimeout(500);
    await page.mouse.click(...MENU.view); await page.waitForTimeout(600);
    await page.screenshot({ path: `${outDir}/wasm-${scheme}-view-menu.png` });
    await page.keyboard.press('Escape'); await page.waitForTimeout(500);
    if (scheme === 'light') {
      // View > Dark Theme (second item), then reload: still dark, stored in localStorage
      await page.reload(); await page.waitForTimeout(WAIT);
      await menuItem(page, 'view', 2);
      await page.screenshot({ path: `${outDir}/wasm-switched-dark.png` });
      const stored = await page.evaluate(() => Object.keys(localStorage)
        .filter(k => /Theme/.test(k)).map(k => `${k}=${localStorage.getItem(k)}`));
      console.log('localStorage after the switch:', stored.join(' ') || '(none)');
      await page.reload();
      await page.waitForTimeout(WAIT);
      await page.screenshot({ path: `${outDir}/wasm-dark-after-reload.png` });
      const after = await hook(page);
      console.log('after reload, browser light: theme', after && after.theme);
      if (!stored.some(x => /=dark$/.test(x)) || !after || after.theme !== 'dark') {
        console.log('FAIL: dark not kept across the reload'); failed = true;
      }
    }
    await ctx.close();
  }
  await browser.close();
  process.exit(failed ? 1 : 0);
})();
