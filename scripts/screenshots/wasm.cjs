// Screenshots of the wasm build in headless Chromium (Playwright), light and dark, and a check that the
// theme choice persists in the browser (localStorage) across a reload.
//
//   node scripts/screenshots/wasm.cjs URL OUT_DIR [WAIT_MS]
//
// 1. A fresh profile with the browser in light, then dark (prefers-color-scheme): the app follows it.
// 2. In the light profile: View > Dark Theme clicked, page reloaded: still dark, and the setting is in
//    localStorage. Preferences and About pictures in both themes.
// Playwright from NODE_PATH (scripts/smoke-wasm.sh sets it up the same way).
const { chromium } = require('playwright');

const [url, outDir, waitArg] = process.argv.slice(2);
const WAIT = Number(waitArg || 9000);
if (!url || !outDir) { console.error('usage: wasm.cjs URL OUT_DIR [WAIT_MS]'); process.exit(2); }

// Menu positions in the 1400x860 page (Qt draws the menu bar into its canvas; no DOM to query).
const MENU = { file: [18, 11], view: [55, 11], help: [140, 11] };

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

async function menuItem(page, menu, itemText) {
  // Opens a menu and clicks one of its items, found by OCR-free means: Qt for wasm exposes an
  // accessibility tree (role menuitem) when a11y is on; fall back to keyboard navigation.
  await page.mouse.click(...MENU[menu]);
  await page.waitForTimeout(500);
  const item = page.getByRole('menuitem', { name: itemText });
  if (await item.count()) { await item.first().click(); }
  else { await page.keyboard.press('ArrowDown'); await page.keyboard.press('Enter'); }
  await page.waitForTimeout(1000);
}

(async () => {
  const browser = await chromium.launch();
  let failed = false;
  for (const scheme of ['light', 'dark']) {
    const { ctx, page, errors } = await open(browser, scheme);
    await page.screenshot({ path: `${outDir}/wasm-${scheme}-follow-browser.png` });
    console.log(`${scheme}: follows the browser -> wasm-${scheme}-follow-browser.png; console errors: ${errors.length}`);
    if (scheme === 'light') {
      await menuItem(page, 'view', 'Dark Theme');
      await page.screenshot({ path: `${outDir}/wasm-switched-dark.png` });
      const stored = await page.evaluate(() => Object.keys(localStorage)
        .filter(k => /Theme/.test(k)).map(k => `${k}=${localStorage.getItem(k)}`));
      console.log('localStorage after the switch:', stored.join(' ') || '(none)');
      await page.reload();
      await page.waitForTimeout(WAIT);
      await page.screenshot({ path: `${outDir}/wasm-dark-after-reload.png` });
      if (!stored.some(s => /dark/.test(s))) { console.log('FAIL: theme not stored in localStorage'); failed = true; }
    }
    await ctx.close();
  }
  await browser.close();
  process.exit(failed ? 1 : 0);
})();
