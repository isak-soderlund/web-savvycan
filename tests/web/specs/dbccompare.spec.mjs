// RE Tools > DBC Comparison (stage 4, file-based RE tools): compares two DBC files, each loaded with the window's own
// button through the browser's file picker (its own DBCFile, not the DBC File Manager's). Two DBCs with known
// differences: a node and a message on one side only, signals missing on either side, a signal moved, resized,
// rescaled, a 64-byte CAN FD message's signals past byte 8. Then a larger pair (1000 messages, every one with a signal
// missing in the first file: SavvyCAN's second loop ran on the message index and read past the message list).
// Pictures: results/dbccompare-{light,dark}.png.
import { join } from 'node:path';
import { test, expect, RESULTS } from '../lib/harness.mjs';
import { writeText, nextPick, savedText, node, kids, waitOpen } from '../lib/retools.mjs';

const DBC_A = `VERSION ""

BU_: VCU BMS INV

BO_ 1960 MSG_A: 8 VCU
 SG_ Speed : 0|16@1+ (0.1,0) [0|6553.5] "km/h" BMS
 SG_ Gear : 16|4@1+ (1,0) [0|15] "" BMS
 SG_ OnlyInA : 24|8@1+ (1,0) [0|255] "" BMS

BO_ 2016 MSG_FD: 64 BMS
 SG_ CellV : 320|16@1+ (0.001,0) [0|65.535] "V" VCU

BO_ 1000 MSG_ONLY_A: 8 INV
 SG_ X : 0|8@1+ (1,0) [0|255] "" VCU
`;
const DBC_B = `VERSION ""

BU_: VCU BMS CHG

BO_ 1960 MSG_A: 8 VCU
 SG_ Speed : 0|16@1+ (0.05,0) [0|3276.75] "km/h" BMS
 SG_ Gear : 20|4@1+ (1,0) [0|15] "" BMS
 SG_ OnlyInB : 32|8@1+ (1,0) [0|255] "" BMS

BO_ 2016 MSG_FD: 64 BMS
 SG_ CellV : 320|12@1+ (0.001,-1) [0|65.535] "V" VCU
 SG_ CellT : 504|8@1+ (1,-40) [-40|215] "C" VCU

BO_ 1001 MSG_ONLY_B: 8 CHG
 SG_ Y : 0|8@1+ (1,0) [0|255] "" VCU
`;
const state = (page) => page.evaluate(() => window.webSavvyCAN.dbcCompare());

async function loadBoth(app, page, a, b) {
  await nextPick(page, a);
  await app.click('button', 'Load New File');
  await expect.poll(async () => (await state(page)).first.loaded, { message: 'first DBC loaded', timeout: 60_000 }).toBe(true);
  await nextPick(page, b);
  await app.click('button', 'Load A File');
  await expect.poll(async () => (await state(page)).second.loaded, { message: 'second DBC loaded', timeout: 60_000 }).toBe(true);
  await expect.poll(async () => (await state(page)).tree.length, { message: 'compared' }).toBe(2);
  return state(page);
}

