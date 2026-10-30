// RE Tools > File Comparison (stage 4, file-based RE tools): compares a log (side 1) with one or more others (side 2),
// each loaded with the window's own buttons through the browser's file picker. Two GVRET CSVs that differ in known IDs,
// bytes and bits (CAN FD: byte 40 and bit 511 of 64-byte frames), then a candump log added to side 2: the tree's IDs
// on one side only, per shared ID the bits and the values per byte found on one side only, "Show only data unique to
// Side 1", Save Details to File (a download), Clear Frames. Pictures: results/filecompare-{light,dark}.png.
import { join } from 'node:path';
import { test, expect, RESULTS } from '../lib/harness.mjs';
import { writeCsv, writeText, nextPick, savedText, node, kids, hexNum, waitOpen } from '../lib/retools.mjs';

const N = 2000, T0 = 1_000_000;
const fd64 = (b40, b63) => Array.from({ length: 64 }, (_, j) => (j === 40 ? b40 : j === 63 ? b63 : j));
// side 1: 0x100 (as side 2), 0x101 (byte 2 0x10 / 0x20), 0x7E8 (FD 64: byte 40 0xA5 every 4th, byte 63 0x80),
// 0x18DAF110 (29-bit FD 12, only here)
const sideA = (i) => {
  const t = T0 + i * 1000;
  switch (i % 4) {
    case 0: return { time: t, id: 0x100, data: [1, 2, 3, 4, 5, 6, 7, 8] };
    case 1: return { time: t, id: 0x101, data: [0, 0, (i >> 2) % 2 ? 0x20 : 0x10, 0, 0, 0, 0, 0] };
    case 2: return { time: t, id: 0x7e8, fd: true, brs: true, data: fd64((i >> 2) % 4 === 0 ? 0xa5 : 0, 0x80) };
    default: return { time: t, id: 0x18daf110, ext: true, fd: true, data: Array(12).fill(0x42) };
  }
};
// side 2: 0x100, 0x101 (byte 2 0x10 only), 0x7E8 (byte 40 0, byte 63 0), 0x200 (only here)
const sideB = (i) => {
  const t = T0 + i * 1000;
  switch (i % 4) {
    case 0: return { time: t, id: 0x100, data: [1, 2, 3, 4, 5, 6, 7, 8] };
    case 1: return { time: t, id: 0x101, data: [0, 0, 0x10, 0, 0, 0, 0, 0] };
    case 2: return { time: t, id: 0x7e8, fd: true, brs: true, data: fd64(0, 0) };
    default: return { time: t, id: 0x200, data: [9, 9, 9, 9] };
  }
};
const state = (page) => page.evaluate(() => window.webSavvyCAN.fileCompare());

