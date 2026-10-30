// Stage 3 and stage 4 together (merged 2026-10-30): a DBC loaded while the live RE tools run (Sniffer and Flow View in
// Live Mode keep following the bus, the trace decodes the new DBC's messages, the open Signal Viewer lists it), and every
// window of both stages in the dark theme: the pixels each window adds to the screen are dark (mean luminance), as
// the Qt dark palette's, not a light window left over (near-white pixels counted). Pictures: results/combined-*.png (look at them).
import { join } from 'node:path';
import { test, expect, fake, RESULTS } from '../lib/harness.mjs';
import { DBC, NODE, loadDbc, closeWindow, interpret, parseRows, checkRow } from '../lib/dbc.mjs';
import { writeCsv } from '../lib/retools.mjs';

/** The pixels that differ between two screenshots (the window that opened): how many, their mean luminance (0-255) and
 * the share of them near white (a light window's background and fields). */
async function addedLuminance(page, before, after) {
  return page.evaluate(async ([a64, b64]) => {
    const load = async (b) => {
      const img = await createImageBitmap(await (await fetch('data:image/png;base64,' + b)).blob());
      const c = new OffscreenCanvas(img.width, img.height);
      const g = c.getContext('2d');
      g.drawImage(img, 0, 0);
      return g.getImageData(0, 0, img.width, img.height).data;
    };
    const [p, q] = [await load(a64), await load(b64)];
    let s = 0, n = 0, white = 0;
    for (let i = 0; i < q.length; i += 4) {
      if (Math.abs(p[i] - q[i]) + Math.abs(p[i + 1] - q[i + 1]) + Math.abs(p[i + 2] - q[i + 2]) < 24) continue;
      s += 0.2126 * q[i] + 0.7152 * q[i + 1] + 0.0722 * q[i + 2];
      if (q[i] > 225 && q[i + 1] > 225 && q[i + 2] > 225) white++;
      n++;
    }
    return { mean: n ? s / n : 0, pixels: n, white: n ? white / n : 0 };
  }, [before.toString('base64'), after.toString('base64')]);
}

test('a DBC loaded while Sniffer and Flow View (Live Mode) run: both keep following the bus, the trace decodes, ' +
  'Signal Viewer lists the new DBC', async ({ app, page }) => {
  test.setTimeout(150_000);
  await app.open();
  await app.connect({ protocol: 'gvret', fd: true });
  await fake.setTraffic(page, 're', { state: 0 });
  await app.menu('RE Tools', 'Sniffer');
  await page.waitForFunction(() => window.webSavvyCAN.sniffer().open, null, { timeout: 10_000 });
  await app.menu('RE Tools', 'Flow View');
  await page.waitForFunction(() => window.webSavvyCAN.flowView().open, null, { timeout: 10_000 });
  expect(await page.evaluate(() => window.webSavvyCAN.flowViewDo('live', true))).toBe(true);
  await app.menu('RE Tools', 'Signal Viewer');
  await expect.poll(async () => (await page.evaluate(() => window.webSavvyCAN.signalViewer())).open).toBe(true);
  await page.waitForTimeout(1500);
  const sn0 = await page.evaluate(() => window.webSavvyCAN.sniffer());
  const fv0 = await page.evaluate(() => window.webSavvyCAN.flowView());

  // The DBC loaded with both open, then the Leaf traffic (the DBC's IDs) on top of nothing else.
  await loadDbc(app, page);
  await closeWindow(page);
  await fake.setTraffic(page, 'leaf');
  await interpret(app, true);
  await page.waitForTimeout(2500);
  const sn1 = await page.evaluate(() => window.webSavvyCAN.sniffer());
  const fv1 = await page.evaluate(() => window.webSavvyCAN.flowView());
  expect(sn1.framesSeen, 'Sniffer: still counting').toBeGreaterThan(sn0.framesSeen + 1000);
  for (const id of [0x1da, 0x1db, 0x5bc]) expect(sn1.rows.some((r) => r.id === id), `Sniffer: the Leaf's 0x${id.toString(16)}`).toBe(true);
  expect(fv1.ids, 'Flow View: the new IDs listed').toEqual(expect.arrayContaining([0x120, 0x1da, 0x1db]));
  await page.screenshot({ path: join(RESULTS, 'combined-dbc-live.png') });

  // The trace decodes the newest frames as the DBC says
  await fake.setTraffic(page, 'idle');
  await page.waitForTimeout(800);
  const tail = await page.evaluate(() => window.webSavvyCAN.traceTail(300));
  const rows = parseRows(await page.evaluate(() => window.webSavvyCAN.traceDecoded(300)));
  let decoded = 0;
  rows.forEach((row, i) => {
    expect(row.id).toBe(tail[i].id);
    if (DBC.has(tail[i].id)) decoded++;
    checkRow(row, tail[i], `row ${i} 0x${tail[i].id.toString(16)}`);
  });
  expect(decoded).toBeGreaterThan(100);
  expect(fv1.frames, 'Flow View (Live Mode): more frames of its ID').toBeGreaterThan(fv0.frames);

  // Signal Viewer (open since before the load) lists the DBC's nodes, messages and signals
  expect(await page.evaluate(([n]) => window.webSavvyCAN.signalViewerPick(n, 'INV_Motor', 'MotorSpeed'), [NODE('INV')]), 'the new DBC listed').toBe(true);
  expect(await page.evaluate(([n]) => window.webSavvyCAN.signalViewerPick(n, 'TEST_FD64', 'HighBits'), [NODE('TEST')])).toBe(true);
});

