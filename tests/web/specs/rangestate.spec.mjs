// RE Tools > Range State (stage 4, live RE tools): finds signals that move smoothly within a range. Its candidates
// must be exactly those of SavvyCAN's own test (lib/retools-ref.mjs, its processSignal written out) on the frames
// that were sent, the planted signals among them (a 16-bit triangle in a classic frame and at start bit 400 of a
// 64-byte CAN FD frame), none from random bytes; the graph of a candidate; the page responsive while it searches;
// live: the ID list fills from the bus across the trace ring's wraps. Pictures: results/rangestate-{light,dark}.png;
// numbers: results/rangestate.json.
import { join } from 'node:path';
import { mkdirSync, writeFileSync } from 'node:fs';
import { test, expect, fake, RESULTS, saveResult } from '../lib/harness.mjs';
import { rangeSearch } from '../lib/retools-ref.mjs';

const rs = (page) => page.evaluate(() => window.webSavvyCAN.rangeState());
const hex = (n, w) => n.toString(16).toUpperCase().padStart(w, '0');
const key = (c) => `${c.id}:${c.startBit}:${c.bits}:${c.signed}:${c.bigEndian}`;

/** Sets the boxes, presses Recalculate Candidate Signals, waits for the search to end. */
async function run(page, opts) {
  const runs = (await rs(page)).runs;
  expect(await page.evaluate((o) => window.webSavvyCAN.rangeStateRun(o), opts)).toBe(true);
  await page.waitForFunction((n) => window.webSavvyCAN.rangeState().runs > n, runs, { timeout: 120_000, polling: 100 });
  return rs(page);
}

/** A GVRET CSV with the patterns: 0x120 (counter, constant), 0x121 (16-bit triangle), 0x130 (random), and the FD ID. */
function patternLog(n) {
  const rows = ['Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8'];
  const byId = new Map([[0x120, []], [0x121, []], [0x130, []], [0x18ff0001, []]]);
  let rnd = 99;
  const random = () => ((rnd = (Math.imul(rnd, 1103515245) + 12345) >>> 0) >>> 16) & 0xff;
  for (let k = 0; k < n; k++) {
    const t = 1_000_000 + k * 10_000;
    const tri = 1000 + 10 * Math.abs(((k + 200) % 400) - 200);
    const frames = [
      [0x120, false, [k & 0xff, 0x5a, 0, 0, 0x11, 0x22, 0x33, 0x44]],
      [0x121, false, [tri & 0xff, tri >> 8, 0, 0, 0, 0, 0, 0]],
      [0x130, false, Array.from({ length: 8 }, random)],
    ];
    const fd = new Array(64).fill(0);
    fd[40] = k & 0xff; fd[63] = 0xa5; fd[50] = tri & 0xff; fd[51] = tri >> 8;
    frames.push([0x18ff0001, true, fd]);
    for (const [id, ext, d] of frames) {
      byId.get(id).push(d);
      rows.push(`${t},${hex(id, 8)},${ext},Rx,0,${d.length},${d.map((b) => hex(b, 2)).join(',')}${d.length > 8 ? ',FD+BRS' : ''},`);
    }
  }
  mkdirSync(RESULTS, { recursive: true });
  const file = join(RESULTS, 'rangestate-log.csv');
  writeFileSync(file, rows.join('\n') + '\n');
  return { file, byId, frames: n * 4 };
}

