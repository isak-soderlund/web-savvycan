// The real rc1 on the rig, CAN FD 500k/2M, the stage 4 RE tools on the peer's `re` traffic (can_peer.py, the fake rc1's
// 're' patterns: 0x120 a counter in byte 0, 0x5A in byte 1, the state in bit 19 and bits 26-27; 0x121 a 16-bit triangle;
// 0x130 random bytes; 0x18FF0001 FD 64 BRS with a counter in byte 40, the state in bit 270, the triangle at bit 400),
// each checked against the frames the peer sent:
// - Sniffer: every ID with its newest bytes, every frame counted, the counter changing and the constant not;
// - Flow View: every frame of one ID, byte for byte, classic and 64-byte FD; Live Mode follows the newest frame;
// - Range State: exactly SavvyCAN's candidates (lib/retools-ref.mjs) on the frames sent, the planted triangles among them;
// - Single/Multi State, Realtime: the peer's state bits toggled on the window's cue ("Go to state N"): exactly the planted
//   fields found;
// - Frame Search: a value at known frames (the counter, classic and byte 40 of FD), a bit's changes where the peer
//   changed it.
// Pictures: results/rig-{sniffer,flowview,rangestate,discretestate,search}.png (look at them).
import { join } from 'node:path';
import { test, expect, RESULTS, saveResult } from '../lib/harness.mjs';
import { rangeSearch } from '../lib/retools-ref.mjs';
import { arrived, peer, rigBus, stateFile } from './peer.mjs';

const BUS = { bitrate: 500000, dbitrate: 2000000 };
const CONNECT = { protocol: 'gvret', bitrate: 500000, fd: true, dataBitrate: 2000000 };
const RE_IDS = [0x120, 0x121, 0x130, 0x18ff0001];
const byIdOf = (frames) => {
  const m = new Map(RE_IDS.map((id) => [id, []]));
  for (const f of frames) m.get(f.id).push(f.data);
  return m;
};

