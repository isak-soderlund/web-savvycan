// DBC (stage 3, switched on 2026-10-30): File > DBC File Manager (the browser's file picker to load, a download to
// save, remembered for the next visit), Interpret Frames in the trace, the DBC editors, graphs by signal name (Add New
// Graph's "Graph a DBC Signal" and the trace's "Add to graph"), RE Tools > Signal Viewer, File > Save Decoded Frames
// (text and CSV). CAN FD: messages up to 64 bytes, signals past bit 63, 64-bit integers, floats, an extended FD ID.
// The DBC is tests/web/fixtures/stage3_test.dbc (our own: the Leaf IDs the fake's `leaf` traffic sends, with signals
// of every kind, and FD test messages). Every decoded value is checked against a decoder written here from the DBC
// rules (Intel/Motorola bit numbering as SavvyCAN's, factor, offset, value descriptions on the raw value).
// Pictures: results/dbc-*.png (light and dark).
import { join } from 'node:path';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { test, expect, fake, settle, RESULTS, WEB } from '../lib/harness.mjs';
import { DBC_NAME, DBC_TEXT, NODE, parseDbc, DBC, bitsOf, lastByte, rawBits, decode, expectedSignals, checkShown, dbcState, pickNext, loadDbc, closeWindow, interpret, parseRows, checkRow } from '../lib/dbc.mjs';
// ---------------------------------------------------------------- a log of every message, classic and FD
const hex = (b) => b.toString(16).toUpperCase().padStart(2, '0');
function rng(seed) {
  let x = seed >>> 0;
  return () => ((x = (Math.imul(x, 1664525) + 1013904223) >>> 0) >>> 24);
}
/** [{t, id, ext, fd, data}] and the GVRET CSV of them: every DBC message ~25 times with random bytes (floats and
 * doubles set to known numbers), plus an ID the DBC doesn't have. */
function makeLog() {
  const r = rng(12345);
  const frames = [];
  let t = 1e6;
  const ids = [...DBC.keys(), 0x123];
  for (let k = 0; k < 25; k++) {
    for (const id of ids) {
      const msg = DBC.get(id);
      const len = msg ? msg.len : 8;
      const data = Array.from({ length: len }, () => r());
      if (id === 0x70f) {
        const dv = new DataView(new ArrayBuffer(64));
        dv.setFloat32(20, k * 0.5 - 3.25, true);
        dv.setFloat64(32, (k - 12) * 1234.5678, true);
        for (let i = 20; i < 24; i++) data[i] = dv.getUint8(i);
        for (let i = 32; i < 40; i++) data[i] = dv.getUint8(i);
      }
      if (id === 0x5c0) data[0] = (data[0] & 0xfc) | (k % 4); // the multiplexor: 0, 1, 2, 3 in turn
      frames.push({ t, id, ext: !!(msg && msg.ext), fd: len > 8, data });
      t += 1000;
    }
  }
  const rows = ['Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8'];
  for (const f of frames) {
    const cols = f.fd ? f.data : [...f.data, ...Array(8 - f.data.length).fill(0)];
    rows.push(`${f.t},${f.id.toString(16).toUpperCase().padStart(8, '0')},${f.ext},Rx,0,${f.data.length},${cols.map(hex).join(',')},${f.fd ? 'FD+BRS,' : ''}`);
  }
  mkdirSync(RESULTS, { recursive: true });
  const file = join(RESULTS, 'dbc-log.csv');
  writeFileSync(file, rows.join('\n') + '\n');
  return { frames, file };
}

