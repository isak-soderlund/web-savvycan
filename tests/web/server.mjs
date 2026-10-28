#!/usr/bin/env node
// A static server for the browser tests: the built app (dist/) at /, the harness (fake rc1, self-test page) at
// /__harness/. No dependencies. Usage: node server.mjs [--port 8751] [--dist DIR]   (env WSC_DIST, WSC_PORT)
import { createServer } from 'node:http';
import { createReadStream, statSync } from 'node:fs';
import { extname, join, normalize, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const arg = (name, def) => {
  const i = process.argv.indexOf(name);
  return i > 0 ? process.argv[i + 1] : def;
};
const port = Number(arg('--port', process.env.WSC_PORT || 8751));
const dist = resolve(arg('--dist', process.env.WSC_DIST || join(here, '../../dist')));
const TYPES = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.mjs': 'text/javascript',
  '.wasm': 'application/wasm', '.css': 'text/css', '.svg': 'image/svg+xml', '.png': 'image/png', '.json': 'application/json',
  '.csv': 'text/csv', '.data': 'application/octet-stream', '.ico': 'image/x-icon' };

function file(root, rel) {
  const p = normalize(join(root, rel));
  if (!p.startsWith(root)) return null;
  try {
    const s = statSync(p);
    if (s.isDirectory()) return file(p, 'index.html');
    return { p, size: s.size };
  } catch {
    return null;
  }
}

createServer((req, res) => {
  const url = new URL(req.url, 'http://x');
  let path = decodeURIComponent(url.pathname);
  let root = dist;
  if (path.startsWith('/__harness/')) {
    root = here;
    path = path.slice('/__harness'.length);
  }
  if (path.includes('node_modules')) path = '/nope';
  const f = file(root, path);
  if (!f) {
    res.writeHead(404, { 'content-type': 'text/plain' }).end(`not found: ${url.pathname}\n`);
    return;
  }
  res.writeHead(200, { 'content-type': TYPES[extname(f.p)] || 'application/octet-stream', 'content-length': f.size,
    'cache-control': 'no-store' });
  createReadStream(f.p).pipe(res);
}).listen(port, '127.0.0.1', () => console.log(`web-savvycan test server: http://127.0.0.1:${port}/ (dist ${dist})`));