test.describe('rig: RE tools on a live bus', () => {
  test.use({ fakeOptions: { serial: { remote: true } } });
  rigBus(test, BUS);

  test('Sniffer: every ID and its newest bytes, every frame counted; the counter changes, the constant doesn\'t',
    async ({ app, page }) => {
      await app.open();
      await app.connect(CONNECT);
      await app.menu('RE Tools', 'Sniffer');
      await page.waitForFunction(() => window.webSavvyCAN.sniffer().open, null, { timeout: 10_000 });
      await page.evaluate(() => window.webSavvyCAN.snifferDo('noExpire', 0, true)); // the rows stay after the peer stops
      const sending = peer.send('re', { count: 4000, rate: 800 });
      const marks = { b0: 0, b1: 0, b40: 0, samples: 0 };
      await page.waitForTimeout(1500);
      for (let i = 0; i < 8; i++) {
        const s = await page.evaluate(() => window.webSavvyCAN.sniffer());
        const a = s.rows.find((r) => r.id === 0x120), f = s.rows.find((r) => r.id === 0x18ff0001);
        if (a && f) {
          marks.samples++;
          if (a.changes[0] !== 0) marks.b0++;
          if (a.changes[1] !== 0) marks.b1++;
          if (f.changes[40] !== 0) marks.b40++;
        }
        await page.waitForTimeout(300);
      }
      const sent = await sending;
      expect(sent.acked, sent.error).toBe(4000);
      await arrived(page, 4000);
      await page.waitForTimeout(600);
      const s = await page.evaluate(() => window.webSavvyCAN.sniffer());
      expect(s.rows.map((r) => r.id), 'every ID, in ID order').toEqual(RE_IDS);
      expect(s.framesSeen, 'every frame counted').toBe(4000);
      for (const r of s.rows) {
        const newest = sent.frames.filter((f) => f.id === r.id).pop();
        expect(r.len, `0x${r.id.toString(16)}: length`).toBe(newest.data.length);
        expect(r.data, `0x${r.id.toString(16)}: the newest bytes`).toEqual(newest.data);
      }
      expect(marks.samples).toBeGreaterThanOrEqual(6);
      expect(marks.b0, 'the counter byte marked changing').toBeGreaterThanOrEqual(marks.samples - 1);
      expect(marks.b40, 'the FD counter byte (40) marked changing').toBeGreaterThanOrEqual(marks.samples - 1);
      expect(marks.b1, 'the constant byte never').toBe(0);
      await page.screenshot({ path: join(RESULTS, 'rig-sniffer.png') });
    });

  test('Flow View: every frame of one ID byte for byte (classic and 64-byte FD); Live Mode follows the newest',
    async ({ app, page }) => {
      await app.open();
      await app.connect(CONNECT);
      const sent = await peer.send('re', { count: 1600, rate: 800 });
      expect(sent.acked, sent.error).toBe(1600);
      await arrived(page, 1600);
      await app.menu('RE Tools', 'Flow View');
      await page.waitForFunction(() => window.webSavvyCAN.flowView().open, null, { timeout: 10_000 });
      let s = await page.evaluate(() => window.webSavvyCAN.flowView());
      expect(s.ids, 'every ID').toEqual(RE_IDS);
      for (const id of [0x120, 0x18ff0001]) {
        expect(await page.evaluate((i) => window.webSavvyCAN.flowViewDo('select', i), id)).toBe(true);
        s = await page.evaluate(() => window.webSavvyCAN.flowView());
        const want = sent.frames.filter((f) => f.id === id);
        expect(s.frames, `0x${id.toString(16)}: every frame`).toBe(want.length);
        const got = await page.evaluate((n) => window.webSavvyCAN.flowViewFrames(0, n), s.frames);
        expect(got.map((f) => f.data), `0x${id.toString(16)}: byte for byte`).toEqual(want.map((f) => f.data));
        const times = got.map((f) => f.time);
        expect(times.every((t, i) => i === 0 || t > times[i - 1]), 'rc1\'s times increasing').toBe(true);
        // 4 frames a round at 800 frames/s: 5 ms between frames of one ID
        expect((times[times.length - 1] - times[0]) / (times.length - 1)).toBeGreaterThan(4000);
        expect((times[times.length - 1] - times[0]) / (times.length - 1)).toBeLessThan(6000);
        expect(s.gridBytes, 'the grid as long as the frames').toBe(want[0].data.length);
      }
      expect(await page.evaluate(() => window.webSavvyCAN.flowViewDo('select', 0x120))).toBe(true);
      expect(await page.evaluate(() => window.webSavvyCAN.flowViewDo('live', true))).toBe(true);
      const more = await peer.send('re', { count: 800, rate: 800 });
      expect(more.acked, more.error).toBe(800);
      await arrived(page, 2400);
      await page.waitForTimeout(800);
      s = await page.evaluate(() => window.webSavvyCAN.flowView());
      const all = [...sent.frames, ...more.frames].filter((f) => f.id === 0x120);
      expect(s.frames, 'Live Mode: the new frames in').toBe(all.length);
      expect(s.curr, 'Live Mode: at the newest frame').toEqual(all[all.length - 1].data);
      await page.screenshot({ path: join(RESULTS, 'rig-flowview.png') });
    });

  test('Range State: exactly SavvyCAN\'s candidates on the frames sent, the planted triangles found (classic and FD bit 400)',
    async ({ app, page }) => {
      await app.open();
      await app.connect(CONNECT);
      const sent = await peer.send('re', { count: 4000, rate: 1000 }); // 1000 rounds: 2.5 triangle periods
      expect(sent.acked, sent.error).toBe(4000);
      await arrived(page, 4000);
      await app.menu('RE Tools', 'Range State');
      await page.waitForFunction(() => window.webSavvyCAN.rangeState().open, null, { timeout: 10_000 });
      const p = { minBits: 16, maxBits: 16, granularity: 8, mode: 1, sign: 1, sensitivity: 10 };
      const runs = (await page.evaluate(() => window.webSavvyCAN.rangeState())).runs;
      expect(await page.evaluate((o) => window.webSavvyCAN.rangeStateRun(o), p)).toBe(true);
      await page.waitForFunction((n) => window.webSavvyCAN.rangeState().runs > n, runs, { timeout: 120_000, polling: 100 });
      const s = await page.evaluate(() => window.webSavvyCAN.rangeState());
      const key = (c) => `${c.id}:${c.startBit}:${c.bits}:${c.signed}:${c.bigEndian}`;
      expect(s.lastRunFrames, 'every frame searched').toBe(4000);
      expect(s.candidates.map(key), 'exactly SavvyCAN\'s candidates').toEqual(rangeSearch(byIdOf(sent.frames), p).map(key));
      expect(s.candidates.some((c) => c.id === 0x121 && c.startBit === 0 && c.bits === 16), 'the classic triangle').toBe(true);
      expect(s.candidates.some((c) => c.id === 0x18ff0001 && c.startBit === 400 && c.bits === 16), 'the FD triangle at bit 400').toBe(true);
      expect(s.candidates.filter((c) => c.id === 0x130), 'nothing from random bytes').toEqual([]);
      const row = s.candidates.findIndex((c) => c.id === 0x18ff0001 && c.startBit === 400);
      await page.evaluate((r) => window.webSavvyCAN.rangeStateSelect(r), row);
      await page.waitForTimeout(300);
      const g = await page.evaluate(() => window.webSavvyCAN.rangeState());
      expect([g.plot.points, g.plot.minValue, g.plot.maxValue], 'its graph: every frame, the triangle\'s range').toEqual([1000, 1000, 3000]);
      await page.screenshot({ path: join(RESULTS, 'rig-rangestate.png') });
    });

  test('Single/Multi State realtime: the peer toggles its state bits on the window\'s cue: exactly those fields found',
    async ({ app, page }) => {
      test.setTimeout(240_000);
      await app.open();
      await app.connect(CONNECT);
      await app.menu('RE Tools', 'Single/Multi State');
      await page.waitForFunction(() => window.webSavvyCAN.discreteState().open, null, { timeout: 10_000 });
      const cue = stateFile();
      const sending = peer.send('re', { seconds: 35, rate: 400, stateFile: cue.file, list: false }); // the run took 17 s on the rig
      try {
        await page.waitForTimeout(1500);
        const t0 = Date.now();
        expect(await page.evaluate(() => window.webSavvyCAN.discreteStateRun({ states: 2, realtime: true, freq: 1, iterations: 3 }))).toBe(true);
        // The user: holds the state the window asks for.
        const asked = [];
        let s;
        for (;;) {
          s = await page.evaluate(() => window.webSavvyCAN.discreteState());
          if (s.state === 'DONE') break;
          if (s.state === 'COUNTDOWN_SIGNAL' || s.state === 'GETTING_SIGNAL') {
            cue.set(s.toggleState);
            if (s.state === 'GETTING_SIGNAL' && asked[asked.length - 1] !== s.label) asked.push(s.label);
          }
          expect(Date.now() - t0, `realtime run not done: ${JSON.stringify(s)}`).toBeLessThan(30_000);
          await page.waitForTimeout(40);
        }
        const ms = Date.now() - t0;
        const key = (m) => `${m.id.toString(16)}:${m.startBit}:${m.bits}:${m.values.join('/')}`;
        expect(asked, 'asked for each state in turn').toEqual(['Go to state 1', 'Go to state 2', 'Go to state 1', 'Go to state 2', 'Go to state 1', 'Go to state 2']);
        expect(s.matches.map(key), 'exactly the planted fields').toEqual(['120:19:1:0/1', '120:26:1:0/1', '18ff0001:270:1:0/1']);
        expect(s.matches.every((m) => m.share >= 0.9)).toBe(true);
        expect(s.sampler.ids, 'every ID recorded').toBe(4);
        await page.screenshot({ path: join(RESULTS, 'rig-discretestate.png') });
        saveResult('rig-discretestate', { ms, matches: s.matches, sampler: s.sampler });
      } finally {
        cue.set(0);
        const sent = await sending;
        cue.remove();
        expect(sent.acked, sent.error).toBe(14000);
      }
    });

  test('Frame Search: the counter\'s value at the known frames (classic, FD byte 40); a state bit\'s changes where the peer made them',
    async ({ app, page }) => {
      await app.open();
      await app.connect(CONNECT);
      const a = await peer.send('re', { count: 1200, rate: 800, state: 0 });
      expect(a.acked, a.error).toBe(1200);
      const b = await peer.send('re', { count: 1200, rate: 800, state: 1 });
      expect(b.acked, b.error).toBe(1200);
      await arrived(page, 2400);
      const frames = [...a.frames, ...b.frames];
      await app.menu('RE Tools', 'Frame Search');
      await page.waitForFunction(() => window.webSavvyCAN.search().open, null, { timeout: 20_000 });
      const search = async (set, fn, what) => {
        const want = frames.map((f, i) => (fn(f) ? i : -1)).filter((i) => i >= 0);
        expect(await page.evaluate((x) => window.webSavvyCAN.searchSet(x), set), `${what}: set`).toBe(true);
        await app.click('button', 'Search');
        // label, rows and the trace's selection together (two searches in a row can have the same label)
        await expect.poll(async () => {
          const s = await page.evaluate(() => window.webSavvyCAN.search());
          return { label: s.label, rows: s.rows, selected: s.selectedRow };
        }, { message: `${what}: the frames, the first selected in the trace` })
          .toEqual({ label: want.length ? `Match 1 of ${want.length}` : 'No matches found.', rows: want, selected: want[0] });
        return want;
      };
      const base = { len: null, dir: 'both', intel: true, signed: false, scale: 1, bias: 0 };
      // counter 0x2A: rounds 42 and 298 of each batch (300 rounds a batch, the counter from 0 in each)
      const c = await search({ ...base, id: '0x120', mode: 'specific', startBit: 0, bits: 8, value: '0x2A' },
        (f) => f.id === 0x120 && f.data[0] === 0x2a, 'counter = 0x2A');
      expect(c).toEqual([168, 1192, 1368, 2392]);
      const d = await search({ ...base, id: '0x18FF0001', mode: 'specific', startBit: 320, bits: 8, value: '42' },
        (f) => f.id === 0x18ff0001 && f.data[40] === 42, 'FD byte 40 = 42');
      expect(d.map((i) => i - 3), 'the FD frame of the same rounds').toEqual(c);
      // bit 19 (the state): any change: the first frame of 0x120, then where the second batch began (frame 1200)
      const e = await search({ ...base, id: '0x120', mode: 'any', startBit: 19, bits: 1 },
        (f) => f.id === 0x120 && (f === frames[0] || f === b.frames[0]), 'bit 19 changes');
      expect(e).toEqual([0, 1200]);
      await page.screenshot({ path: join(RESULTS, 'rig-search.png') });
    });
});
