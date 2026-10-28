// Logging to disk (RESOLVE.md MVP #7): started and stopped through the UI, the file (OPFS behind the mocked picker)
// in SavvyCAN's native GVRET CSV, every frame the fake sent in it exactly, committed while it runs.
import { test, expect, fake, settle } from '../lib/harness.mjs';

test.use({ fakeOptions: { serial: { device: { record: true } } } });

async function readLog(page, name) {
  return page.evaluate(async (n) => {
    const text = await window.__pickers.read(n);
    return { text, parsed: RC1Fake.parseSavvyCsv(text) };
  }, name);
}

test('log every FD length to a file: exact contents, committed while running', async ({ app, page }) => {
  await app.open('commit=1000');
  await page.evaluate(() => window.__pickers.clear());
  await app.connect({ protocol: 'gvret', fd: true });
  await expect.poll(async () => (await fake.stats(page)).channel.open).toBe(true);
  await app.startLog();
  await fake.setTraffic(page, 'fd-mix', { seconds: 4 });
  // Committed while running: the file on disk grows before the log is stopped (a crash loses at most the last seconds).
  await page.waitForTimeout(2600);
  const name = (await fake.appStats(page)).logFile;
  expect(name, 'the log file is named').toBeTruthy();
  const mid = await readLog(page, name);
  expect(mid.parsed.frames.length, 'rows on disk while logging').toBeGreaterThan(500);
  await page.waitForTimeout(1700);
  await settle(page);
  await app.stopLog();
  const sent = await fake.recorded(page);
  const { parsed } = await readLog(page, name);
  expect(parsed.header).toBe('Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8');
  expect(parsed.frames.length).toBe(sent.length);
  const norm = (f) => ({ id: f.id, ext: f.ext, fd: f.fd, brs: f.fd && f.brs, data: f.data });
  expect(parsed.frames.map(norm)).toEqual(sent.map(norm));
  // Time stamps: µs, increasing.
  const ts = parsed.frames.map((f) => f.ts);
  expect(ts.every((t, i) => i === 0 || t >= ts[i - 1])).toBe(true);
  const calls = await page.evaluate(() => window.__pickers.calls);
  expect(calls.length, 'one picker call').toBe(1);
});

test('the start time names the file when logging starts without a prompt', async ({ app, page }) => {
  await app.open();
  await page.evaluate(() => window.__pickers.clear());
  await app.startLog();
  await app.stopLog();
  const files = await page.evaluate(() => window.__pickers.list());
  expect(files.length).toBe(1);
  expect(files[0].name).toMatch(/20\d\d-\d\d-\d\d/);
});
