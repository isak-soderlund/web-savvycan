// RE Tools > Flow View (stage 4, live RE tools): one ID's frames one by one: playback controls, the timeline, seek
// values and trigger bits (CAN FD bits past byte 7), the byte table, the bit grid and the graphs around the current
// frame, Live Mode, light and dark; bounded and exact across the trace ring's wraps at rc1's full rate.
// Traffic: the fake rc1's 're' patterns (fake/rc1.js; sniffer.spec has the list) and classic-full / fd64-full.
// Pictures: results/flowview-{light,dark}.png; numbers: results/flowview-live.json.
import { join } from 'node:path';
import { test, expect, fake, RESULTS, saveResult } from '../lib/harness.mjs';

const fv = (page) => page.evaluate(() => window.webSavvyCAN.flowView());
const frames = (page, from, n) => page.evaluate(([f, k]) => window.webSavvyCAN.flowViewFrames(f, k), [from, n]);
const act = (page, action, a = 0, b = 0) => page.evaluate(([x, y, z]) => window.webSavvyCAN.flowViewDo(x, y, z), [action, a, b]);
const setState = (page, n) => page.evaluate((x) => window.__rc1.setReState(x), n);
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

/** The trace's frames of one ID (the whole trace: small here). */
async function traceOf(page, id) {
  return page.evaluate((x) => {
    const n = window.webSavvyCAN.stats().framesInTrace;
    return window.webSavvyCAN.traceTail(n).filter((f) => f.id === x);
  }, id);
}

async function waitStopped(page, ms = 20000) {
  await page.waitForFunction(() => !window.webSavvyCAN.flowView().playing, null, { timeout: ms, polling: 50 });
  return fv(page);
}

