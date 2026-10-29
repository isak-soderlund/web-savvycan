// The real rc1 on the rig, CAN FD 500 kbit/s / 2 Mbit/s: every FD length with BRS on and off, 11- and 29-bit IDs,
// classic and remote frames from the peer arrive exactly over GVRET and LAWICEL; a continuous log of them downloads
// with every row as sent (SavvyCAN's native CSV, loaded by SavvyCAN's own parser when nativecsv_tool is built).
import { existsSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { test, expect, fake, RESULTS, NATIVECSV, savvycanCheck } from '../lib/harness.mjs';
import { arrived, peer, rigBus } from './peer.mjs';

// Remote frames: rc1's GVRET has no RTR flag, a remote frame comes as a data frame of length 0 (as in the fake).
const norm = (f) => ({ id: f.id, ext: !!f.ext, fd: !!f.fd, brs: !!f.fd && !!f.brs, data: f.rtr ? [] : Array.from(f.data) });

test.describe('rig: CAN FD 500k/2M', () => {
  test.use({ fakeOptions: { serial: { remote: true } } });
  rigBus(test, { bitrate: 500000, dbitrate: 2000000 });

  for (const protocol of ['gvret', 'slcan']) {
    test(`${protocol}: every FD length, BRS, 29-bit, classic and remote frames arrive exactly`, async ({ app, page }) => {
      await app.open();
      await app.connect({ protocol, bitrate: 500000, fd: true, dataBitrate: 2000000 });
      const before = (await fake.appStats(page)).framesReceived;
      const sent = await peer.send('fd-mix', { count: 800, rate: 800 });
      expect(sent.acked, sent.error).toBe(800);
      const [, a] = await arrived(page, before + 800);
      expect(a.framesReceived - before).toBe(800);
      expect(a.framesDropped).toBe(0);
      const tail = await page.evaluate((n) => window.webSavvyCAN.traceTail(n), 800);
      expect(tail.map(norm)).toEqual(sent.frames.map(norm));
    });
  }

  test('a continuous log downloads every frame as sent', async ({ app, page }) => {
    await app.open();
    await app.connect({ protocol: 'gvret', bitrate: 500000, fd: true, dataBitrate: 2000000 });
    await app.startLog();
    const name = (await fake.appStats(page)).logFile;
    const before = (await fake.appStats(page)).framesReceived;
    const sent = await peer.send('fd-mix', { count: 2000, rate: 1000 });
    expect(sent.acked, sent.error).toBe(2000);
    await arrived(page, before + 2000);
    const dl = await app.stopLog();
    expect(dl.suggestedFilename()).toBe(name);
    const file = join(RESULTS, 'rig-fd-log.csv');
    await dl.saveAs(file);
    const parsed = await page.evaluate((t) => RC1Fake.parseSavvyCsv(t), readFileSync(file, 'latin1'));
    expect(parsed.frames.map(norm)).toEqual(sent.frames.map(norm));
    const ts = parsed.frames.map((f) => f.ts);
    expect(ts.every((t, i) => i === 0 || t >= ts[i - 1]), 'time stamps increasing').toBe(true);
    // rc1's µs clock: 2000 frames at 1000/s span about 2 s.
    expect(ts[ts.length - 1] - ts[0]).toBeGreaterThan(1.8e6);
    expect(ts[ts.length - 1] - ts[0]).toBeLessThan(2.5e6);
    if (existsSync(NATIVECSV)) {
      test.info().annotations.push({ type: 'savvycan', description: savvycanCheck(file, '--exact', '2000', '--first', '-1') });
    }
  });
});
