// The DBC helpers of the specs (dbc.spec, search.spec, the rig's dbc spec): tests/web/fixtures/stage3_test.dbc parsed
// and decoded here from the DBC rules (Intel/Motorola bit numbering as SavvyCAN's, factor, offset, value descriptions on
// the raw value), the DBC File Manager driven through the browser's file picker, the trace's decoded rows checked.
import { join } from 'node:path';
import { readFileSync } from 'node:fs';
import { expect, WEB } from './harness.mjs';

export const DBC_NAME = 'stage3_test.dbc';
export const DBC_TEXT = readFileSync(join(WEB, 'fixtures', DBC_NAME), 'latin1');
export const NODE = (n) => `stage3_test::${n}`; // the fully qualified node names of the Node boxes

// ---------------------------------------------------------------- the DBC, parsed and decoded here
export function parseDbc(text) {
  const msgs = new Map();
  let cur = null;
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    let m;
    if ((m = line.match(/^BO_ (\d+) (\w+) *: *(\d+) (\w+)/))) {
      const big = BigInt(m[1]);
      cur = { id: Number(big & 0x1fffffffn), ext: (big & 0x80000000n) !== 0n, name: m[2], len: +m[3], sender: m[4], signals: [] };
      msgs.set(cur.id, cur);
    } else if ((m = line.match(/^SG_ (\w+) *(M|m\d+)? *: *(\d+)\|(\d+)@([01])([+-]) \(([^,]+),([^)]+)\) \[[^\]]*\] "([^"]*)"/)) && cur) {
      cur.signals.push({
        name: m[1], mux: m[2] === 'M', muxVal: m[2] && m[2] !== 'M' ? +m[2].slice(1) : null, start: +m[3], size: +m[4],
        intel: m[5] === '1', signed: m[6] === '-', factor: +m[7], offset: +m[8], unit: m[9], type: 'int', values: new Map(),
      });
    } else if ((m = line.match(/^VAL_ (\d+) (\w+) (.*);/))) {
      const sig = msgs.get(Number(BigInt(m[1]) & 0x1fffffffn)).signals.find((s) => s.name === m[2]);
      for (const v of m[3].matchAll(/(-?\d+) "([^"]*)"/g)) sig.values.set(BigInt(v[1]), v[2]);
    } else if ((m = line.match(/^SIG_VALTYPE_ (\d+) (\w+) *: *(\d);/))) {
      msgs.get(Number(BigInt(m[1]) & 0x1fffffffn)).signals.find((s) => s.name === m[2]).type = m[3] === '1' ? 'float' : 'double';
    }
  }
  for (const msg of msgs.values()) msg.signals.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0)); // QMap order
  return msgs;
}
export const DBC = parseDbc(DBC_TEXT);

export const bitsOf = (s) => (s.type === 'float' ? 32 : s.type === 'double' ? 64 : s.size);
export function lastByte(s) {
  const n = bitsOf(s);
  if (s.intel) return (s.start + n - 1) >> 3;
  const inFirst = (s.start % 8) + 1;
  return n <= inFirst ? s.start >> 3 : (s.start >> 3) + Math.ceil((n - inFirst) / 8);
}
export function rawBits(data, s) {
  const n = bitsOf(s);
  let v = 0n;
  let b = s.start;
  for (let i = 0; i < n; i++) {
    const set = (data[b >> 3] >> (b & 7)) & 1;
    if (set) v |= 1n << BigInt(s.intel ? i : n - 1 - i);
    b = s.intel ? b + 1 : b % 8 === 0 ? b + 15 : b - 1;
  }
  return v;
}
/** A signal of a frame as SavvyCAN shows it: { value (number), text ("value unit"), raw } or null if it doesn't fit. */
export function decode(data, s) {
  if (lastByte(s) >= data.length) return null;
  let raw = rawBits(data, s);
  let value, shown;
  if (s.type === 'float' || s.type === 'double') {
    const dv = new DataView(new ArrayBuffer(8));
    if (s.type === 'float') dv.setUint32(0, Number(raw), true);
    else dv.setBigUint64(0, raw, true);
    value = (s.type === 'float' ? dv.getFloat32(0, true) : dv.getFloat64(0, true)) * s.factor + s.offset;
    shown = null; // compared as a number (Qt prints 6 significant digits)
  } else {
    if (s.signed && s.size < 64 && raw & (1n << BigInt(s.size - 1))) raw -= 1n << BigInt(s.size);
    if (s.signed && s.size === 64) raw = BigInt.asIntN(64, raw);
    value = Number(raw) * s.factor + s.offset;
    const isInteger = Number.isInteger(s.factor) && Number.isInteger(s.offset);
    shown = isInteger ? String(raw * BigInt(s.factor) + BigInt(s.offset)) : null;
  }
  if (s.values.has(raw)) shown = s.values.get(raw);
  return { value, shown, raw };
}
/** The signals SavvyCAN's trace shows for a frame: every signal not inside a multiplexor, then the multiplexed ones
 * whose value the multiplexor has. name -> decode() */