test('flow view: an ID\'s frames one by one (= the trace), playback, timeline, seek value, trigger bits incl. CAN FD ' +
  'bit 270, graphs around the current frame, the 64-byte layout, Live Mode, light and dark', async ({ app, page }) => {
  test.setTimeout(150_000);
  await app.open();
  await app.connect({ protocol: 'gvret', fd: true });
  // 3 s of the patterns, the state switched on half way.
  await fake.setTraffic(page, 're', { state: 0, seconds: 3 });
  await page.waitForTimeout(1500);
  await setState(page, 1);
  await page.waitForTimeout(1800);
  await quiet(page);

  await app.menu('RE Tools', 'Flow View');
  await page.waitForFunction(() => window.webSavvyCAN.flowView().open, null, { timeout: 10_000 });
  let s = await fv(page);
  expect(s.ids, 'every ID').toEqual([0x120, 0x121, 0x130, 0x18ff0001]);
  expect(s.hasId && s.id, 'the first ID selected as the window opens').toBe(0x120);

  // The cache is exactly the trace's frames of the ID.
  const tr = await traceOf(page, 0x120);
  expect(s.frames, 'every frame of 0x120').toBe(tr.length);
  expect(tr.length).toBeGreaterThan(250);
  const all = await frames(page, 0, s.frames);
  expect(all.map((f) => f.data), 'byte for byte').toEqual(tr.map((f) => f.data));
  expect(all.map((f) => f.time), 'times').toEqual(tr.map((f) => f.time));
  expect([s.tableRows, s.gridBytes, s.plot.graphs], 'a row, the grid and a graph per byte').toEqual([8, 8, 8]);

  // Step through: Stop -> frame 0, forward x3, back, the timeline.
  await act(page, 'stop');
  s = await fv(page);
  expect([s.position, s.curr]).toEqual([0, tr[0].data]);
  for (let i = 0; i < 3; i++) await act(page, 'forward');
  await act(page, 'back');
  s = await fv(page);
  expect([s.position, s.curr], 'forward 3, back 1').toEqual([2, tr[2].data]);
  expect(s.label).toBe(`3 of ${tr.length}`);
  await act(page, 'goto', 100);
  s = await fv(page);
  expect([s.position, s.curr], 'the timeline').toEqual([100, tr[100].data]);
  // Graphs: the current frame +- the chart range (50) on the x axis (Graph by timestamp, on by default: the frames'
  // times in µs; off: frame numbers), the stored points around it.
  expect([s.plot.xLower, s.plot.xUpper]).toEqual(s.cbTimeGraph ? [tr[50].time, tr[150].time] : [50, 150]);
  await act(page, 'byTime', !s.cbTimeGraph);
  const s2 = await fv(page);
  expect([s2.plot.xLower, s2.plot.xUpper], 'the other x axis').toEqual(s2.cbTimeGraph ? [tr[50].time, tr[150].time] : [50, 150]);
  await act(page, 'byTime', s.cbTimeGraph);
  expect(s.graphFrom).toBeLessThanOrEqual(50);
  expect(s.graphTo).toBeGreaterThanOrEqual(150);
  expect(s.plot.points, 'graphs hold only the frames around the current one').toBeLessThanOrEqual(8 * (4 * 50 + 1));

  // Seek value: byte 0 (the counter) = frame 0's + 50: playing from frame 0 stops at frame 50.
  await act(page, 'stop');
  await act(page, 'speed', 1);
  await act(page, 'seek', 0, (tr[0].data[0] + 50) & 0xff);
  await act(page, 'play');
  s = await waitStopped(page);
  expect([s.position, s.curr], 'stopped at the seek value').toEqual([50, tr[50].data]);
  await act(page, 'seek', 0, -1);

  // Trigger bit 19 (byte 2 bit 3, the state): stops at the first frame where it changed.
  const firstOn = tr.findIndex((f) => f.data[2] & 8);
  expect(firstOn).toBeGreaterThan(50);
  await act(page, 'triggerBit', 19);
  await act(page, 'play');
  s = await waitStopped(page);
  expect(s.position, 'stopped where the state bit changed').toBe(firstOn);
  await act(page, 'triggerBit', 19); // off again

  // CAN FD: 0x18FF0001, 64 bytes: rows, grid and graphs for 64 bytes; trigger bit 270 (byte 33 bit 6) stops playback
  // where it changed (the 64-bit chunks past byte 7 were read past the frame).
  await act(page, 'select', 0x18ff0001);
  s = await fv(page);
  const fd = await traceOf(page, 0x18ff0001);
  expect([s.id, s.frames, s.maxLength]).toEqual([0x18ff0001, fd.length, 64]);
  expect([s.tableRows, s.gridBytes, s.plot.graphs], '64 bytes').toEqual([64, 64, 64]);
  expect(s.curr).toEqual(fd[0].data);
  const fdOn = fd.findIndex((f) => f.data[33] & 0x40);
  expect(fdOn).toBeGreaterThan(10);
  await act(page, 'stop');
  await act(page, 'triggerBit', 270);
  await act(page, 'play');
  s = await waitStopped(page);
  expect([s.position, s.curr[33]], 'stopped where FD bit 270 changed').toEqual([fdOn, 0x40]);
  await act(page, 'goto', fdOn + 20);
  await page.waitForTimeout(500);
  await page.screenshot({ path: join(RESULTS, 'flowview-light.png') });
  await app.setTheme('dark');
  await page.waitForTimeout(1000);
  await page.screenshot({ path: join(RESULTS, 'flowview-dark.png') });
  s = await fv(page);
  expect(s.plot.tickLabelColor, 'the plot themed dark').toBe('#d0d0d0');
  await app.setTheme('light');

  // Live Mode: new frames join, the current one is the newest.
  await act(page, 'live', true);
  const before = (await fv(page)).frames;
  await fake.setTraffic(page, 're', { state: 0, seconds: 2 });
  await page.waitForTimeout(2500);
  await quiet(page);
  s = await fv(page);
  const fd2 = await traceOf(page, 0x18ff0001);
  expect(s.frames, 'every new frame joined, none twice').toBe(fd2.length);
  expect(s.frames - before).toBeGreaterThan(150);
  expect(s.position, 'live: the newest frame').toBe(s.frames - 1);
  expect(s.curr).toEqual(fd2[fd2.length - 1].data);
});