test('two DBCs with known differences: nodes, messages, missing and modified signals (CAN FD past byte 8), save', async ({ app, page }) => {
  test.setTimeout(90_000);
  const a = writeText('dbccompare-a.dbc', DBC_A), b = writeText('dbccompare-b.dbc', DBC_B);
  await app.open();
  await app.menu('RE Tools', 'DBC Comparison');
  await waitOpen(page, 'dbcCompare');
  const s = await loadBoth(app, page, a, b);
  expect([s.firstLabel, s.secondLabel]).toEqual(['dbccompare-a.dbc', 'dbccompare-b.dbc']);
  expect([s.first.messages, s.second.messages]).toEqual([3, 3]);
  const t = s.tree;
  expect(t.map((n) => n.t)).toEqual(['Node Differences', 'Message Differences']);
  expect(kids(node(t, 'Node Differences', 'Nodes not found in first'))).toEqual(['CHG']);
  expect(kids(node(t, 'Node Differences', 'Nodes not found in second'))).toEqual(['INV']);
  const m = ['Message Differences'];
  expect(kids(node(t, ...m, 'Messages not found in first'))).toEqual(['MSG_ONLY_B (0x3E9)']);
  expect(kids(node(t, ...m, 'Messages not found in second'))).toEqual(['MSG_ONLY_A (0x3E8)']);
  const missing = [...m, 'Messages with missing signals'];
  expect(kids(node(t, ...missing, 'Missing from first'))).toEqual(['MSG_A (0x7A8)', 'MSG_FD (0x7E0)']);
  expect(kids(node(t, ...missing, 'Missing from first', 'MSG_A'))).toEqual(['OnlyInB']);
  expect(kids(node(t, ...missing, 'Missing from first', 'MSG_FD')), 'a signal at byte 63').toEqual(['CellT']);
  expect(kids(node(t, ...missing, 'Missing from second'))).toEqual(['MSG_A (0x7A8)']);
  expect(kids(node(t, ...missing, 'Missing from second', 'MSG_A'))).toEqual(['OnlyInA']);
  const mod = [...m, 'Modified Signals'];
  expect(kids(node(t, ...mod))).toEqual(['MSG_A (0x7A8)', 'MSG_FD (0x7E0)']);
  expect(kids(node(t, ...mod, 'MSG_A'))).toEqual(['Gear', 'Speed']);
  const gear = kids(node(t, ...mod, 'MSG_A', 'Gear'));
  expect(gear).toHaveLength(1);
  expect(gear[0]).toMatch(/^Start Bit\s+First DBC: 16\s+Second: 20$/);
  expect(kids(node(t, ...mod, 'MSG_A', 'Speed'))[0]).toMatch(/^Factor\s+First DBC: 0\.1\s+Second: 0\.05$/);
  const cell = kids(node(t, ...mod, 'MSG_FD', 'CellV'));
  expect(cell[0]).toMatch(/^Size\s+First DBC: 16\s+Second: 12$/);
  expect(cell[1]).toMatch(/^Bias\s+First DBC: 0\s+Second: -1$/);

  await page.waitForTimeout(500);
  await page.screenshot({ path: join(RESULTS, 'dbccompare-light.png') });
  await app.setTheme('dark');
  await page.waitForTimeout(800);
  await page.screenshot({ path: join(RESULTS, 'dbccompare-dark.png') });
  await app.setTheme('light');

  const saved = await savedText(app, page, 'Save Details to File');
  expect(saved, 'the details came out').toBeTruthy();
  const lines = saved.text.split('\n');
  expect(lines[0]).toBe('Node Differences');
  expect(lines).toContain('            OnlyInB');
});

test('1000 messages, each with a signal only in the second file: every one listed, in well under a second', async ({ app, page }) => {
  test.setTimeout(90_000);
  const msgs = (extra) => Array.from({ length: 1000 }, (_, i) =>
    `BO_ ${0x100 + i} M${i}: 8 VCU\n SG_ S${i} : 0|8@1+ (1,0) [0|255] "" BMS\n` +
    (extra ? ` SG_ T${i} : 8|8@1+ (1,0) [0|255] "" BMS\n` : '')).join('\n');
  const a = writeText('dbccompare-big-a.dbc', `VERSION ""\n\nBU_: VCU BMS\n\n${msgs(false)}`);
  const b = writeText('dbccompare-big-b.dbc', `VERSION ""\n\nBU_: VCU BMS\n\n${msgs(true)}`);
  await app.open();
  await app.menu('RE Tools', 'DBC Comparison');
  await waitOpen(page, 'dbcCompare');
  const s = await loadBoth(app, page, a, b);
  expect([s.first.messages, s.second.messages]).toEqual([1000, 1000]);
  const first = node(s.tree, 'Message Differences', 'Messages with missing signals', 'Missing from first');
  expect(first.c.length, 'every message').toBe(1000);
  expect(first.c[999].c.map((x) => x.t)).toEqual(['T999']);
  expect(kids(node(s.tree, 'Message Differences', 'Modified Signals'))).toEqual([]);
  expect(s.compareMs, `compared in ${s.compareMs} ms`).toBeLessThan(1000);
});
