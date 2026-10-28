// The MVP's blob logging with a large log: gigabytes of rows collected in the page, then one download.
// Measures, every 256 MB: the page's JS heap (Chromium, after a GC), the stand-in wasm heap (a fixed
// buffer the rows are copied out of, as WebDiskLog::write does from the real wasm heap), the browser's
// memory (PSS of every process of this browser) and the profile's size on disk (Chrome pages blobs
// into its blob_storage there); then the download's time, size and SHA-256 against the rows written.
//
// Not in the normal runs (project "chromium-large"); run it through the heavy-job wrapper:
//   ~/.local/share/web-savvycan/heavy.sh npx playwright test --project=chromium-large
// WSC_LARGE_GB=2,4  sizes to try (default 2); WSC_LARGE_DIR: profile + downloads (default
// test-results/large, on disk: /tmp is a RAM disk here); WSC_LARGE_BROWSER=firefox for Firefox.
import { test, expect, chromium, firefox } from '@playwright/test';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { execSync } from 'node:child_process';
import { HARNESS, HEADER, ROWS, OUT, BASE, WEB, note } from './common.mjs';

const SIZES = (process.env.WSC_LARGE_GB || '2').split(',').map(Number);
const DIR = path.resolve(process.env.WSC_LARGE_DIR || path.join(WEB, 'test-results/large'));
const BROWSER = process.env.WSC_LARGE_BROWSER || 'chromium';
const MB = 1 << 20;
const STEP = 256 * MB;

// PSS (kB) of a process and all its descendants
function treePssMB(rootPid) {
  const kids = new Map();
  for (const line of execSync('ps -e -o pid=,ppid=', { encoding: 'utf8' }).trim().split('\n')) {
    const [pid, ppid] = line.trim().split(/\s+/).map(Number);
    if (!kids.has(ppid)) kids.set(ppid, []);
    kids.get(ppid).push(pid);
  }
  let kb = 0;
  const stack = [rootPid];
  while (stack.length) {
    const p = stack.pop();
    try {
      const m = fs.readFileSync(`/proc/${p}/smaps_rollup`, 'utf8').match(/^Pss:\s+(\d+)/m);
      if (m) kb += Number(m[1]);
    } catch { /* gone */ }
    stack.push(...(kids.get(p) || []));
  }
  return Math.round(kb / 1024);
}

function duMB(dir) {
  try { return Math.round(Number(execSync(`du -sk "${dir}" 2>/dev/null | cut -f1`, { encoding: 'utf8' })) / 1024); }
  catch { return 0; }
}

function browserPid(profile) {
  // the browser's main process: the one with the profile on its command line and no parent that has it
  const out = execSync('ps -e -o pid=,ppid=,args=', { encoding: 'utf8' }).split('\n')
    .map((l) => l.trim().match(/^(\d+)\s+(\d+)\s+(.*)$/)).filter((m) => m && m[3].includes(profile));
  const pids = new Set(out.map((m) => Number(m[1])));
  const main = out.find((m) => !pids.has(Number(m[2])));
  return main ? Number(main[1]) : null;
}

// whole rows from the reference file, about 1 MiB
function makeBlock() {
  const text = fs.readFileSync(ROWS, 'latin1');
  let end = text.lastIndexOf('\n', MB) + 1;
  return Buffer.from(text.slice(0, end), 'latin1');
}

