// Tabs instead of windows (owner 2026-10-30, RESOLVE.md "Tabs instead of windows"; wasm/windowtabs.h): in the browser
// every non-modal window (the graphing window, Frame Data Analysis, Temporal Graph, the connection window, ...) opens
// as a tab of the main window, in the menu bar row. The owner's report: a click on the main window beside a graph
// buried the graph (a page has no window manager). Here: a click beside a tool's content never loses it; the tab bar
// switches, reopens and closes; the main screen is always the first tab; a live graph keeps taking frames while its tab
// is in the background; many tabs stay reachable (elided, then the bar scrolls). Clicks are real mouse clicks on the
// places the test hook reports (webSavvyCAN.tabs()). Pictures: results/tabs/<theme>-<what>.png.
import { join } from 'node:path';
import { test, expect, fake, RESULTS } from '../lib/harness.mjs';

const tabs = (page) => page.evaluate(() => window.webSavvyCAN.tabs());
const current = async (page) => {
  const s = await tabs(page);
  return s.tabs[s.current];
};
const shot = (page, theme, what) => page.screenshot({ path: join(RESULTS, 'tabs', `${theme}-${what}.png`) });
/** Waits until a tab of this window class is the current one. */
async function waitCurrent(page, cls) {
  await page.waitForFunction((c) => {
    const s = window.webSavvyCAN.tabs();
    return s.tabs[s.current] && s.tabs[s.current].class === c;
  }, cls, { timeout: 15000 });
}
/** A real click on a tab (its middle, away from the close button). */
async function clickTab(page, index) {
  const t = (await tabs(page)).tabs[index];
  await page.mouse.click(t.x + Math.min(30, t.width / 3), t.y + t.height / 2);
  await page.waitForTimeout(300);
}
/** A real click on a tab's close button (it shows on the current and the hovered tab). */
async function clickClose(page, index) {
  const t = (await tabs(page)).tabs[index];
  await page.mouse.move(t.x + t.width / 2, t.y + t.height / 2);
  await page.waitForTimeout(200);
  await page.mouse.click(t.x + t.width - 6 - 9, t.y + t.height / 2);
  await page.waitForTimeout(400);
}
/** Share of pixels that differ between two screenshots' areas below the tab row (the page shown changed). */
async function areaDiffers(page, a, b) {
  return page.evaluate(async ([a64, b64]) => {
    const load = (s) => new Promise((r) => { const i = new Image(); i.onload = () => r(i); i.src = 'data:image/png;base64,' + s; });
    const [ia, ib] = await Promise.all([load(a64), load(b64)]);
    const c = new OffscreenCanvas(ia.width, ia.height), g = c.getContext('2d');
    g.drawImage(ia, 0, 0);
    const da = g.getImageData(0, 40, ia.width, ia.height - 80).data;
    g.drawImage(ib, 0, 0);
    const db = g.getImageData(0, 40, ia.width, ia.height - 80).data;
    let n = 0;
    for (let i = 0; i < da.length; i += 16) if (Math.abs(da[i] - db[i]) + Math.abs(da[i + 1] - db[i + 1]) + Math.abs(da[i + 2] - db[i + 2]) > 30) n++;
    return n / (da.length / 16);
  }, [a.toString('base64'), b.toString('base64')]);
}

// The four windows of the owner's report, each opened as its menu does.
const WINDOWS = [
  { name: 'graph', cls: 'GraphingWindow', open: (page) => page.evaluate(() => window.webSavvyCAN.addGraph(0x1da, 0, 8, true)) },
  { name: 'frameInfo', cls: 'FrameInfoWindow', open: (page, app) => app.menu('RE Tools', 'Frame Data Analysis') },
  { name: 'temporal', cls: 'TemporalGraphWindow', open: (page, app) => app.menu('RE Tools', 'Temporal Graph') },
  { name: 'connection', cls: 'ConnectionWindow', open: (page, app) => app.menu('Connection', 'Open Connection Window') },
];

