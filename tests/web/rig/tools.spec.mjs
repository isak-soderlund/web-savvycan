// The real rc1 on the rig, the stage 2 and 4 windows on a live bus from the peer, checked against the frames the peer
// sent: a graph (Graph > Add New Graph through the test hook) takes every frame of its ID, RE Tools > Temporal Graph
// counts every frame of every ID, RE Tools > Frame Data Analysis of an FD ID counts every bit as sent. Pictures:
// results/rig-*.png (look at them).
import { join } from 'node:path';
import { test, expect, fake, RESULTS } from '../lib/harness.mjs';
import { arrived, peer, rigBus } from './peer.mjs';

const LEAF = [0x1DA, 0x1DB, 0x1DC, 0x1F2, 0x50A, 0x50C, 0x55A, 0x55B, 0x59E, 0x5A9, 0x5BC, 0x5C0, 0x11A, 0x180, 0x182, 0x284];

/** Frame Data Analysis of `payloads` as the app computes it: per bit, changes from the frame before and frames set. */
function analysis(payloads) {
  const flips = Array(512).fill(0), sets = Array(512).fill(0), ref = Array(64).fill(null);
  for (const d of payloads) {
    d.forEach((v, c) => {
      if (ref[c] === null) ref[c] = v;
      for (let l = 0; l < 8; l++) {
        if (v & (1 << l)) sets[c * 8 + l]++;
        if ((ref[c] ^ v) & (1 << l)) flips[c * 8 + l]++;
      }
      ref[c] = v;
    });
  }
  return { frames: payloads.length, flips, sets };
}

test.describe('rig: graphs and RE tools on a live bus', () => {
  test.use({ fakeOptions: { serial: { remote: true } } });
  rigBus(test, { bitrate: 500000, dbitrate: 2000000 });

  test('a live graph of 0x1DA takes every frame of it, its last value the last sent', async ({ app, page }) => {
    await app.open();
    await app.connect({ protocol: 'gvret', bitrate: 500000, fd: true, dataBitrate: 2000000 });
    const first = await peer.send('leaf', { count: 800, rate: 800 });
    expect(first.acked, first.error).toBe(800);
    await arrived(page, 800);
    expect(await page.evaluate(() => window.webSavvyCAN.addGraph(0x1da, 0, 8, true)), 'graph created').toBe(true);
    const a = await page.evaluate(() => window.webSavvyCAN.graphState());
    const more = await peer.send('leaf', { count: 3200, rate: 800 });
    expect(more.acked, more.error).toBe(3200);
    await arrived(page, 4000);
    await page.waitForTimeout(1000);
    const b = await page.evaluate(() => window.webSavvyCAN.graphState());
    const of1DA = [...first.frames, ...more.frames].filter((f) => f.id === 0x1da);
    expect(b.series[0].samples, 'every 0x1DA frame graphed').toBe(of1DA.length);
    expect(b.series[0].lastValue, 'the last value: byte 0 of the last 0x1DA').toBe(of1DA[of1DA.length - 1].data[0]);
    expect(b.points).toBeGreaterThan(a.points);
    expect(Math.abs(b.xUpper - b.lastKey), 'follows the end').toBeLessThan(1000);
    await page.screenshot({ path: join(RESULTS, 'rig-graph-live.png') });
  });

  test('Temporal Graph: every frame of every ID counted while the bus runs', async ({ app, page }) => {
    await app.open();
    await app.connect({ protocol: 'gvret', bitrate: 500000, fd: true, dataBitrate: 2000000 });
    await app.menu('RE Tools', 'Temporal Graph');
    await page.waitForFunction(() => window.webSavvyCAN.temporalState().open, null, { timeout: 10_000 });
    const sent = await peer.send('leaf', { count: 4000, rate: 1000 });
    expect(sent.acked, sent.error).toBe(4000);
    await arrived(page, 4000);
    await page.waitForTimeout(1500);
    const s = await page.evaluate(() => window.webSavvyCAN.temporalState());
    expect(s.frames, 'every frame').toBe(4000);
    expect(s.ids.map((r) => r.id), 'every ID').toEqual([...LEAF].sort((x, y) => x - y));
    for (const r of s.ids) expect(r.frames, `0x${r.id.toString(16)}`).toBe(sent.frames.filter((f) => f.id === r.id).length);
    // rc1's clock: 4000 frames at 1000/s span ~4 s.
    expect(s.lastUs - s.firstUs).toBeGreaterThan(3.5e6);
    await page.screenshot({ path: join(RESULTS, 'rig-temporal-live.png') });
  });

  test('Frame Data Analysis of FD IDs: every bit as the peer sent it', async ({ app, page }) => {
    await app.open();
    await app.connect({ protocol: 'gvret', bitrate: 500000, fd: true, dataBitrate: 2000000 });
    await app.menu('RE Tools', 'Frame Data Analysis');
    await expect.poll(async () => (await page.evaluate(() => window.webSavvyCAN.frameInfo())).open).toBe(true);
    const sent = await peer.send('fd-mix', { count: 2000, rate: 1000 });
    expect(sent.acked, sent.error).toBe(2000);
    await arrived(page, 2000);
    await page.waitForTimeout(1000);
    const s0 = await page.evaluate(() => window.webSavvyCAN.frameInfo());
    expect([...s0.ids].sort((x, y) => x - y), 'every ID in the list').toEqual([...new Set(sent.frames.map((f) => f.id))].sort((x, y) => x - y));
    // Up to three IDs with 64-byte frames (the peer's fd-mix gives each FD length its own IDs).
    const byId = {};
    for (const f of sent.frames) (byId[f.id] ||= []).push(f.rtr ? [] : f.data);
    const pick = Object.keys(byId).map(Number).filter((id) => byId[id].some((d) => d.length === 64)).slice(0, 3);
    expect(pick.length).toBeGreaterThan(0);
    for (const id of pick) {
      expect(await page.evaluate((i) => window.webSavvyCAN.frameInfoSelect(i), id), `0x${id.toString(16)} listed`).toBe(true);
      const s = await page.evaluate(() => window.webSavvyCAN.frameInfo());
      const want = analysis(byId[id]);
      expect(s.frames, `0x${id.toString(16)}: frames`).toBe(want.frames);
      expect(s.flips, `0x${id.toString(16)}: bit changes`).toEqual(want.flips);
      expect(s.sets, `0x${id.toString(16)}: bits set`).toEqual(want.sets);
      expect(s.gridBytes).toBe(64);
    }
    await page.screenshot({ path: join(RESULTS, 'rig-frameinfo-live.png') });
  });
});
