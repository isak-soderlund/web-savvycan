// The real rc1 on the rig: the last connection is made again after a reload, without the connection window or the
// chooser (rc1 already allowed), with the same bus; the peer's frames arrive.
import { test, expect, fake } from '../lib/harness.mjs';
import { arrived, peer, rigBus } from './peer.mjs';

test.describe('rig: auto-connect', () => {
  test.use({ fakeOptions: { serial: { remote: true, granted: true } } });
  rigBus(test, { bitrate: 500000, dbitrate: 2000000 });

  for (const protocol of ['gvret', 'slcan']) {
    test(`${protocol} FD: after a reload connects by itself, frames arrive`, async ({ app, page }) => {
      await app.open();
      await app.connect({ protocol, bitrate: 500000, fd: true, dataBitrate: 2000000 });
      await app.reload();
      await expect.poll(async () => (await fake.appStats(page)).connected, { timeout: 20000 }).toBe(true);
      await expect.poll(async () => (await fake.stats(page)).channel.open, { timeout: 20000 }).toBe(true);
      const s = await fake.stats(page);
      expect(s.mode).toBe(protocol);
      expect(s.channel).toMatchObject({ bitrate: 500000, fdMode: true, dataBitrate: 2000000, listenOnly: false });
      expect((await fake.events(page)).some((e) => e.type === 'requestPort'), 'no chooser').toBe(false);
      expect(await app.role('button', 'Add New Device Connection').count(), 'no connection window').toBe(0);
      // GVRET's ESP32 start: the channel opens ~3 s after the port.
      await page.waitForTimeout(1000);
      const before = (await fake.appStats(page)).framesReceived;
      const sent = await peer.send('fd-mix', { count: 400, rate: 400 });
      expect(sent.acked, sent.error).toBe(400);
      const [, a] = await arrived(page, before + 400);
      expect(a.framesReceived - before).toBe(400);
    });
  }
});