// ---------------------------------------------------------------- driving the windows
async function captureSave(page, action) {
  await page.evaluate(() => {
    window.__saved = null;
    window.showSaveFilePicker = async (opts) => {
      const parts = [];
      const name = opts?.suggestedName || 'saved';
      return {
        kind: 'file', name,
        createWritable: async () => ({
          write: async (chunk) => { parts.push(chunk?.data ?? chunk); },
          close: async () => { window.__saved = { name, text: await new Blob(parts).text() }; },
        }),
      };
    };
  });
  const download = page.waitForEvent('download', { timeout: 20000 }).catch(() => null);
  await action();
  const picked = page.waitForFunction(() => window.__saved, null, { timeout: 20000 }).then((h) => h.jsonValue()).catch(() => null);
  const downloaded = download.then(async (d) => (d ? { name: d.suggestedFilename(), text: readFileSync(await d.path(), 'utf8') } : null));
  const got = await Promise.race([picked.then((v) => v || downloaded), downloaded.then((v) => v || picked)]);
  expect(got, 'the file came out').toBeTruthy();
  return got;
}
/** File > DBC File Manager > Load with `text` picked as `name`; waits until it is loaded. */
async function graphMenu(page, top, item) {
  await page.getByRole('menuitem', { name: new RegExp(`^${top}$`) }).last().click({ force: true });
  await page.getByRole('menuitem', { name: new RegExp(`^${item}$`) }).last().click({ force: true });
}
// ---------------------------------------------------------------- the tests
test('DBC File Manager loads the DBC; Interpret Frames decodes classic and FD frames exactly; Save Decoded Frames', async ({ app, page }) => {
  test.setTimeout(180_000);
  const { frames, file } = makeLog();
  await app.open();
  await loadDbc(app, page);
  const st = await dbcState(page);
  expect(st.files[0].name, 'the file as picked').toBe(DBC_NAME);
  expect(st.managerRows, 'its row in the manager').toEqual([DBC_NAME]);
  const byId = new Map(st.files[0].messages.map((m) => [m.id, m]));
  expect(byId.size, 'every message').toBe(DBC.size);
  for (const [id, m] of DBC) {
    const got = byId.get(id);
    expect([got.name, got.len, got.ext, got.signals.length], `0x${id.toString(16)}`).toEqual([m.name, m.len, m.ext, m.signals.length]);
  }
  expect([byId.get(0x70f).fd, byId.get(0x709).fd, byId.get(0x1234576).fd, byId.get(0x1db).fd], 'CAN FD messages').toEqual([true, true, true, false]);
  const fd64 = byId.get(0x70f).signals.find((s) => s.name === 'HighBits');
  expect([fd64.startBit, fd64.size], 'a signal past bit 63').toEqual([500, 7]);
  await page.screenshot({ path: join(RESULTS, 'dbc-manager-light.png') });
  await closeWindow(page);

  await app.loadFile(file, frames.length);
  await interpret(app, true);
  const rows = parseRows(await page.evaluate((n) => window.webSavvyCAN.traceDecoded(n), frames.length));
  expect(rows.length).toBe(frames.length);
  let checked = 0;
  rows.forEach((row, i) => {
    expect(row.id, `row ${i}: the frame's ID`).toBe(frames[i].id);
    checked += checkRow(row, frames[i], `row ${i} (0x${frames[i].id.toString(16)})`);
  });
  console.log(`decoded trace: ${rows.length} rows, ${checked} signal values checked`);
  expect(checked).toBeGreaterThan(600);

  // Row colours: a message's own background with black text on it; its own text colour where it reads; else the theme's.
  const lastOf = (id) => [...rows].reverse().find((r) => r.id === id);
  const plain = lastOf(0x11a);
  expect([lastOf(0x1db).bg, lastOf(0x1db).fg], 'LBC_Battery: #ffe0e0 with black text').toEqual(['#ffe0e0', '#000000']);
  expect([lastOf(0x55b).bg, lastOf(0x55b).fg], 'LBC_SOC: its own colours').toEqual(['#203040', '#ffffff']);
  expect(lastOf(0x1da).fg, 'INV_Motor: its blue text on the light theme').toBe('#0000ff');
  await page.screenshot({ path: join(RESULTS, 'dbc-trace-light.png') });
  await app.setTheme('dark');
  await page.waitForTimeout(800);
  const dark = parseRows(await page.evaluate(() => window.webSavvyCAN.traceDecoded(40)));
  const darkOf = (id) => [...dark].reverse().find((r) => r.id === id);
  expect([darkOf(0x1db).bg, darkOf(0x1db).fg], 'dark: LBC_Battery keeps its background, black text').toEqual(['#ffe0e0', '#000000']);
  expect(darkOf(0x1da).fg, 'dark: the blue text (unreadable on dark) is the theme\'s text').not.toBe('#0000ff');
  const lum = (c) => parseInt(c.slice(1, 3), 16) + parseInt(c.slice(3, 5), 16) + parseInt(c.slice(5, 7), 16);
  expect(lum(darkOf(0x1da).fg), 'dark: light text').toBeGreaterThan(400);
  expect(lum(darkOf(0x11a).bg), `dark: a message without colours on the dark rows (light: ${plain.bg})`).toBeLessThan(300);
  await page.screenshot({ path: join(RESULTS, 'dbc-trace-dark.png') });

  // File > Save Decoded Frames: the text download, every frame with its decoded signals.
  const txt = await captureSave(page, () => app.menu('File', /^Save Decoded Frames$/i));
  writeFileSync(join(RESULTS, 'dbc-decoded.txt'), txt.text);
  const blocks = txt.text.split(/\n(?=Time: )/);
  expect(blocks.length, 'a block per frame').toBe(frames.length);
  blocks.forEach((b, i) => {
    const f = frames[i];
    const lines = b.split('\n');
    expect(parseInt(lines[0].match(/ID: (0x[0-9A-F]+)/)[1], 16), `block ${i}: ID`).toBe(f.id);
    expect(lines[0], `block ${i}: length`).toContain(`Len: ${f.data.length}`);
    const msg = DBC.get(f.id);
    if (!msg) return;
    // Save Decoded Frames lists every signal that fits (the multiplexed ones too, as SavvyCAN), "\tName: value unit"
    const shown = new Map(lines.filter((l) => l.startsWith('\t')).map((l) => [l.slice(1).split(':')[0], l.slice(1)]));
    for (const s of msg.signals) {
      const d = decode(f.data, s);
      checkShown(shown.get(s.name).slice(s.name.length + 2), d, s, `decoded text, frame ${i} ${s.name}`);
    }
  });

  // File > Save Decoded Frames CSV: a column per signal (as SavvyCAN: "Name: value unit" in each).
  const csv = await captureSave(page, () => app.menu('File', 'Save Decoded Frames CSV'));
  writeFileSync(join(RESULTS, 'dbc-decoded.csv'), csv.text);
  const csvLines = csv.text.trim().split('\n');
  expect(csvLines.length, 'header + a row per frame').toBe(frames.length + 1);
  const header = csvLines[0].split(',');
  expect(header.slice(0, 4)).toEqual(['Time', 'ID', 'Bus', 'DataLen']);
  csvLines.slice(1).forEach((line, i) => {
    const f = frames[i];
    const cells = line.split(',');
    expect(parseInt(cells[1], 16), `CSV row ${i}: ID`).toBe(f.id);
    const msg = DBC.get(f.id);
    if (!msg) return;
    for (const s of msg.signals) {
      const col = header.indexOf(s.name);
      const cell = cells.find((c, k) => k >= 4 && c.startsWith(s.name + ': '));
      expect(col, `CSV: a column for ${s.name}`).toBeGreaterThan(3);
      checkShown(cell.slice(s.name.length + 2), decode(f.data, s), s, `CSV row ${i} ${s.name}`);
    }
  });
});

