// RE Tools > Single/Multi State (stage 4, live RE tools): finds the bits that follow a user action. SavvyCAN's window
// had the interface but no search; the fork's (re/retools.cpp) is checked against what was sent:
// - Realtime: the test plays the user: while the window asks for "Go to state N" the fake rc1's 're' traffic holds
//   state N-1 (__rc1.setReState). The matches must be exactly the planted fields: 0x120 bit 19 and bits 26-27,
//   0x18FF0001 (29-bit, CAN FD 64 bytes) bit 270; nothing from counters, the triangle or random bytes.
// - From logged data: a log with known fields; the matches = the reference search (lib/retools-ref.mjs) on its frames,
//   the ID list's unchecked IDs left out.
// Pictures: results/discretestate-{light,dark}.png; numbers: results/discretestate.json.
import { join } from 'node:path';
import { mkdirSync, writeFileSync } from 'node:fs';
import { test, expect, fake, RESULTS, saveResult } from '../lib/harness.mjs';
import { discreteLogged } from '../lib/retools-ref.mjs';

const ds = (page) => page.evaluate(() => window.webSavvyCAN.discreteState());
const hex = (n, w) => n.toString(16).toUpperCase().padStart(w, '0');
const key = (m) => `${m.id.toString(16)}:${m.startBit}:${m.bits}:${m.values.join('/')}`;

/** Plays the user through a realtime run: holds the state asked for, until DONE. Returns the states seen. */
async function playUser(page, ms) {
  const end = Date.now() + ms;
  const asked = [];
  for (;;) {
    const s = await ds(page);
    if (s.state === 'DONE') return { s, asked };
    if (s.state === 'COUNTDOWN_SIGNAL' || s.state === 'GETTING_SIGNAL') {
      await page.evaluate((n) => window.__rc1.setReState(n), s.toggleState);
      if (s.state === 'GETTING_SIGNAL' && asked[asked.length - 1] !== s.label) asked.push(s.label);
    }
    if (Date.now() > end) throw new Error(`realtime run not done: ${JSON.stringify(s)}`);
    await page.waitForTimeout(40);
  }
}

test('single/multi state realtime: 2 states, then 3: exactly the fields that follow the user (CAN FD bit 270 too), ' +
  'nothing from counters, triangles or random bytes; bounded recording; light and dark', async ({ app, page }) => {
  test.setTimeout(180_000);
  await app.open();
  await app.connect({ protocol: 'gvret', fd: true });
  await fake.setTraffic(page, 're', { state: 0 });
  await app.menu('RE Tools', 'Single/Multi State');
  await page.waitForFunction(() => window.webSavvyCAN.discreteState().open, null, { timeout: 10_000 });
  await page.waitForTimeout(500);

  // 2 states, 1 s each, 3 iterations.
  expect(await page.evaluate(() => window.webSavvyCAN.discreteStateRun({ states: 2, realtime: true, freq: 1, iterations: 3 }))).toBe(true);
  let { s, asked } = await playUser(page, 60_000);
  expect(asked, 'asked for each state in turn').toEqual(['Go to state 1', 'Go to state 2', 'Go to state 1', 'Go to state 2', 'Go to state 1', 'Go to state 2']);
  expect(s.matches.map(key), 'exactly the planted fields').toEqual(['120:19:1:0/1', '120:26:1:0/1', '18ff0001:270:1:0/1']);
  expect(s.matches.every((m) => m.share >= 0.9)).toBe(true);
  expect(s.matches[2].ext).toBe(true);
  expect(s.treeRows).toBe(3);
  expect(s.firstRow[0]).toBe('0x120');
  expect(s.sampler.ids, 'every ID recorded').toBe(4);
  expect(s.sampler.memoryBytes).toBeLessThan(32 << 20);
  await page.screenshot({ path: join(RESULTS, 'discretestate-light.png') });
  const two = { matches: s.matches.length, sampler: s.sampler, ms: s.lastRunMs };

  // 3 states: only bits 26-27 take a value per state (0, 1, 2); bit 19 and bit 270 are 0,1,0 and 0,1,0.
  await page.evaluate(() => window.__rc1.setReState(0));
  expect(await page.evaluate(() => window.webSavvyCAN.discreteStateRun({ states: 3, realtime: true, freq: 1, iterations: 3 }))).toBe(true);
  ({ s, asked } = await playUser(page, 90_000));
  expect(asked.length).toBe(9);
  expect(s.matches.map(key), 'the 3-state field').toEqual(['120:26:2:0/1/2']);
  await app.setTheme('dark');
  await page.waitForTimeout(1000);
  await page.screenshot({ path: join(RESULTS, 'discretestate-dark.png') });
  s = await ds(page);
  expect(s.labelColor, 'DONE in the dark theme\'s green').toBe('#40e040');
  await app.setTheme('light');
  saveResult('discretestate', { twoStates: two, threeStates: { matches: s.matches, sampler: s.sampler, ms: s.lastRunMs } });
});

