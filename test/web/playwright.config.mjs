// Browser tests of web-savvycan's JS parts. Run: npm test (from test/web).
import { defineConfig, devices } from '@playwright/test';

// WSC_TEST_PORT: another port when a second worktree's tests run at the same time (the server is reused)
const PORT = Number(process.env.WSC_TEST_PORT || 8765);

export default defineConfig({
  testDir: './tests',
  timeout: 120_000,
  workers: 1,
  reporter: [['list']],
  globalSetup: './global-setup.mjs',
  use: { baseURL: `http://localhost:${PORT}` },
  webServer: { command: `node server.mjs ${PORT}`, url: `http://localhost:${PORT}/test/web/disklog/harness.html`, reuseExistingServer: true },
  projects: [
    { name: 'chromium', use: { ...devices['Desktop Chrome'] }, testIgnore: /firefox|xvfb|large/ },
    // the multi-GB blob log (tests/download-large.spec.mjs): run on its own, through heavy.sh
    { name: 'chromium-large', use: { ...devices['Desktop Chrome'] }, testMatch: /large/, timeout: 3_600_000 },
    // headed, real pickers and files: under xvfb-run (see tests/disklog-xvfb.spec.mjs)
    { name: 'chromium-xvfb', use: { ...devices['Desktop Chrome'] }, testMatch: /xvfb/ },
    { name: 'firefox', use: { ...devices['Desktop Firefox'] }, testMatch: /firefox|download(-app)?\.spec/ },
  ],
});