test('graphs by DBC signal name: Add New Graph and the trace\'s "Add to graph" equal the decoded values', async ({ app, page }) => {
  test.setTimeout(120_000);
  const { frames, file } = makeLog();
  await app.open();
  await loadDbc(app, page);
  await closeWindow(page);
  await app.loadFile(file, frames.length);
  const valuesOf = (id, name) => {
    const s = DBC.get(id).signals.find((x) => x.name === name);
    return frames.filter((f) => f.id === id).map((f) => decode(f.data, s).value);
  };

  // RE Tools > New Graphing Window, Graph > Add New Graph, "Graph a DBC Signal": node, message, signal, Copy Signal Parameters
  await app.menu('RE Tools', 'New Graphing Window');
  for (const [i, [msg, sig, id]] of [['TEST_FD64', 'Tail32', 0x70f], ['TEST_FD64', 'Double64', 0x70f], ['LBC_Capacity', 'Temperature', 0x5bc]].entries()) {
    await graphMenu(page, 'Graph', 'Add New Graph');
    await expect.poll(async () => (await page.evaluate(() => window.webSavvyCAN.graphDialog())).open).toBe(true);
    expect(await page.evaluate(([n, m, s]) => window.webSavvyCAN.graphDialogPick(n, m, s), [NODE(msg.startsWith('TEST') ? 'TEST' : 'LBC'), msg, sig]), `${sig} picked`).toBe(true);
    await app.click('button', 'Copy Signal Parameters');
    if (i === 0) {
      const dlg = await page.evaluate(() => window.webSavvyCAN.graphDialog());
      expect([dlg.txtStartBit, dlg.txtDataLen, dlg.gridBytes], 'Tail32 copied, the 64-byte grid').toEqual(['487', '32', 64]);
      await page.screenshot({ path: join(RESULTS, 'dbc-graph-dialog-light.png') });
    }
    await app.click('button', 'Add this graph');
    await expect.poll(async () => (await page.evaluate(() => window.webSavvyCAN.graphState())).graphs).toBe(i + 1);
    const pts = await page.evaluate((k) => window.webSavvyCAN.graphPoints(k), i);
    expect([pts.name, pts.signal], 'the graph is the DBC signal').toEqual([sig, sig]);
    const want = valuesOf(id, sig);
    expect(pts.values.length, `${sig}: a point per frame`).toBe(want.length);
    pts.values.forEach((v, k) => expect(Math.abs(v - want[k]), `${sig} point ${k}: ${v} vs ${want[k]}`).toBeLessThanOrEqual(Math.abs(want[k]) * 1e-6));
  }
  await page.screenshot({ path: join(RESULTS, 'dbc-graph-light.png') });
  await closeWindow(page);

  // The trace's context menu over a decoded signal line: Add to a new graphing window.
  await interpret(app, true);
  const rows = parseRows(await page.evaluate((n) => window.webSavvyCAN.traceDecoded(n), frames.length));
  const fromEnd = [...rows].reverse().findIndex((r) => r.id === 0x1db);
  const line = rows[rows.length - 1 - fromEnd].lines.findIndex((l) => l.startsWith('BatteryVoltage:'));
  expect(line, 'the signal\'s line in the cell').toBeGreaterThan(0);
  expect(await page.evaluate(([r, l]) => window.webSavvyCAN.traceContextMenu(r, l), [fromEnd, line])).toBe(true);
  await app.click('menuitem', 'Add to a new graphing window');
  await expect.poll(async () => (await page.evaluate(() => window.webSavvyCAN.graphState())).graphs, { message: 'a graph from the trace' }).toBe(1);
  const pts = await page.evaluate(() => window.webSavvyCAN.graphPoints(0));
  expect([pts.name, pts.signal]).toEqual(['BatteryVoltage', 'BatteryVoltage']);
  expect(pts.values, 'BatteryVoltage: the decoded values').toEqual(valuesOf(0x1db, 'BatteryVoltage'));
  await app.setTheme('dark');
  await page.waitForTimeout(800);
  await page.screenshot({ path: join(RESULTS, 'dbc-graph-dark.png') });
});

