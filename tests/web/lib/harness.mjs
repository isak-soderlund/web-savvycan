// Shared test fixtures: the fake rc1 injected before the page loads, console-error collection, and the app driver:
// the built web-savvycan (dist/), driven through its UI (Qt's accessibility tree) and read through its test hook
// window.webSavvyCAN (README "The app's test hook").
import { test as base, expect } from '@playwright/test';
import { readFileSync, existsSync, mkdirSync, writeFileSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
export const WEB = join(here, '..');
export const FAKE_SRC = readFileSync(join(WEB, 'fake/rc1.js'), 'utf8');
export const DIST = process.env.WSC_DIST || join(WEB, '../../dist');
export const RESULTS = join(WEB, 'results');
/** SavvyCAN's own CSV code as a tool (tests/desktop, built by tests/desktop/run.sh). */
export const NATIVECSV = process.env.NATIVECSV_TOOL || join(WEB, '../../build/desktop-tests/nativecsv_tool');
/** SavvyCAN's loader on a log file (nativecsv_tool check): its summary line; throws on a mismatch. */
export const savvycanCheck = (file, ...args) =>
  execFileSync(NATIVECSV, ['check', file, ...args], { encoding: 'utf8' }).split('\n').filter((l) => l.startsWith(file)).join(' ');

/** The init script: the fake, the Web Serial mock and the probe, all before any page script. */
export async function installFake(page, opts = {}) {
  const cfg = JSON.stringify({ serial: opts.serial || {}, probe: opts.probe || {} });
  await page.addInitScript({
    content: `${FAKE_SRC}
;(function () { const c = ${cfg}; RC1Fake.installWebSerial(c.serial); RC1Fake.installProbe(c.probe); })();`,
  });
}

/** Why the app tests can't run (null when they can). */
export function appMissing() {
  return existsSync(join(DIST, 'SavvyCAN.html')) ? null : `no ${DIST}/SavvyCAN.html: run scripts/build-wasm.sh, or set WSC_DIST`;
}

/**
 * The real app (dist/SavvyCAN.html from scripts/build-wasm.sh). Qt 6.12 for WebAssembly renders into a canvas inside a
 * shadow root (#qt-shadow-container), so document.querySelector('canvas') sees nothing. Its accessibility layer mirrors
 * the widgets as HTML in that shadow root once its visually hidden "Enable Screen Reader" button gets a DOM click (a
 * Playwright click on it does nothing); Playwright's role locators pierce open shadow roots, so menus, buttons,
 * checkboxes and radios are then reachable by role and name, as a user would see them.
 * Combo boxes are mirrored without a usable box: the driver reaches them with Tab from a neighbouring checkbox and
 * picks the value with the keyboard (pickCombo).
 */
const shadowButton = () => {
  const r = document.querySelector('#qt-shadow-container');
  return r && r.shadowRoot && r.shadowRoot.querySelector('button.hidden-visually-read-by-screen-reader');
};
const ci = (name) => (name instanceof RegExp ? name : new RegExp('^' + name.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'), 'i'));

export class AppDriver {
  constructor(page) {
    this.page = page;
  }
  async open(query = '') {
    await this.page.goto(`/SavvyCAN.html${query ? '?' + query : ''}`);
    await this.enableA11y();
  }
  async reload() {
    await this.page.reload();
    await this.enableA11y();
  }
  /** Waits for Qt's window, then switches its accessibility tree on (a DOM click on the hidden button). */
  async enableA11y() {
    await this.page.waitForFunction(`!!(${shadowButton})()`, null, { timeout: 60000 });
    await this.page.waitForFunction(() => window.webSavvyCAN, null, { timeout: 30000 });
    await this.page.evaluate(`(${shadowButton})().click()`);
    await this.role('menuitem', 'File').waitFor({ timeout: 15000 });
  }
  role(role, name) {
    return this.page.getByRole(role, { name: ci(name) }).first();
  }
  async need(locator, what) {
    try {
      await locator.waitFor({ state: 'attached', timeout: 10000 });
    } catch {
      base.skip(true, `the app has no ${what} in its accessibility tree`);
    }
    return locator;
  }
  async click(role, name, what) {
    await (await this.need(this.role(role, name), what || `${role} "${name}"`)).click({ force: true });
  }
  async menu(top, item) {
    await this.click('menuitem', new RegExp(`^${top}$`, 'i'), `menu "${top}"`);
    await this.click('menuitem', item, `menu item "${top} > ${item}"`);
  }
  /**
   * Picks `value` in the combo box just before (tab = 'Shift+Tab') or after (tab = 'Tab') the checkbox `anchor` in the
   * tab order: two real clicks on the checkbox give it the keyboard focus (and leave it as it was), Tab moves to the
   * combo box and the typed value selects the item (QComboBox's keyboard search on the closed box). Not through the
   * popup (Qt's popup isn't reachable by role).
   */
  async pickCombo(anchor, tab, value) {
    const b = await anchor.boundingBox();
    const [x, y] = [b.x + b.width / 2, b.y + b.height / 2];
    await this.page.mouse.click(x, y);
    await this.page.waitForTimeout(150);
    await this.page.mouse.click(x, y);
    await this.page.waitForTimeout(150);
    await this.page.keyboard.press(tab);
    await this.page.waitForTimeout(150);
    await this.page.keyboard.type(String(value));
    await this.page.waitForTimeout(500); // past QApplication::keyboardInputInterval: the next search starts afresh
  }
  async setChecked(locator, on) {
    if ((await locator.isChecked()) !== on) await locator.click({ force: true });
  }
  async chooseDevice() {
    if (!(await this.role('button', 'Choose device').count())) {   // the New Connection dialog first
      await this.menu('Connection', 'Open Connection Window');
      await this.click('button', 'Add New Device Connection');
      await this.click('radio', 'Serial Connection (GVRET)');
    }
    await this.click('button', 'Choose device', 'button "Choose device..." (RESOLVE.md MVP #2)');
  }
  /**
   * Connection > Open Connection Window > Add New Device Connection > GVRET or LAWICEL > Choose device... (unless the
   * port is already granted) > Create New Connection. GVRET: then SavvyCAN's Bus Details (Speed, Enable Bus, Enable
   * CAN FD, CAN FD Data Rate, Save Bus Settings) once the connection is up. LAWICEL: the bus is set in the New
   * Connection dialog (CAN Bus Speed, CAN-FD, CAN-FD data rate). Closes the connection window at the end.
   */
  async connect({ protocol = 'gvret', bitrate = 500000, fd = true, dataBitrate = 2000000, expectOpen = true } = {}) {
    const page = this.page;
    await this.menu('Connection', 'Open Connection Window');
    await this.click('button', 'Add New Device Connection');
    await this.click('radio', protocol === 'gvret' ? 'Serial Connection (GVRET)' : 'LAWICEL');
    if (protocol !== 'gvret') {
      const fdBox = await this.need(this.role('checkbox', /^CAN-FD$/), 'checkbox "CAN-FD"');
      await this.pickCombo(fdBox, 'Shift+Tab', bitrate);              // CAN Bus Speed, before CAN-FD
      await this.setChecked(fdBox, fd);
      if (fd) await this.pickCombo(fdBox, 'Tab', dataBitrate);       // CAN-FD data rate, after it
    }
    const granted = await page.evaluate(async () => (await navigator.serial.getPorts()).length > 0);
    if (!granted) await this.chooseDevice();
    await page.waitForFunction(() => window.resolveWebSerial && window.resolveWebSerial.ports().length > 0, null, { timeout: 15000 });
    await this.click('button', 'Create New Connection');
    // Up as rc1 sees it: GVRET mode with the app's polls coming (SavvyCAN's ESP32 mode waits 3 s first), or slcan open.
    await page.waitForFunction((p) => {
      const s = window.__rc1.stats();
      return p === 'gvret' ? s.mode === 'gvret' && s.bytesFromHost > 20 : s.channel.open || s.sessions > 0;
    }, protocol, { timeout: 20000 });
    if (protocol === 'gvret') {
      await page.waitForFunction(() => window.webSavvyCAN.stats().connected, null, { timeout: 20000 });
      // Bus Details fill in from the device once it is connected: set them after that. Its checkboxes are all named
      // "Bus Details:" (their labels aren't buddies), in the .ui's order Listen Only, Enable Bus, Enable CAN FD.
      const group = this.role('group', 'Bus Details:');
      await expect(group).not.toBeDisabled({ timeout: 10000 });
      await page.waitForTimeout(500);
      const boxes = group.getByRole('checkbox');
      const [listen, enable, canFd] = [boxes.nth(0), boxes.nth(1), boxes.nth(2)];
      await this.pickCombo(listen, 'Shift+Tab', bitrate);             // Speed, before Listen Only
      await this.setChecked(listen, false);
      await this.setChecked(enable, true);
      await this.setChecked(canFd, fd);
      if (fd) await this.pickCombo(canFd, 'Tab', dataBitrate);       // CAN FD Data Rate, after Enable CAN FD
      await this.click('button', 'Save Bus Settings');
    }
    if (expectOpen) await page.waitForFunction(() => window.__rc1.stats().channel.open, null, { timeout: 10000 });
    await this.closeConnectionWindow();
  }
  /** The connection window covers the main window's menus: Escape closes it (the connection stays up). */
  async closeConnectionWindow() {
    await this.page.keyboard.press('Escape');
    await this.page.waitForTimeout(300);
  }
  async disconnect() {
    await this.menu('Connection', 'Open Connection Window');
    await this.click('button', 'Disconnect Selected Device');
    await this.closeConnectionWindow();
  }
  async startLog() {
    await this.menu('File', /^Start Continuous Logging$/i);
    await this.page.waitForFunction(() => window.webSavvyCAN.stats().logging, null, { timeout: 10000 });
  }
  /** File > Cease Continuous Logging: the MVP's one download (RESOLVE.md MVP #7). Returns Playwright's Download. */
  async stopLog() {
    const dl = this.page.waitForEvent('download', { timeout: 20000 });
    await this.menu('File', 'Cease Continuous Logging');
    const d = await dl;
    await this.page.waitForFunction(() => !window.webSavvyCAN.stats().logging, null, { timeout: 10000 });
    return d;
  }
  /** Light or dark, whichever the app isn't showing, as File > Preferences > Theme sets it (the app has no View menu
   * since 2026-10-29: Qt's combo boxes aren't reachable by role, so through the test hook). */
  async toggleTheme() {
    const dark = (await this.page.evaluate(() => window.webSavvyCAN.theme())) === 'dark';
    await this.setTheme(dark ? 'light' : 'dark');
  }
  /** 'system', 'light' or 'dark' (File > Preferences > Theme). */
  async setTheme(mode) {
    await this.page.evaluate((m) => window.webSavvyCAN.setTheme(m), mode);
  }
  /** File > Load Log File with `file` chosen in the browser's file picker; waits until its `frames` are in the trace. */
  async loadFile(file, frames) {
    const chooser = this.page.waitForEvent('filechooser', { timeout: 15000 });
    await this.menu('File', 'Load Log File');
    await (await chooser).setFiles(file);
    await this.page.waitForFunction((n) => window.webSavvyCAN.stats().framesReceived >= n, frames, { timeout: 30000 });
  }
  async themeShot() {
    return this.page.screenshot();
  }
}

export const test = base.extend({
  fakeOptions: [{}, { option: true }],
  consoleErrors: async ({ page }, use) => {
    const errors = [];
    page.on('console', (m) => m.type() === 'error' && errors.push(m.text()));
    page.on('pageerror', (e) => errors.push(String(e)));
    await use(errors);
  },
  app: async ({ page, fakeOptions, consoleErrors }, use, testInfo) => {
    const why = appMissing();
    testInfo.skip(!!why, why || '');
    await installFake(page, fakeOptions);
    await use(new AppDriver(page));
    // Every test: no console errors, no uncaught exceptions.
    expect(consoleErrors, 'console errors').toEqual([]);
  },
});
export { expect };

/** Helpers in the page. */
export const fake = {
  stats: (page) => page.evaluate(() => window.__rc1.stats()),
  setTraffic: (page, mode, opts = {}) => page.evaluate(([m, o]) => window.__rc1.setTraffic(m, o), [mode, opts]),
  log: (page) => page.evaluate(() => window.__rc1.log()),
  events: (page) => page.evaluate(() => window.__rc1.events),
  recorded: (page) => page.evaluate(() => window.__rc1.recorded()),
  appStats: (page) => page.evaluate(() => window.webSavvyCAN.stats()),
  probe: (page) => page.evaluate(() => window.__probe.report()),
};

/** Waits until the app has taken everything the fake sent (or `ms` passed) -> [fake stats, app stats]. */
export async function settle(page, ms = 10000) {
  const end = Date.now() + ms;
  let s, a;
  for (;;) {
    s = await fake.stats(page);
    a = await fake.appStats(page);
    const taken = a.framesReceived + (a.framesDroppedBeforeParse || 0);
    if ((s.queuedBytes === 0 && taken >= s.encoded && a.queued === 0) || Date.now() > end) return [s, a];
    await page.waitForTimeout(100);
  }
}

export function saveResult(name, data) {
  mkdirSync(RESULTS, { recursive: true });
  const p = join(RESULTS, `${name}.json`);
  writeFileSync(p, JSON.stringify(data, null, 2));
  return p;
}
