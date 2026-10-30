// The live RE tools at rc1's full rate (stage 4): Sniffer, Flow View (Live Mode), Range State and Single/Multi State
// (a realtime run) all open while the bus runs at 8 000 classic frames/s for 20 s, then 6 900 64-byte CAN FD frames/s
// for 20 s, with a 50 000-frame trace ring that wraps many times. Bounds: no frame lost at rc1 (< 0.1 %, as the
// full-load replay), the page responsive (no stall over 1 s; 10 ms timer p95 < 100 ms), memory level once the windows'
// bounds are reached; each tool exact: the sniffer saw every frame once, Flow View's cache every frame of its ID once
// (consecutive sequence numbers), Range State's list every ID, the realtime run within its memory budget.
// Numbers: results/retools-fullrate.json.
import { test, expect, fake, saveResult } from '../lib/harness.mjs';

const seqOf = (d) => (d[0] | (d[1] << 8) | (d[2] << 16) | (d[3] << 24)) >>> 0;

async function quiet(page, ms = 30000) {
  const end = Date.now() + ms;
  let last = -1, same = 0;
  for (;;) {
    const [s, a] = [await fake.stats(page), await fake.appStats(page)];
    same = s.queuedBytes === 0 && a.queued === 0 && a.framesReceived === last ? same + 1 : 0;
    last = a.framesReceived;
    if (same >= 4 || Date.now() > end) return a;
    await page.waitForTimeout(250);
  }
}

test('every live RE tool open at rc1\'s full rate, classic then CAN FD 64: responsive, nothing lost, exact, bounded',
  async ({ app, page }) => {
    test.setTimeout(240_000);
    await page.addInitScript(() => localStorage.setItem('qt-v0-EVTV-SavvyCAN-Main/MaximumFrames', '50000'));
    await app.open();
    await app.connect({ protocol: 'gvret', bitrate: 1000000, fd: true, dataBitrate: 8000000 });
    for (const [item, hook] of [['Sniffer', 'sniffer'], ['Flow View', 'flowView'], ['Range State', 'rangeState'],
      ['Single/Multi State', 'discreteState']]) {
      await app.menu('RE Tools', item);
      await page.waitForFunction((h) => window.webSavvyCAN[h]().open, hook, { timeout: 10_000 });
    }
    const result = {};
    // classic: Flow View on 0x100 (16 IDs in turn: sequence numbers 16 apart); FD: on 0x18DB33F1, every 7th frame, new
    // in that part (sequence numbers 7 apart)
    for (const [mode, seconds, opts, id, step] of [['classic-full', 20, {}, 0x100, 16],
      ['fd64-full', 20, { extendedEvery: 7 }, 0x18db33f1, 7]]) {
      await page.evaluate(() => window.__probe.reset());
      const before = await fake.appStats(page);
      const rc1Before = await fake.stats(page);
      await fake.setTraffic(page, mode, { seconds, ...opts });
      await page.waitForFunction((x) => window.webSavvyCAN.flowView().ids.includes(x), id, { timeout: 10_000 });
      await page.evaluate((x) => { window.webSavvyCAN.flowViewDo('select', x); window.webSavvyCAN.flowViewDo('live', true); }, id);
      // a realtime Single/Multi State run while the bus runs (2 states, 2 s each, 2 iterations: ~14 s)
      await page.evaluate(() => window.webSavvyCAN.discreteStateRun({ states: 2, realtime: true, freq: 2, iterations: 2 }));
      const mem = [];
      for (let t = 0; t < seconds; t += 4) {
        await page.waitForTimeout(4000);
        mem.push((await page.evaluate(() => window.webSavvyCAN.memory())).heapUsed);
      }
      const probe = await fake.probe(page);
      const st = await quiet(page);
      const rc1 = await fake.stats(page);
      await page.waitForFunction(() => ['DONE', 'IDLE'].includes(window.webSavvyCAN.discreteState().state), null, { timeout: 30_000 });
      const [sn, fv, rs, ds] = await page.evaluate(() => ['sniffer', 'flowView', 'rangeState', 'discreteState'].map((h) => window.webSavvyCAN[h]()));
      const received = st.framesReceived - before.framesReceived;
      const bus = rc1.bus - rc1Before.bus, lost = rc1.lost - rc1Before.lost;

      expect(lost / bus, `${mode}: frames lost at rc1 because the page did not read`).toBeLessThan(0.001);
      // The trace alone (no tool open) measured 32-55 ms p95 for FD64 on the rig from run to run (other jobs share the
      // PC); the tools add ~5-20 ms. Bounded at twice the full-load replay's 50 ms; the stall bound is the real limit.
      expect(probe.timer.p95, `${mode}: timer lateness p95 (ms)`).toBeLessThan(100);
      expect(probe.timer.max, `${mode}: longest stall (ms)`).toBeLessThan(1000);
      expect(st.framesEvicted - before.framesEvicted, `${mode}: the trace ring wrapped`).toBeGreaterThan(50_000);
      expect(sn.framesSeen, `${mode}: the sniffer saw every frame once`).toBe(st.framesReceived);
      expect(sn.rows.filter((r) => r.id >= 0x100 && r.id < 0x110).length, 'the sniffer lists the 16 IDs').toBe(16);
      expect(sn.rows.find((r) => r.id === 0x100).len).toBe(mode === 'fd64-full' ? 64 : 8);
      // Flow View: every frame of its ID, consecutive
      expect(fv.id).toBe(id);
      expect((seqOf(fv.lastData) - seqOf(fv.firstData)) / step + 1, `${mode}: Flow View has every frame of its ID once`).toBe(fv.frames);
      expect(fv.frames).toBeGreaterThan(received / step * 0.9);
      expect(fv.position).toBe(fv.frames - 1);
      expect(rs.ids.filter((x) => x.id >= 0x100 && x.id < 0x110).length, 'Range State lists the 16 IDs').toBe(16);
      expect(ds.sampler.memoryBytes, 'the realtime recording within its budget').toBeLessThanOrEqual(32 << 20);
      expect(ds.sampler.framesAdded).toBeGreaterThan(10_000);
      // memory level: the second half of the run grows by less than 8 MB
      const half = mem.slice(Math.floor(mem.length / 2));
      const growth = Math.max(...half) - Math.min(...half);
      expect(growth, `${mode}: heap in use over the second half: ${JSON.stringify(mem)}`).toBeLessThan(8 << 20);
      result[mode] = { received, bus, lost, evicted: st.framesEvicted - before.framesEvicted, timer: probe.timer,
        longGaps: probe.longGaps.length, heapUsed: mem, flowFrames: fv.frames, flowCacheBytes: fv.cacheBytes,
        sampler: ds.sampler, discreteMatches: ds.matches.length, snifferIds: sn.rows.length };
      await page.evaluate(() => window.webSavvyCAN.flowViewDo('live', false));
    }
    saveResult('retools-fullrate', result);
    test.info().annotations.push({ type: 'result', description: JSON.stringify(result) });
  });
