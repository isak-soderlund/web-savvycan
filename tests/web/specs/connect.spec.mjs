// Connecting to rc1 through the UI (the Web Serial chooser bypassed by the mock): GVRET and slcan, classic and FD,
// the bus setup rc1 receives, frames in the trace equal to what the fake sent, and the error cases.
import { test, expect, fake, settle } from '../lib/harness.mjs';

test.describe('connect', () => {
  test.use({ fakeOptions: { serial: { device: { record: true } } } });

  test('GVRET with CAN FD: the chooser is filtered to rc1, the bus set up as asked', async ({ app, page }) => {
    await app.open();
    await app.connect({ protocol: 'gvret', bitrate: 500000, fd: true, dataBitrate: 2000000 });
    await expect.poll(() => fake.log(page)).toContain('open 500000/2000000');
    const ev = await fake.events(page);
    const req = ev.find((e) => e.type === 'requestPort');
    expect(req, 'requestPort called (from a user gesture)').toBeTruthy();
    expect(req.filters, 'filtered to rc1').toEqual(expect.arrayContaining([expect.objectContaining({ usbVendorId: 0x1209, usbProductId: 0x0001 })]));
    const s = await fake.stats(page);
    expect(s.mode).toBe('gvret');
    expect(s.channel).toMatchObject({ open: true, bitrate: 500000, fdMode: true, dataBitrate: 2000000, listenOnly: false });
    expect((await fake.appStats(page)).connected).toBe(true);
  });

  test('GVRET classic at 1 Mbit/s: no data phase', async ({ app, page }) => {
    await app.open();
    await app.connect({ protocol: 'gvret', bitrate: 1000000, fd: false });
    await expect.poll(() => fake.log(page)).toContain('open 1000000');
    expect((await fake.stats(page)).channel.fdMode).toBe(false);
  });

  test('slcan with CAN FD', async ({ app, page }) => {
    await app.open();
    await app.connect({ protocol: 'slcan', bitrate: 500000, fd: true, dataBitrate: 2000000 });
    await expect.poll(() => fake.log(page)).toContain('open 500000/2000000');
    expect((await fake.stats(page)).mode).toBe('slcan');
  });

  for (const protocol of ['gvret', 'slcan']) {
    test(`${protocol}: every FD length, BRS, 29-bit, classic and remote frames arrive exactly`, async ({ app, page }) => {
      await app.open();
      await app.connect({ protocol, bitrate: 500000, fd: true, dataBitrate: 2000000 });
      await expect.poll(async () => (await fake.stats(page)).channel.open).toBe(true);
      await fake.setTraffic(page, 'fd-mix', { seconds: 2 });
      await page.waitForTimeout(2300);
      const [s, a] = await settle(page);
      expect(s.encoded).toBe(1000);
      expect(a.framesReceived).toBe(s.encoded);
      expect(a.framesDropped).toBe(0);
      const sent = await fake.recorded(page);
      const tail = await page.evaluate((n) => window.webSavvyCAN && window.webSavvyCAN.traceTail ? window.webSavvyCAN.traceTail(n) : null, sent.length);
      test.skip(!tail, 'the app has no webSavvyCAN.traceTail(n) (README "The app\'s test hook")');
      const norm = (f) => ({ id: f.id, ext: f.ext, fd: f.fd, brs: f.brs, data: f.data });
      expect(tail.slice(-sent.length).map(norm)).toEqual(sent.map(norm));
    });
  }
});

test.describe('connect: a returning visitor', () => {
  test.use({ fakeOptions: { serial: { granted: true } } });
  test('getPorts() has rc1: connects without the chooser', async ({ app, page }) => {
    await app.open();
    await app.connect({ protocol: 'gvret' });
    await expect.poll(() => fake.log(page)).toContain('open 500000/2000000');
    expect((await fake.events(page)).some((e) => e.type === 'requestPort')).toBe(false);
  });
});

test.describe('connect: the chooser cancelled', () => {
  test.use({ fakeOptions: { serial: { chooser: 'cancel' } } });
  test('no port chosen: not connected, no error', async ({ app, page }) => {
    await app.open();
    await app.chooseDevice();
    expect((await fake.appStats(page)).connected).toBe(false);
  });
});

test.describe('connect: rc1 refuses (gs_usb has the CAN channel)', () => {
  test.use({ fakeOptions: { serial: { device: { refuseOpen: true } } } });
  test('GVRET: the bus stays disabled and nothing arrives', async ({ app, page }) => {
    await app.open();
    await app.connect({ protocol: 'gvret', expectOpen: false });
    await page.waitForTimeout(1500);
    const s = await fake.stats(page);
    expect(s.channel.open).toBe(false);
    expect((await fake.appStats(page)).framesReceived).toBe(0);
  });
  test('the New Connection dialog offers only GVRET and LAWICEL (no other type, not even hidden)', async ({ app, page }) => {
    await app.open();
    await app.menu('Connection', 'Open Connection Window');
    await app.click('button', 'Add New Device Connection');
    await expect(app.role('radio', 'Serial Connection (GVRET)')).toBeVisible();
    const radios = await page.getByRole('radio', { includeHidden: true }).evaluateAll(
      (els) => els.map((e) => e.getAttribute('aria-label') || e.textContent.trim()));
    expect(radios.sort()).toEqual(['LAWICEL / SLCAN Serial', 'Serial Connection (GVRET)'].sort());
  });

  test('Enter in the connection window does not disconnect', async ({ app, page }) => {
    await app.open();
    await app.connect({ protocol: 'gvret', bitrate: 500000, fd: false });
    await expect.poll(async () => (await fake.stats(page)).channel.open).toBe(true);
    await app.menu('Connection', 'Open Connection Window');
    await page.keyboard.press('Enter');
    await page.waitForTimeout(1000);
    expect((await fake.appStats(page)).connected).toBe(true);
    expect((await fake.stats(page)).channel.open).toBe(true);
  });
});