for (const theme of ['light', 'dark']) {
  test.describe(theme, () => {
    test.use({ colorScheme: theme });

    test(`${theme}: a click beside a tool's content never buries it; the main screen is the first tab`, async ({ app, page }) => {
      test.setTimeout(180_000);
      await app.open();
      await app.setTheme(theme);
      await app.connect({ protocol: 'gvret', fd: false });
      await fake.setTraffic(page, 'leaf', { seconds: 4 });
      await page.waitForTimeout(1500);
      const main = await page.screenshot();
      let s = await tabs(page);
      expect(s.tabs.length, 'only the main screen open').toBe(1);
      expect(s.tabs[0], 'the first tab is the main screen').toMatchObject({ class: 'MainWindow', closable: false, current: true });
      for (const w of WINDOWS) {
        await w.open(page, app);
        await waitCurrent(page, w.cls);
        await page.waitForTimeout(600);
        s = await tabs(page);
        expect(s.topLevels, `${w.name}: no floating window (only the main window is a top-level)`).toBe(1);
        expect(s.pageWidth, `${w.name}: its page fills the width`).toBeGreaterThanOrEqual(1270);
        const before = await page.screenshot();
        if (w.name === 'graph') await shot(page, theme, 'graph');
        // The owner's click: on the page beside where the floating window used to be (its right half, the bottom).
        for (const [x, y] of [[1100, 450], [1200, 760], [640, 700]]) {
          await page.mouse.click(x, y);
          await page.waitForTimeout(250);
        }
        expect((await current(page)).class, `${w.name}: still the tab in front after clicks beside it`).toBe(w.cls);
        expect(await areaDiffers(page, main, await page.screenshot()), `${w.name}: its page is shown, not the main screen`).toBeGreaterThan(0.05);
        // to the main screen and back, by mouse on the tabs
        await clickTab(page, 0);
        expect((await current(page)).class, 'the main screen').toBe('MainWindow');
        expect(await areaDiffers(page, main, await page.screenshot()), 'the main screen is shown').toBeLessThan(0.2);
        await clickTab(page, (await tabs(page)).tabs.findIndex((t) => t.class === w.cls));
        expect((await current(page)).class, `${w.name}: back`).toBe(w.cls);
        expect(await areaDiffers(page, before, await page.screenshot()), `${w.name}: back as it was`).toBeLessThan(0.2);
      }
      s = await tabs(page);
      expect(s.tabs.map((t) => t.class), 'one tab each, the main screen first').toEqual(['MainWindow', ...WINDOWS.map((w) => w.cls)]);
      expect(s.tabs.map((t) => t.title)).toEqual(['Frames', 'Graph', 'Frame Data Analysis', 'Temporal Graph', 'Connection']);
      expect(s.tabs.every((t) => t.visible), 'every tab in view').toBe(true);
      await shot(page, theme, 'four-tools');
      await clickTab(page, 0);
      await shot(page, theme, 'four-tools-main');
    });

    test(`${theme}: reopening switches to the tab, close buttons close as SavvyCAN does`, async ({ app, page }) => {
      await app.open();
      await app.setTheme(theme);
      await app.menu('RE Tools', 'Frame Data Analysis');
      await waitCurrent(page, 'FrameInfoWindow');
      await clickTab(page, 0);
      // opened again from its menu: its tab comes to the front, no second one
      await app.menu('RE Tools', 'Frame Data Analysis');
      await waitCurrent(page, 'FrameInfoWindow');
      expect((await tabs(page)).tabs.length, 'one Frame Data Analysis tab').toBe(2);
      // graphing windows: SavvyCAN opens a new one each time, numbered tabs
      await page.evaluate(() => window.webSavvyCAN.addGraph(0x1da, 0, 8, true));
      await page.evaluate(() => window.webSavvyCAN.addGraph(0x1db, 0, 8, true));
      await page.waitForFunction(() => window.webSavvyCAN.tabs().tabs.length === 4, null, { timeout: 10000 });
      let s = await tabs(page);
      expect(s.tabs.map((t) => t.title).slice(2)).toEqual(['Graph', 'Graph 2']);
      // the close button: shown on the current tab, not on the others until hovered; the main screen has none
      expect(s.tabs[3].closeShown, 'close button on the current tab').toBe(true);
      expect(s.tabs[1].closeShown, 'no close button on a tab in the background').toBe(false);
      expect(s.tabs[0].closable).toBe(false);
      await page.mouse.move(s.tabs[1].x + s.tabs[1].width / 2, s.tabs[1].y + s.tabs[1].height / 2);
      await page.waitForTimeout(200);
      expect((await tabs(page)).tabs[1].closeShown, 'shown on the hovered tab').toBe(true);
      await shot(page, theme, 'hover');
      // close the current graph (its × by mouse): the tab on its left comes to the front (no tab on its right)
      await clickClose(page, 3);
      s = await tabs(page);
      expect(s.tabs.map((t) => t.title), 'the second graph closed').toEqual(['Frames', 'Frame Data Analysis', 'Graph']);
      expect(s.current).toBe(2);
      // close Frame Data Analysis from the background (hover, then its ×): SavvyCAN's close (the window's state: closed)
      await clickClose(page, 1);
      s = await tabs(page);
      expect(s.tabs.map((t) => t.class)).toEqual(['MainWindow', 'GraphingWindow']);
      expect((await page.evaluate(() => window.webSavvyCAN.frameInfo())).open, 'Frame Data Analysis closed').toBe(false);
      // reopened from its menu after the close: a tab again, working
      await app.menu('RE Tools', 'Frame Data Analysis');
      await waitCurrent(page, 'FrameInfoWindow');
      expect((await page.evaluate(() => window.webSavvyCAN.frameInfo())).open, 'Frame Data Analysis open again').toBe(true);
      // a click on the main screen tab's place never closes it (it has no close button)
      await clickClose(page, 0);
      expect((await tabs(page)).tabs[0].class).toBe('MainWindow');
    });
  });
}

