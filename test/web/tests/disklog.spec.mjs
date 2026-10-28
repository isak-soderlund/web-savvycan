// SavvyDiskLog (wasm/disklog/disklog.js) in headless Chromium.
//
// Headless Chromium cancels the browser's pickers and cannot grant write permission to a real file,
// so the File System Access writer is tested against the origin-private file system (OPFS): the same
// FileSystemDirectoryHandle / FileSystemFileHandle / createWritable API, a folder the test "remembers"
// in place of the one the user picks. The pickers: their cancel handling here; the real dialogs, the
// permission prompts and real files in disklog-xvfb.spec.mjs (headed Chromium under Xvfb).
import { test, expect, chromium } from '@playwright/test';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execSync } from 'node:child_process';
import { HARNESS, HEADER, ROWS_COUNT, loadRows, bytesFor, saveBytes, savvycanCheck, OUT, note, BASE } from './common.mjs';

async function freshHarness(page) {
  await page.goto(HARNESS);
  await page.evaluate(async () => {
    const root = await navigator.storage.getDirectory();
    for await (const name of root.keys()) await root.removeEntry(name, { recursive: true });
    await SavvyDiskLog.forgetDirectory();
  });
}

test('support: File System Access in Chromium', async ({ page }) => {
  await page.goto(HARNESS);
  const s = await page.evaluate(() => SavvyDiskLog.support());
  expect(s).toMatchObject({ fsa: true, download: true, opfs: true, backend: 'fsa' });
});

test('direct: file named by the start time in the remembered folder; every row; loads in SavvyCAN', async ({ page }) => {
  await freshHarness(page);
  await loadRows(page);
  const t0 = Date.now();
  const res = await page.evaluate(async ({ header, count }) => {
    await SavvyDiskLog._rememberDirectory(await opfsDir('logs'));
    const started = await SavvyDiskLog.start({ mode: 'direct', header });
    feedBatches(count);
    const stopped = await SavvyDiskLog.stop();
    const bytes = await readOpfs('logs', started.name);
    return { started, stopped, bytes, events: events.map((e) => e.type), status: SavvyDiskLog.status() };
  }, { header: HEADER, count: ROWS_COUNT });
  const secs = (Date.now() - t0) / 1000;
  expect(res.started.name).toMatch(/^SavvyCAN_\d{4}-\d\d-\d\d_\d\d-\d\d-\d\d\.csv$/);
  expect(res.started.backend).toBe('fsa');
  expect(res.events[0]).toBe('started');
  expect(res.events.at(-1)).toBe('stopped');
  expect(res.status.state).toBe('idle');
  const want = await bytesFor(page, ROWS_COUNT);
  expect(res.bytes.length).toBe(want);
  expect(res.stopped.bytes).toBe(want);
  const file = saveBytes('direct.csv', res.bytes);
  const summary = savvycanCheck(file, '--exact', String(ROWS_COUNT));
  note(`${summary} (${secs.toFixed(1)} s in the page)`);
});

test('direct: a second log in the same second gets its own name', async ({ page }) => {
  await freshHarness(page);
  const names = await page.evaluate(async () => {
    await SavvyDiskLog._rememberDirectory(await opfsDir('logs'));
    const out = [];
    for (let i = 0; i < 3; i++) {
      const s = await SavvyDiskLog.start({ mode: 'direct', suggestedName: 'SavvyCAN_2026-10-28_12-00-00.csv', header: 'h\n' });
      out.push(s.name);
      await SavvyDiskLog.stop();
    }
    return out;
  });
  expect(names).toEqual(['SavvyCAN_2026-10-28_12-00-00.csv', 'SavvyCAN_2026-10-28_12-00-00_2.csv',
    'SavvyCAN_2026-10-28_12-00-00_3.csv']);
});

test('commit: while logging, the file on disk holds everything up to the last commit', async ({ page }) => {
  await freshHarness(page);
  await loadRows(page);
  const r = await page.evaluate(async (header) => {
    await SavvyDiskLog._rememberDirectory(await opfsDir('logs'));
    const s = await SavvyDiskLog.start({ mode: 'direct', header, commitIntervalMs: 400 });
    feedBatches(1000);
    const c1 = await new Promise((resolve) => SavvyDiskLog.setListener((e) => { events.push(e); if (e.type === 'committed') resolve(e); }));
    const onDisk1 = (await readOpfs('logs', s.name)).length;
    feedBatches(3000);                               // written, not yet committed
    await new Promise((r) => setTimeout(r, 50));
    const st = SavvyDiskLog.status();
    const onDisk2 = (await readOpfs('logs', s.name)).length;
    await SavvyDiskLog.commit();                     // commit now
    const onDisk3 = (await readOpfs('logs', s.name)).length;
    await SavvyDiskLog.stop();
    return { c1: c1.bytes, onDisk1, onDisk2, onDisk3, written: st.bytesWritten, committed: st.bytesCommitted };
  }, HEADER);
  const b1000 = await bytesFor(page, 1000), b3000 = await bytesFor(page, 3000);
  expect(r.c1).toBe(b1000);
  expect(r.onDisk1).toBe(b1000);
  expect(r.written).toBe(b3000);
  expect(r.onDisk2).toBe(r.committed);              // what is not committed is not in the file yet
  expect(r.onDisk2).toBeLessThan(b3000);
  expect(r.onDisk3).toBe(b3000);
});

