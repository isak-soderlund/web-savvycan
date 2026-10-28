// Browser tests of web-savvycan's JS parts. Run: npm test (from test/web).
import { defineConfig, devices } from '@playwright/test';

export default defineConfig({
  testDir: './tests',
  timeout: 120_000,
  workers: 1,
  reporter: [['list']],
  globalSetup: './global-setup.mjs',
  use: { baseURL: 'http://localhost:8765' },
  webServer: { command: 'node server.mjs 8765', url: 'http://localhost:8765/test/web/disklog/harness.html', reuseExistingServer: true },
  projects: [
    { name: 'chromium', use: { ...devices['Desktop Chrome'] }, testIgnore: /firefox|xvfb/ },
    // headed, real pickers and files: under xvfb-run (see tests/disklog-xvfb.spec.mjs)
    { name: 'chromium-xvfb', use: { ...devices['Desktop Chrome'] }, testMatch: /xvfb/ },
    { name: 'firefox', use: { ...devices['Desktop Firefox'] }, testMatch: /firefox/ },
  ],
});
