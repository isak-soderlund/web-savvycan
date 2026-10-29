// Graphs (stage 2): a graph of a live bus keeps growing and follows the newest data (Follow End of Graph, on by
// default since 2026-10-29), as in SavvyCAN. The fake rc1 plays the Leaf bus; 0x1DA comes every 10 ms with byte 0
// counting up. Picture: results/graph-live.png.
import { join } from 'node:path';
import { test, expect, fake, RESULTS } from '../lib/harness.mjs';

test('a live graph grows with the bus and follows its end', async ({ app, page }) => {
  await app.open();
  await app.connect({ protocol: 'gvret', fd: false });
  await fake.setTraffic(page, 'leaf', { seconds: 8 });
  await page.waitForTimeout(1000);
  expect(await page.evaluate(() => window.webSavvyCAN.addGraph(0x1da, 0, 8, true)), 'graph created').toBe(true);
  const state = () => page.evaluate(() => window.webSavvyCAN.graphState());
  const first = await state();
  await page.waitForTimeout(3000);
  const later = await state();
  expect(later.points, `points grow while the bus runs (${first.points} -> ${later.points})`).toBeGreaterThan(first.points + 200);
  // x is time in the app's time style (microseconds by default): 3 s of bus later, the newest point is >= 2 s newer.
  expect(later.lastKey - first.lastKey, 'newer data (µs)').toBeGreaterThan(2e6);
  // Follow End of Graph: the visible range ends at the newest point and moves with it.
  expect(Math.abs(later.xUpper - later.lastKey), `x range ends at the newest point (${later.xUpper} vs ${later.lastKey})`).toBeLessThan(1000);
  expect(later.xUpper, 'the x range moved').toBeGreaterThan(first.xUpper);
  // Byte 0 counts 0..255: the value axis has grown to take it in.
  expect(later.yUpper, `value axis up to ${later.yUpper}`).toBeGreaterThanOrEqual(255);
  expect(later.yLower, `value axis from ${later.yLower}`).toBeLessThanOrEqual(0);
  await page.screenshot({ path: join(RESULTS, 'graph-live.png') });
});