test('prompt: the save picker cancelled -> "cancelled", nothing left open', async ({ page }) => {
  await freshHarness(page);
  await page.click('#go');                           // a user gesture, as the menu click gives
  const r = await page.evaluate(async () => {
    try { await SavvyDiskLog.start({ mode: 'prompt', header: 'h\n' }); return 'started'; } catch (e) { return e.code; }
  });
  expect(r).toBe('cancelled');                       // headless Chromium aborts every picker
  expect(await page.evaluate(() => SavvyDiskLog.status().state)).toBe('idle');
});

test('direct: no folder remembered yet -> the folder picker; cancelled -> "cancelled"', async ({ page }) => {
  await freshHarness(page);
  await page.click('#go');
  const r = await page.evaluate(async () => {
    const before = await SavvyDiskLog.rememberedDirectory();
    let code = 'started';
    try { await SavvyDiskLog.start({ mode: 'direct', header: 'h\n' }); } catch (e) { code = e.code; }
    return { before, code, after: await SavvyDiskLog.rememberedDirectory() };
  });
  expect(r).toEqual({ before: null, code: 'cancelled', after: null });
});

test('remembered folder: kept in IndexedDB across a reload, with its permission', async ({ page }) => {
  await freshHarness(page);
  await page.evaluate(async () => SavvyDiskLog._rememberDirectory(await opfsDir('logs')));
  await page.reload();
  const d = await page.evaluate(() => SavvyDiskLog.rememberedDirectory());
  expect(d).toEqual({ name: 'logs', permission: 'granted' });
});

test('permission withdrawn mid-log -> "permission" error event; the file keeps the last commit', async ({ page }) => {
  await freshHarness(page);
  await loadRows(page);
  const r = await page.evaluate(async (header) => {
    const real = await opfsDir('logs');
    let fileHandle;
    // the folder as the app sees it; the test keeps the file handle to take its permission away
    const dir = { name: 'logs', getFileHandle: async (n, o) => (fileHandle = await real.getFileHandle(n, o)) };
    const s = await SavvyDiskLog.start({ mode: 'direct', directory: dir, header, commitIntervalMs: 300 });
    feedBatches(500);
    await SavvyDiskLog.commit();
    fileHandle.queryPermission = async () => 'prompt';   // as after "forget" in the site settings
    feedBatches(900);
    const err = await new Promise((resolve) => {
      const t = setInterval(() => { const e = events.find((x) => x.type === 'error'); if (e) { clearInterval(t); resolve(e); } }, 50);
    });
    const accepted = SavvyDiskLog.write('more\n');
    return { err, accepted, state: SavvyDiskLog.status().state, onDisk: (await readOpfs('logs', s.name)).length };
  }, HEADER);
  expect(r.err.code).toBe('permission');
  expect(r.err.bytes).toBe(await bytesFor(page, 500));
  expect(r.accepted).toBe(false);
  expect(r.state).toBe('idle');
  expect(r.onDisk).toBe(await bytesFor(page, 500));
});

test('a write error from the disk -> error event with its code (disk full)', async ({ page }) => {
  await freshHarness(page);
  const r = await page.evaluate(async () => {
    const real = await opfsDir('logs');
    const dir = { name: 'logs', getFileHandle: async (n, o) => {
      const h = await real.getFileHandle(n, o);
      const cw = h.createWritable.bind(h);
      h.createWritable = async (opts) => {
        const w = await cw(opts);
        const write = w.write.bind(w);
        let n = 0;
        w.write = async (d) => { if (++n > 1) throw new DOMException('no space', 'QuotaExceededError'); return write(d); };
        return w;
      };
      return h;
    } };
    await SavvyDiskLog.start({ mode: 'direct', directory: dir, header: 'h\n' });
    await new Promise((r) => setTimeout(r, 50));
    SavvyDiskLog.write('row\n');
    await new Promise((r) => setTimeout(r, 200));
    return events.filter((e) => e.type === 'error');
  });
  expect(r.length).toBe(1);
  expect(r[0].code).toBe('quota');
});

