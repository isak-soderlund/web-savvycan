// The theme switch (RESOLVE.md MVP #8): light <-> dark, the screen visibly darker, remembered after a reload.
import { test, expect, fake } from '../lib/harness.mjs';

/** Mean luminance (0-255) of a PNG screenshot, decoded in the page. */
async function luminance(page, png) {
  return page.evaluate(async (b64) => {
    const img = await createImageBitmap(await (await fetch('data:image/png;base64,' + b64)).blob());
    const c = new OffscreenCanvas(img.width, img.height);
    const g = c.getContext('2d');
    g.drawImage(img, 0, 0);
    const d = g.getImageData(0, 0, img.width, img.height).data;
    let s = 0;
    for (let i = 0; i < d.length; i += 4) s += 0.2126 * d[i] + 0.7152 * d[i + 1] + 0.0722 * d[i + 2];
    return s / (d.length / 4);
  }, png.toString('base64'));
}

test('switch to dark and back; remembered between visits', async ({ app, page }) => {
  await app.open();
  await fake.appStats(page); // skips while the app has no test hook
  const theme = () => page.evaluate(() => window.webSavvyCAN.theme());
  const start = await theme();
  expect(start).toBe('light'); // SavvyCAN's normal look on a first visit
  const light = await luminance(page, await app.themeShot());
  await app.toggleTheme();
  await expect.poll(theme).toBe('dark');
  const dark = await luminance(page, await app.themeShot());
  expect(dark, `dark (${dark.toFixed(0)}) darker than light (${light.toFixed(0)})`).toBeLessThan(light - 40);
  await app.reload();
  await expect.poll(theme).toBe('dark');
  await app.toggleTheme();
  await expect.poll(theme).toBe('light');
});

test.describe('the system is dark', () => {
  test.use({ colorScheme: 'dark' });
  test('first visit follows the system (dark); Light Theme overrides it; Follow System Theme goes back', async ({ app, page, target }) => {
    test.skip(target !== 'app', 'the reference client has no "follow the system" setting');
    await app.open();
    const theme = () => page.evaluate(() => window.webSavvyCAN.theme());
    await expect.poll(theme).toBe('dark');
    await app.menu('View', 'Light Theme');
    await expect.poll(theme).toBe('light');
    await app.menu('View', 'Follow System Theme');
    await expect.poll(theme).toBe('dark');
  });
});
