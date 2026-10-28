// web-savvycan's browser tests. Run: tests/web/run.sh (README.md). Projects:
//   app       the built app from dist/ (WSC_DIST) with the fake rc1: every spec but the logging module's own
//   download  the logging module (wasm/disklog/disklog.js) on its own page, Chromium
//   firefox   the same in Firefox
// Opt-in tags (WSC_RUN=long,large,screenshots; run.sh --long / --large / --screenshots):
//   @long         the 60 s full-load replays
//   @large        a multi-GB log in the logging module (heavy: run.sh puts it through heavy.sh)
//   @screenshots  pictures of the app in light and dark (results/screenshots/)
import { defineConfig } from '@playwright/test';

const port = Number(process.env.WSC_PORT || 8751);
const run = (process.env.WSC_RUN || '').split(',');
const off = ['long', 'large', 'screenshots'].filter((t) => !run.includes(t));
const MODULE = /(download|large)\.spec/;
export default defineConfig({
  testDir: './specs',
  outputDir: './results/artifacts',
  timeout: 60_000,
  fullyParallel: false,
  workers: 1, // one browser at a time: the rig's memory is shared (RESOLVE.md)
  grepInvert: off.length ? new RegExp(off.map((t) => '@' + t).join('|')) : undefined,
  reporter: [['list'], ['json', { outputFile: './results/report.json' }]],
  use: {
    baseURL: `http://127.0.0.1:${port}`,
    headless: true,
    viewport: { width: 1280, height: 800 },
    trace: 'retain-on-failure',
  },
  projects: [
    { name: 'app', testIgnore: MODULE,
      use: { browserName: 'chromium', launchOptions: { args: ['--enable-experimental-web-platform-features'] } } },
    { name: 'download', testMatch: MODULE, use: { browserName: 'chromium' } },
    { name: 'firefox', testMatch: /download\.spec/, use: { browserName: 'firefox' } },
  ],
  webServer: {
    command: `node server.mjs --port ${port}`,
    url: `http://127.0.0.1:${port}/__harness/lib/disklog.html`,
    reuseExistingServer: false, // another worktree's server on the port would serve its own harness and dist: set WSC_PORT
  },
});
