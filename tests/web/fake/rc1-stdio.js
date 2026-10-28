#!/usr/bin/env node
// The fake rc1 (rc1.js) on stdio, for tests/desktop/rc1_pty.py: host bytes in on stdin, device bytes out on stdout,
// control lines (JSON) in on fd 3: {"cmd":"open"} (the host opened the port: DTR/RTS up), {"cmd":"closed"} (the host
// closed it: session end), {"cmd":"traffic","mode":"leaf","opts":{...}}, {"cmd":"stats"}. Stats as JSON lines on
// stderr (every second, on "stats", and at exit).
//   node rc1-stdio.js [--traffic MODE] [--buffer BYTES] [--refuse-open]
'use strict';
const fs = require('node:fs');
const R = require('./rc1.js');

const argv = process.argv.slice(2);
const arg = (n, d) => (argv.includes(n) ? argv[argv.indexOf(n) + 1] : d);
const dev = new R.Rc1Device({ bufferBytes: Number(arg('--buffer', 65536)), refuseOpen: argv.includes('--refuse-open') });
const traffic = arg('--traffic', null);
// Traffic starts when the channel first opens, so a test sees it from the start.
let trafficStarted = false;
const stats = () => process.stderr.write(JSON.stringify(Object.assign({ t: Date.now() }, dev.stats(), { log: dev.log.slice(-20) })) + '\n');

process.stdin.on('data', (b) => dev.write(b));
process.stdin.on('end', () => {
  stats();
  process.exit(0);
});
let ctl = '';
fs.createReadStream(null, { fd: 3 }).on('data', (b) => {
  ctl += b.toString();
  let i;
  while ((i = ctl.indexOf('\n')) >= 0) {
    const line = ctl.slice(0, i);
    ctl = ctl.slice(i + 1);
    if (!line.trim()) continue;
    const c = JSON.parse(line);
    if (c.cmd === 'open') dev.setControl({ dtr: true, rts: true, coding: 'pty' });
    else if (c.cmd === 'closed') dev.hostClosed();
    else if (c.cmd === 'traffic') dev.setTraffic(c.mode, c.opts || {});
    else if (c.cmd === 'stats') stats();
  }
}).on('error', () => {});

setInterval(() => {
  if (traffic && !trafficStarted && dev.chan.open) {
    trafficStarted = true;
    dev.setTraffic(traffic, {});
  }
  const out = dev.read(65536);
  if (out.length) process.stdout.write(out);
}, 1);
setInterval(stats, 1000);