test('flow view live at rc1\'s full rate: across the trace ring\'s wraps every frame of the ID once, bounded to its ' +
  '200 000 newest, the page responsive; then 64-byte FD frames of the same ID', async ({ app, page }) => {
  test.setTimeout(180_000);
  await page.addInitScript(() => localStorage.setItem('qt-v0-EVTV-SavvyCAN-Main/MaximumFrames', '50000'));
  await app.open();
  await app.connect({ protocol: 'gvret', bitrate: 1000000, fd: true, dataBitrate: 8000000 });
  await app.menu('RE Tools', 'Flow View');
  await page.waitForFunction(() => window.webSavvyCAN.flowView().open, null, { timeout: 10_000 });
  // One ID at rc1's full classic rate (8 000 frames/s) for 30 s: 240 000 frames, more than the cache keeps.
  await fake.setTraffic(page, 'classic-full', { ids: 1, seconds: 30 });
  await page.waitForFunction(() => window.webSavvyCAN.flowView().ids.length > 0, null, { timeout: 10_000 });
  await act(page, 'select', 0x100);
  await act(page, 'live', true);
  await page.evaluate(() => window.__probe.reset());
  const mem0 = await page.evaluate(() => window.webSavvyCAN.memory());
  await page.waitForTimeout(15_000);
  const mid = await fv(page);
  await page.waitForTimeout(16_000);
  const st = await quiet(page);
  const probe = await fake.probe(page);
  const rc1 = await fake.stats(page);
  const s = await fv(page);
  const mem1 = await page.evaluate(() => window.webSavvyCAN.memory());
  expect(st.framesEvicted, 'the trace ring wrapped').toBeGreaterThan(150_000);
  expect(s.frames, 'bounded').toBeLessThanOrEqual(s.maxFrames);
  expect(s.frames).toBeGreaterThan(s.maxFrames * 0.8);
  expect(s.dropped + s.frames, 'every frame of the ID taken once').toBe(st.framesReceived);
  // the frames kept are consecutive (sequence numbers in bytes 0..3): no gap, none twice
  expect(seqOf(s.lastData) - seqOf(s.firstData) + 1, 'consecutive').toBe(s.frames);
  expect(s.position, 'live: the newest frame').toBe(s.frames - 1);
  expect(seqOf(s.curr)).toBe(rc1.lastSeq);
  expect(s.cacheBytes, `cache ${s.cacheBytes} bytes`).toBeLessThanOrEqual(s.maxFrames * (8 + 9));
  expect(rc1.lost / rc1.bus, 'frames lost at rc1 because the page did not read').toBeLessThan(0.001);
  expect(probe.timer.p95, 'timer lateness p95 (ms)').toBeLessThan(50);
  expect(probe.timer.max, 'longest stall (ms)').toBeLessThan(1000);

  // 64-byte FD frames of the same ID at the full FD rate for 10 s: rows, graphs and cache grow to 64 bytes.
  await fake.setTraffic(page, 'fd64-full', { ids: 1, seconds: 10 });
  await page.waitForTimeout(11_000);
  const st2 = await quiet(page);
  const s2 = await fv(page);
  expect([s2.maxLength, s2.tableRows, s2.gridBytes, s2.plot.graphs]).toEqual([64, 64, 64, 64]);
  expect(s2.dropped + s2.frames, 'still every frame once').toBe(st2.framesReceived);
  expect(s2.frames).toBeLessThanOrEqual(s2.maxFrames);
  expect(s2.lastData.length).toBe(64);
  expect(s2.cacheBytes, `cache ${s2.cacheBytes} bytes`).toBeLessThanOrEqual(s2.maxFrames * (64 + 9));
  const result = { classic: { framesReceived: st.framesReceived, framesEvicted: st.framesEvicted, cacheFrames: s.frames,
    dropped: s.dropped, cacheBytes: s.cacheBytes, midFrames: mid.frames, timer: probe.timer, lost: rc1.lost,
    heapUsedBefore: mem0.heapUsed, heapUsedAfter: mem1.heapUsed },
    fd: { framesReceived: st2.framesReceived, cacheFrames: s2.frames, cacheBytes: s2.cacheBytes } };
  saveResult('flowview-live', result);
  test.info().annotations.push({ type: 'result', description: JSON.stringify(result) });
});
