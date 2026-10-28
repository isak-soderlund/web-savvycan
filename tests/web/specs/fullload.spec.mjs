// Full-load replay (RESOLVE.md "Frame loss in the browser"): rc1's measured full rates for 60 s (classic 8 bytes at
// 1 Mbit/s ~8 000 frames/s, FD 64 bytes BRS at 500k/8M ~6 900/s; resolve-can docs/research/usb-throughput.md),
// with the trace live. Measures the page's responsiveness (a 10 ms timer's lateness, animation frame intervals)
// and checks the frame accounting; numbers to results/fullload-<target>-<mode>.json.
// Tagged @long: run.sh --long (or WSC_LONG=1). WSC_FULLLOAD_SECONDS shortens it.
import { test, expect, fake, settle, saveResult } from '../lib/harness.mjs';

const SECONDS = Number(process.env.WSC_FULLLOAD_SECONDS || 60);

for (const [mode, fd, bitrate, dataBitrate] of [['classic-full', false, 1000000, 0], ['fd64-full', true, 500000, 8000000]]) {
  test(`${mode} for ${SECONDS} s: responsive, every frame accounted for @long`, async ({ app, page, target }) => {
    test.setTimeout((SECONDS + 90) * 1000);
    await app.open();
    await app.connect({ protocol: 'gvret', bitrate, fd, dataBitrate: dataBitrate || 2000000 });
    await expect.poll(async () => (await fake.stats(page)).channel.open).toBe(true);
    await page.evaluate(() => window.__probe.reset());
    await fake.setTraffic(page, mode, { seconds: SECONDS });
    const samples = [];
    for (let t = 0; t < SECONDS; t += 5) {
      await page.waitForTimeout(5000);
      const [s, a] = await Promise.all([fake.stats(page), fake.appStats(page)]);
      samples.push({ t: t + 5, bus: s.bus, encoded: s.encoded, lost: s.lost, received: a.framesReceived, dropped: a.framesDropped,
        inTrace: a.framesInTrace });
    }
    const probe = await fake.probe(page);
    const [s, a] = await settle(page, 20000);
    const heap = await page.evaluate(() => (performance.memory ? performance.memory.usedJSHeapSize : null));
    const result = { target, mode, seconds: SECONDS, fake: s, app: a, probe, heapBytes: heap, samples,
      rateDelivered: Math.round(s.encoded / SECONDS) };
    const file = saveResult(`fullload-${target}-${mode}`, result);
    test.info().annotations.push({ type: 'result', description: file });
    console.log(`${mode}: ${JSON.stringify({ rate: result.rateDelivered, lost: s.lost, dropped: a.framesDropped, timer: probe.timer, raf: probe.raf, longGaps: probe.longGaps.length })}`);
    // Accounting: everything rc1 put out was parsed; the app's own losses are counted.
    expect(a.framesReceived).toBe(s.encoded);
    expect(a.framesInTrace + (a.framesEvicted || 0) + a.framesDropped).toBe(a.framesReceived);
    // rc1's buffers only overflow when the page stops reading: at most a few frames over the run.
    expect(s.lost / s.bus, 'frames lost because the page did not read').toBeLessThan(0.001);
    // Responsive: the 10 ms timer seldom late by more than a frame or two, never a stall over a second.
    expect(probe.timer.p95, 'timer lateness p95 (ms)').toBeLessThan(50);
    expect(probe.timer.max, 'longest stall (ms)').toBeLessThan(1000);
  });
}