for (const gb of SIZES) {
  test(`large blob log: ${gb} GB of rows collected, one download (${BROWSER})`, async () => {
    fs.mkdirSync(DIR, { recursive: true });
    const profile = fs.mkdtempSync(path.join(DIR, 'profile-'));
    const downloads = fs.mkdtempSync(path.join(DIR, 'downloads-'));
    const type = BROWSER === 'firefox' ? firefox : chromium;
    const ctx = await type.launchPersistentContext(profile, {
      headless: true, baseURL: BASE, acceptDownloads: true, downloadsPath: downloads,
    });
    const samples = [];
    try {
      const page = ctx.pages()[0] || await ctx.newPage();
      await page.goto(HARNESS);
      const cdp = BROWSER === 'chromium' ? await ctx.newCDPSession(page) : null;
      const pid = browserPid(profile);
      const block = makeBlock();
      const blocks = Math.ceil(gb * 1024 * MB / block.length);
      const total = HEADER.length + blocks * block.length;
      const hash = crypto.createHash('sha256').update(HEADER);
      for (let i = 0; i < blocks; i++) hash.update(block);
      const wantSha = hash.digest('hex');

      // the block sits in a fixed "wasm heap" buffer, copied out per write (writeCopy), 1 MiB per call
      await page.evaluate(async ({ b64, header }) => {
        const bin = atob(b64);
        window.heap = new Uint8Array(new ArrayBuffer(64 << 20));
        for (let i = 0; i < bin.length; i++) window.heap[i] = bin.charCodeAt(i);
        window.blockLen = bin.length;
        window.writeBlocks = async (n) => {
          for (let i = 0; i < n; i++) {
            if (!SavvyDiskLog.writeCopy(window.heap.subarray(0, window.blockLen))) throw new Error('write refused');
            if (i % 16 === 15) await new Promise((r) => setTimeout(r, 0));   // the event loop runs, as with a device
          }
        };
        await SavvyDiskLog.start({ mode: 'blob', header });
      }, { b64: block.toString('base64'), header: HEADER });

      const sample = async (written) => {
        let jsHeapMB = null;
        if (cdp) {
          await cdp.send('HeapProfiler.collectGarbage');
          const h = await cdp.send('Runtime.getHeapUsage');
          jsHeapMB = Math.round(h.usedSize / MB);
        }
        const st = await page.evaluate(() => ({ s: SavvyDiskLog.status(), heap: window.heap.byteLength }));
        const s = { writtenMB: Math.round(written / MB), jsHeapMB, wasmHeapMB: st.heap / MB,
                    pendingMB: Math.round(st.s.pendingBytes / MB), browserPssMB: pid ? treePssMB(pid) : null,
                    profileDiskMB: duMB(profile), t: Date.now() };
        samples.push(s);
        console.log(`  ${JSON.stringify(s)}`);
      };

      const t0 = Date.now();
      await sample(0);
      const perStep = Math.round(STEP / block.length);
      let done = 0;
      while (done < blocks) {
        const n = Math.min(perStep, blocks - done);
        await page.evaluate((n) => writeBlocks(n), n);
        done += n;
        await sample(HEADER.length + done * block.length);
      }
      const tCollected = Date.now();
      const dlP = page.waitForEvent('download', { timeout: 600_000 });
      const stopped = await page.evaluate(() => SavvyDiskLog.stop());
      const d = await dlP;
      const file = await d.path();          // resolves when the download has finished
      const tDownloaded = Date.now();
      await sample(total);
      expect(await d.failure()).toBeNull();
      const size = fs.statSync(file).size;
      const sha = await new Promise((resolve, reject) => {
        const h = crypto.createHash('sha256');
        fs.createReadStream(file, { highWaterMark: 8 * MB }).on('data', (c) => h.update(c))
          .on('end', () => resolve(h.digest('hex'))).on('error', reject);
      });
      expect(stopped.bytes).toBe(total);
      expect(size).toBe(total);
      expect(sha).toBe(wantSha);
      const js = samples.map((s) => s.jsHeapMB).filter((x) => x !== null);
      const pss = samples.map((s) => s.browserPssMB).filter((x) => x !== null);
      const summary = {
        browser: BROWSER, gb, bytes: total, rows: blocks * (block.toString('latin1').match(/\n/g).length),
        collectSeconds: (tCollected - t0) / 1000, collectMBps: Math.round(total / MB / ((tCollected - t0) / 1000)),
        downloadSeconds: (tDownloaded - tCollected) / 1000,
        jsHeapMB: js.length ? { min: Math.min(...js), max: Math.max(...js) } : 'n/a',
        wasmHeapMB: samples[0].wasmHeapMB,
        browserPssMB: pss.length ? { start: pss[0], max: Math.max(...pss), end: pss.at(-1) } : 'n/a',
        profileDiskMBMax: Math.max(...samples.map((s) => s.profileDiskMB)),
        sha256: 'match',
      };
      fs.writeFileSync(path.join(OUT, `large-${BROWSER}-${gb}GB.json`), JSON.stringify({ summary, samples }, null, 1));
      note(JSON.stringify(summary));
      fs.rmSync(file, { force: true });
    } finally {
      await ctx.close().catch(() => {});
      fs.rmSync(profile, { recursive: true, force: true });
      fs.rmSync(downloads, { recursive: true, force: true });
    }
  });
}