test('a live graph keeps taking frames while its tab is in the background', async ({ app, page }) => {
  await app.open();
  await app.connect({ protocol: 'gvret', fd: false });
  await fake.setTraffic(page, 'leaf', { seconds: 10 });
  await page.waitForTimeout(1000);
  expect(await page.evaluate(() => window.webSavvyCAN.addGraph(0x1da, 0, 8, true))).toBe(true);
  await waitCurrent(page, 'GraphingWindow');
  await page.waitForTimeout(1000);
  await clickTab(page, 0);
  expect((await current(page)).class).toBe('MainWindow');
  const first = await page.evaluate(() => window.webSavvyCAN.graphState());
  await page.waitForTimeout(3000);
  const later = await page.evaluate(() => window.webSavvyCAN.graphState());
  expect(later.open, 'the graph is open (its tab in the background)').toBe(true);
  expect(later.points, `points grow in the background (${first.points} -> ${later.points})`).toBeGreaterThan(first.points + 200);
  expect(later.lastKey - first.lastKey, 'newer data (µs)').toBeGreaterThan(2e6);
  await clickTab(page, 1);
  const shown = await page.evaluate(() => window.webSavvyCAN.graphState());
  expect(Math.abs(shown.xUpper - shown.lastKey), 'it follows the end when shown again').toBeLessThan(1e6);
});

test('the connection window is a tab: clicks elsewhere keep it, its menu brings it back, connect specs close it', async ({ app, page }) => {
  await app.open();
  await app.menu('Connection', 'Open Connection Window');
  await waitCurrent(page, 'ConnectionWindow');
  await page.mouse.click(1200, 700);
  await page.mouse.click(640, 760);
  await page.waitForTimeout(300);
  expect((await current(page)).class, 'still in front').toBe('ConnectionWindow');
  await clickTab(page, 0);
  await app.menu('Connection', 'Open Connection Window');
  await waitCurrent(page, 'ConnectionWindow');
  expect((await tabs(page)).tabs.length, 'one connection tab').toBe(2);
  // Escape in a tab doesn't close it (a stray key would lose a tool)
  await page.keyboard.press('Escape');
  await page.waitForTimeout(300);
  expect((await current(page)).class).toBe('ConnectionWindow');
  await app.closeConnectionWindow();
  expect((await tabs(page)).tabs.map((t) => t.class)).toEqual(['MainWindow']);
});