test('Signal Viewer shows live DBC values (Leaf bus, then CAN FD), equal to the newest frame', async ({ app, page }) => {
  test.setTimeout(120_000);
  await app.open();
  await loadDbc(app, page);
  await closeWindow(page);
  await app.connect({ protocol: 'gvret', fd: true });
  await fake.setTraffic(page, 'leaf');
  await app.menu('RE Tools', 'Signal Viewer');
  await expect.poll(async () => (await page.evaluate(() => window.webSavvyCAN.signalViewer())).open).toBe(true);
  const add = async (node, msg, sig) => {
    expect(await page.evaluate(([n, m, s]) => window.webSavvyCAN.signalViewerPick(n, m, s), [NODE(node), msg, sig]), `${msg}.${sig} picked`).toBe(true);
    await app.click('button', 'Add To Viewer');
  };
  const viewed = [[0x1db, 'LBC', 'LBC_Battery', 'BatteryVoltage'], [0x1db, 'LBC', 'LBC_Battery', 'BatteryStatus'], [0x1da, 'INV', 'INV_Motor', 'MotorSpeed'],
    [0x5bc, 'LBC', 'LBC_Capacity', 'Temperature'], [0x50b, 'VCM', 'VCM_HVAC', 'HvacPower']];
  for (const [, node, msg, sig] of viewed) await add(node, msg, sig);
  const rowsNow = async () => (await page.evaluate(() => window.webSavvyCAN.signalViewer())).rows;
  await expect.poll(async () => (await rowsNow()).every((r) => r.value), { message: 'every signal has a value' }).toBe(true);
  const first = (await rowsNow()).map((r) => r.value);
  await page.waitForTimeout(1500);
  const later = (await rowsNow()).map((r) => r.value);
  expect(later.filter((v, i) => v !== first[i]).length, `live: values change (${first} -> ${later})`).toBeGreaterThanOrEqual(3);

  // CAN FD: fd-mix sends 0x70F (64 bytes), 0x709 (12) and the extended 0x1234576 (64)
  await fake.setTraffic(page, 'fd-mix');
  viewed.push([0x70f, 'TEST', 'TEST_FD64', 'HighBits'], [0x70f, 'TEST', 'TEST_FD64', 'Wide64'], [0x709, 'TEST', 'TEST_FD12', 'MotoAcross'],
    [0x1234576, 'TEST', 'TEST_FDX64', 'ExtLast'], [0x1234576, 'TEST', 'TEST_FDX64', 'ExtU64']);
  for (const [, node, msg, sig] of viewed.slice(5)) await add(node, msg, sig);
  await page.waitForTimeout(3000);
  await page.screenshot({ path: join(RESULTS, 'dbc-signalviewer-light.png') });
  await fake.setTraffic(page, 'idle');
  await settle(page);
  await page.waitForTimeout(800);
  const rows = await rowsNow();
  const tail = await page.evaluate(() => window.webSavvyCAN.traceTail(5000));
  viewed.forEach(([id, , , name], i) => {
    const newest = [...tail].reverse().find((f) => f.id === id);
    expect(newest, `a frame of 0x${id.toString(16)} in the trace`).toBeTruthy();
    const s = DBC.get(id).signals.find((x) => x.name === name);
    expect(rows[i].signal).toBe(name);
    checkShown(rows[i].value, decode(newest.data, s), s, `Signal Viewer ${name}`);
  });
  await app.setTheme('dark');
  await page.waitForTimeout(800);
  await page.screenshot({ path: join(RESULTS, 'dbc-signalviewer-dark.png') });
});

