// Helpers of the file-based RE tools' specs (stage 4: bisect, filecompare, dbccompare, search, retools-big): logs and
// DBCs of known content, the browser's file picker answered with a file for a window's own Load button, saved files
// read back.
import { join, basename, relative, isAbsolute, sep } from 'node:path';
import { mkdirSync, writeFileSync, readFileSync, openSync, writeSync, closeSync, statSync } from 'node:fs';
import { RESULTS, WEB } from './harness.mjs';

export const hex = (n, w = 2) => n.toString(16).toUpperCase().padStart(w, '0');

/** A row of SavvyCAN's native GVRET CSV (FrameFileIO::nativeCSVRow): classic rows 8 data columns, FD rows max(8, LEN)
 * then the flags column. */
export const csvRow = ({ time, id, ext = false, fd = false, brs = false, tx = false, bus = 0, data }) => {
  const cols = fd ? Math.max(8, data.length) : 8;
  const bytes = Array.from({ length: cols }, (_, i) => (i < data.length ? hex(data[i]) : '00'));
  return `${time},${hex(id, 8)},${ext},${tx ? 'Tx' : 'Rx'},${bus},${data.length},${bytes.join(',')},` +
    (fd ? `FD${brs ? '+BRS' : ''},` : '');
};
export const CSV_HEADER = 'Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8';

/** Writes frames (objects for csvRow, or a generator function i -> frame for n frames) as a GVRET CSV under results/. */
export function writeCsv(name, frames, n) {
  mkdirSync(RESULTS, { recursive: true });
  const file = join(RESULTS, name);
  const fd = openSync(file, 'w');
  writeSync(fd, CSV_HEADER + '\n');
  let out = [];
  const count = typeof frames === 'function' ? n : frames.length;
  for (let i = 0; i < count; i++) {
    out.push(csvRow(typeof frames === 'function' ? frames(i) : frames[i]));
    if (out.length === 50_000) { writeSync(fd, out.join('\n') + '\n'); out = []; }
  }
  if (out.length) writeSync(fd, out.join('\n') + '\n');
  closeSync(fd);
  return { file, bytes: statSync(file).size };
}

export function writeText(name, text) {
  mkdirSync(RESULTS, { recursive: true });
  const file = join(RESULTS, name);
  writeFileSync(file, text);
  return file;
}

/** The next showOpenFilePicker() (Qt's QFileDialog::getOpenFileContent in the browser) hands over `file`, as the user's
 * pick would: a file under tests/web is fetched from the test server (big files), others go over as base64. */
export async function nextPick(page, file) {
  const rel = relative(WEB, file);
  const url = !rel.startsWith('..') && !isAbsolute(rel) ? '/__harness/' + rel.split(sep).join('/') : null;
  const b64 = url ? null : readFileSync(file).toString('base64');
  await page.evaluate(([name, src, data64]) => {
    const real = window.showOpenFilePicker;
    window.showOpenFilePicker = async () => {
      window.showOpenFilePicker = real;
      const f = src
        ? new File([await (await fetch(src)).blob()], name)
        : new File([Uint8Array.from(atob(data64), (c) => c.charCodeAt(0))], name);
      return [{ kind: 'file', name, getFile: async () => f }];
    };
  }, [basename(file), url, b64]);
}

/** Clicks `button` (a save button) and returns the text Qt handed to the browser: a download, or Chrome's save picker
 * (answered here, as the user would). */
export async function savedText(app, page, button) {
  await page.evaluate(() => {
    window.__saved = null;
    window.showSaveFilePicker = async (opts) => {
      const parts = [];
      return {
        kind: 'file', name: opts?.suggestedName || 'saved.txt',
        createWritable: async () => ({
          write: async (chunk) => { parts.push(chunk?.data ?? chunk); },
          close: async () => { window.__saved = { name: opts?.suggestedName, text: await new Blob(parts).text() }; },
        }),
      };
    };
  });
  const download = page.waitForEvent('download', { timeout: 30000 }).catch(() => null);
  await app.click('button', button);
  const picked = page.waitForFunction(() => window.__saved, null, { timeout: 30000 }).then((h) => h.jsonValue()).catch(() => null);
  const downloaded = download.then(async (d) => (d ? { name: d.suggestedFilename(), text: readFileSync(await d.path(), 'utf8') } : null));
  return Promise.race([picked.then((v) => v || downloaded), downloaded.then((v) => v || picked)]);
}

/** A tree from the hook ({t, c}) -> its node at a path of text prefixes, or undefined. */
export function node(tree, ...path) {
  let level = tree, n;
  for (const p of path) {
    n = (level || []).find((x) => x.t.startsWith(p));
    if (!n) return undefined;
    level = n.c;
  }
  return n;
}
/** The texts of a node's children ([] for a leaf or a missing node). */
export const kids = (n) => (n && n.c ? n.c.map((x) => x.t) : []);

/** 0x7E8 as the app formats IDs (Utility::formatHexNum: 2, 4 or 8 digits; formatCANID: 3 or 8). */
export const hexNum = (n) => '0x' + hex(n, n < 256 ? 2 : n < 65536 ? 4 : 8);
export const canId = (n) => '0x' + hex(n, n < 0x800 ? 3 : 8);

/** Waits until the page has an open window `fn` (a hook function name) reports open. */
export async function waitOpen(page, fn) {
  await page.waitForFunction((f) => window.webSavvyCAN[f]().open, fn, { timeout: 20000 });
}
