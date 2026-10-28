// Logging (RESOLVE.md MVP #7, LOG_TO_DISK_LIVE off): File > Start Continuous Logging starts at once, Cease Continuous
// Logging starts one download of SavvyCAN's native GVRET CSV, named by the start time; every frame the fake sent is in
// it exactly (every FD length, BRS, 29-bit, classic and remote frames), and it loads in the desktop SavvyCAN (its own
// parser, nativecsv_tool, when tests/desktop/run.sh has built it). File > Choose the Log's Name When Logging Stops:
// the name asked at Cease is the download's.
import { existsSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { test, expect, fake, settle, RESULTS, NATIVECSV, savvycanCheck } from '../lib/harness.mjs';

test.use({ fakeOptions: { serial: { device: { record: true } } } });

async function saved(download, page, name) {
  const file = join(RESULTS, name);
  await download.saveAs(file);
  const text = readFileSync(file, 'latin1');
  return { file, text, parsed: await page.evaluate((t) => RC1Fake.parseSavvyCsv(t), text) };
}

test('log every FD length: one download on stop, contents equal what rc1 sent', async ({ app, page }) => {
  await app.open();
  await app.connect({ protocol: 'gvret', fd: true });
  await app.startLog();
  const name = (await fake.appStats(page)).logFile;
  expect(name, 'named by the start time').toMatch(/^SavvyCAN_20\d\d-\d\d-\d\d_\d\d-\d\d-\d\d\.csv$/);
  await fake.setTraffic(page, 'fd-mix', { seconds: 3 });
  await page.waitForTimeout(3300);
  await settle(page);
  await page.waitForTimeout(500);
  expect((await fake.appStats(page)).logging, 'still logging until Cease').toBe(true);
  const dl = await app.stopLog();
  expect(dl.suggestedFilename()).toBe(name);
  const sent = await fake.recorded(page);
  expect(sent.length).toBe(1500);
  const { file, parsed } = await saved(dl, page, 'log.csv');
  expect(parsed.header).toBe('Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8');
  expect(parsed.frames.length).toBe(sent.length);
  const norm = (f) => ({ id: f.id, ext: f.ext, fd: f.fd, brs: f.fd && f.brs, data: f.data });
  expect(parsed.frames.map(norm)).toEqual(sent.map(norm));
  expect(parsed.frames.filter((f) => f.fd && f.data.length > 8).length, 'CAN FD rows over 8 bytes').toBeGreaterThan(100);
  expect(parsed.frames.some((f) => f.fd && f.brs), 'FD+BRS rows').toBe(true);
  // Time stamps: µs, increasing.
  const ts = parsed.frames.map((f) => f.ts);
  expect(ts.every((t, i) => i === 0 || t >= ts[i - 1])).toBe(true);
  expect((await fake.appStats(page)).logFramesWritten).toBe(sent.length);
  if (existsSync(NATIVECSV)) {
    test.info().annotations.push({ type: 'savvycan', description: savvycanCheck(file, '--exact', String(sent.length), '--first', '-1') });
  }
});

test('logging with nothing on the bus: a download with the header only', async ({ app, page }) => {
  await app.open();
  await app.startLog();
  const dl = await app.stopLog();
  expect(dl.suggestedFilename()).toMatch(/^SavvyCAN_20\d\d-\d\d-\d\d_\d\d-\d\d-\d\d\.csv$/);
  const { parsed } = await saved(dl, page, 'log-empty.csv');
  expect(parsed.header).toBe('Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8');
  expect(parsed.frames.length).toBe(0);
});

test.describe('the name asked at Cease', () => {
  test.use({ viewport: { width: 1400, height: 900 } }); // the name field's position below
  test('"Choose the Log\'s Name When Logging Stops": Keep Logging keeps it running; the typed name is the download\'s', async ({ app, page }) => {
    await app.open();
    await app.connect({ protocol: 'gvret', fd: true });
    await app.menu('File', 'Choose the Log');                   // a checkable item
    await app.startLog();
    await fake.setTraffic(page, 'fd-mix', { seconds: 2 });
    await page.waitForTimeout(2300);
    await settle(page);
    await app.menu('File', 'Cease Continuous Logging');
    await app.click('button', 'Keep Logging');
    await page.waitForTimeout(500);
    expect((await fake.appStats(page)).logging, 'Keep Logging: still logging').toBe(true);
    await app.menu('File', 'Cease Continuous Logging');
    await page.getByRole('textbox').first().waitFor({ timeout: 10000 });
    // A real click into the field (Qt's window then gets the keys; the accessibility element's box is not on screen):
    // the dialog sits in the middle of the 1400x900 window.
    await page.mouse.click(690, 405);
    await page.keyboard.press('Control+A');
    await page.keyboard.type('leaf-drive-1');
    const dl = page.waitForEvent('download');
    await app.click('button', 'Download');
    const d = await dl;
    expect(d.suggestedFilename()).toBe('leaf-drive-1.csv');
    const { parsed } = await saved(d, page, 'log-named.csv');
    expect(parsed.frames.length).toBe((await fake.recorded(page)).length);
  });
});
