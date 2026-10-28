// Traffic: the Leaf-like bus, a garbled stream, the dropped-frames counter under overload, unplugging.
import { test, expect, fake, settle } from '../lib/harness.mjs';

async function connected(app, page, opts = {}) {
  await app.open(opts.query || '');
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

test.describe('overload', () => {
  // Selftest: a UI that takes 200 ms per batch and a 200-frame ingest queue, at rc1's full classic rate: frames
  // must be dropped visibly and every one accounted for. App: rc1 at 5x its real rate (40 000/s), whatever the app
  // can take: received = in trace + dropped (nothing lost silently), and the fake's own losses (rc1's buffers) only
  // when the app stopped reading.
  test('the dropped-frames counter accounts for every frame', async ({ app, page, target }) => {
    const sel = target === 'selftest';
    await connected(app, page, { query: sel ? 'stall=200&queue=200&ring=1000000' : '' });
    await fake.setTraffic(page, 'classic-full', { seconds: 5, rate: sel ? 8000 : 40000 });
    await page.waitForTimeout(5500);
    const [s, a] = await settle(page, 15000);
    test.info().annotations.push({ type: 'numbers', description: JSON.stringify({ fake: s, app: a }) });
    expect(s.encoded + s.lost).toBe(s.bus - s.busWhileClosed);
    expect(a.framesReceived, 'every frame rc1 sent was parsed').toBe(s.encoded);
    expect(a.framesInTrace + a.framesEvicted + a.framesDropped, 'received = in trace + scrolled out + dropped').toBe(a.framesReceived);
    if (sel) expect(a.framesDropped, 'the slow UI dropped frames, visibly').toBeGreaterThan(0);
  });
});

test.describe('unplugged mid-stream', () => {
  test('rc1 unplugged: the app notices, no crash; plugged back in: reconnects', async ({ app, page, consoleErrors, target }) => {
    // Not for the app through this driver: Qt 6.12 wasm with its accessibility layer on (the driver needs it) throws
    // "Cannot read properties of undefined (reading 'setAttribute')" from Qt's accessibility code when rc1 is
    // unplugged, and the window stops drawing (wasm/webserial/tests/e2e.mjs, with fewer steps, doesn't hit it).
    // Users without a screen reader don't run that code: the next test does this without the layer.
    test.skip(target === 'app', 'Qt 6.12 accessibility layer: setAttribute of undefined on unplug (see the next test)');
    await connected(app, page);
    await fake.setTraffic(page, 'leaf');
    await page.waitForTimeout(1500);
    await page.evaluate(() => window.__rc1.unplug());
    await expect.poll(async () => (await fake.appStats(page)).connected, { timeout: 5000 }).toBe(false);
    expect((await fake.events(page)).some((e) => e.type === 'unplug')).toBe(true);
    await page.waitForTimeout(500);
    const before = (await fake.appStats(page)).framesReceived;
    expect(before).toBeGreaterThan(0);
    await fake.setTraffic(page, 'idle');
    await page.evaluate(() => window.__rc1.replug());
    await app.connect({ protocol: 'gvret' });
    const encoded0 = (await fake.stats(page)).encoded;
    await fake.setTraffic(page, 'leaf', { seconds: 1 });
    await page.waitForTimeout(1300);
    const [s, a] = await settle(page);
    expect(a.connected).toBe(true);
    expect(a.framesReceived - before).toBe(s.encoded - encoded0);
    // The read error on unplug is expected; it must be handled, not thrown.
    consoleErrors.splice(0, consoleErrors.length, ...consoleErrors.filter((e) => !/device has been lost/.test(e)));
  });
});

// The same as users run it: no accessibility layer (no screen reader), plain mouse clicks on Qt's canvas at the
// positions of the 1280x800 viewport (playwright.config.mjs). GVRET is the New Connection dialog's default and the
// granted port is preselected, so no combo box is needed.
test.describe('unplugged mid-stream, no accessibility layer', () => {
  test.use({ fakeOptions: { serial: { granted: true } } });
  test('rc1 unplugged while frames arrive: no page error; plugged back in: reconnects, frames again', async ({ page, target, app }) => {
    test.skip(target !== 'app', 'clicks at the app\'s own canvas positions');
    await page.goto('/SavvyCAN.html');
    await page.waitForFunction(() => window.webSavvyCAN, null, { timeout: 60000 });
    await page.waitForTimeout(1500);
    const click = async (x, y) => {
      await page.mouse.click(x, y);
      await page.waitForTimeout(700);
    };
    const addConnection = async (addY) => {
      await click(420, addY);                                // Add New Device Connection (moves up once a row is listed)
      await click(630, 605);                                 // New Connection: Create New Connection
      await page.waitForFunction(() => window.webSavvyCAN.stats().connected, null, { timeout: 20000 });
      await page.waitForTimeout(5000);                       // Bus Details filled in from the device (ESP32 start)
      await click(282, 604);                                 // Enable Bus
      await click(496, 691);                                 // Save Bus Settings
      await page.waitForFunction(() => window.__rc1.stats().channel.open, null, { timeout: 10000 });
    };
    await click(148, 11);                                    // Connection
    await click(230, 37);                                    // Open Connection Window
    await addConnection(432);
    await fake.setTraffic(page, 'leaf');
    await page.waitForTimeout(1500);
    await page.evaluate(() => window.__rc1.unplug());
    await expect.poll(async () => (await fake.appStats(page)).connected, { timeout: 5000 }).toBe(false);
    const before = (await fake.appStats(page)).framesReceived;
    expect(before).toBeGreaterThan(0);
    await page.evaluate(() => window.__rc1.replug());
    await page.waitForTimeout(1000);
    await addConnection(401);
    await fake.setTraffic(page, 'leaf', { seconds: 1 });
    await page.waitForTimeout(1300);
    const [s, a] = await settle(page);
    expect(a.framesReceived - before).toBe(s.encoded);
  });
});
