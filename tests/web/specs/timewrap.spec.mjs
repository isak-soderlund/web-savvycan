// Logs longer than 71.6 minutes (RESOLVE.md): GVRET sends a frame's time as 32-bit µs, which wraps 2^32 µs after
// the device's clock started. The app unwraps it (GVRetSerial), so times keep increasing across the wrap in the
// trace, the continuous log and a graph; the GVRET protocol and rc1 stay as they are. The fake rc1's clock is set
// 30 s before the wrap, before connecting (its time sync answer then comes from just before the wrap too).
// Also: a GVRET CSV whose times start at "0" loads with exactly its times (upstream made up times shorter than
// 4 characters).
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { test, expect, fake, settle, RESULTS, AppDriver } from '../lib/harness.mjs';

const WRAP = 2 ** 32;
const increasing = (ts) => ts.findIndex((t, i) => i > 0 && t < ts[i - 1]);

test.describe('the 32-bit GVRET time wraps', () => {
  test.use({ fakeOptions: { serial: { device: { record: true } } } });

  test('times keep increasing across the wrap: trace, continuous log and graph', async ({ app, page }) => {
    test.setTimeout(120000);
    await app.open();
    await page.evaluate((us) => window.__rc1.setClock(us), WRAP - 30e6);
    await app.connect({ protocol: 'gvret', fd: false });
    const clockAtStart = await page.evaluate(() => window.__rc1.clockUs());
    expect(WRAP - clockAtStart, 'connected at least 6 s before the wrap (else the test would not cross it)').toBeGreaterThan(6e6);
    expect(WRAP - clockAtStart, 'and not past it').toBeLessThan(30e6);
    await app.startLog();
    await fake.setTraffic(page, 'leaf');
    await page.waitForTimeout(1000);
    expect(await page.evaluate(() => window.webSavvyCAN.addGraph(0x1da, 0, 8, true)), 'graph created').toBe(true);
    await page.waitForTimeout(1000);
    const graphBefore = await page.evaluate(() => window.webSavvyCAN.graphState());
    // Until 3 s past the wrap.
    await page.waitForFunction((w) => window.__rc1.clockUs() < w / 2 && window.__rc1.clockUs() > 3e6, WRAP, { timeout: 40000, polling: 200 });
    await fake.setTraffic(page, 'idle');
    await settle(page);
    await page.waitForTimeout(500);

    const sent = await fake.recorded(page);
    const raw = sent.map((f) => f.tsUs);
    expect(raw[0], 'frames before the wrap').toBeGreaterThan(WRAP / 2);
    expect(raw[raw.length - 1], 'frames after the wrap').toBeLessThan(WRAP / 2);
    // What the device's times are, unwrapped (one wrap here), relative to the first frame.
    const deviceSpan = raw.map((t) => t + (t < raw[0] - WRAP / 2 ? WRAP : 0) - raw[0]);

    // The trace: every frame, in increasing time, spaced as the device stamped them.
    const a = await fake.appStats(page);
    expect(a.framesReceived).toBe(sent.length);
    expect(a.framesDropped).toBe(0);
    const tail = await page.evaluate((n) => window.webSavvyCAN.traceTail(n), sent.length);
    expect(tail.length).toBe(sent.length);
    expect(tail.map((f) => f.id)).toEqual(sent.map((f) => f.id));
    const traceTs = tail.map((f) => f.time);
    expect(increasing(traceTs), 'first trace frame whose time goes back (-1: none)').toBe(-1);
    expect(traceTs.map((t) => t - traceTs[0]), 'trace times spaced as the device stamped them').toEqual(deviceSpan);
    expect(traceTs[0], `trace times from the app's start, not 2^32 µs off (${traceTs[0]})`).toBeLessThan(600e6);

    // The graph: its newest point is past the wrap, not stuck at the last time before it.
    const graphAfter = await page.evaluate(() => window.webSavvyCAN.graphState());
    expect(graphAfter.points).toBeGreaterThan(graphBefore.points + 300);
    expect(graphAfter.lastKey - graphBefore.lastKey, 'graph keeps going past the wrap (µs)').toBeGreaterThan(5e6);
    expect(Math.abs(graphAfter.xUpper - graphAfter.lastKey), 'and follows its end').toBeLessThan(1000);

    // The continuous log: every frame, times increasing and as in the trace.
    const dl = await app.stopLog();
    const file = join(RESULTS, 'log-timewrap.csv');
    await dl.saveAs(file);
    const parsed = await page.evaluate((t) => RC1Fake.parseSavvyCsv(t), readFileSync(file, 'latin1'));
    expect(parsed.frames.length).toBe(sent.length);
    const logTs = parsed.frames.map((f) => f.ts);
    expect(increasing(logTs), 'first log row whose time goes back (-1: none)').toBe(-1);
    expect(logTs, 'log times = trace times').toEqual(traceTs);
    test.info().annotations.push({ type: 'result', description:
      `${sent.length} frames, device clock ${raw[0]} -> ${raw[raw.length - 1]} µs, log ${logTs[0]} -> ${logTs[logTs.length - 1]} µs` });
  });
});

test('a log loads with its exact times, also "0".."999" (its first millisecond)', async ({ page, consoleErrors }) => {
  const times = [0, 523, 999, 1000, 1001, 20000, 4294967296 + 17];
  const rows = ['Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8'];
  times.forEach((t, i) => rows.push(`${t},000001DA,false,Rx,0,8,${i.toString(16).padStart(2, '0')},00,00,00,00,00,00,00,`));
  mkdirSync(RESULTS, { recursive: true });
  const file = join(RESULTS, 'load-short-times.csv');
  writeFileSync(file, rows.join('\n') + '\n');

  await page.goto('/SavvyCAN.html');
  const app = new AppDriver(page);
  await app.enableA11y();
  await app.loadFile(file, times.length);
  const tail = await page.evaluate((n) => window.webSavvyCAN.traceTail(n), times.length);
  expect(tail.map((f) => f.data[0])).toEqual(times.map((_, i) => i));
  expect(tail.map((f) => f.time)).toEqual(times);
  expect(consoleErrors, 'console errors').toEqual([]);
});
