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
  test.use({ fakeOptions: { serial: { disconnectAfterMs: 1500 } } });
  test('rc1 unplugged: the app notices, no crash; plugged back in: reconnects', async ({ app, page, consoleErrors }) => {
    await connected(app, page);
    await fake.setTraffic(page, 'leaf');
    await expect.poll(async () => (await fake.appStats(page)).connected, { timeout: 5000 }).toBe(false);
    expect((await fake.events(page)).some((e) => e.type === 'unplug')).toBe(true);
    const before = (await fake.appStats(page)).framesReceived;
    expect(before).toBeGreaterThan(0);
    await page.evaluate(() => window.__rc1.replug());
    await page.evaluate(() => (window.__rc1.options.disconnectAfterMs = 0));
    await app.connect({ protocol: 'gvret' });
    await fake.setTraffic(page, 'leaf', { seconds: 1 });
    await page.waitForTimeout(1300);
    const [s, a] = await settle(page);
    expect(a.framesReceived - before).toBe(s.encoded);
    // The read error on unplug is expected; it must be handled, not thrown.
    consoleErrors.splice(0, consoleErrors.length, ...consoleErrors.filter((e) => !/device has been lost/.test(e)));
  });
});
