// SavvyDiskLog with the browser's real pickers and real files on disk: headed Chromium under Xvfb,
// the GTK file dialogs and Chrome's "Allow this site to edit files?" prompt answered with xdotool.
//   xvfb-run -a -s "-screen 0 1280x900x24" npx playwright test --project=chromium-xvfb
// (run-disklog-tests.sh does this when xvfb-run and xdotool are installed)
import { test, expect, chromium } from '@playwright/test';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execSync } from 'node:child_process';
import { HARNESS, HEADER, loadRows, bytesFor, savvycanCheck, note, BASE } from './common.mjs';

// on the real disk (/tmp is a RAM disk on the rig)
const DIR = path.join(os.homedir(), '.cache/web-savvycan-disklog-xvfb');
const PROFILE = path.join(DIR, 'profile');
const LOGS = path.join(DIR, 'logs');

test.skip(!process.env.DISPLAY, 'needs a display (xvfb-run)');
test.describe.configure({ mode: 'serial' });

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const sh = (cmd) => execSync(cmd, { stdio: ['ignore', 'pipe', 'pipe'] }).toString();

async function waitWindow(title, ms = 10000) {
  for (let t = 0; t < ms; t += 200) {
    try { return sh(`xdotool search --onlyvisible --name "${title}"`).trim().split('\n')[0]; } catch { await sleep(200); }
  }
  throw new Error(`no window "${title}"`);
}

// GTK save dialog (showSaveFilePicker): its title is Chrome's warning text
async function answerSaveDialog(file) {
  const w = await waitWindow('edits you make');
  sh(`xdotool windowfocus --sync ${w} key ctrl+a type --delay 10 "${file}"`);
  sh('xdotool key Return');
}

// GTK folder dialog (showDirectoryPicker)
async function answerFolderDialog(dir) {
  const w = await waitWindow('Select where this site');
  sh(`xdotool windowfocus --sync ${w} key ctrl+l type --delay 10 "${dir}/"`);
  sh('xdotool key Return');
}

// Chrome's permission prompts, by mouse (their keyboard focus is not certain); coordinates from the
// window's corner, for a 1280x900 Xvfb screen:
//  - "Allow this site to edit files?" (a folder just picked): "Allow" at (794, 194)
//  - after a browser restart, the restore prompt "... wants to view and edit files from the last time you
//    visited": "Allow on every visit" at (280, 320) (then no prompt on later visits)
async function clickInWindow(dx, dy) {
  const w = await waitWindow('SavvyDiskLog test harness');
  const [, x, y] = sh(`xdotool getwindowgeometry ${w}`).match(/Position: (\d+),(\d+)/).map(Number);
  sh(`xdotool mousemove ${x + dx} ${y + dy} click 1`);
}

async function allowEditFiles() {
  await sleep(1500);
  await clickInWindow(794, 194);
}

// answers whichever prompt is up; `settled()` tells when the page has its answer
async function allowAfterRestart(settled) {
  await sleep(1500);
  await clickInWindow(280, 320);              // restore prompt: "Allow on every visit"
  await sleep(700);
  if (!settled()) await clickInWindow(794, 194); // or the plain "Allow"
  await sleep(700);
  return settled();
}

const launch = () => chromium.launchPersistentContext(PROFILE, { headless: false, baseURL: BASE });

test.beforeAll(() => {
  fs.rmSync(DIR, { recursive: true, force: true });
  fs.mkdirSync(LOGS, { recursive: true });
});

test('prompt: the real save dialog; rows written to the chosen file; loads in SavvyCAN', async () => {
  const ctx = await launch();
  const page = ctx.pages()[0] || await ctx.newPage();
  await page.goto(HARNESS);
  await loadRows(page);
  await page.click('#go');
  const file = path.join(LOGS, 'picked.csv');
  const started = page.evaluate((header) => SavvyDiskLog.start({ mode: 'prompt', header }), HEADER);
  await answerSaveDialog(file);
  expect(await started).toMatchObject({ name: 'picked.csv', backend: 'fsa' });
  await page.evaluate(async () => { feedBatches(50000); await SavvyDiskLog.stop(); });
  await ctx.close();
  expect(fs.statSync(file).size).toBe(await (async () => {
    // bytesFor needs a page with the rows: compute from the reference file instead
    const rows = fs.readFileSync(path.join(path.dirname(new URL(import.meta.url).pathname), '../test-results/disklog/rows.csv'), 'latin1');
    let n = 0, off = 0; while (n < 50000) { off = rows.indexOf('\n', off) + 1; n++; }
    return HEADER.length + off;
  })());
  expect(fs.readdirSync(LOGS).filter((f) => f.endsWith('.crswap'))).toEqual([]);
  note(savvycanCheck(file, '--exact', '50000'));
});

test('direct: first log asks for the folder and the permission; the next one asks nothing', async () => {
  const ctx = await launch();
  const page = ctx.pages()[0] || await ctx.newPage();
  await page.goto(HARNESS);
  await page.evaluate(() => SavvyDiskLog.forgetDirectory());
  await page.click('#go');
  const first = page.evaluate(() => SavvyDiskLog.start({ mode: 'direct', header: 'h\n' }));
  await answerFolderDialog(LOGS);
  await allowEditFiles();
  const s1 = await first;
  expect(s1.name).toMatch(/^SavvyCAN_\d{4}-\d\d-\d\d_\d\d-\d\d-\d\d\.csv$/);
  await page.evaluate(() => SavvyDiskLog.stop());
  // the second log: no dialog, no prompt (start would hang otherwise)
  await page.click('#go');
  const t0 = Date.now();
  const s2 = await page.evaluate(() => SavvyDiskLog.start({ mode: 'direct', header: 'h\n', suggestedName: 'second.csv' }));
  await page.evaluate(() => SavvyDiskLog.stop());
  expect(s2.name).toBe('second.csv');
  const dir = await page.evaluate(() => SavvyDiskLog.rememberedDirectory());
  await ctx.close();
  expect(dir).toEqual({ name: 'logs', permission: 'granted' });
  expect(fs.existsSync(path.join(LOGS, s1.name))).toBe(true);
  expect(fs.existsSync(path.join(LOGS, 'second.csv'))).toBe(true);
  note(`first: ${s1.name} after the folder dialog + permission; second: no prompt, ${Date.now() - t0} ms`);
});

