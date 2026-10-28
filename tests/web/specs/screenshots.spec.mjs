// Pictures of the app for a look by eye (opt-in: run.sh --screenshots): the main window with Leaf traffic, the View
// menu, About and Preferences, in light and dark. Into results/screenshots/<theme>-<what>.png.
import { join } from 'node:path';
import { test, fake, RESULTS } from '../lib/harness.mjs';

for (const theme of ['light', 'dark']) {
  test.describe(theme, () => {
    test.use({ colorScheme: theme, viewport: { width: 1400, height: 860 } });
    test(`${theme}: main window, View menu, About, Preferences @screenshots`, async ({ app, page }) => {
      const shot = (what) => page.screenshot({ path: join(RESULTS, 'screenshots', `${theme}-${what}.png`) });
      await app.open();
      await app.connect({ protocol: 'gvret', fd: true });
      await fake.setTraffic(page, 'fd-mix', { seconds: 3 });
      await page.waitForTimeout(3500);
      await shot('trace');
      await app.click('menuitem', /^View$/i, 'menu "View"');
      await page.waitForTimeout(500);
      await shot('view-menu');
      await page.keyboard.press('Escape');
      await app.menu('Help', 'About');
      await page.waitForTimeout(1000);
      await shot('about');
      await page.keyboard.press('Escape');
      await page.waitForTimeout(500);
      await app.menu('File', 'Preferences');
      await page.waitForTimeout(1000);
      await shot('preferences');
    });
  });
}
