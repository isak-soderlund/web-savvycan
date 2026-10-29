// RE Tools > Sniffer (stage 4, live RE tools): every ID of the live bus with its newest bytes, changing bytes green
// (up) or red (down), notching to hide the bits that change anyway so a user action stands out, filters, expiry,
// CAN FD 64 bytes, light and dark. The fake rc1's 're' traffic (fake/rc1.js) has known patterns: 0x120 byte 0 counts,
// byte 1 constant 0x5A, byte 2 bit 3 and byte 3 bits 2-3 follow the "state" (__rc1.setReState), 0x121 a triangle,
// 0x130 random bytes, 0x18FF0001 (29-bit, FD 64 bytes, BRS) byte 40 counts, byte 63 constant, byte 33 bit 6 the state.
// The full-rate run with every live tool open is retools-fullrate.spec.
// Pictures: results/sniffer-{light,dark}.png, results/sniffer-bits-{light,dark}.png.
import { join } from 'node:path';
import { test, expect, fake, RESULTS, saveResult } from '../lib/harness.mjs';

const sn = (page) => page.evaluate(() => window.webSavvyCAN.sniffer());
const act = (page, action, id = 0, on = true) =>
  page.evaluate(([a, i, o]) => window.webSavvyCAN.snifferDo(a, i, o), [action, id, on]);
const setState = (page, n) => page.evaluate((x) => window.__rc1.setReState(x), n);
const row = (s, id) => s.rows.find((r) => r.id === id);
const RE_IDS = [0x120, 0x121, 0x130, 0x18ff0001];

/** Waits until rc1 has nothing queued and the trace stops growing. */
async function quiet(page, ms = 20000) {
  const end = Date.now() + ms;
  let last = -1, same = 0;
  for (;;) {
    const [s, a] = [await fake.stats(page), await fake.appStats(page)];
    same = s.queuedBytes === 0 && a.queued === 0 && a.framesReceived === last ? same + 1 : 0;
    last = a.framesReceived;
    if (same >= 4 || Date.now() > end) return a;
    await page.waitForTimeout(250);
  }
}

/** The newest frame of each ID in the trace. */
async function newestById(page, n) {
  const tail = await page.evaluate((k) => window.webSavvyCAN.traceTail(k), n);
  const m = new Map();
  for (const f of tail) m.set(f.id, f);
  return m;
}

