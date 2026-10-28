// web-savvycan browser tests. Run: tests/web/run.sh (README.md). Projects:
//   selftest  the harness's reference client (proves the fake rc1, the Web Serial mock, the file pickers, the probe)
//   app       the built app from dist/ (WSC_DIST); tests skip with the reason while a stage-1 part is missing
// Long runs (the 60 s full-load replays) are tagged @long: WSC_LONG=1 (run.sh --long) includes them.
import { defineConfig } from '@playwright/test';

const port = Number(process.env.WSC_PORT || 8751);
export default defineConfig({
  testDir: './specs',
  outputDir: './results/artifacts',
  timeout: 60_000,
  fullyParallel: false,
  workers: 1, // one Chromium at a time: the rig's memory is shared (RESOLVE.md)
  grepInvert: process.env.WSC_LONG ? undefined : /@long/,
  reporter: [['list'], ['json', { outputFile: './results/report.json' }]],
  use: {
    baseURL: `http://127.0.0.1:${port}`,
    headless: true,
    viewport: { width: 1280, height: 800 },
    trace: 'retain-on-failure',
    launchOptions: { args: ['--enable-experimental-web-platform-features'] },
  },
  projects: [
    { name: 'selftest', use: { target: 'selftest', browserName: 'chromium' } },
    { name: 'app', use: { target: 'app', browserName: 'chromium' } },
  ],
  webServer: {
    command: `node server.mjs --port ${port}`,
    url: `http://127.0.0.1:${port}/__harness/selftest/index.html`,
    reuseExistingServer: true,
  },
});