test('DBC editors with CAN FD; a DBC saved (download), loaded back the same; remembered after a reload', async ({ app, page }) => {
  test.setTimeout(120_000);
  await app.open();
  await loadDbc(app, page);
  const original = (await dbcState(page)).files[0];

  // Edit: the main editor, then the 64-byte message's editor and a signal past bit 63 in the signal editor
  expect(await page.evaluate(() => window.webSavvyCAN.dbcSelectFile(0))).toBe(true);
  await app.click('button', /^Edit$/);
  await expect.poll(async () => (await page.evaluate(() => window.webSavvyCAN.dbcEditors())).main, { message: 'the DBC editor' }).toBe(true);
  await page.waitForTimeout(500);
  await page.screenshot({ path: join(RESULTS, 'dbc-editor-light.png') });
  expect(await page.evaluate(() => window.webSavvyCAN.dbcEdit('TEST_FD64'))).toBe(true);
  await expect.poll(async () => (await page.evaluate(() => window.webSavvyCAN.dbcEditors())).message?.lineFrameLen).toBe('64');
  const me = (await page.evaluate(() => window.webSavvyCAN.dbcEditors())).message;
  expect([me.lineMsgName, me.checkCanFD, me.checkBRS], 'the message editor').toEqual(['TEST_FD64', true, true]);
  await page.screenshot({ path: join(RESULTS, 'dbc-message-editor-light.png') });
  await closeWindow(page);
  expect(await page.evaluate(() => window.webSavvyCAN.dbcEdit('TEST_FD64', 'HighBits'))).toBe(true);
  await expect.poll(async () => (await page.evaluate(() => window.webSavvyCAN.dbcEditors())).signal?.txtName).toBe('HighBits');
  const se = (await page.evaluate(() => window.webSavvyCAN.dbcEditors())).signal;
  expect([se.txtBitLength, se.gridBytes], 'the signal editor: 7 bits, the 64-byte grid').toEqual(['7', 64]);
  // the message's signals: Scaled 88..99, Double64 256..319, Byte40Word 320..335, Wide64 384..447, Tail32 (Motorola
  // 487) bytes 60..63, HighBits 500..506
  for (const b of [88, 99, 256, 319, 320, 335, 384, 447, 487, 480, 511, 500, 506]) expect(se.usedBits, `bit ${b} marked as a signal's`).toContain(b);
  for (const b of [100, 336, 448, 460, 479]) expect(se.usedBits, `bit ${b} not a signal's`).not.toContain(b);
  await page.screenshot({ path: join(RESULTS, 'dbc-signal-editor-light.png') });
  await app.setTheme('dark');
  await page.waitForTimeout(800);
  await page.screenshot({ path: join(RESULTS, 'dbc-signal-editor-dark.png') });
  await closeWindow(page);
  await closeWindow(page);  // the main editor
  await page.waitForTimeout(300);

  // Save (the manager's selected row): a download of the DBC
  expect(await page.evaluate(() => window.webSavvyCAN.dbcSelectFile(0))).toBe(true);
  const saved = await captureSave(page, () => app.click('button', /^Save$/));
  writeFileSync(join(RESULTS, 'dbc-saved.dbc'), saved.text);
  expect(saved.text).toMatch(/^BO_ 1807 TEST_FD64: 64 TEST$/m);
  expect(saved.text).toMatch(/^BO_ 2166572406 TEST_FDX64: 64 TEST$/m);
  expect(saved.text).toMatch(/SG_ HighBits : 500\|7@1\+/);
  await page.screenshot({ path: join(RESULTS, 'dbc-manager-dark.png') });

  // Remove it, load the saved file back: the same messages and signals
  await app.click('button', /^Remove$/);
  await expect.poll(async () => (await dbcState(page)).files.length).toBe(0);
  await pickNext(page, 'stage3_saved.dbc', saved.text);
  await app.click('button', /^Load$/);
  await expect.poll(async () => (await dbcState(page)).files.length).toBe(1);
  const norm = (f) => f.messages.map((m) => ({ ...m, signals: [...m.signals].sort((a, b) => a.name.localeCompare(b.name)) }))
    .sort((a, b) => a.id - b.id);
  const back = (await dbcState(page)).files[0];
  expect(back.name).toBe('stage3_saved.dbc');
  expect(norm(back), 'saved and loaded back: the same DBC').toEqual(norm(original));

  // The next visit: the DBC is loaded again (the page keeps it)
  await closeWindow(page);
  await app.reload();
  await expect.poll(async () => (await dbcState(page)).files.length, { message: 'remembered after a reload' }).toBe(1);
  const again = (await dbcState(page)).files[0];
  expect(again.name).toBe('stage3_saved.dbc');
  expect(norm(again), 'the same messages and signals').toEqual(norm(original));
});

