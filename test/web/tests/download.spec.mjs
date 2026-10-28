// The MVP's browser logging (LOG_TO_DISK_LIVE false, owner 2026-10-28): SavvyDiskLog's "blob" mode.
// Rows are collected in the page as Blobs; on stop one normal download (<a download>) of the CSV starts.
// Runs in Chromium and in Firefox (projects "chromium" and "firefox").
import { test, expect } from '@playwright/test';
import fs from 'node:fs';
import path from 'node:path';
import { HARNESS, HEADER, ROWS, ROWS_COUNT, loadRows, bytesFor, savvycanCheck, OUT, note } from './common.mjs';

const NAME_RE = /^SavvyCAN_\d{4}-\d\d-\d\d_\d\d-\d\d-\d\d\.csv$/;

test('support: blob logging in this browser', async ({ page }) => {
  await page.goto(HARNESS);
  const s = await page.evaluate(() => SavvyDiskLog.support());
  expect(s.blob).toBe(true);
});

test('blob: start, rows in batches, stop -> one download named by the start time; equals the rows; loads in SavvyCAN', async ({ page, browserName }) => {
  await page.goto(HARNESS);
  await loadRows(page);
  const dl = page.waitForEvent('download');
  const t0 = Date.now();
  const res = await page.evaluate(async ({ header, count }) => {
    events.length = 0;
    const started = await SavvyDiskLog.start({ mode: 'blob', header });
    // in pieces over a few seconds, as a log runs (past a commit tick, which folds the batches into a Blob)
    for (let i = 1; i <= 8; i++) { feedBatches(Math.round(count * i / 8)); await new Promise((r) => setTimeout(r, 500)); }
    const during = SavvyDiskLog.status();
    const stopped = await SavvyDiskLog.stop();
    return { started, during, stopped, events: events.map((e) => e.type), status: SavvyDiskLog.status(),
             last: SavvyDiskLog.lastDownload() };
  }, { header: HEADER, count: ROWS_COUNT });
  const d = await dl;
  const secs = (Date.now() - t0) / 1000;
  expect(res.started).toMatchObject({ backend: 'blob', crashSafe: false });
  expect(res.started.name).toMatch(NAME_RE);
  expect(res.during.state).toBe('open');
  expect(res.events[0]).toBe('started');
  expect(res.events).toContain('committed');
  expect(res.events.at(-1)).toBe('stopped');
  expect(res.status.state).toBe('idle');
  expect(d.suggestedFilename()).toBe(res.started.name);
  const file = path.join(OUT, `blob-${browserName}.csv`);
  await d.saveAs(file);
  const want = await bytesFor(page, ROWS_COUNT);
  expect(fs.statSync(file).size).toBe(want);
  expect(res.stopped.bytes).toBe(want);
  expect(res.last).toEqual({ name: res.started.name, bytes: want });
  // byte for byte: the header and the reference rows (made by SavvyCAN's own nativeCSVRow)
  const got = fs.readFileSync(file);
  expect(got.equals(Buffer.concat([Buffer.from(HEADER), fs.readFileSync(ROWS)]))).toBe(true);
  note(`${d.suggestedFilename()}: ${want} B, ${ROWS_COUNT} rows identical to the reference; `
       + `${savvycanCheck(file, '--exact', String(ROWS_COUNT))} (${secs.toFixed(1)} s)`);
});

test('blob: the name chosen at stop (Choose name...) is the download\'s; unsafe characters replaced', async ({ page }) => {
  await page.goto(HARNESS);
  const dl = page.waitForEvent('download');
  await page.evaluate(async (header) => {
    await SavvyDiskLog.start({ mode: 'blob', header });
    SavvyDiskLog.write('1000,000007DF,false,Rx,0,8,02,01,0C,00,00,00,00,00,\n');
    await SavvyDiskLog.stop({ name: 'leaf drive/1: test' });
  }, HEADER);
  const d = await dl;
  expect(d.suggestedFilename()).toBe('leaf drive_1_ test.csv');
  const file = path.join(OUT, 'blob-named.csv');
  await d.saveAs(file);
  expect(fs.readFileSync(file, 'latin1')).toBe(HEADER + '1000,000007DF,false,Rx,0,8,02,01,0C,00,00,00,00,00,\n');
});

test('blob: an empty log (no frames) downloads the header only; a second log after it starts fresh', async ({ page }) => {
  await page.goto(HARNESS);
  const names = [];
  for (let i = 0; i < 2; i++) {
    const dl = page.waitForEvent('download');
    await page.evaluate(async ({ header, i }) => {
      await SavvyDiskLog.start({ mode: 'blob', header });
      if (i) SavvyDiskLog.write('5,00000100,false,Rx,0,1,AA,\n');
      await SavvyDiskLog.stop();
    }, { header: HEADER, i });
    const d = await dl;
    const file = path.join(OUT, `blob-seq${i}.csv`);
    await d.saveAs(file);
    expect(fs.readFileSync(file, 'latin1')).toBe(HEADER + (i ? '5,00000100,false,Rx,0,1,AA,\n' : ''));
    names.push(d.suggestedFilename());
  }
  for (const n of names) expect(n).toMatch(NAME_RE);
});

test('blob: closing the tab while logging asks first (beforeunload); after stop it does not', async ({ page }) => {
  await page.goto(HARNESS);
  await page.evaluate((header) => SavvyDiskLog.start({ mode: 'blob', header }), HEADER);
  // a user gesture first: browsers show the "leave site?" box only after the page had one
  await page.click('#go');
  const dialogs = [];
  page.on('dialog', async (dl) => { dialogs.push(dl.type()); await dl.dismiss(); });
  await page.close({ runBeforeUnload: true });
  await expect.poll(() => dialogs.length, { timeout: 5000 }).toBe(1);
  expect(dialogs[0]).toBe('beforeunload');
  expect(page.isClosed()).toBe(false);
  const dl = page.waitForEvent('download');
  await page.evaluate(() => SavvyDiskLog.stop());
  await dl;
  expect(await page.evaluate(() => SavvyDiskLog.status().state)).toBe('idle');
  await page.close({ runBeforeUnload: true });
  await new Promise((r) => setTimeout(r, 2000));
  expect(dialogs.length).toBe(1);   // no second "leave site?" box once the log is downloaded
});