test('every window of stages 3 and 4 in the dark theme: what it adds to the screen is dark', async ({ app, page }) => {
  test.setTimeout(240_000);
  // a log with the re patterns' IDs and the DBC's (for the windows that work on the trace)
  const frames = Array.from({ length: 4000 }, (_, i) => {
    const k = Math.floor(i / 4), tri = 1000 + 10 * Math.abs(((k + 200) % 400) - 200), t = 1_000_000 + i * 2500;
    switch (i % 4) {
      case 0: return { time: t, id: 0x120, data: [k & 0xff, 0x5a, 0, 0, 0x11, 0x22, 0x33, 0x44] };
      case 1: return { time: t, id: 0x121, data: [tri & 0xff, tri >> 8, 0, 0, 0, 0, 0, 0] };
      case 2: return { time: t, id: 0x1db, data: [k & 0xff, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80] };
      default: return { time: t, id: 0x70f, fd: true, brs: true, data: Array.from({ length: 64 }, (_, j) => (j + k) & 0xff) };
    }
  });
  const { file } = writeCsv('combined-dark.csv', frames);
  await app.open();
  await app.setTheme('dark');
  await expect.poll(() => page.evaluate(() => window.webSavvyCAN.theme())).toBe('dark');
  await app.loadFile(file, frames.length);
  await loadDbc(app, page);
  await closeWindow(page);
  await interpret(app, true);
  const windows = [
    ['DBC File Manager', () => app.menu('File', 'DBC File Manager')],
    ['DBC editor', async () => {
      expect(await page.evaluate(() => window.webSavvyCAN.dbcSelectFile(0))).toBe(true);
      await app.click('button', /^Edit$/);
    }],
    ['Signal Viewer', () => app.menu('RE Tools', 'Signal Viewer')],
    ['Sniffer', () => app.menu('RE Tools', 'Sniffer')],
    ['Flow View', () => app.menu('RE Tools', 'Flow View')],
    ['Range State', () => app.menu('RE Tools', 'Range State')],
    ['Single/Multi State', () => app.menu('RE Tools', 'Single/Multi State')],
    ['Capture Bisector', () => app.menu('RE Tools', 'Capture Bisector')],
    ['File Comparison', () => app.menu('RE Tools', 'File Comparison')],
    ['DBC Comparison', () => app.menu('RE Tools', 'DBC Comparison')],
    ['Frame Search', () => app.menu('RE Tools', 'Frame Search')],
    ['Frame Data Analysis', () => app.menu('RE Tools', 'Frame Data Analysis')],
    ['Temporal Graph', () => app.menu('RE Tools', 'Temporal Graph')],
  ];
  const results = [];
  for (const [name, open] of windows) {
    const before = await page.screenshot();
    await open();
    await page.waitForTimeout(1500);
    const after = await page.screenshot({ path: join(RESULTS, `combined-dark-${name.replace(/\W+/g, '-').toLowerCase()}.png`) });
    const l = await addedLuminance(page, before, after);
    results.push(`${name}: mean ${l.mean.toFixed(0)}, ${(l.white * 100).toFixed(1)} % white (${l.pixels} px)`);
    expect(l.pixels, `${name}: a window opened`).toBeGreaterThan(20_000);
    // no light background or fields: under 5 % near-white pixels (a light Qt window is mostly white or #f0f0f0); the
    // mean stays below the middle grey (a data plot, e.g. Temporal Graph's heat colours, may be bright)
    expect(l.white, `${name}: near-white pixels ${(l.white * 100).toFixed(1)} %`).toBeLessThan(0.05);
    expect(l.mean, `${name}: mean luminance of its pixels ${l.mean.toFixed(0)}`).toBeLessThan(128);
    if (name === 'DBC editor') { await closeWindow(page); await closeWindow(page); } // the editor and the File Manager
  }
  console.log('dark windows:', results.join('; '));
});