test('two logs that differ in known IDs, bytes and bits (CAN FD 64), a third added to side 2, unique to side 1, save, clear',
  async ({ app, page }) => {
    test.setTimeout(120_000);
    const a = writeCsv('filecompare-side1.csv', sideA, N).file;
    const b = writeCsv('filecompare-side2.csv', sideB, N).file;
    // candump: 0x101 with byte 2 0x30 and 0x300, 100 frames each (SavvyCAN's candump loader, by autodetect)
    const c = writeText('filecompare-side2b.log', Array.from({ length: 200 }, (_, i) =>
      `(${(10 + i / 1000).toFixed(6)}) can0 ${i % 2 ? '300#AA' : '101#0000300000000000'}`).join('\n') + '\n');
    await app.open();
    await app.menu('RE Tools', 'File Comparison');
    await waitOpen(page, 'fileCompare');

    await nextPick(page, a);
    await app.click('button', 'Load New File');
    await expect.poll(async () => (await state(page)).side1.frames, { message: 'side 1 loaded' }).toBe(N);
    let s = await state(page);
    expect(s.interestedName).toBe('filecompare-side1.csv');
    expect(s.side1.ids).toBe(4);
    expect(s.tree, 'nothing to compare yet').toEqual([]);

    await nextPick(page, b);
    await app.click('button', 'Load A File');
    await expect.poll(async () => (await state(page)).side2.frames, { message: 'side 2 loaded' }).toBe(N);
    s = await state(page);
    expect(s.refLabel).toBe(`Loaded frames: ${N}`);
    expect(s.tree.map((n) => n.t)).toEqual(['IDs found only in filecompare-side1.csv',
      'IDs found only in Side 2 - Reference frames', 'IDs found on both sides']);
    expect(kids(node(s.tree, 'IDs found only in filecompare-side1')), 'only in side 1').toEqual([hexNum(0x18daf110)]);
    expect(kids(node(s.tree, 'IDs found only in Side 2')), 'only in side 2').toEqual([hexNum(0x200)]);
    const both = node(s.tree, 'IDs found on both sides');
    expect(kids(both), 'on both sides').toEqual([hexNum(0x100), hexNum(0x101), hexNum(0x7e8)]);
    // 0x7E8, 64-byte FD: byte 40's 0xA5 (bits 320, 322, 325, 327) and byte 63's bit 7 (511) only in side 1
    const fd = ['IDs found on both sides', hexNum(0x7e8)];
    expect(kids(node(s.tree, ...fd, 'Bits set only in filecompare')))
      .toEqual(['320 (40:0)', '322 (40:2)', '325 (40:5)', '327 (40:7)', '511 (63:7)']);
    expect(kids(node(s.tree, ...fd, 'Bits set only in Side 2'))).toEqual([]);
    expect(kids(node(s.tree, ...fd)).filter((t) => t.startsWith('Byte ')).length, 'a node per byte: 64').toBe(64);
    expect(kids(node(s.tree, ...fd, 'Byte 40', 'Values found only in filecompare'))).toEqual(['0xA5']);
    expect(kids(node(s.tree, ...fd, 'Byte 40', 'Values found only in Side 2'))).toEqual([]);
    expect(kids(node(s.tree, ...fd, 'Byte 63', 'Values found only in filecompare'))).toEqual(['0x80']);
    expect(kids(node(s.tree, ...fd, 'Byte 63', 'Values found only in Side 2'))).toEqual(['0x00']);
    for (const byte of [0, 8, 39, 41, 62])
      expect(kids(node(s.tree, ...fd, `Byte ${byte}`, 'Values found only in filecompare')), `byte ${byte}: the same`).toEqual([]);
    // 0x101: byte 2 0x20 (bit 21) only in side 1; 0x100 the same on both sides
    expect(kids(node(s.tree, 'IDs found on both sides', hexNum(0x101), 'Bits set only in filecompare'))).toEqual(['21 (2:5)']);
    expect(kids(node(s.tree, 'IDs found on both sides', hexNum(0x101), 'Byte 2', 'Values found only in filecompare'))).toEqual(['0x20']);
    expect(kids(node(s.tree, 'IDs found on both sides', hexNum(0x100), 'Bits set only in filecompare'))).toEqual([]);
    // bounded: the summaries, not the frames (~2 KB an ID)
    expect(s.side1.memoryBytes + s.side2.memoryBytes, 'summaries').toBeLessThan(100_000);

    // a candump log added to side 2: 0x101 byte 2 also 0x30 there, 0x300 only there
    await nextPick(page, c);
    await app.click('button', 'Load A File');
    await expect.poll(async () => (await state(page)).side2.frames, { message: 'side 2 takes a second file' }).toBe(N + 200);
    s = await state(page);
    expect(kids(node(s.tree, 'IDs found only in Side 2'))).toEqual([hexNum(0x200), hexNum(0x300)]);
    const id101 = ['IDs found on both sides', hexNum(0x101)];
    expect(kids(node(s.tree, ...id101, 'Bits set only in filecompare')), '0x10 | 0x30 covers 0x20\'s bit').toEqual([]);
    expect(kids(node(s.tree, ...id101, 'Byte 2', 'Values found only in filecompare'))).toEqual(['0x20']);
    expect(kids(node(s.tree, ...id101, 'Byte 2', 'Values found only in Side 2'))).toEqual(['0x30']);

    await page.waitForTimeout(500);
    await page.screenshot({ path: join(RESULTS, 'filecompare-light.png') });
    await app.setTheme('dark');
    await page.waitForTimeout(800);
    await page.screenshot({ path: join(RESULTS, 'filecompare-dark.png') });
    await app.setTheme('light');

    // Save Details to File: the tree as indented text
    const saved = await savedText(app, page, 'Save Details to File');
    expect(saved, 'the details came out').toBeTruthy();
    const lines = saved.text.split('\n');
    expect(lines[0]).toBe('IDs found only in filecompare-side1.csv');
    expect(lines).toContain('   ' + hexNum(0x18daf110));
    expect(lines).toContain('         511 (63:7)');

    // "Show only data unique to Side 1" (applies at the next comparison, as in SavvyCAN): 0x100 has nothing unique
    await app.click('checkbox', 'Show only data unique to Side 1');
    await nextPick(page, a);
    await app.click('button', 'Load New File');
    await expect.poll(async () => (await state(page)).tree.length, { message: 'compared again, unique to side 1' }).toBe(2);
    s = await state(page);
    expect(kids(node(s.tree, 'IDs found on both sides'))).toEqual([hexNum(0x101), hexNum(0x7e8)]);
    expect(node(s.tree, ...fd, 'Bits set only in Side 2'), 'no side 2 lists').toBeUndefined();

    // (the main window has a "Clear Frames" button too: the comparator's is the later one)
    await page.getByRole('button', { name: /^Clear Frames$/ }).last().click({ force: true });
    await expect.poll(async () => (await state(page)).side2.frames).toBe(0);
    s = await state(page);
    expect(s.tree).toEqual([]);
    expect(s.refLabel).toBe('Loaded frames: 0');
    expect(s.side1.frames, 'side 1 kept').toBe(N);
  });
