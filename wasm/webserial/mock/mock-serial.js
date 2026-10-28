// web-savvycan: a small navigator.serial mock with one fake rc1 behind it, for the Web Serial bridge's own tests (Node
// or a page). It answers SavvyCAN's GVRET like resolve-can's test/hil/gvret_fake.py (one bus, CAN FD, the bus
// closed at the start of a session) and LAWICEL/slcan's open/close/speed with CR/BEL. The full device fake
// (traffic modes, flow control, every slcan command, file pickers) is the test harness's tests/web/fake/rc1.js.
//
//   installMockSerial({granted: true})  -> {serial, port, device}; also globalThis.__mockSerial
//   device.busFrame({id, data, ext, fd, brs})  a frame on the bus (reaches the host while the bus is open)
//   device.log                                 what the host asked for ("gvret", "setup bus0 ...", "tx 123 ...")
//   port.unplug() / port.replug()
(function (root) {
  'use strict';

  function le32(v) { return [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff]; }
  function rd32(a, i) { return (a[i] | (a[i + 1] << 8) | (a[i + 2] << 16) | (a[i + 3] << 24)) >>> 0; }
  function hex(a) { return Array.from(a, function (b) { return ('0' + b.toString(16)).slice(-2); }).join(''); }

  function FakeRc1(send) {
    this.send = send; // (Uint8Array) -> void: bytes to the host
    this.reset();
    this.t0 = Date.now();
  }
  FakeRc1.prototype.reset = function () {
    this.gvret = false; this.enabled = false; this.listenOnly = false; this.fd = false;
    this.bitrate = 500000; this.dataBitrate = 2000000; this.buf = []; this.log = this.log || [];
    this.ops = this.ops || {}; // GVRET opcodes from the host, counted (9 = comm validation, every 250 ms when connected)
  };
  FakeRc1.prototype.us = function () { return ((Date.now() - this.t0) * 1000) >>> 0; };
  FakeRc1.prototype.fromHost = function (bytes) {
    for (var i = 0; i < bytes.length; i++) this.buf.push(bytes[i]);
    for (;;) {
      var before = this.buf.length;
      if (this.gvret) this.parseGvret(); else this.parseSlcan();
      if (!this.buf.length || this.buf.length === before) break;
    }
  };
  FakeRc1.prototype.parseSlcan = function () {
    var b = this.buf;
    if (b[0] === 0xE7 && b[1] === 0xE7) { b.splice(0, 2); this.gvret = true; this.enabled = false; this.log.push('gvret'); return; }
    if (b.length === 1 && b[0] === 0xE7) return;
    var cr = b.indexOf(13);
    if (cr < 0) return;
    var cmd = String.fromCharCode.apply(null, b.splice(0, cr + 1).slice(0, cr));
    this.log.push('slcan ' + cmd);
    var ok = true;
    var c = cmd[0];
    if (c === 'O' || c === 'L') { this.enabled = true; this.listenOnly = c === 'L'; }
    else if (c === 'C') this.enabled = false;
    else if (c === 'S') ok = !this.enabled;
    else if (c === 'Y') { ok = !this.enabled; this.fd = true; }
    else if (c === 'V') { this.send(new TextEncoder().encode('V0103\r')); return; }
    else if ('tTdDbB'.indexOf(c) >= 0) ok = this.enabled && !this.listenOnly;
    else if (cmd.length) ok = false;
    this.send(new Uint8Array([ok ? 13 : 7]));
  };
  FakeRc1.prototype.parseGvret = function () {
    var b = this.buf;
    var start = b.indexOf(0xF1);
    if (start < 0) { b.length = 0; return; }
    b.splice(0, start);
    if (b.length < 2) return;
    var op = b[1];
    if (op === 0xF1) { b.splice(0, 1); return; }
    this.ops[op] = (this.ops[op] || 0) + 1;
    var n = { 5: 8, 21: 6, 14: 12, 4: 1, 8: 1, 10: 1 }[op];
    if (op === 0 || op === 11 || op === 20) {
      if (b.length < 8) return;
      n = 6 + (b[7] & 0x7f);
    }
    if (n === undefined) { b.splice(0, 2); this.reply(op); return; }
    if (b.length < 2 + n) return;
    var a = b.splice(0, 2 + n).slice(2);
    this.command(op, a);
  };
  FakeRc1.prototype.reply = function (op) {
    var body = {
      1: le32(this.us()), 2: [0, 0], 3: new Array(15).fill(0),
      6: [(this.enabled ? 1 : 0) | (this.listenOnly ? 0x10 : 0)].concat(le32(this.bitrate), [0, 0, 0, 0, 0]),
      7: [100, 0, 0x20, 0, 0, 0], 9: [0xde, 0xad], 12: [1], 13: new Array(15).fill(0),
      22: [1, 0x01 | (this.fd ? 2 : 0)].concat(le32(this.dataBitrate))
    }[op];
    if (body) this.send(new Uint8Array([0xF1, op].concat(body)));
  };
  FakeRc1.prototype.command = function (op, a) {
    if (op === 5) {
      var w = rd32(a, 0);
      if (w & 0x80000000) { this.enabled = !!(w & 0x40000000); this.listenOnly = !!(w & 0x20000000); this.bitrate = w & 0x0fffffff; }
      this.log.push('setup bus0 enabled=' + (+this.enabled) + ' listen_only=' + (+this.listenOnly) + ' bitrate=' + this.bitrate);
    } else if (op === 21) {
      this.fd = !!(a[1] & 2);
      var r = rd32(a, 2);
      if (r) this.dataBitrate = r;
      this.log.push('setup_fd fd=' + (+this.fd) + ' data_bitrate=' + this.dataBitrate);
    } else if (op === 0 || op === 11 || op === 20) {
      var len = a[5] & 0x7f;
      this.log.push('tx ' + (rd32(a, 0) & 0x1fffffff).toString(16) + ' ' + hex(a.slice(6, 6 + len)));
    }
  };
  FakeRc1.prototype.busFrame = function (f) {
    if (!this.enabled) return false;
    var data = Array.from(f.data || []);
    if (this.gvret) {
      var gid = (f.id | (f.ext ? 0x80000000 : 0)) >>> 0;
      var head = [0xF1, f.fd ? 20 : 0].concat(le32(this.us()), le32(gid));
      head = head.concat(f.fd ? [data.length | (f.brs ? 0x80 : 0), 0] : [data.length]);
      this.send(new Uint8Array(head.concat(data, [0])));
    } else {
      var id = f.ext ? ('0000000' + f.id.toString(16)).slice(-8) : ('00' + f.id.toString(16)).slice(-3);
      var s = (f.ext ? 'T' : 't') + id.toUpperCase() + data.length + hex(data).toUpperCase() + '\r';
      this.send(new TextEncoder().encode(s));
    }
    return true;
  };

  class MockPort extends EventTarget {}
  function initPort(self0, serial, info) {
    var self = self0;
    self._serial = serial; self._info = info; self._plugged = true; self._granted = false;
    self.readable = null; self.writable = null; self._controller = null; self.signals = [];
    self.device = new FakeRc1(function (bytes) {
      if (self._controller) { try { self._controller.enqueue(bytes); } catch (e) { /* closed */ } }
    });
    return self;
  }
  MockPort.prototype.getInfo = function () { return Object.assign({}, this._info); };
  MockPort.prototype.open = function (opts) {
    var self = this;
    if (!this._plugged) return Promise.reject(new DOMException('The device has been lost.', 'NetworkError'));
    if (this.readable) return Promise.reject(new DOMException('The port is already open.', 'InvalidStateError'));
    if (!opts || !opts.baudRate) return Promise.reject(new TypeError('baudRate required'));
    this.options = opts;
    this.openCount = (this.openCount || 0) + 1;
    this.device.reset();
    this.readable = new ReadableStream({
      start: function (c) { self._controller = c; },
      cancel: function () { self._controller = null; }
    });
    this.writable = new WritableStream({
      write: function (chunk) {
        if (!self._plugged) throw new DOMException('The device has been lost.', 'NetworkError');
        self.device.fromHost(chunk);
      }
    });
    return Promise.resolve();
  };
  MockPort.prototype.setSignals = function (s) {
    if (!this.readable) return Promise.reject(new DOMException('The port is closed.', 'InvalidStateError'));
    this.signals.push(Object.assign({}, s));
    return Promise.resolve();
  };
  MockPort.prototype.close = function () {
    if (!this.readable) return Promise.reject(new DOMException('The port is already closed.', 'InvalidStateError'));
    if (this.readable.locked || this.writable.locked) return Promise.reject(new TypeError('streams locked'));
    this.readable = null; this.writable = null; this._controller = null;
    this.device.log.push('closed');
    return Promise.resolve();
  };
  MockPort.prototype.unplug = function () {
    this._plugged = false;
    if (this._controller) {
      try { this._controller.error(new DOMException('The device has been lost.', 'NetworkError')); } catch (e) { /* */ }
      this._controller = null;
    }
    this.readable = null; this.writable = null;
    this._serial._fire('disconnect', this);
  };
  MockPort.prototype.replug = function () {
    this._plugged = true;
    this._serial._fire('connect', this);
  };

  class MockSerial extends EventTarget {
    constructor() { super(); this.ports = []; this.requests = []; }
  }
  MockSerial.prototype.getPorts = function () {
    return Promise.resolve(this.ports.filter(function (p) { return p._granted && p._plugged; }));
  };
  MockSerial.prototype.requestPort = function (opts) {
    this.requests.push(opts || {});
    var filters = (opts && opts.filters) || [];
    var match = this.ports.filter(function (p) {
      return p._plugged && (!filters.length || filters.some(function (f) {
        return (f.usbVendorId === undefined || f.usbVendorId === p._info.usbVendorId) &&
               (f.usbProductId === undefined || f.usbProductId === p._info.usbProductId);
      }));
    });
    if (this.cancelNext || !match.length) {
      this.cancelNext = false;
      return Promise.reject(new DOMException('No port selected by the user.', 'NotFoundError'));
    }
    match[0]._granted = true;
    return Promise.resolve(match[0]);
  };
  MockSerial.prototype._fire = function (type, port) {
    var e = new Event(type);
    Object.defineProperty(e, 'target', { value: port });
    this.dispatchEvent(e);
  };

  function installMockSerial(o) {
    o = o || {};
    var serial = new MockSerial();
    var port = initPort(new MockPort(), serial, { usbVendorId: 0x1209, usbProductId: 0x0001 });
    port._granted = !!o.granted;
    serial.ports.push(port);
    if (o.otherPort) {
      var other = initPort(new MockPort(), serial, { usbVendorId: 0x0403, usbProductId: 0x6001 });
      other._granted = !!o.granted;
      serial.ports.push(other);
    }
    Object.defineProperty(root.navigator, 'serial', { value: serial, configurable: true });
    var api = { serial: serial, port: port, device: port.device };
    root.__mockSerial = api;
    return api;
  }

  root.installMockSerial = installMockSerial;
  if (typeof module === 'object' && module.exports) module.exports = { installMockSerial: installMockSerial, FakeRc1: FakeRc1 };
})(typeof globalThis !== 'undefined' ? globalThis : this);
