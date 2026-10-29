// @long. The real rc1 on the rig for longer than 71.6 min, the wrap of its 32-bit µs clock (GVRET frame times): the
// peer sends the Leaf IDs at 100 frames/s for 75 min while the app logs continuously; the download has every frame,
// in order, its times increasing across the wrap and spaced as sent (10 ms); the page stays responsive and its
// memory level. Run: tests/web/run.sh --rig --long -- rig/timewrap.spec.mjs (results/rig-timewrap.json).
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { test, expect, fake, RESULTS, saveResult } from '../lib/harness.mjs';
import { arrived, peer, rigBus } from './peer.mjs';

const MINUTES = Number(process.env.WSC_RIG_WRAP_MINUTES || 75);
const RATE = 100;

test.describe('rig: past rc1\'s clock wrap @long', () => {
  test.use({ fakeOptions: { serial: { remote: true } } });
  rigBus(test, { bitrate: 500000 });

  test(`${MINUTES} min of logging: every frame, times increasing across the wrap`, async ({ app, page }) => {
    test.setTimeout((MINUTES + 10) * 60_000);
    await app.open();
    await app.connect({ protocol: 'gvret', bitrate: 500000, fd: false });
    await app.startLog();
    await page.evaluate(() => window.__probe.reset());
    const count = MINUTES * 60 * RATE;
    const sending = peer.send('leaf', { count, rate: RATE, list: false });
    const memory = [];
    const t0 = Date.now();
    let done = false;
    sending.finally(() => { done = true; });
    while (!done) {
      await page.waitForTimeout(60_000);
      const m = await page.evaluate(() => ({ ...window.webSavvyCAN.memory(), ...window.webSavvyCAN.stats() }));
      memory.push({ min: Math.round((Date.now() - t0) / 60000), heapUsed: m.heapUsed, heapSize: m.heapSize,
        framesReceived: m.framesReceived, logFramesWritten: m.logFramesWritten });
    }
    const sent = await sending;
    expect(sent.acked, sent.error).toBe(count);
    await arrived(page, count, 30000);
    const probe = await fake.probe(page);
    const dl = await app.stopLog();
    const file = join(RESULTS, 'rig-timewrap.csv');
    await dl.saveAs(file);
    const rows = readFileSync(file, 'latin1').split('\n').slice(1).filter(Boolean);
    const ts = rows.map((r) => Number(r.split(',')[0]));
    const seqs = rows.map((r) => { const c = r.split(','); return parseInt(c[6], 16) | (parseInt(c[7], 16) << 8) | (parseInt(c[8], 16) << 16); });
    const steps = ts.slice(1).map((t, i) => t - ts[i]);
    const result = { minutes: MINUTES, frames: count, rows: rows.length, firstUs: ts[0], lastUs: ts[ts.length - 1],
      spanMin: (ts[ts.length - 1] - ts[0]) / 60e6, minStepUs: Math.min(...steps), maxStepUs: Math.max(...steps),
      probe, memory };
    saveResult('rig-timewrap', result);
    expect(rows.length, 'every frame in the log').toBe(count);
    expect(seqs.every((q, i) => q === (i & 0xffffff)), 'in order, none missing').toBe(true);
    expect(result.minStepUs, 'times increasing across the wrap').toBeGreaterThan(0);
    expect(result.spanMin, 'the log spans the run (past 71.6 min)').toBeGreaterThan(MINUTES - 1);
    expect(result.maxStepUs, 'no jump (10 ms apart as sent)').toBeLessThan(200_000);
    expect(probe.longGaps.filter((g) => g > 1000)).toEqual([]);
  });
});
