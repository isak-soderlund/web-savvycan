// The real rc1 on the rig, CAN FD 500k/2M, with a DBC (stage 3): the peer sends the messages of the test DBC
// (tests/web/fixtures/stage3_test.dbc: classic, 12- and 64-byte FD, a 29-bit FD ID, a multiplexed message, plus an ID the
// DBC doesn't have; can_peer.py pattern `dbc`). Interpret Frames decodes every frame of the trace as the DBC says
// (checked against the decoder in lib/dbc.mjs on the frames the peer sent), and RE Tools > Signal Viewer follows the
// bus live: its values change while the peer sends, and at the end each is the newest frame's value.
// Pictures: results/rig-dbc-{trace,signalviewer}-{light,dark}.png (look at them).
import { join } from 'node:path';
import { test, expect, RESULTS } from '../lib/harness.mjs';
import { DBC, NODE, decode, expectedSignals, checkShown, checkRow, parseRows, loadDbc, closeWindow, interpret } from '../lib/dbc.mjs';
import { arrived, peer, rigBus } from './peer.mjs';

const BUS = { bitrate: 500000, dbitrate: 2000000 };
const CONNECT = { protocol: 'gvret', bitrate: 500000, fd: true, dataBitrate: 2000000 };

test.describe('rig: DBC decoding on a live bus', () => {
  test.use({ fakeOptions: { serial: { remote: true } } });
  rigBus(test, BUS);

  test('Interpret Frames: every frame from the peer decoded as the DBC says (classic, FD 12/64, 29-bit, multiplexed)',
    async ({ app, page }) => {
      await app.open();
      await loadDbc(app, page);
      await closeWindow(page);
      await app.connect(CONNECT);
      await interpret(app, true);
      const sent = await peer.send('dbc', { count: 1200, rate: 600 });
      expect(sent.acked, sent.error).toBe(1200);
      const [, a] = await arrived(page, 1200, 20000);
      expect(a.framesReceived, 'every frame from rc1').toBe(1200);
      expect(a.framesDropped).toBe(0);
      // the trace's rows follow the frame store on its update timer
      await expect.poll(async () => page.evaluate(() => window.webSavvyCAN.traceDecoded(1200).length), { message: 'every frame a row' }).toBe(1200);
      const rows = parseRows(await page.evaluate((n) => window.webSavvyCAN.traceDecoded(n), 1200));
      let signals = 0, decoded = 0;
      rows.forEach((row, i) => {
        const f = sent.frames[i];
        expect(row.id, `row ${i}: the frame sent`).toBe(f.id);
        const n = checkRow(row, f, `row ${i} 0x${f.id.toString(16)}`);
        signals += n;
        if (DBC.has(f.id)) decoded++;
      });
      expect(decoded, 'every frame of a DBC message decoded').toBe(1100);
      const want = sent.frames.reduce((n, f) => n + (DBC.has(f.id) ? expectedSignals(DBC.get(f.id), f.data).size : 0), 0);
      expect(signals, 'every signal value shown and checked').toBe(want);
      console.log(`rig decoded trace: ${rows.length} rows, ${signals} signal values checked`);
      await page.screenshot({ path: join(RESULTS, 'rig-dbc-trace-light.png') });
      await app.setTheme('dark');
      await page.waitForTimeout(800);
      await page.screenshot({ path: join(RESULTS, 'rig-dbc-trace-dark.png') });
      await app.setTheme('light');
    });

  test('Signal Viewer: live values while the peer sends, each the newest frame\'s value at the end', async ({ app, page }) => {
    await app.open();
    await loadDbc(app, page);
    await closeWindow(page);
    await app.connect(CONNECT);
    await app.menu('RE Tools', 'Signal Viewer');
    await expect.poll(async () => (await page.evaluate(() => window.webSavvyCAN.signalViewer())).open).toBe(true);
    const viewed = [[0x1db, 'LBC', 'LBC_Battery', 'BatteryVoltage'], [0x1db, 'LBC', 'LBC_Battery', 'BatteryStatus'],
      [0x1da, 'INV', 'INV_Motor', 'MotorSpeed'], [0x5bc, 'LBC', 'LBC_Capacity', 'Temperature'], [0x50b, 'VCM', 'VCM_HVAC', 'HvacPower'],
      [0x709, 'TEST', 'TEST_FD12', 'MotoAcross'], [0x70f, 'TEST', 'TEST_FD64', 'HighBits'], [0x70f, 'TEST', 'TEST_FD64', 'Byte40Word'],
      [0x1234576, 'TEST', 'TEST_FDX64', 'ExtLast']];
    for (const [, node, msg, sig] of viewed) {
      expect(await page.evaluate(([n, m, s]) => window.webSavvyCAN.signalViewerPick(n, m, s), [NODE(node), msg, sig]), `${msg}.${sig} picked`).toBe(true);
      await app.click('button', 'Add To Viewer');
    }
    const rowsNow = async () => (await page.evaluate(() => window.webSavvyCAN.signalViewer())).rows;
    expect((await rowsNow()).map((r) => r.signal)).toEqual(viewed.map((v) => v[3]));

    // 12 messages in turn at 600 frames/s: each viewed message 50 times a second for 5 s; the values sampled meanwhile.
    const sending = peer.send('dbc', { count: 3000, rate: 600 });
    const seen = viewed.map(() => new Set());
    const t0 = Date.now();
    while (Date.now() - t0 < 4000) {
      (await rowsNow()).forEach((r, i) => r.value && seen[i].add(r.value));
      await page.waitForTimeout(250);
    }
    const sent = await sending;
    expect(sent.acked, sent.error).toBe(3000);
    await arrived(page, 3000);
    await page.waitForTimeout(800);
    seen.forEach((s, i) => expect(s.size, `${viewed[i][3]}: live, several values while the bus ran`).toBeGreaterThanOrEqual(viewed[i][3] === 'BatteryStatus' ? 2 : 5));
    const rows = await rowsNow();
    viewed.forEach(([id, , msg, name], i) => {
      const newest = sent.frames.filter((f) => f.id === id).pop();
      const s = DBC.get(id).signals.find((x) => x.name === name);
      expect(rows[i].signal).toBe(name);
      checkShown(rows[i].value, decode(newest.data, s), s, `Signal Viewer ${msg}.${name}`);
    });
    await page.screenshot({ path: join(RESULTS, 'rig-dbc-signalviewer-light.png') });
    await app.setTheme('dark');
    await page.waitForTimeout(800);
    await page.screenshot({ path: join(RESULTS, 'rig-dbc-signalviewer-dark.png') });
    await app.setTheme('light');
  });
});