test('range state: the candidates of SavvyCAN\'s test on the frames sent, the planted triangles found (classic and ' +
  'CAN FD start bit 400), none from random bytes; a candidate\'s graph; responsive while searching; light and dark',
  async ({ app, page }) => {
    test.setTimeout(300_000);
    const N = 1000;
    const { file, byId, frames } = patternLog(N);
    await app.open();
    await app.loadFile(file, frames, 120_000);
    await app.menu('RE Tools', 'Range State');
    await page.waitForFunction(() => window.webSavvyCAN.rangeState().open, null, { timeout: 10_000 });
    let s = await rs(page);
    expect(s.ids.map((x) => x.id), 'every ID, checked').toEqual([0x120, 0x121, 0x130, 0x18ff0001]);

    // 16-bit little-endian unsigned on byte boundaries, the window's default sensitivity (10).
    const p1 = { minBits: 16, maxBits: 16, granularity: 8, mode: 1, sign: 1, sensitivity: 10 };
    s = await run(page, p1);
    const want1 = rangeSearch(byId, p1);
    expect(s.candidates.map(key), 'exactly SavvyCAN\'s candidates').toEqual(want1.map(key));
    expect(s.lastRunFrames).toBe(frames);
    expect(s.candidates.some((c) => c.id === 0x121 && c.startBit === 0 && c.bits === 16), 'the classic triangle').toBe(true);
    expect(s.candidates.some((c) => c.id === 0x18ff0001 && c.startBit === 400 && c.bits === 16), 'the FD triangle at start bit 400').toBe(true);
    expect(s.candidates.filter((c) => c.id === 0x130), 'nothing from random bytes').toEqual([]);
    expect(s.listed).toBe(s.candidates.length);

    // The FD candidate's graph: every frame of the ID, the triangle's range.
    const row = s.candidates.findIndex((c) => c.id === 0x18ff0001 && c.startBit === 400);
    await page.evaluate((r) => window.webSavvyCAN.rangeStateSelect(r), row);
    await page.waitForTimeout(300);
    s = await rs(page);
    expect([s.plot.points, s.plot.minValue, s.plot.maxValue]).toEqual([N, 1000, 3000]);
    expect(s.plot.penColor, 'the line black in light').toBe('#000000');
    await page.screenshot({ path: join(RESULTS, 'rangestate-light.png') });
    await app.setTheme('dark');
    await page.waitForTimeout(1000);
    s = await rs(page);
    expect(s.plot.penColor, 'light grey in dark').toBe('#e0e0e0');
    await page.screenshot({ path: join(RESULTS, 'rangestate-dark.png') });
    await app.setTheme('light');

    // A broad search of the classic IDs (8..16 bits, every start bit, both byte orders and signs), timed, with the
    // event loop probed: same candidates as SavvyCAN's test.
    await page.evaluate(() => window.webSavvyCAN.rangeStateIds([0x18ff0001], false));
    const p2 = { minBits: 8, maxBits: 16, granularity: 1, mode: 2, sign: 2, sensitivity: 10 };
    await page.evaluate(() => window.__probe.reset());
    s = await run(page, p2);
    const probe2 = await fake.probe(page);
    const classic = new Map([...byId].filter(([id]) => id !== 0x18ff0001));
    const want2 = rangeSearch(classic, p2);
    expect(s.candidates.map(key), 'exactly SavvyCAN\'s candidates (broad)').toEqual(want2.map(key));
    expect(s.candidates.filter((c) => c.id === 0x130), 'nothing from random bytes').toEqual([]);
    const broadMs = s.lastRunMs;
    const gap2 = Math.max(0, ...probe2.longGaps);
    expect(gap2, `longest event-loop gap while searching: ${gap2} ms`).toBeLessThan(1000);

    // CAN FD alone, 16 bits at every start bit (512 start bits x 4 kinds), timed.
    await page.evaluate(() => { window.webSavvyCAN.rangeStateIds('none'); window.webSavvyCAN.rangeStateIds([0x18ff0001], true); });
    const p3 = { minBits: 16, maxBits: 16, granularity: 1, mode: 2, sign: 2, sensitivity: 10 };
    await page.evaluate(() => window.__probe.reset());
    s = await run(page, p3);
    const probe3 = await fake.probe(page);
    const want3 = rangeSearch(new Map([[0x18ff0001, byId.get(0x18ff0001)]]), p3);
    expect(s.candidates.map(key), 'exactly SavvyCAN\'s candidates (FD)').toEqual(want3.map(key));
    expect(s.candidates.some((c) => c.startBit === 400 && !c.bigEndian && !c.signed)).toBe(true);
    const gap3 = Math.max(0, ...probe3.longGaps);
    expect(gap3, `longest event-loop gap while searching: ${gap3} ms`).toBeLessThan(1000);
    const result = { framesPerId: N, run1: { candidates: want1.length }, broad: { candidates: want2.length, ms: broadMs },
      fd: { candidates: want3.length, ms: s.lastRunMs }, maxGapMs: Math.max(gap2, gap3) };
    saveResult('rangestate', result);
    test.info().annotations.push({ type: 'result', description: JSON.stringify(result) });
  });

test('range state live: the ID list fills from the bus and keeps every ID across the trace ring\'s wraps; a search ' +
  'on the live trace finds the triangle', async ({ app, page }) => {
  test.setTimeout(120_000);
  await page.addInitScript(() => localStorage.setItem('qt-v0-EVTV-SavvyCAN-Main/MaximumFrames', '50000'));
  await app.open();
  await app.connect({ protocol: 'gvret', bitrate: 1000000, fd: true, dataBitrate: 8000000 });
  await app.menu('RE Tools', 'Range State');
  await page.waitForFunction(() => window.webSavvyCAN.rangeState().open, null, { timeout: 10_000 });
  await fake.setTraffic(page, 're', { seconds: 6 });
  await page.waitForTimeout(6500);
  let s = await rs(page);
  expect(s.ids.map((x) => x.id), 'the IDs of the bus').toEqual([0x120, 0x121, 0x130, 0x18ff0001]);
  await page.evaluate(() => { window.webSavvyCAN.rangeStateIds('none'); window.webSavvyCAN.rangeStateIds([0x121], true); });
  s = await run(page, { minBits: 16, maxBits: 16, granularity: 8, mode: 1, sign: 1, sensitivity: 10 });
  expect(s.candidates.map(key), 'the live triangle').toContain(key({ id: 0x121, startBit: 0, bits: 16, signed: false, bigEndian: false }));
  // classic-full over 16 IDs for 10 s with a 50 000-frame ring: it wraps, the list gets all of them.
  await fake.setTraffic(page, 'classic-full', { seconds: 10 });
  await page.waitForTimeout(11_000);
  const st = await fake.appStats(page);
  s = await rs(page);
  expect(st.framesEvicted, 'the ring wrapped').toBeGreaterThan(30_000);
  const want = [0x120, 0x121, 0x130, ...Array.from({ length: 16 }, (_, i) => 0x100 + i), 0x18ff0001].sort((a, b) => a - b);
  expect(s.ids.map((x) => x.id).sort((a, b) => a - b), 'every ID seen').toEqual(want);
});
