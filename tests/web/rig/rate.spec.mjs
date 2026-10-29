// The real rc1 on the rig at the bus's full rate: the peer sends back to back (as fast as the bus takes them, every
// frame ACKed) and every frame reaches the app, in order, none dropped; the page stays responsive.
import { test, expect, fake, saveResult } from '../lib/harness.mjs';
import { arrived, peer } from './peer.mjs';

const seq = (f) => f.data[0] | (f.data[1] << 8) | (f.data[2] << 16) | (f.data[3] * 2 ** 24);
const cases = [
  { name: 'classic 1 Mbit/s, 8 bytes', bus: { bitrate: 1000000 }, connect: { bitrate: 1000000, fd: false }, pattern: 'classic', count: 40000 },
  { name: 'CAN FD 500k/4M, 64 bytes BRS', bus: { bitrate: 500000, dbitrate: 4000000 },
    connect: { bitrate: 500000, fd: true, dataBitrate: 4000000 }, pattern: 'fd64', count: 20000 },
];

test.describe('rig: full bus rate', () => {
  test.use({ fakeOptions: { serial: { remote: true } } });
  let released = [];
  test.beforeAll(() => { released = peer.release(); });
  test.afterAll(() => { peer.restore(); peer.reclaim(released); });

  for (const c of cases) {
    test(`${c.name}: every frame arrives`, async ({ app, page }) => {
      peer.setup(c.bus);
      await app.open();
      await app.connect({ protocol: 'gvret', ...c.connect });
      await page.evaluate(() => window.__probe.reset());
      const before = (await fake.appStats(page)).framesReceived;
      const sent = await peer.send(c.pattern, { count: c.count, rate: 0, list: false });
      expect(sent.acked, sent.error).toBe(c.count);
      const [s, a] = await arrived(page, before + c.count, 20000);
      const probe = await fake.probe(page);
      const tail = await page.evaluate((n) => window.webSavvyCAN.traceTail(n), 2000);
      const seqs = tail.map(seq);
      const result = { case: c.name, frames: c.count, seconds: sent.seconds, perSecond: Math.round(c.count / sent.seconds),
        received: a.framesReceived - before, dropped: a.framesDropped, bytesFromRc1: s.bytesToHost, probe };
      saveResult(`rig-rate-${c.pattern}`, result);
      test.info().annotations.push({ type: 'rate', description: JSON.stringify(result) });
      expect(a.framesReceived - before).toBe(c.count);
      expect(a.framesDropped).toBe(0);
      expect(seqs, 'the newest 2000 in order, ending with the last sent').toEqual(seqs.map((_, i) => c.count - 2000 + i));
      expect(probe.longGaps.filter((g) => g > 1000), 'no stall over 1 s').toEqual([]);
    });
  }
});
