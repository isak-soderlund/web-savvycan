// Reference rows for the disk-log tests, made by SavvyCAN's own row formatter (test/nativecsv).
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import { TOOL, ROWS, ROWS_COUNT, OUT } from './tests/common.mjs';

export default function () {
  if (!fs.existsSync(TOOL)) {
    throw new Error(`${TOOL} missing: build it first (see test/nativecsv/CMakeLists.txt or test/web/run-disklog-tests.sh)`);
  }
  fs.mkdirSync(OUT, { recursive: true });
  execFileSync(TOOL, ['gen', ROWS, String(ROWS_COUNT)], { stdio: 'inherit' });
}
