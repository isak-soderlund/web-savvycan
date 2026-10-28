// SavvyDiskLog in Firefox: no File System Access API, so the streaming download (disklog-sw.js).
import { test, expect } from '@playwright/test';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { execSync } from 'node:child_process';
import { HARNESS, HEADER, loadRows, bytesFor, savvycanCheck, OUT, note } from './common.mjs';

test('support: Firefox falls back to the streaming download', async ({ page }) => {
  await page.goto(HARNESS);
  const s = await page.evaluate(() => SavvyDiskLog.support());
  expect(s).toMatchObject({ fsa: false, download: true, backend: 'download' });
});

test('download: rows streamed while logging, finished on stop; loads in SavvyCAN', async ({ page }) => {
  await page.goto(HARNESS);
  await loadRows(page);
  const dl = page.waitForEvent('download');
  const r = await page.evaluate(async (header) => {
    const s = await SavvyDiskLog.start({ header });   // default mode here: download
    // in pieces over a few seconds, as a log runs (and past one keep-alive ping)
    for (let i = 1; i <= 20; i++) { feedBatches(i * 1000); await new Promise((r) => setTimeout(r, 600)); }
    await SavvyDiskLog.stop();
    return s;
  }, HEADER);
  expect(r).toMatchObject({ backend: 'download', crashSafe: false });
  const d = await dl;
  expect(d.suggestedFilename()).toMatch(/^SavvyCAN_.*\.csv$/);
  const file = path.join(OUT, 'download-firefox.csv');
  await d.saveAs(file);
  expect(fs.statSync(file).size).toBe(await bytesFor(page, 20000));
  note(savvycanCheck(file, '--exact', '20000'));
});

// The UI says a crash or a closed tab loses a download log. What Firefox really does with the stream
// left open: recorded here (seen 2026-10-28, Firefox 142: the download ends as "finished" ~30 s later,
// when the service worker is stopped, holding what had been streamed).
test('download: the tab closed mid-log -> what the download holds', async ({ page }) => {
  await page.goto(HARNESS);
  await loadRows(page);
  const dl = page.waitForEvent('download');
  await page.evaluate(async (header) => {
    await SavvyDiskLog.start({ header });
    feedBatches(5000);
    await new Promise((r) => setTimeout(r, 500));
  }, HEADER);
  const d = await dl;
  const want = await bytesFor(page, 5000);
  await page.close({ runBeforeUnload: false });
  const t0 = Date.now();
  const failure = await d.failure();
  const secs = ((Date.now() - t0) / 1000).toFixed(0);
  if (failure !== null) {
    note(`download after the tab closed: failed (${failure}) after ${secs} s`);
    return;
  }
  const file = path.join(OUT, 'download-firefox-closed.csv');
  await d.saveAs(file);
  const size = fs.statSync(file).size;
  let check;
  try { check = savvycanCheck(file, '--min', '1'); } catch (e) { check = 'does NOT load: ' + String(e.stdout || e.message).trim(); }
  note(`download after the tab closed: ended as finished after ${secs} s, ${size} of ${want} bytes; ${check}`);
  expect(size).toBeLessThanOrEqual(want);
});

// The whole browser killed mid-log (as a crash): what is left in the download folder.
test('download: Firefox killed (SIGKILL) mid-log -> what is left on disk', async ({ browserName }) => {
  const { firefox } = await import('@playwright/test');
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'disklog-ff-dl-'));
  const server = await firefox.launchServer({ downloadsPath: dir });
  const browser = await firefox.connect(server.wsEndpoint());
  const page = await browser.newPage({ acceptDownloads: true, baseURL: 'http://localhost:8765' });
  await page.goto(HARNESS);
  await loadRows(page);
  const dl = page.waitForEvent('download');
  await page.evaluate(async (header) => {
    await SavvyDiskLog.start({ header });
    feedBatches(5000);
    await new Promise((r) => setTimeout(r, 1500));
  }, HEADER);
  await dl;
  const want = await bytesFor(page, 5000);
  const pid = server.process().pid;
  execSync(`pkill -KILL -P ${pid} || true; kill -KILL ${pid} || true`);
  await new Promise((r) => setTimeout(r, 1000));
  const files = fs.readdirSync(dir).map((f) => `${f} (${fs.statSync(path.join(dir, f)).size} B)`);
  note(`after the kill, ${want} B had been streamed; download folder: ${files.join(', ') || 'empty'}`);
  await server.close().catch(() => {});
  fs.rmSync(dir, { recursive: true, force: true });
});