test('a big DBC and a big log: load time, Interpret Frames, Save Decoded Frames of 100 000 frames of 50 messages (measured)', async ({ app, page }) => {
  test.setTimeout(240_000);
  // 1 000 messages of 16 signals (8 Intel, 8 Motorola, 4 bits each, factor 0.5): ~1.2 MB of DBC
  const lines = ['VERSION ""', '', 'NS_ :', '', 'BS_:', '', 'BU_: BIG', ''];
  for (let m = 0; m < 1000; m++) {
    lines.push(`BO_ ${0x100 + m} BIG_${m}: 8 BIG`);
    for (let s = 0; s < 16; s++) {
      const intel = s < 8;
      const start = intel ? s * 4 : 32 + (s - 8) * 4 + 3;
      lines.push(` SG_ S${m}_${s} : ${start}|4@${intel ? 1 : 0}+ (0.5,1) [1|8.5] "u" BIG`);
    }
    lines.push('');
  }
  const big = lines.join('\n');
  const N = 100000;
  const r = rng(7);
  const rows = ['Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8'];
  for (let i = 0; i < N; i++) {
    const id = 0x100 + (i % 50); // 50 of the DBC's messages: the CSV has a column per signal of every message seen
    rows.push(`${1e6 + i * 1000},${id.toString(16).toUpperCase().padStart(8, '0')},false,Rx,0,8,${Array.from({ length: 8 }, () => hex(r())).join(',')},`);
  }
  const file = join(RESULTS, 'dbc-big-log.csv');
  writeFileSync(file, rows.join('\n') + '\n');

  await app.open();
  const t0 = Date.now();
  await loadDbc(app, page, 'big.dbc', big);
  const loadMs = Date.now() - t0;
  const st = await dbcState(page);
  expect(st.files[0].messages.length).toBe(1000);
  await closeWindow(page);
  await app.loadFile(file, N, 60000);
  const mem0 = await page.evaluate(() => window.webSavvyCAN.memory());
  await interpret(app, true);
  const t1 = Date.now();
  const shown = await page.evaluate(() => window.webSavvyCAN.traceDecoded(200));
  const decode200Ms = Date.now() - t1;
  expect(shown[199].text).toContain(`<BIG_${(N - 1) % 50}>`);
  const t2 = Date.now();
  const csv = await captureSave(page, () => app.menu('File', 'Save Decoded Frames CSV'));
  const csvMs = Date.now() - t2;
  const t3 = Date.now();
  const txt = await captureSave(page, () => app.menu('File', /^Save Decoded Frames$/i));
  const txtMs = Date.now() - t3;
  const mem1 = await page.evaluate(() => window.webSavvyCAN.memory());
  writeFileSync(join(RESULTS, 'dbc-big-decoded.csv'), csv.text);
  writeFileSync(join(RESULTS, 'dbc-big-decoded.txt'), txt.text);
  expect(csv.text.trim().split('\n').length).toBe(N + 1);
  expect(txt.text.split(/\n(?=Time: )/).length).toBe(N);
  const result = {
    dbcBytes: big.length, dbcLoadMs: loadMs, frames: N, decode200RowsMs: decode200Ms,
    csvBytes: csv.text.length, csvMs, txtBytes: txt.text.length, txtMs,
    heapUsedBefore: mem0.heapUsed, heapUsedAfter: mem1.heapUsed, maxFootprint: mem1.maxFootprint, heapSize: mem1.heapSize,
  };
  console.log('dbc-big', JSON.stringify(result));
  writeFileSync(join(RESULTS, 'dbc-big.json'), JSON.stringify(result, null, 2));
});
