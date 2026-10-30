// RE Tools > Capture Bisector (stage 4, file-based RE tools): splits the trace by frame number, percentage, ID range or
// bus to narrow down where a frame is. A log of known content: 10 000 frames over 100 s, 16 classic IDs (0x103 on bus
// 1), and 0x7E8 as 64-byte CAN FD frames (BRS) only from 62 to 63 s. Narrowed down as a user would: halves, Replace main
// list with split frames, a percentage, the ID range; each split checked against the frames (count, IDs, times), the
// split saved (a download of SavvyCAN's GVRET CSV) and read back. Pictures: results/bisect-{light,dark}.png.
import { join } from 'node:path';
import { test, expect, fake, RESULTS } from '../lib/harness.mjs';
import { writeCsv, savedText, canId, waitOpen } from '../lib/retools.mjs';

const N = 10_000, T0 = 1_000_000, STEP = 10_000;          // µs: 100 s
const hidden = (i) => i >= 6200 && i < 6300 && i % 2 === 0; // 0x7E8 only from 62 to 63 s
function frame(i) {
  if (hidden(i)) {
    return { time: T0 + i * STEP, id: 0x7e8, fd: true, brs: true,
      data: Array.from({ length: 64 }, (_, j) => (j === 0 ? i & 0xff : j === 63 ? 0xa5 : j)) };
  }
  const k = i % 16;
  return { time: T0 + i * STEP, id: 0x100 + k, bus: k === 3 ? 1 : 0, data: [i & 0xff, (i >> 8) & 0xff, k, 0, 0, 0, 0, 0] };
}
const FRAMES = Array.from({ length: N }, (_, i) => frame(i));

const state = (page) => page.evaluate(() => window.webSavvyCAN.bisect());
/** What a split of `frames` (rows of the main list) should hold. */
function want(frames) {
  const ids = {};
  for (const f of frames) ids[f.id] = (ids[f.id] || 0) + 1;
  return { count: frames.length, ids, first: frames.length ? frames[0].time : 0, last: frames.length ? frames.at(-1).time : 0 };
}
async function split(app, page, set, frames, what) {
  expect(await page.evaluate((s) => window.webSavvyCAN.bisectSet(s), set), `${what}: set`).toBe(true);
  await app.click('button', 'Calculate Split');
  const w = want(frames);
  // done when the window's label shows it too (the page runs on while a split goes through the trace)
  await expect.poll(async () => { const x = await state(page); return [x.splitCount, x.splitLabel]; },
    { message: `${what}: split frames` }).toEqual([w.count, String(w.count)]);
  const s = await state(page);
  expect(s.splitIds, `${what}: frames per ID`).toEqual(Object.fromEntries(Object.entries(w.ids).map(([k, v]) => [String(k), v])));
  expect([s.splitFirstUs, s.splitLastUs], `${what}: first and last time`).toEqual([w.first, w.last]);
  expect(s.splitLabel, `${what}: "Saved Frames after split"`).toBe(String(w.count));
  return s;
}

test('narrows a capture down to the 1 s where a CAN FD frame appears: halves, Replace, percentage, ID range, bus, save',
  async ({ app, page }) => {
    test.setTimeout(180_000);
    const { file } = writeCsv('bisect.csv', FRAMES);
    await app.open();
    await app.loadFile(file, N);
    await app.menu('RE Tools', 'Capture Bisector');
    await waitOpen(page, 'bisect');
    let s = await state(page);
    expect(s.mainLabel, 'Number of Frames in Main List').toBe(String(N));
    expect(s.ids, 'every ID in the ID lists, in order').toEqual([...Array.from({ length: 16 }, (_, k) => canId(0x100 + k)), canId(0x7e8)]);

    // halves: the lower half hasn't got 0x7E8, the upper has all of it
    await split(app, page, { mode: 'frame', frame: N / 2, lower: true }, FRAMES.slice(0, N / 2), 'up to frame 5000');
    await split(app, page, { mode: 'frame', frame: N / 2, lower: false }, FRAMES.slice(N / 2), 'after frame 5000');
    // Replace main list with split frames: the trace is the upper half now
    await app.click('button', 'Replace main list with split frames');
    await expect.poll(async () => (await fake.appStats(page)).framesInTrace, { message: 'the trace holds the split' }).toBe(N / 2);
    await expect.poll(async () => (await state(page)).mainLabel).toBe(String(N / 2));
    const main = FRAMES.slice(N / 2);
    const tail = await page.evaluate(() => window.webSavvyCAN.traceTail(1));
    expect(tail[0].time, 'the trace ends with the capture\'s last frame').toBe(main.at(-1).time);

    // a quarter of what is left: 0x7E8's first half second
    await split(app, page, { mode: 'percent', percent: 25, lower: true }, main.slice(0, 1250), 'up to 25 %');
    s = await split(app, page, { mode: 'percent', percent: 25, lower: false }, main.slice(1250), 'after 25 %');
    // the ID range 0x7E8 .. 0x7E8: the 50 hidden frames, 62.00 .. 62.98 s
    const fd = main.filter((f) => f.id === 0x7e8);
    s = await split(app, page, { mode: 'id', lowId: canId(0x7e8), highId: canId(0x7e8), lower: true }, fd, 'ID 0x7E8 only');
    expect(fd.length).toBe(50);
    expect(s.splitFirstUs).toBe(T0 + 6200 * STEP);
    expect(s.splitLastUs).toBe(T0 + 6298 * STEP);
    await split(app, page, { mode: 'id', lowId: canId(0x100), highId: canId(0x107), lower: false }, main.filter((f) => f.id > 0x107),
      'outside 0x100 .. 0x107');
    await split(app, page, { mode: 'bus', bus: 1, lower: true }, main.filter((f) => (f.bus || 0) === 1), 'only bus 1');
    await split(app, page, { mode: 'bus', bus: 1, lower: false }, main.filter((f) => (f.bus || 0) !== 1), 'not bus 1');

    // the 0x7E8 split saved: SavvyCAN's GVRET CSV with the 64-byte FD rows as they were
    await split(app, page, { mode: 'id', lowId: canId(0x7e8), highId: canId(0x7e8), lower: true }, fd, 'ID 0x7E8 again');
    const saved = await savedText(app, page, 'Save split frames to a new file');
    expect(saved, 'the split came out as a file').toBeTruthy();
    expect(saved.name).toMatch(/\.csv$/);
    const rows = saved.text.trim().split('\n');
    expect(rows[0]).toMatch(/^Time Stamp,ID,Extended,Dir,Bus,LEN,D1/);
    expect(rows.length - 1, 'a row per split frame').toBe(50);
    for (let r = 0; r < fd.length; r++) {
      const c = rows[r + 1].split(',');
      const f = fd[r];
      expect([Number(c[0]), parseInt(c[1], 16), Number(c[5])], `row ${r}: time, ID, length`).toEqual([f.time, 0x7e8, 64]);
      expect(c.slice(6, 70).map((x) => parseInt(x, 16)), `row ${r}: all 64 bytes`).toEqual(f.data);
      expect(c[70]).toBe('FD+BRS');
    }

    await page.waitForTimeout(500);
    await page.screenshot({ path: join(RESULTS, 'bisect-light.png') });
    await app.setTheme('dark');
    await page.waitForTimeout(800);
    await page.screenshot({ path: join(RESULTS, 'bisect-dark.png') });
    await app.setTheme('light');
  });
