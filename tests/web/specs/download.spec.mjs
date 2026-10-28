// The logging module (wasm/disklog/disklog.js) on its own page, in Chromium and Firefox (projects "download" and
// "firefox"): rows collected in the page as Blobs, one download on stop (RESOLVE.md MVP #7). The reference rows are
// made by SavvyCAN's own row formatter and the download is loaded with SavvyCAN's own parser (nativecsv_tool, built
// by tests/desktop/run.sh; NATIVECSV_TOOL elsewhere). Skipped without the tool.
import { test, expect } from '@playwright/test';
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { RESULTS, NATIVECSV as TOOL, savvycanCheck } from '../lib/harness.mjs';

const OUT = path.join(RESULTS, 'download');
const ROWS = path.join(OUT, 'rows.csv');
const ROWS_COUNT = 200000;
const PAGE = '/__harness/lib/disklog.html';
const HEADER = 'Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8\n';
const NAME_RE = /^SavvyCAN_\d{4}-\d\d-\d\d_\d\d-\d\d-\d\d\.csv$/;

test.skip(!fs.existsSync(TOOL), `no ${TOOL}: build it with tests/desktop/run.sh (or set NATIVECSV_TOOL)`);

test.beforeAll(() => {
  fs.mkdirSync(OUT, { recursive: true });
  execFileSync(TOOL, ['gen', ROWS, String(ROWS_COUNT)]);
});

async function saved(download, name) {
  const file = path.join(OUT, name);
  await download.saveAs(file);
  return file;
}

test('start, rows in batches, stop: one download named by the start time, equal to the rows; loads in SavvyCAN', async ({ page, browserName }) => {
  await page.goto(PAGE);
  const dl = page.waitForEvent('download');
  const res = await page.evaluate(async (header) => {
    const lines = (await (await fetch('/__harness/results/download/rows.csv')).text()).split(/(?<=\n)/);
    const started = await SavvyDiskLog.start({ header });
    // in batches of 1..97 rows (as frames come from a connection), over a few seconds
    let fed = 0, batch = 0, midBlob = null, midBytes = 0;
    for (let step = 1; step <= 8; step++) {
      const until = Math.round(lines.length * step / 8);
      while (fed < until) {
        const n = Math.min(1 + (batch++ * 13) % 97, until - fed);
        SavvyDiskLog.write(lines.slice(fed, fed + n).join(''));
        fed += n;
      }
      if (step === 4) {
        midBlob = SavvyDiskLog._blob().size;
        midBytes = header.length + lines.slice(0, fed).join('').length;
      }
      await new Promise((r) => setTimeout(r, 300));
    }
    const during = SavvyDiskLog.status();
    const stopped = await SavvyDiskLog.stop();
    return { started, during, stopped, midBlob, midBytes, after: SavvyDiskLog.status() };
  }, HEADER);
  const d = await dl;
  expect(res.started.name).toMatch(NAME_RE);
  expect(res.during.state).toBe('open');
  expect(res.midBlob, '_blob() holds every row written so far').toBe(res.midBytes);
  expect(res.after.state).toBe('idle');
  expect(d.suggestedFilename()).toBe(res.started.name);
  const file = await saved(d, `blob-${browserName}.csv`);
  const want = Buffer.concat([Buffer.from(HEADER), fs.readFileSync(ROWS)]);
  expect(res.stopped.bytes).toBe(want.length);
  expect(fs.readFileSync(file).equals(want), 'byte for byte: the header and the reference rows').toBe(true);
  test.info().annotations.push({ type: 'result', description: savvycanCheck(file, '--exact', String(ROWS_COUNT)) });
});

test('the name chosen at stop is the download\'s; unsafe characters replaced', async ({ page }) => {
  await page.goto(PAGE);
  const row = '1000,000007DF,false,Rx,0,8,02,01,0C,00,00,00,00,00,\n';
  const dl = page.waitForEvent('download');
  await page.evaluate(async ([header, row]) => {
    await SavvyDiskLog.start({ header });
    SavvyDiskLog.write(row);
    await SavvyDiskLog.stop({ name: 'leaf drive/1: test' });
  }, [HEADER, row]);
  const d = await dl;
  expect(d.suggestedFilename()).toBe('leaf drive_1_ test.csv');
  expect(fs.readFileSync(await saved(d, 'blob-named.csv'), 'latin1')).toBe(HEADER + row);
});

test('an empty log downloads the header only; a second log after it starts fresh', async ({ page }) => {
  await page.goto(PAGE);
  const row = '5,00000100,false,Rx,0,1,AA,\n';
  for (let i = 0; i < 2; i++) {
    const dl = page.waitForEvent('download');
    await page.evaluate(async ([header, row]) => {
      await SavvyDiskLog.start({ header });
      if (row) SavvyDiskLog.write(row);
      await SavvyDiskLog.stop();
    }, [HEADER, i ? row : '']);
    const d = await dl;
    expect(d.suggestedFilename()).toMatch(NAME_RE);
    expect(fs.readFileSync(await saved(d, `blob-seq${i}.csv`), 'latin1')).toBe(HEADER + (i ? row : ''));
  }
});

test('closing the tab while logging asks first (beforeunload); after stop it does not', async ({ page }) => {
  await page.goto(PAGE);
  await page.evaluate((header) => SavvyDiskLog.start({ header }), HEADER);
  await page.click('#go'); // browsers show the "leave site?" box only after a user gesture
  const dialogs = [];
  page.on('dialog', async (d) => { dialogs.push(d.type()); await d.dismiss(); });
  await page.close({ runBeforeUnload: true });
  await expect.poll(() => dialogs.length, { timeout: 5000 }).toBe(1);
  expect(dialogs[0]).toBe('beforeunload');
  expect(page.isClosed()).toBe(false);
  const dl = page.waitForEvent('download');
  await page.evaluate(() => SavvyDiskLog.stop());
  await dl;
  await page.close({ runBeforeUnload: true });
  await new Promise((r) => setTimeout(r, 2000));
  expect(dialogs.length).toBe(1);
});