test('sniffer: the live bus per ID, its newest bytes, counting bytes marked and constants not; notch hides the steady ' +
  'changes so a toggled bit stands out; unnotch, filters, expiry; CAN FD 64 bytes; light and dark', async ({ app, page }) => {
  test.setTimeout(150_000);
  await app.open();
  await app.connect({ protocol: 'gvret', fd: true });
  await app.menu('RE Tools', 'Sniffer');
  await page.waitForFunction(() => window.webSavvyCAN.sniffer().open, null, { timeout: 10_000 });

  await fake.setTraffic(page, 're', { state: 0 });
  await page.waitForTimeout(2000);
  let s = await sn(page);
  expect(s.rows.map((r) => r.id), 'every ID of the bus, in ID order').toEqual(RE_IDS);
  expect(s.ids.map((r) => r.id).sort((a, b) => a - b), 'every ID in the filter list, checked').toEqual(RE_IDS);
  expect(s.ids.every((r) => r.on)).toBe(true);
  expect(row(s, 0x120).len).toBe(8);
  expect(row(s, 0x18ff0001).len, 'CAN FD: 64 bytes').toBe(64);

  // Counting bytes are marked as changing (up, green), constant ones never; sampled over a few marker periods.
  const counts = { b0: 0, b40: 0, samples: 0 };
  for (let i = 0; i < 6; i++) {
    s = await sn(page);
    const a = row(s, 0x120), f = row(s, 0x18ff0001);
    counts.samples++;
    if (a.changes[0] !== 0) counts.b0++;
    if (f.changes[40] !== 0) counts.b40++;
    expect(a.changes[1], '0x120 byte 1 is constant').toBe(0);
    expect(a.changes.slice(4), '0x120 bytes 4..7 are constant').toEqual([0, 0, 0, 0]);
    expect(f.changes[63], 'FD byte 63 is constant').toBe(0);
    expect(f.changes.filter((c, j) => c !== 0 && ![40, 50, 51].includes(j)), 'FD: only the counter and the triangle change').toEqual([]);
    await page.waitForTimeout(300);
  }
  expect(counts.b0, 'the counter byte shows changes').toBeGreaterThanOrEqual(5);
  expect(counts.b40, 'the FD counter byte (byte 40) shows changes').toBeGreaterThanOrEqual(5);

  // Notch for 5 s (a click every 300 ms: every marker period of 750 ms is taken in): the counters, the triangle and
  // the random bytes are notched; then the state changes: only its bits show a change.
  await app.click('button', 'Notch', 'the Notch button');
  for (let i = 0; i < 16; i++) {
    await act(page, 'notch');
    await page.waitForTimeout(300);
  }
  s = await sn(page);
  expect(row(s, 0x120).notch[0], '0x120 counter byte notched').toBe(0xff);
  expect(row(s, 0x120).notch[2], 'the state byte not notched').toBe(0);
  expect(row(s, 0x18ff0001).notch[40]).toBe(0xff);
  for (const r of s.rows) expect(r.changes.filter((c) => c !== 0), `0x${r.id.toString(16)}: no change shown once notched`).toEqual([]);
  await setState(page, 1);
  let seen = null;
  for (let i = 0; i < 60 && !seen; i++) {
    await page.waitForTimeout(50);
    const x = await sn(page);
    if (row(x, 0x120).changes[2] !== 0) seen = x;
  }
  expect(seen, 'the state change shows within two marker periods').not.toBeNull();
  const changed = [];
  for (const r of seen.rows) r.changes.forEach((c, j) => c !== 0 && changed.push(`${r.id.toString(16)}:${j}:${c}`));
  expect(changed.sort(), 'only the state\'s bytes changed, up (green)').toEqual(['120:2:1', '120:3:1', '18ff0001:33:1'].sort());
  await page.screenshot({ path: join(RESULTS, 'sniffer-light.png') });

  // Unnotch: the counters show their changes again.
  await act(page, 'unnotch');
  await page.waitForTimeout(1600);
  s = await sn(page);
  expect(row(s, 0x120).notch[0]).toBe(0);
  expect(row(s, 0x120).changes[0], 'the counter shows changes again').not.toBe(0);

  // Fade: bytes that haven't changed for long are older (sequence interval) than the counter's.
  await act(page, 'fade', 0, true);
  await page.waitForTimeout(1000);
  s = await sn(page);
  expect(s.cbFadeInactive).toBe(true);
  expect(row(s, 0x120).seqInterval[1], 'a constant byte ages').toBeGreaterThan(row(s, 0x120).seqInterval[0]);

  // Filters: 0x130 unchecked -> not shown; All -> back.
  await act(page, 'filter', 0x130, false);
  await page.waitForTimeout(400);
  s = await sn(page);
  expect(s.filtering).toBe(true);
  expect(s.rows.map((r) => r.id), 'filtered').toEqual(RE_IDS.filter((id) => id !== 0x130));
  await act(page, 'all');
  await page.waitForTimeout(400);
  expect((await sn(page)).rows.map((r) => r.id)).toEqual(RE_IDS);

  // View Bits, pictures light and dark.
  await act(page, 'viewBits', 0, true);
  await page.waitForTimeout(800);
  await page.screenshot({ path: join(RESULTS, 'sniffer-bits-light.png') });
  await app.setTheme('dark');
  await page.waitForTimeout(1000);
  await page.screenshot({ path: join(RESULTS, 'sniffer-bits-dark.png') });
  await act(page, 'viewBits', 0, false);
  await page.waitForTimeout(800);
  await page.screenshot({ path: join(RESULTS, 'sniffer-dark.png') });
  await app.setTheme('light');

  // The bus stops: the bytes shown are exactly the newest frame of each ID; every frame was seen once.
  await fake.setTraffic(page, 'idle');
  const st = await quiet(page);
  s = await sn(page);
  const newest = await newestById(page, 400);
  for (const id of RE_IDS) expect(row(s, id).data, `0x${id.toString(16)}: the newest frame's bytes`).toEqual(newest.get(id).data);
  expect(s.framesSeen, 'every frame of the bus seen once').toBe(st.framesReceived);

  // Expiry: with Never Expire the IDs stay; without, they go after the expire interval (1 s here).
  await act(page, 'noExpire', 0, true);
  await act(page, 'expire', 1000);
  await page.waitForTimeout(1800);
  expect((await sn(page)).rows.length, 'Never Expire: still listed').toBe(4);
  await act(page, 'noExpire', 0, false);
  await page.waitForTimeout(1200);
  s = await sn(page);
  expect(s.rows.length, 'expired').toBe(0);
  expect(s.ids.length, 'and gone from the filter list').toBe(0);
  saveResult('sniffer', { framesSeen: s.framesSeen, framesReceived: st.framesReceived });
});