test('direct after a browser restart: the folder is remembered, the permission confirmed once ("Allow on every visit"); the next restart asks nothing', async () => {
  const run = async (name) => {
    const ctx = await launch();
    const page = ctx.pages()[0] || await ctx.newPage();
    await page.goto(HARNESS);
    const before = await page.evaluate(() => SavvyDiskLog.rememberedDirectory());
    await page.click('#go');
    let done = false;
    const started = page.evaluate((n) => SavvyDiskLog.start({ mode: 'direct', header: 'h\n', suggestedName: n }), name)
      .finally(() => { done = true; });
    let prompted = false;
    if (before.permission !== 'granted') prompted = await allowAfterRestart(() => done);
    const s = await started;
    await page.evaluate(() => SavvyDiskLog.stop());
    await ctx.close();
    expect(fs.existsSync(path.join(LOGS, name))).toBe(true);
    return { s, before, prompted };
  };
  const r1 = await run('after-restart-1.csv');
  const r2 = await run('after-restart-2.csv');
  expect(r1.before).toEqual({ name: 'logs', permission: 'prompt' });
  expect(r1.prompted).toBe(true);
  expect(r2.before).toEqual({ name: 'logs', permission: 'granted' });
  note(`restart 1: folder "${r1.before.name}" remembered, permission "${r1.before.permission}" -> confirmed once; `
    + `restart 2: permission "${r2.before.permission}", no prompt`);
});

test('crash: browser killed (SIGKILL) while logging into a real file -> the file holds the last commit', async () => {
  let ctx = await launch();
  const page = ctx.pages()[0] || await ctx.newPage();
  await page.goto(HARNESS);
  await loadRows(page);
  await page.click('#go');
  const before = await page.evaluate(async (header) => {
    const s = await SavvyDiskLog.start({ mode: 'direct', header, commitIntervalMs: 1000, suggestedName: 'crash.csv' });
    setInterval(() => feedBatches(Math.min(rows.length, fed + 50)), 20);
    await new Promise((resolve) => {
      const t = setInterval(() => { if (events.filter((e) => e.type === 'committed').length >= 3) { clearInterval(t); resolve(); } }, 20);
    });
    await new Promise((r) => setTimeout(r, 400));
    const c = events.filter((e) => e.type === 'committed');
    return { name: s.name, lastCommitted: c.at(-1).bytes, written: SavvyDiskLog.status().bytesWritten };
  }, HEADER);
  const file = path.join(LOGS, 'crash.csv');
  sh(`pkill -KILL -f -- '${PROFILE.replace('web-savvycan-disklog-xvfb', '[w]eb-savvycan-disklog-xvfb')}' || true`);
  await ctx.close().catch(() => {});
  const size = fs.statSync(file).size;
  const leftovers = fs.readdirSync(LOGS).filter((f) => f.endsWith('.crswap'));
  const text = fs.readFileSync(file, 'latin1');
  const minRows = text.slice(0, before.lastCommitted).split('\n').length - 2;
  expect(size).toBeGreaterThanOrEqual(before.lastCommitted);
  expect(size).toBeLessThanOrEqual(before.written);
  note(`${savvycanCheck(file, '--min', String(minRows))}; last commit seen ${before.lastCommitted} B, written ${before.written} B, `
    + `lost ${before.written - size} B; swap files left: ${leftovers.join(', ') || 'none'}`);
  // the next start cleans up: Chrome removes stale .crswap files when it opens the folder again
  ctx = await launch();
  await ctx.close();
});

test('commit cost on a real file: commit time vs file size', async () => {
  test.setTimeout(200_000);
  const ctx = await launch();
  const page = ctx.pages()[0] || await ctx.newPage();
  await page.goto(HARNESS);
  await page.click('#go');
  const r = await page.evaluate(async () => {
    const row = '1000000,00000123,false,Rx,0,64,' + '5A,'.repeat(64) + 'FD+BRS,\n';
    const chunk = new TextEncoder().encode(row.repeat(Math.floor((64 << 20) / row.length)));
    await SavvyDiskLog.start({ mode: 'direct', header: 'h\n', suggestedName: 'big.csv' });
    const out = [];
    for (let mb = 64; mb <= 512; mb += 64) {
      SavvyDiskLog.write(chunk);
      const t = performance.now();
      await SavvyDiskLog.commit();
      out.push({ mb, commitMs: Math.round(performance.now() - t), intervalMs: Math.round(SavvyDiskLog.status().commitIntervalMs) });
    }
    await SavvyDiskLog.stop();
    return out;
  });
  await ctx.close();
  fs.rmSync(path.join(LOGS, 'big.csv'), { force: true });
  note(r.filter((o) => o.mb % 128 === 0 || o.mb === 64).map((o) => `${o.mb} MB: ${o.commitMs} ms (interval ${o.intervalMs} ms)`).join('; '));
});
