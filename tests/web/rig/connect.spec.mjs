// The real rc1 on the rig, classic bus: connect over GVRET and LAWICEL through the app's UI, the peer's frames in the
// trace exactly as sent (order, IDs, lengths, data), nothing dropped. (rig/README.md)
import { test, expect, fake } from '../lib/harness.mjs';
import { arrived, peer, rigBus } from './peer.mjs';

const norm = (f) => ({ id: f.id, ext: !!f.ext, fd: !!f.fd, brs: !!f.brs, data: Array.from(f.data) });

test.describe('rig: classic 500 kbit/s', () => {
  test.use({ fakeOptions: { serial: { remote: true } } });
  rigBus(test, { bitrate: 500000 });

  for (const protocol of ['gvret', 'slcan']) {
    test(`${protocol}: 2000 frames from the peer arrive exactly`, async ({ app, page }) => {
      await app.open();
      await app.connect({ protocol, bitrate: 500000, fd: false });
      const before = (await fake.appStats(page)).framesReceived;
      const sent = await peer.send('classic', { count: 2000, rate: 2000 });
      expect(sent.sent).toBe(2000);
      const [s, a] = await arrived(page, before + 2000);
      expect(a.framesReceived - before, 'frames the app parsed').toBe(2000);
      expect(a.framesDropped).toBe(0);
      expect(s.encoded, 'frames rc1 sent the page').toBeGreaterThanOrEqual(2000);
      const tail = await page.evaluate((n) => window.webSavvyCAN.traceTail(n), 2000);
      expect(tail.map(norm)).toEqual(sent.frames.map(norm));
    });
  }
});
