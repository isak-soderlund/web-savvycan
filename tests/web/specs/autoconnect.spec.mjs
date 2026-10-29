// The browser remembers the last connection and makes it again as the page opens (RESOLVE.md "remember the
// connection and connect by itself"): GVRET or slcan, bus speed, CAN FD on/off, data speed, when rc1 is plugged in
// and already allowed (getPorts()). Without the connection window; nothing without a granted rc1; a setting to stop it.
// A reload keeps the page's storage (the same Playwright context) but starts a fresh fake rc1 (back in slcan, its
// CAN channel closed), as a real rc1 is when the page lets go of it.
import { test, expect, fake, settle } from '../lib/harness.mjs';

/** The connection window is up: its buttons are in the accessibility tree. */
const connectionWindowShown = (app) => app.role('button', 'Add New Device Connection').count();

/** Leaf traffic for 2 s: every frame the fake sent is in the app. */
async function trafficArrives(page) {
  await fake.setTraffic(page, 'leaf', { seconds: 2 });
  await page.waitForTimeout(2300);
  const [s, a] = await settle(page);
  expect(s.encoded).toBeGreaterThan(1000);
  expect(a.framesReceived).toBe(s.encoded);
}

/** Nothing for a while (longer than GVRET's 3 s ESP32 wait): not connected, the port never opened. */
async function staysUnconnected(page) {
  await page.waitForTimeout(5000);
  expect((await fake.appStats(page)).connected).toBe(false);
  const ev = await fake.events(page);
  expect(ev.filter((e) => e.type === 'open' || e.type === 'requestPort')).toEqual([]);
  expect((await fake.stats(page)).sessions).toBe(0);
}

test.describe('auto-connect: rc1 allowed and plugged in', () => {
  test.use({ fakeOptions: { serial: { granted: true } } });

  const cases = [
    { protocol: 'gvret', bitrate: 1000000, fd: false, log: 'open 1000000', channel: { bitrate: 1000000, fdMode: false } },
    { protocol: 'gvret', bitrate: 250000, fd: true, dataBitrate: 4000000, log: 'open 250000/4000000',
      channel: { bitrate: 250000, fdMode: true, dataBitrate: 4000000 } },
    { protocol: 'slcan', bitrate: 500000, fd: true, dataBitrate: 4000000, log: 'open 500000/4000000',
      channel: { bitrate: 500000, fdMode: true, dataBitrate: 4000000 } },
  ];
  for (const c of cases) {
    const what = `${c.protocol} ${c.bitrate / 1000}k ${c.fd ? `FD ${c.dataBitrate / 1e6}M` : 'classic'}`;
    test(`${what}: after a reload connects by itself with the same bus, no connection window`, async ({ app, page }) => {
      await app.open();
      expect(await connectionWindowShown(app), 'the connection window is not open at first').toBe(0);
      await app.connect(c);
      await expect.poll(() => fake.log(page)).toContain(c.log);

      await app.reload();
      await expect.poll(async () => (await fake.stats(page)).channel.open, { timeout: 20000 }).toBe(true);
      await expect.poll(() => fake.log(page), { timeout: 10000 }).toContain(c.log);
      const s = await fake.stats(page);
      expect(s.mode).toBe(c.protocol);
      expect(s.channel).toMatchObject(Object.assign({ open: true, listenOnly: false }, c.channel));
      expect((await fake.appStats(page)).connected).toBe(true);
      expect((await fake.events(page)).some((e) => e.type === 'requestPort'), 'no chooser').toBe(false);
      expect(await connectionWindowShown(app), 'the connection window never opened').toBe(0);
      await trafficArrives(page);
    });
  }

  test('switched off in File > Preferences: after a reload not connected', async ({ app, page }) => {
    await app.open();
    await app.connect({ protocol: 'gvret', bitrate: 500000, fd: false });
    await app.menu('File', 'Preferences');
    const box = await app.need(app.role('checkbox', 'Connect to the last device when the page opens'),
      'checkbox "Connect to the last device when the page opens"');
    expect(await box.isChecked(), 'on by default').toBe(true);
    await app.setChecked(box, false);
    await expect.poll(() => box.isChecked()).toBe(false);
    await app.reload();
    await staysUnconnected(page);
    // Back on: the next visit connects again (the connection was still remembered).
    await app.menu('File', 'Preferences');
    const again = await app.need(app.role('checkbox', 'Connect to the last device when the page opens'), 'the checkbox');
    expect(await again.isChecked()).toBe(false);
    await app.setChecked(again, true);
    await expect.poll(() => again.isChecked()).toBe(true);
    await app.reload();
    await expect.poll(async () => (await fake.stats(page)).channel, { timeout: 20000 })
      .toMatchObject({ open: true, bitrate: 500000, fdMode: false });
  });
});

test.describe('auto-connect: rc1 not allowed on this visit', () => {
  // Granted through the chooser on the first visit only: the reloaded mock's getPorts() is empty, as after the user
  // took the permission back (or rc1 isn't plugged in).
  test('a remembered connection but no granted port: nothing happens, no error, no chooser', async ({ app, page }) => {
    await app.open();
    await app.connect({ protocol: 'gvret', bitrate: 1000000, fd: false });
    await app.reload();
    await staysUnconnected(page);
    expect(await connectionWindowShown(app)).toBe(0);
  });
});
