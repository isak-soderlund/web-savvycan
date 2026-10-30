// Traffic: the Leaf-like bus, a garbled stream, the dropped-frames counter under overload, unplugging.
import { test, expect, fake, settle } from '../lib/harness.mjs';

async function connected(app, page, opts = {}) {
  await app.open();
  await app.connect(Object.assign({ protocol: 'gvret' }, opts));
  await expect.poll(async () => (await fake.stats(page)).channel.open).toBe(true);
}

test('the Leaf bus for 5 s: every frame counted', async ({ app, page }) => {
  await connected(app, page);
  await fake.setTraffic(page, 'leaf', { seconds: 5 });
  await page.waitForTimeout(5300);
  const [s, a] = await settle(page);
  expect(s.encoded).toBeGreaterThan(4100);
  expect(s.lost).toBe(0);
  expect(a.framesReceived).toBe(s.encoded);
  expect(a.framesDropped).toBe(0);
  expect(a.framesInTrace).toBe(s.encoded);
});

test.describe('a garbled stream', () => {
  test.use({ fakeOptions: { serial: { device: {} } } });
  test('junk bytes between GVRET frames: the parser resynchronises, no frame invented or lost', async ({ app, page }) => {
    await connected(app, page);
    await fake.setTraffic(page, 'leaf', { seconds: 3, garbageEvery: 7 });
    await page.waitForTimeout(3300);
    const [s, a] = await settle(page);
    expect(s.garbageBytes).toBeGreaterThan(0);
    expect(a.framesReceived).toBe(s.encoded);
  });
});

test.describe('slcan error lines', () => {
  test('bus errors reported by rc1 (s/e lines) do not count as frames', async ({ app, page }) => {
    await connected(app, page, { protocol: 'slcan' });
    await fake.setTraffic(page, 'errors', { seconds: 2 });
    await page.waitForTimeout(2300);
    const [s, a] = await settle(page);
    expect(s.errorFrames).toBeGreaterThan(10);
    expect(a.framesReceived).toBe(s.encoded);
  });
});

test('overload: rc1 at 5x its real rate (40 000/s); the dropped-frames counter accounts for every frame', async ({ app, page }) => {
  // Whatever the app can take: received = in trace + scrolled out + dropped (nothing lost silently).
  await connected(app, page);
  await fake.setTraffic(page, 'classic-full', { seconds: 5, rate: 40000 });
  await page.waitForTimeout(5500);
  const [s, a] = await settle(page, 15000);
  test.info().annotations.push({ type: 'numbers', description: JSON.stringify({ fake: s, app: a }) });
  expect(s.encoded + s.lost).toBe(s.bus - s.busWhileClosed);
  expect(a.framesReceived, 'every frame rc1 sent was parsed').toBe(s.encoded);
  expect(a.framesInTrace + a.framesEvicted + a.framesDropped, 'received = in trace + scrolled out + dropped').toBe(a.framesReceived);
});

// Unplugging: as users run it, with no accessibility layer (with it, Qt 6.12 throws "setAttribute of undefined" on unplug
// and stops drawing; README), plain mouse clicks on Qt's canvas at the positions of the 1280x800 viewport (the connection window is a tab since
// 2026-10-30). GVRET is the New Connection dialog's default and the
// granted port is preselected, so no combo box is needed.
test.describe('unplugged mid-stream, no accessibility layer', () => {
  test.use({ fakeOptions: { serial: { granted: true } } });
  test('rc1 unplugged while frames arrive: no page error; plugged back in: reconnects, frames again', async ({ page, app }) => {
    await page.goto('/SavvyCAN.html');
    await page.waitForFunction(() => window.webSavvyCAN, null, { timeout: 60000 });
    await page.waitForTimeout(1500);
    const click = async (x, y) => {
      await page.mouse.click(x, y);
      await page.waitForTimeout(700);
    };
    const addConnection = async (addY) => {
      await click(427, addY);                                // Add New Device Connection (moves up once a row is listed)
      await click(630, 605);                                 // New Connection: Create New Connection
      await page.waitForFunction(() => window.webSavvyCAN.stats().connected, null, { timeout: 20000 });
      await page.waitForTimeout(5000);                       // Bus Details filled in from the device (ESP32 start)
      await click(180, 617);                                 // Enable Bus
      await click(503, 704);                                 // Save Bus Settings
      await page.waitForFunction(() => window.__rc1.stats().channel.open, null, { timeout: 10000 });
    };
    await click(205, 19);                                    // Connection (the menu row with the tabs)
    await click(282, 47);                                    // Open Connection Window (a tab filling the page)
    await addConnection(445);
    await fake.setTraffic(page, 'leaf');
    await page.waitForTimeout(1500);
    await page.evaluate(() => window.__rc1.unplug());
    await expect.poll(async () => (await fake.appStats(page)).connected, { timeout: 5000 }).toBe(false);
    const before = (await fake.appStats(page)).framesReceived;
    expect(before).toBeGreaterThan(0);
    await page.evaluate(() => window.__rc1.replug());
    await page.waitForTimeout(1000);
    await addConnection(414);
    await fake.setTraffic(page, 'leaf', { seconds: 1 });
    await page.waitForTimeout(1300);
    const [s, a] = await settle(page);
    expect(a.framesReceived - before).toBe(s.encoded);
  });
});