test('single/multi state from logged data: the fields with exactly N values = the reference search on the log, ' +
  'CAN FD fields past byte 8, unchecked IDs left out', async ({ app, page }) => {
  test.setTimeout(120_000);
  // 0x2A0: counter, a 3-valued field at bits 12-13 (0, 1, 3), constants; 0x2A1: bit 5 toggles, byte 3 random;
  // 0x18FF0002 (FD 64): a field at bits 300-301 with values 0, 1, 3 and a 2-valued bit 401.
  const rows = ['Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8'];
  const byId = new Map([[0x2a0, []], [0x2a1, []], [0x18ff0002, []]]);
  let rnd = 7;
  const random = () => ((rnd = (Math.imul(rnd, 1103515245) + 12345) >>> 0) >>> 16) & 0xff;
  const N = 3000;
  for (let k = 0; k < N; k++) {
    const t = 1_000_000 + k * 5000;
    const v3 = [0, 1, 3][Math.floor(k / 70) % 3];
    const fdv = [0, 1, 3][Math.floor(k / 90) % 3];
    const a = [k & 0xff, v3 << 4, 0x5a, 0, 0, 0, 0, 0x11];
    const b = [0, (Math.floor(k / 33) % 2) << 5, 0, random(), 0, 0, 0, 0].slice(0, 4);
    const fd = new Array(64).fill(0);
    fd[37] = fdv << 4;                                // bits 300-301: byte 37 bits 4-5
    fd[50] = (Math.floor(k / 120) % 2) << 1;          // bit 401
    for (const [id, ext, d] of [[0x2a0, false, a], [0x2a1, false, b], [0x18ff0002, true, fd]]) {
      byId.get(id).push(d);
      rows.push(`${t},${hex(id, 8)},${ext},Rx,0,${d.length},${d.map((x) => hex(x, 2)).join(',')}${d.length > 8 ? ',FD+BRS' : ''},`);
    }
  }
  mkdirSync(RESULTS, { recursive: true });
  const file = join(RESULTS, 'discretestate-log.csv');
  writeFileSync(file, rows.join('\n') + '\n');
  await app.open();
  await app.loadFile(file, N * 3, 60_000);
  await app.menu('RE Tools', 'Single/Multi State');
  await page.waitForFunction(() => window.webSavvyCAN.discreteState().open, null, { timeout: 10_000 });
  let s = await ds(page);
  expect(s.ids.map((x) => x.id), 'every ID, checked').toEqual([0x2a0, 0x2a1, 0x18ff0002]);

  const runLogged = async (states, minBits, maxBits) => {
    const runs = (await ds(page)).runs;
    expect(await page.evaluate((o) => window.webSavvyCAN.discreteStateRun(o), { states, realtime: false, minBits, maxBits })).toBe(true);
    await page.waitForFunction((n) => window.webSavvyCAN.discreteState().runs > n, runs, { timeout: 60_000 });
    return ds(page);
  };
  const ref = (ids, states, minBits, maxBits) =>
    ids.flatMap((id) => discreteLogged(id, id > 0x7ff, byId.get(id), states, minBits, maxBits));

  s = await runLogged(3, 1, 8);
  const want3 = ref([0x2a0, 0x2a1, 0x18ff0002], 3, 1, 8);
  expect(s.matches.map(key), 'exactly the reference search').toEqual(want3.map(key));
  expect(s.matches.map(key)).toContain('2a0:12:2:0/1/3');
  expect(s.matches.map(key)).toContain('18ff0002:300:2:0/1/3');
  s = await runLogged(2, 1, 4);
  const want2 = ref([0x2a0, 0x2a1, 0x18ff0002], 2, 1, 4);
  expect(s.matches.map(key)).toEqual(want2.map(key));
  expect(s.matches.map(key)).toContain('2a1:13:1:0/1');
  expect(s.matches.map(key)).toContain('18ff0002:401:1:0/1');
  // 0x2A1 unchecked: its fields go.
  await page.evaluate(() => window.webSavvyCAN.discreteStateIds([0x2a1], false));
  s = await runLogged(2, 1, 4);
  expect(s.matches.map(key)).toEqual(ref([0x2a0, 0x18ff0002], 2, 1, 4).map(key));
  expect(s.matches.some((m) => m.id === 0x2a1)).toBe(false);
});