test('many tabs stay reachable: they shrink to elided titles, then the bar scrolls; the page\'s reserve is kept', async ({ app, page }) => {
  await app.open();
  for (let i = 0; i < 12; i++) await page.evaluate(() => window.webSavvyCAN.addGraph(0x1da, 0, 8, true));
  await page.waitForFunction(() => window.webSavvyCAN.tabs().tabs.length === 13, null, { timeout: 20000 });
  let s = await tabs(page);
  expect(s.barLeft + s.barWidth, 'the bar stays in the row').toBeLessThanOrEqual(1280);
  expect(s.natural, 'they would want more than the row').toBeGreaterThan(s.barWidth);
  // every tab reachable: selected one by one from the last, each comes to the front and into view
  for (const i of [12, 6, 1, 0]) {
    await page.evaluate((k) => window.webSavvyCAN.selectTab(k), i);
    s = await tabs(page);
    expect(s.current).toBe(i);
    expect(s.tabs[i].visible, `tab ${i} scrolled into view`).toBe(true);
    // the main screen's tab is pinned: in view however the tools' tabs are scrolled
    expect(s.tabs[0].x, 'Frames pinned at the start of the bar').toBe(s.barLeft);
  }
  // the page's texts take the row's right end (website): the bar stops before them
  await page.evaluate(() => window.savvycanTopBar.setReserve(500));
  s = await tabs(page);
  expect(s.barLeft + s.barWidth, 'the bar ends before the reserve').toBeLessThanOrEqual(1280 - 500);
  await page.evaluate(() => window.savvycanTopBar.setReserve(0));
  // all closed again, one by one with their close buttons: back to the main screen only
  for (let n = 12; n > 0; n--) expect(await page.evaluate((k) => window.webSavvyCAN.closeTab(k), n)).toBe(true);
  s = await tabs(page);
  expect(s.tabs.map((t) => t.class)).toEqual(['MainWindow']);
  expect(s.current).toBe(0);
});

/** The page answers within 5 s (a hung page never answers an evaluate). */
const answers = (page) => Promise.race([page.evaluate(() => true), new Promise((r) => setTimeout(() => r(false), 5000))]);

test('reopening a window and clicking in it never hangs the page (screen-reader layer on)', async ({ app, page }) => {
  // the connection tab closed by connect(), opened again with a live connection, then a click in it: hung the page
  // through Qt's browser accessibility (focus passed to and fro with a covered page) before the fix in select()
  await app.open();
  await app.connect({ protocol: 'gvret', fd: false });
  await app.menu('Connection', 'Open Connection Window');
  await waitCurrent(page, 'ConnectionWindow');
  await app.click('checkbox', 'Enable Console');
  expect(await answers(page), 'the page answers after a click in the reopened connection tab').toBe(true);
  await app.click('checkbox', 'Enable Console');
  await app.closeConnectionWindow();
  // TESTING.md's old known issue: RE Tools > Frame Search again while it was open, then a click in it
  await app.menu('RE Tools', 'Frame Search');
  await waitCurrent(page, 'SearchWindow');
  await clickTab(page, 0);
  await app.menu('RE Tools', 'Frame Search');
  await waitCurrent(page, 'SearchWindow');
  await page.mouse.click(640, 400);
  await page.getByRole('button').last().click({ force: true, timeout: 5000 }).catch(() => {});
  expect(await answers(page), 'the page answers after clicks in Frame Search opened again').toBe(true);
  expect((await tabs(page)).tabs.filter((t) => t.class === 'SearchWindow').length, 'one Frame Search tab').toBe(1);
});