// ---- crash tests --------------------------------------------------------------------------------

// Logging into OPFS at ~2 500 rows/s (a busy bus), committing every second; returns once at least
// `commits` commits are done. The feeder runs from a timer, as frames arrive.
async function logUntilCommits(page, commits) {
  await loadRows(page);
  return page.evaluate(async ({ header, commits }) => {
    await SavvyDiskLog._rememberDirectory(await opfsDir('logs'));
    const s = await SavvyDiskLog.start({ mode: 'direct', header, commitIntervalMs: 1000 });
    window.logName = s.name;
    let batch = 0;
    window.feeder = setInterval(() => feedBatches(Math.min(rows.length, fed + 50)), 20);
    await new Promise((resolve) => {
      const t = setInterval(() => { if (events.filter((e) => e.type === 'committed').length >= commits) { clearInterval(t); resolve(); } }, 20);
    });
    await new Promise((r) => setTimeout(r, 400));      // into the next interval: rows written, not committed
    const c = events.filter((e) => e.type === 'committed');
    return { name: s.name, lastCommitted: c.at(-1).bytes, written: SavvyDiskLog.status().bytesWritten, fed };
  }, { header: HEADER, commits });
}

function rowsIn(bytes) {
  let n = 0;
  for (let i = 0; i < bytes.length; i++) if (bytes.charCodeAt(i) === 10) n++;
  return n - 1; // header
}

test('crash: the renderer crashes mid-log -> the file holds everything up to the last commit', async ({ page, context }) => {
  await freshHarness(page);
  const before = await logUntilCommits(page, 3);
    const cdp = await context.newCDPSession(page);
  const crashed = page.waitForEvent('crash', { timeout: 10000 });
  cdp.send('Page.crash').catch(() => {});              // the tab's process dies (as "Aw, Snap!"); never answers
  await crashed;
  const p2 = await context.newPage();
  await p2.goto(HARNESS);
  const bytes = await p2.evaluate(async (name) => await readOpfs('logs', name), before.name);
  const file = saveBytes('crash-renderer.csv', bytes);
  const minRows = rowsIn(bytes.slice(0, before.lastCommitted));
  expect(bytes.length).toBeGreaterThanOrEqual(before.lastCommitted);
  expect(bytes.length).toBeLessThanOrEqual(before.written);
  const summary = savvycanCheck(file, '--min', String(minRows));
  note(
    `${summary}; last commit seen ${before.lastCommitted} B, written ${before.written} B (${before.fed} rows), `
    + `lost ${before.written - bytes.length} B = ${((before.written - bytes.length) / (before.written / before.fed) / 2500).toFixed(2)} s of rows`);
});

test('crash: the whole browser is killed (SIGKILL) mid-log -> the file holds everything up to the last commit', async ({}, testInfo) => {
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'disklog-profile-'));
  const launch = () => chromium.launchPersistentContext(profile, { headless: true, baseURL: BASE });
  let ctx = await launch();
  let page = ctx.pages()[0] || await ctx.newPage();
  await freshHarness(page);
  const before = await logUntilCommits(page, 3);
  execSync(`pkill -KILL -f -- '${profile.replace('disklog-profile', '[d]isklog-profile')}' || true`);  // every process of that browser, no clean-up
  await ctx.close().catch(() => {});
  ctx = await launch();
  page = ctx.pages()[0] || await ctx.newPage();
  await page.goto('/test/web/disklog/harness.html');
  const bytes = await page.evaluate(async (name) => await readOpfs('logs', name), before.name);
  await ctx.close();
  fs.rmSync(profile, { recursive: true, force: true });
  const file = saveBytes('crash-browser.csv', bytes);
  const minRows = rowsIn(bytes.slice(0, before.lastCommitted));
  expect(bytes.length).toBeGreaterThanOrEqual(before.lastCommitted);
  expect(bytes.length).toBeLessThanOrEqual(before.written);
  const summary = savvycanCheck(file, '--min', String(minRows));
  note(
    `${summary}; last commit seen ${before.lastCommitted} B, written ${before.written} B, lost ${before.written - bytes.length} B`);
});

// ---- the cost of a commit as the file grows ----------------------------------------------------

