// RE Tools > Frame Search (stage 4, file-based RE tools): finds the frames whose bits hold a value (decimal, 0x hex or
// 0b binary bit patterns), that change, or that match an ID / length / direction, and steps through them in the trace.
// A log of known content with classic and CAN FD frames: a value at byte 40 of 64-byte frames, a Motorola 12-bit value
// ending at bit 511 (byte 63), a byte that changes every 10 frames of its ID, short frames that don't hold the signal
// (SavvyCAN read them as 0), Tx frames. Every result is checked row for row against the frames; Next / Previous select
// the rows in the trace. Pictures: results/search-{light,dark}.png.
import { join } from 'node:path';
import { test, expect, RESULTS } from '../lib/harness.mjs';
import { writeCsv, waitOpen } from '../lib/retools.mjs';

const N = 5000, T0 = 1_000_000;
// 5 kinds in turn: 0x100 (bytes 6-7 = the frame number, Intel), 0x7E8 (FD 64: byte 40 = m % 16, byte 50 = m / 10 % 3,
// bytes 62-63 Motorola 12 bits = m, m = the frame's turn), 0x18DAF110 (29-bit FD 12, Tx), 0x200 (4 bytes), 0x101 (zeros)
function frame(i) {
  const t = T0 + i * 1000, m = Math.floor(i / 5);
  switch (i % 5) {
    case 0: return { time: t, id: 0x100, data: [1, 1, 1, 1, 1, 1, i & 0xff, (i >> 8) & 0xff] };
    case 1: {
      const d = Array.from({ length: 64 }, (_, j) => j);
      d[40] = m % 16;
      d[50] = Math.floor(m / 10) % 3;
      d[62] = (m >> 8) & 0x0f;
      d[63] = m & 0xff;
      return { time: t, id: 0x7e8, fd: true, brs: true, data: d };
    }
    case 2: return { time: t, id: 0x18daf110, ext: true, fd: true, tx: true, data: Array(12).fill(0x42) };
    case 3: return { time: t, id: 0x200, data: [0, 0, 0, 0] };
    default: return { time: t, id: 0x101, data: [0, 0, 0, 0, 0, 0, 0, 0] };
  }
}
const FRAMES = Array.from({ length: N }, (_, i) => frame(i));
const rowsWhere = (fn) => FRAMES.map((f, i) => (fn(f, i) ? i : -1)).filter((i) => i >= 0);
const state = (page) => page.evaluate(() => window.webSavvyCAN.search());

async function search(app, page, set, want, what) {
  expect(await page.evaluate((s) => window.webSavvyCAN.searchSet(s), set), `${what}: set`).toBe(true);
  await page.evaluate(() => { window.__searched = window.webSavvyCAN.search().searchMs; });
  await app.click('button', 'Search');
  // done when the label says so (the page runs on while a search goes through a big trace)
  await expect.poll(async () => (await state(page)).label, { message: `${what}: matches` })
    .toBe(want.length ? `Match 1 of ${want.length}` : 'No matches found.');
  const s = await state(page);
  expect(s.rows, `${what}: the matching rows`).toEqual(want.slice(0, 5000));
  if (want.length) {
    expect(s.label).toBe(`Match 1 of ${want.length}`);
    expect(s.selectedRow, `${what}: the first match selected in the trace`).toBe(want[0]);
  }
  return s;
}

test('values, bit patterns and changes in classic and 64-byte CAN FD frames, row for row; Next / Previous in the trace',
  async ({ app, page }) => {
    test.setTimeout(120_000);
    const { file } = writeCsv('search.csv', FRAMES);
    await app.open();
    await app.loadFile(file, N);
    await app.menu('RE Tools', 'Frame Search');
    await waitOpen(page, 'search');

    // byte 40 of 0x7E8 = 0x0A (hex), the bit grid as large as the frame (64 bytes)
    const b40 = rowsWhere((f) => f.id === 0x7e8 && f.data[40] === 0x0a);
    let s = await search(app, page, { id: '0x7E8', len: null, dir: 'both', mode: 'specific', startBit: 320, bits: 8,
      intel: true, signed: false, scale: 1, bias: 0, value: '0x0A' }, b40, 'byte 40 = 0x0A');
    expect(b40.length).toBe(62);   // m % 16 = 10 for m < 1000
    expect(s.gridBytes, 'the bit grid covers the 64-byte frames').toBe(64);
    await page.waitForTimeout(300);
    await page.screenshot({ path: join(RESULTS, 'search-light.png') });
    await app.setTheme('dark');
    await page.waitForTimeout(800);
    await page.screenshot({ path: join(RESULTS, 'search-dark.png') });
    await app.setTheme('light');

    await app.click('button', 'Next >');
    await app.click('button', 'Next >');
    await expect.poll(async () => (await state(page)).selectedRow, { message: 'Next twice' }).toBe(b40[2]);
    expect((await state(page)).label).toBe(`Match 3 of ${b40.length}`);
    await app.click('button', '< Previous');
    await expect.poll(async () => (await state(page)).selectedRow, { message: 'Previous' }).toBe(b40[1]);

    // a Motorola 12-bit pattern ending at bit 511 (start bit 499: byte 62 bits 3..0, then byte 63), typed in binary
    const moto = rowsWhere((f) => f.id === 0x7e8 && ((f.data[62] << 8) | f.data[63]) === 0x2bc);
    expect(moto.length).toBe(1);
    await search(app, page, { id: null, startBit: 499, bits: 12, intel: false, value: '0b001010111100' }, moto, 'Motorola 0x2BC at the end of FD frames');

    // bytes 6-7 = 0 (decimal), any ID: the 0x101 frames and 0x100's first, not the 4-byte 0x200 frames
    const zero = rowsWhere((f) => f.data.length >= 8 && f.data[6] === 0 && f.data[7] === 0);
    expect(zero.length).toBe(1001);
    await search(app, page, { id: null, startBit: 48, bits: 16, intel: true, value: '0' }, zero, 'bytes 6-7 = 0');
    // scale and bias: 0x100's frame number * 0.5 + 10 = 1010 at frame 2000
    await search(app, page, { id: '0x100', startBit: 48, bits: 16, scale: 0.5, bias: 10, value: '1010' }, [2000], 'scaled value');

    // any change of byte 50 of 0x7E8 (every 10 of its frames), per ID
    const changes = rowsWhere((f, i) => f.id === 0x7e8 && (i === 1 || f.data[50] !== FRAMES[i - 5].data[50]));
    await search(app, page, { id: '0x7E8', mode: 'any', startBit: 400, bits: 8, scale: 1, bias: 0 }, changes, 'byte 50 changes');
    expect(changes.length).toBe(100);

    // no value: Tx 12-byte frames; 4-byte frames
    await search(app, page, { id: null, len: 12, dir: 'tx', mode: 'none' }, rowsWhere((f) => f.tx), 'Tx, 12 bytes');
    await search(app, page, { id: null, len: 4, dir: 'both', mode: 'none' }, rowsWhere((f) => f.data.length === 4), '4 bytes');

    // a value that is not a number
    await page.evaluate(() => window.webSavvyCAN.searchSet({ len: null, mode: 'specific', value: 'zz' }));
    await app.click('button', 'Search');
    await expect.poll(async () => (await state(page)).label).toMatch(/not a number/);
    expect((await state(page)).count).toBe(0);
  });