export function expectedSignals(msg, data) {
  const out = new Map();
  const mux = msg.signals.find((s) => s.mux);
  const muxVal = mux ? decode(data, mux) : null;
  for (const s of msg.signals) {
    if (s.muxVal !== null && (!muxVal || Number(muxVal.raw) !== s.muxVal)) continue;
    const d = decode(data, s);
    if (d) out.set(s.name, d);
  }
  return out;
}
/** "Name: value unit" (or "value unit") checked against decode(): exact for integers and value descriptions, 6
 * significant digits for the others (QString::number). */
export function checkShown(text, want, sig, what) {
  const valueText = text.slice(0, text.length - (sig.unit.length + 1));
  expect(text.endsWith(' ' + sig.unit), `${what}: unit in "${text}"`).toBe(true);
  if (want.shown !== null) {
    expect(valueText, what).toBe(want.shown);
  } else if (Number.isNaN(want.value)) {
    expect(valueText.toLowerCase(), what).toBe('nan');
  } else {
    const got = Number(valueText);
    expect(Math.abs(got - want.value), `${what}: ${valueText} vs ${want.value}`).toBeLessThanOrEqual(Math.abs(want.value) * 5e-6 + 1e-9);
  }
}


export const dbcState = (page) => page.evaluate(() => window.webSavvyCAN.dbc());
/** The next file picker (Chrome's showOpenFilePicker, which Qt's getOpenFileContent opens) answered with this file. */
export async function pickNext(page, name, text) {
  await page.evaluate(([n, t]) => {
    const real = window.showOpenFilePicker;
    window.showOpenFilePicker = async () => {
      window.showOpenFilePicker = real;
      const f = new File([t], n);
      return [{ kind: 'file', name: n, getFile: async () => f }];
    };
  }, [name, text]);
}
/** What the action hands to the browser to save: a download, or Chrome's save picker (answered as the user would).
 * -> { name, text } */
export async function loadDbc(app, page, name = DBC_NAME, text = DBC_TEXT) {
  const before = (await dbcState(page)).files.length;
  await app.menu('File', 'DBC File Manager');
  await pickNext(page, name, text);
  await app.click('button', /^Load$/);
  await expect.poll(async () => (await dbcState(page)).files.length, { message: `${name} loaded` }).toBe(before + 1);
}
export async function closeWindow(page) {
  await page.keyboard.press('Escape');
  await page.waitForTimeout(300);
}
export async function interpret(app, on) {
  const box = await app.need(app.role('checkbox', 'Interpret Frames'), 'checkbox "Interpret Frames"');
  await app.setChecked(box, on);
  await expect.poll(async () => (await dbcState(app.page)).interpret).toBe(on);
}
/** A menu of the newest graphing window (its menu bar comes after the main window's in the accessibility tree). */
/** The trace's rows split into the decoded lines: [{ id, name (the DBC message), lines: Map(signal -> text), bg, fg }] */
export function parseRows(rows) {
  return rows.map((r) => {
    const lines = r.text.split('\n');
    const at = lines.findIndex((l) => /<\w+>$/.test(l));
    const sigs = new Map();
    if (at >= 0) for (const l of lines.slice(at + 1)) {
      const m = l.match(/^(\w+): (.*)$/);
      if (m) sigs.set(m[1], l);
    }
    return { id: parseInt(r.id, 16), name: at >= 0 ? lines[at].match(/<(\w+)>$/)[1] : null, sigs, lines, bg: r.bg, fg: r.fg };
  });
}
export function checkRow(row, frame, what) {
  const msg = DBC.get(frame.id);
  if (!msg) {
    expect(row.name, `${what}: not in the DBC, not decoded`).toBe(null);
    return 0;
  }
  expect(row.name, `${what}: the message`).toBe(msg.name);
  const want = expectedSignals(msg, frame.data);
  expect([...row.sigs.keys()].sort(), `${what}: the signals shown`).toEqual([...want.keys()].sort());
  for (const [name, d] of want) {
    const sig = msg.signals.find((s) => s.name === name);
    const line = row.sigs.get(name);
    checkShown(line.slice(name.length + 2), d, sig, `${what} ${name}`);
  }
  return want.size;
}