test('commit cost: commit time vs file size (OPFS), and the stretched commit interval', async ({}) => {
  test.setTimeout(200_000);
  // a profile on the real disk (Playwright's default is in /tmp, a RAM disk here, with a 900 MB quota)
  const profile = path.join(os.homedir(), '.cache/web-savvycan-disklog-profile');
  fs.rmSync(profile, { recursive: true, force: true });
  const ctx = await chromium.launchPersistentContext(profile, { headless: true, baseURL: BASE });
  const page = ctx.pages()[0] || await ctx.newPage();
  await freshHarness(page);
  const r = await page.evaluate(async () => {
    await SavvyDiskLog._rememberDirectory(await opfsDir('logs'));
    // 64-byte CAN FD rows (~220 B), 16 MB per step
    const row = '1000000,00000123,false,Rx,0,64,' + '5A,'.repeat(64) + 'FD+BRS,\n';
    const chunk = new TextEncoder().encode(row.repeat(Math.floor((16 << 20) / row.length)));
    await SavvyDiskLog.start({ mode: 'direct', header: 'h\n', commitIntervalMs: 3000 });
    const out = [];
    const t0 = performance.now();
    for (let mb = 16; mb <= 512; mb += 16) {
      SavvyDiskLog.write(chunk);
      const tc = performance.now();
      await SavvyDiskLog.commit();
      if (mb % 64 === 0) out.push({ mb, commitMs: Math.round(performance.now() - tc), intervalMs: Math.round(SavvyDiskLog.status().commitIntervalMs) });
    }
    const total = performance.now() - t0;
    await SavvyDiskLog.stop();
    const root = await navigator.storage.getDirectory();
    await root.removeEntry('logs', { recursive: true });
    return { out, mbPerS: Math.round(512 / (total / 1000)) };
  });
  await ctx.close();
  fs.rmSync(profile, { recursive: true, force: true });
  const lines = r.out.map((o) => `${o.mb} MB: commit ${o.commitMs} ms, interval ${o.intervalMs} ms`).join('; ');
  note(`${lines}; write+commit ${r.mbPerS} MB/s overall`);
  fs.writeFileSync(path.join(OUT, 'commit-cost.json'), JSON.stringify(r, null, 2));
});

// ---- the streaming download, in Chromium too (Firefox: disklog-firefox.spec.mjs) ----------------

test('download backend: rows streamed into a download, finished on stop; loads in SavvyCAN', async ({ page }) => {
  await freshHarness(page);
  await loadRows(page);
  const dl = page.waitForEvent('download');
  const r = await page.evaluate(async (header) => {
    const s = await SavvyDiskLog.start({ mode: 'download', header, suggestedName: 'SavvyCAN_download.csv' });
    feedBatches(20000);
    await SavvyDiskLog.stop();
    return s;
  }, HEADER);
  expect(r).toMatchObject({ backend: 'download', crashSafe: false });
  const d = await dl;
  expect(d.suggestedFilename()).toBe('SavvyCAN_download.csv');
  const file = path.join(OUT, 'download-chromium.csv');
  await d.saveAs(file);
  expect(fs.statSync(file).size).toBe(await bytesFor(page, 20000));
  note(savvycanCheck(file, '--exact', '20000'));
});

test('split (direct mode): a new file once a part reaches the size limit, each part complete and loadable', async ({ page }) => {
  await freshHarness(page);
  await loadRows(page);
  const r = await page.evaluate(async ({ header, count }) => {
    await SavvyDiskLog._rememberDirectory(await opfsDir('logs'));
    const s = await SavvyDiskLog.start({ mode: 'direct', header, suggestedName: 'SavvyCAN_split.csv', splitBytes: 2 << 20 });
    feedBatches(count);
    const stopped = await SavvyDiskLog.stop();
    const files = {};
    for (const name of stopped.parts) files[name] = await readOpfs('logs', name);
    return { stopped, files, rotated: events.filter((e) => e.type === 'rotated').length };
  }, { header: HEADER, count: 50000 });
  const names = r.stopped.parts;
  expect(names[0]).toBe('SavvyCAN_split.csv');
  expect(names[1]).toBe('SavvyCAN_split_part002.csv');
  expect(r.rotated).toBe(names.length - 1);
  let first = 0, total = 0, joined = '';
  for (const name of names) {
    const text = r.files[name];
    expect(text.startsWith(HEADER)).toBe(true);
    const rows = text.split('\n').length - 2;
    savvycanCheck(saveBytes('split-' + name, text), '--exact', String(rows), '--first', String(first));
    first += rows;
    total += text.length;
    joined += text.slice(HEADER.length);
    expect(text.length).toBeLessThan((2 << 20) + 64 * 1024);  // limit + at most one batch
  }
  expect(first).toBe(50000);
  expect(r.stopped.bytes).toBe(total);
  note(`${names.length} parts of <= 2 MB, ${first} rows, each part loads in SavvyCAN on its own`);
});
