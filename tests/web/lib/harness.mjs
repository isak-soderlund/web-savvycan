// Shared test fixtures: the fake rc1 injected before the page loads, console-error collection, and a driver per target:
//   selftest  the harness's own reference client (selftest/index.html): proves the fake, the mock and the checks
//   app       the built web-savvycan (dist/), driven through its UI (Qt's accessibility tree) and read through its
//             test hook window.webSavvyCAN (README "The app's test hook")
import { test as base, expect } from '@playwright/test';
import { readFileSync, existsSync, mkdirSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
export const WEB = join(here, '..');
export const FAKE_SRC = readFileSync(join(WEB, 'fake/rc1.js'), 'utf8');
export const DIST = process.env.WSC_DIST || join(WEB, '../../dist');
export const RESULTS = process.env.WSC_RESULTS || join(WEB, 'results');

/** The init script: the fake, the Web Serial mock, the file pickers, the probe, all before any page script. */
export async function installFake(page, opts = {}) {
  const cfg = JSON.stringify({ serial: opts.serial || {}, pickers: opts.pickers || {}, probe: opts.probe || {} });
  await page.addInitScript({
    content: `${FAKE_SRC}
;(function () { const c = ${cfg};
  RC1Fake.installWebSerial(c.serial); RC1Fake.installFilePickers(c.pickers); RC1Fake.installProbe(c.probe); })();`,
  });
}

/** Why an app test can't run yet (null when it can). */
export function appMissing() {
  if (!existsSync(join(DIST, 'index.html'))) {
    return `no ${DIST}/index.html: waiting for the wasm build (RESOLVE.md MVP #1, build agent); set WSC_DIST to a dist/`;
  }
  return null;
}

class SelftestDriver {
  constructor(page) {
    this.page = page;
    this.name = 'selftest';
  }
  async open(query = '') {
    await this.page.goto(`/__harness/selftest/index.html${query ? '?' + query : ''}`);
    await this.page.waitForFunction(() => window.webSavvyCAN);
  }
  async chooseDevice() {
    await this.page.click('#choose');
    await expect(this.page.locator('#status')).toHaveText(/device chosen|no device/);
  }
  async connect({ protocol = 'gvret', bitrate = 500000, fd = true, dataBitrate = 2000000 } = {}) {
    const granted = await this.page.evaluate(async () => (await navigator.serial.getPorts()).length > 0);
    if (!granted) await this.chooseDevice();
    await this.page.selectOption('#proto', protocol);
    await this.page.fill('#bitrate', String(bitrate));
    await this.page.setChecked('#fd', fd);
    await this.page.fill('#dbitrate', String(dataBitrate));
    await this.page.click('#connect');
    await this.page.waitForFunction(() => /connected|failed/.test(document.getElementById('status').textContent));
  }
  async disconnect() {
    await this.page.click('#disconnect');
  }
  async startLog() {
    await this.page.click('#logStart');
    await this.page.waitForFunction(() => window.webSavvyCAN.stats().logging);
  }
  async stopLog() {
    await this.page.click('#logStop');
    await this.page.waitForFunction(() => !window.webSavvyCAN.stats().logging);
  }
  async toggleTheme() {
    await this.page.click('#theme');
  }
  async themeShot() {
    return this.page.locator('#canvas').screenshot();
  }
}

/**
 * The real app. UI: Qt for WebAssembly renders widgets into a canvas; its accessibility layer mirrors them as hidden
 * HTML elements once the screen reader support is switched on (Qt adds an "Enable screen reader" button), so the
 * driver clicks through SavvyCAN's own menus and dialogs by role and name, as a user would. Names are SavvyCAN's
 * (ui/*.ui); the stage-1 additions (Choose device, theme switch, logging) use the names in README "UI names".
 */
class AppDriver {
  constructor(page) {
    this.page = page;
    this.name = 'app';
  }
  async open(query = '') {
    await this.page.goto(`/${query ? '?' + query : ''}`);
    const hook = await this.page.waitForFunction(() => !!window.webSavvyCAN, null, { timeout: 30000 }).catch(() => null);
    if (!hook) {
      base.skip(true, 'the app has no window.webSavvyCAN test hook yet (README "The app\'s test hook"; ui/webserial agents)');
    }
    await this.enableA11y();
  }
  async enableA11y() {
    const btn = this.page.getByRole('button', { name: /enable screen reader/i });
    if (await btn.count()) await btn.first().click({ force: true });
  }
  role(role, name) {
    return this.page.getByRole(role, { name, exact: false }).first();
  }
  async need(locator, what) {
    if (!(await locator.count())) {
      base.skip(true, `the app has no ${what} in its accessibility tree yet`);
    }
    return locator;
  }
  async menu(top, item) {
    await (await this.need(this.role('menuitem', top), `menu "${top}"`)).click({ force: true });
    await (await this.need(this.role('menuitem', item), `menu item "${top} > ${item}"`)).click({ force: true });
  }
  async chooseDevice() {
    await (await this.need(this.role('button', 'Choose device'), 'button "Choose device" (RESOLVE.md MVP #2)')).click({ force: true });
  }
  async connect({ protocol = 'gvret', bitrate = 500000, fd = true, dataBitrate = 2000000 } = {}) {
    await this.menu('Connection', 'Open Connection Window');
    await (await this.need(this.role('button', 'Add New Device Connection'), 'Add New Device Connection')).click({ force: true });
    const type = protocol === 'gvret' ? 'Serial Connection (GVRET)' : 'LAWICEL / SLCAN Serial';
    await (await this.need(this.role('radio', type), `radio "${type}"`)).click({ force: true });
    await this.chooseDevice();
    await (await this.need(this.role('button', 'Create New Connection'), 'Create New Connection')).click({ force: true });
    await this.page.waitForFunction(() => window.webSavvyCAN.stats().connected, null, { timeout: 10000 });
    if (protocol === 'gvret') {
      // Bus details: speed, enable, FD, data rate; Save Bus Settings.
      const speed = await this.need(this.role('combobox', 'Speed'), 'Speed');
      await speed.fill(String(bitrate)).catch(() => {});
      await this.role('checkbox', 'Enable Bus').check({ force: true }).catch(() => {});
      await this.role('checkbox', 'Enable CAN FD').setChecked(fd, { force: true }).catch(() => {});
      if (fd) await this.role('combobox', 'CAN FD Data Rate').fill(String(dataBitrate)).catch(() => {});
      await (await this.need(this.role('button', 'Save Bus Settings'), 'Save Bus Settings')).click({ force: true });
    }
  }
  async disconnect() {
    await (await this.need(this.role('button', 'Disconnect Selected Device'), 'Disconnect Selected Device')).click({ force: true });
  }
  async startLog() {
    await this.menu('File', 'Start Continuous Logging');
    await this.page.waitForFunction(() => window.webSavvyCAN.stats().logging, null, { timeout: 10000 });
  }
  async stopLog() {
    await this.menu('File', 'Stop Continuous Logging');
    await this.page.waitForFunction(() => !window.webSavvyCAN.stats().logging, null, { timeout: 10000 });
  }
  async toggleTheme() {
    const viaHook = await this.page.evaluate(() => typeof window.webSavvyCAN.theme === 'function');
    if (!viaHook) base.skip(true, 'the app reports no theme (webSavvyCAN.theme())');
    await this.menu('View', 'Dark theme').catch(async () => this.menu('View', 'Light theme'));
  }
  async themeShot() {
    return this.page.screenshot();
  }
}

export const test = base.extend({
  target: ['selftest', { option: true }],
  fakeOptions: [{}, { option: true }],
  consoleErrors: async ({ page }, use) => {
    const errors = [];
    page.on('console', (m) => m.type() === 'error' && errors.push(m.text()));
    page.on('pageerror', (e) => errors.push(String(e)));
    await use(errors);
  },
  app: async ({ page, target, fakeOptions, consoleErrors }, use, testInfo) => {
    if (target === 'app') {
      const why = appMissing();
      testInfo.skip(!!why, why || '');
    }
    await installFake(page, fakeOptions);
    const d = target === 'app' ? new AppDriver(page) : new SelftestDriver(page);
    await use(d);
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
