// The real rc1 on the rig, unplugged for real (its hub port off, 5 V too) while frames arrive: the page shows it
// disconnected and keeps working; plugged back in, Add New Device Connection again (rc1 listed without the chooser)
// and frames come again. As traffic.spec's unplug test: no accessibility layer (Qt 6.12's a11y code breaks on unplug),
// mouse clicks at the 1280x800 viewport's positions.
import { test, expect, fake } from '../lib/harness.mjs';
import { arrived, peer, rigBus, usb } from './peer.mjs';

const log = (m) => process.env.WSC_RC1_DEBUG && console.log(new Date().toISOString().slice(11, 23), m);

test.describe('rig: unplugged mid-stream', () => {
  test.use({ fakeOptions: { serial: { remote: true, granted: true } } });
  rigBus(test, { bitrate: 500000 });
  test.afterEach(() => usb('on'));

  test('rc1 unplugged while frames arrive: no page error; plugged back in: reconnects, frames again', async ({ page, app }) => {
    await page.goto('/SavvyCAN.html');
    await page.waitForFunction(() => window.webSavvyCAN, null, { timeout: 60000 });
    await page.waitForTimeout(1500);
    const click = async (x, y) => {
      await page.mouse.click(x, y);
      await page.waitForTimeout(700);
    };
    const addConnection = async (addY) => {
      await click(420, addY);                                // Add New Device Connection
      await click(630, 605);                                 // New Connection: Create New Connection
      await page.waitForFunction(() => window.webSavvyCAN.stats().connected, null, { timeout: 20000 });
      await page.waitForTimeout(5000);                       // Bus Details filled in from the device (ESP32 start)
      await click(282, 604);                                 // Enable Bus
      await click(496, 691);                                 // Save Bus Settings
      await page.waitForFunction(() => window.__rc1.stats().channel.open, null, { timeout: 10000 });
      await page.waitForTimeout(500);
    };
    await click(148, 11);                                    // Connection
    await click(230, 37);                                    // Open Connection Window
    await addConnection(432);
    const first = await peer.send('classic', { count: 500, rate: 500, list: false });
    expect(first.acked, first.error).toBe(500);
    await arrived(page, 500);
    const streaming = peer.send('classic', { count: 4000, rate: 1000, list: false });
    await page.waitForTimeout(1000);
    log('usb off'); usb('off'); log('usb is off');
    await expect.poll(async () => (await fake.appStats(page)).connected, { timeout: 10000 }).toBe(false);
    log('wait for the peer'); await streaming; log('peer done');                                         // the peer's frames now go unACKed: dropped
    const before = (await fake.appStats(page)).framesReceived;
    expect(before).toBeGreaterThan(500);
    log('usb on'); usb('on'); log('usb is on');
    await expect.poll(() => page.evaluate(async () => (await navigator.serial.getPorts()).length), { timeout: 20000 }).toBe(1);
    await page.waitForTimeout(2000);
    log('add again'); await addConnection(401); log('added');
    const again = await peer.send('classic', { count: 500, rate: 500, list: false });
    expect(again.acked, again.error).toBe(500);
    const [, a] = await arrived(page, before + 500);
    expect(a.framesReceived - before).toBe(500);
  });
});
