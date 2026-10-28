import { test } from '@playwright/test';
import path from 'node:path';
import fs from 'node:fs';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

export const WEB = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
export const REPO = path.resolve(WEB, '../..');
export const TOOL = process.env.NATIVECSV_TOOL || path.join(REPO, 'build/nativecsv/nativecsv_tool');
export const OUT = path.join(WEB, 'test-results/disklog');
export const ROWS = path.join(OUT, 'rows.csv');
export const ROWS_URL = '/test/web/test-results/disklog/rows.csv';
export const ROWS_COUNT = 200000;
export const HARNESS = '/test/web/disklog/harness.html';
export const HEADER = 'Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8\n';

// SavvyCAN's loader on a produced file; returns its summary line, throws on a mismatch.
export function savvycanCheck(file, ...args) {
  return execFileSync(TOOL, ['check', file, ...args], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] })
    .split('\n').filter((l) => l.startsWith(file) || l === 'OK').join(' ');
}

export function saveBytes(name, bytes) {
  const file = path.join(OUT, name);
  fs.writeFileSync(file, typeof bytes === 'string' ? Buffer.from(bytes, 'latin1') : Buffer.from(bytes));
  return file;
}

// In the page: the reference rows as an array of lines, and a feeder that writes them in batches
// of 1..97 rows (as frames come from a connection).
export async function loadRows(page) {
  await page.evaluate(async (url) => {
    const text = await (await fetch(url)).text();
    window.rows = text.split(/(?<=\n)/);
    window.rowOffsets = [0];
    for (const r of window.rows) window.rowOffsets.push(window.rowOffsets[window.rowOffsets.length - 1] + r.length);
    window.fed = 0;
    window.feedBatches = (count) => {
      let batch = 0;
      while (window.fed < count) {
        const n = Math.min(1 + (batch++ * 13) % 97, count - window.fed);
        SavvyDiskLog.write(window.rows.slice(window.fed, window.fed + n).join(''));
        window.fed += n;
      }
    };
  }, ROWS_URL);
}

// bytes of the header + the first n rows
export async function bytesFor(page, n) {
  return (await page.evaluate((n) => window.rowOffsets[n], n)) + HEADER.length;
}

// a result line: in the report (annotation) and on the console
export function note(description) {
  test.info().annotations.push({ type: 'result', description });
  console.log(`  -> ${test.info().title}: ${description}`);
}
