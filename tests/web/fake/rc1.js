/*
 * A fake resolve-can-1 (rc1) for web-savvycan's tests: its serial port (CDC-ACM) speaking GVRET and LAWICEL/slcan
 * exactly as the firmware does (resolve-can, branch `firmware`: firmware/proto/src/serial.rs, gvret.rs, slcan.rs,
 * firmware/app/src/serialport.rs), a CAN bus behind it with traffic modes, and a `navigator.serial` (Web Serial)
 * mock on top for the browser.
 *
 * One file, no dependencies, loads three ways:
 *   - in the page, before the app (Playwright `addInitScript`): defines `RC1Fake`; `RC1Fake.installWebSerial(opts)`
 *     replaces navigator.serial and exposes the device to the test as `window.__rc1`;
 *   - in Node: `require('./rc1.js')` (unit tests, the desktop pty fake's stdio device `rc1-stdio.js`).
 *
 * What the firmware does and the fake copies:
 *   - the port starts in slcan; 0xE7 0xE7 switches it to GVRET (closing a channel slcan had open);
 *   - the session ends (port back to slcan, CAN channel closed) when DTR falls, or on a repeated "DTR and RTS low"
 *     with the line coding unchanged (control_task); closing the port does that (Linux lowers DTR/RTS on close);
 *   - GVRET: one bus, FD capable and on by default (2 Mbit/s data), starts disabled; replies to 01/02/03/06/07/09/
 *     0C/0D/16; SETUP_CANBUS (05) / SETUP_FD (15) close, set the rates and reopen the channel as needed; error
 *     frames, echoes and other buses are not reported;
 *   - slcan: S0-S9/s/Y1-Y8/O/L/C/Z/F/V/v/N/M/m/A/X/U, frames t T r R d D b B, BEL on refusals, error lines s/e;
 *   - when the host doesn't read, the device's buffers fill and further CAN frames are lost (counted here).
 *
 * Firmware version 0.1.3 (the one RESOLVE.md's bus setup was verified with), USB 1209:0001 "resolve-can-1".
 */
(function (root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.RC1Fake = api;
})(typeof globalThis !== 'undefined' ? globalThis : this, function () {
  'use strict';

  const USB_VENDOR_ID = 0x1209;
  const USB_PRODUCT_ID = 0x0001;
  const PRODUCT = 'resolve-can-1';
  const DEFAULT_INFO = { serial: '205A31A44E5600C0FAE00001', version: [0, 1, 3] };

  const DLC_LEN = [0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64];
  const CAN_SFF_MASK = 0x7ff;
  const CAN_EFF_MASK = 0x1fffffff;
  const HEX = '0123456789ABCDEF';

  function dlcToLen(dlc, fd) {
    const l = DLC_LEN[dlc & 0xf];
    return fd ? l : Math.min(l, 8);
  }
  function lenToDlc(len) {
    const i = DLC_LEN.findIndex((l) => l >= len);
    return i < 0 ? 15 : i;
  }

  /** A CAN frame: {id, ext, fd, brs, esi, rtr, dlc, data: Uint8Array(64)}; `err` set for SocketCAN error frames. */
  function frame(o) {
    const data = new Uint8Array(64);
    const src = o.data || [];
    data.set(src.slice ? src.slice(0, 64) : src);
    const fd = !!o.fd;
    const dlc = o.dlc !== undefined ? o.dlc : fd ? lenToDlc(src.length) : Math.min(src.length, 8);
    return { id: o.id >>> 0, ext: !!o.ext, fd, brs: fd && !!o.brs, esi: fd && !!o.esi, rtr: !fd && !!o.rtr,
      dlc, data, err: !!o.err };
  }
  function frameLen(f) {
    return f.rtr ? 0 : dlcToLen(f.dlc, f.fd);
  }
  function frameData(f) {
    return f.data.slice(0, frameLen(f));
  }

  // SocketCAN error frame flags (firmware/proto/src/status.rs `err`).
  const ERR = {
    LOSTARB: 0x2, CRTL: 0x4, PROT: 0x8, ACK: 0x20, BUSOFF: 0x40, BUSERROR: 0x80, RESTARTED: 0x100, CNT: 0x200,
    CRTL_RX_OVERFLOW: 0x01, CRTL_RX_WARNING: 0x04, CRTL_TX_WARNING: 0x08, CRTL_RX_PASSIVE: 0x10,
    CRTL_TX_PASSIVE: 0x20, CRTL_ACTIVE: 0x40, CRTL_TX_OVERFLOW: 0x02,
    PROT_FORM: 0x02, PROT_STUFF: 0x04, PROT_BIT0: 0x08, PROT_BIT1: 0x10, PROT_LOC_CRC_SEQ: 0x08,
  };
  /** Error frames for the `errors` traffic mode, as the firmware builds them (status.rs error_frame). */
  const ERROR_FRAMES = {
    warning: (tec, rec) => frame({ id: ERR.CRTL | ERR.CNT, err: true, data: [0, ERR.CRTL_TX_WARNING, 0, 0, 0, 0, tec, rec] }),
    passive: (tec, rec) => frame({ id: ERR.CRTL | ERR.CNT, err: true, data: [0, ERR.CRTL_TX_PASSIVE, 0, 0, 0, 0, tec, rec] }),
    busoff: () => frame({ id: ERR.BUSOFF | ERR.CNT, err: true, data: [0, 0, 0, 0, 0, 0, 255, 0] }),
    restarted: () => frame({ id: ERR.RESTARTED | ERR.CNT, err: true, data: [0, 0, 0, 0, 0, 0, 0, 0] }),
    ack: () => frame({ id: ERR.ACK, err: true, data: [0, 0, 0, 0x19, 0, 0, 0, 0] }),
    overflow: () => frame({ id: ERR.CRTL, err: true, data: [0, ERR.CRTL_RX_OVERFLOW, 0, 0, 0, 0, 0, 0] }),
  };

  const Req = {
    bitrate: (rate) => ({ kind: 'bitrate', rate }),
    dataBitrate: (rate) => ({ kind: 'dataBitrate', rate }),
    open: (listenOnly) => ({ kind: 'open', listenOnly }),
    close: () => ({ kind: 'close' }),
    send: (f) => ({ kind: 'send', frame: f }),
  };

  function u32le(v) {
    return [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff];
  }
  function rdU32(a, i) {
    return (a[i] | (a[i + 1] << 8) | (a[i + 2] << 16) | (a[i + 3] << 24)) >>> 0;
  }

  // ---------------------------------------------------------------- GVRET (firmware/proto/src/gvret.rs)
  const START = 0xf1;
  const OP = { BUILD_CAN_FRAME: 0, TIME_SYNC: 1, DIG_INPUTS: 2, ANA_INPUTS: 3, SET_DIG_OUT: 4, SETUP_CANBUS: 5,
    GET_CANBUS_PARAMS: 6, GET_DEV_INFO: 7, SET_SW_MODE: 8, KEEPALIVE: 9, SET_SYSTYPE: 10, ECHO_CAN_FRAME: 11,
    GET_NUMBUSES: 12, GET_EXT_BUSES: 13, SET_EXT_BUSES: 14, BUILD_FD_FRAME: 20, SETUP_FD: 21, GET_FD: 22 };
  const HEADER = 6;

  function gvretFrameBytes(f, ts) {
    const len = frameLen(f);
    let id = f.id & (f.ext ? CAN_EFF_MASK : CAN_SFF_MASK);
    if (f.ext) id = (id | 0x80000000) >>> 0;
    const m = [START, f.fd ? OP.BUILD_FD_FRAME : OP.BUILD_CAN_FRAME, ...u32le(ts >>> 0), ...u32le(id)];
    if (f.fd) m.push(len | (f.brs ? 0x80 : 0), 0);
    else m.push(len);
    for (let i = 0; i < len; i++) m.push(f.data[i]);
    m.push(0);
    return m;
  }

  class Gvret {
    constructor(info) {
      this.info = info;
      this.nowUs = 0;
      this.rx = 'idle';
      this.op = 0;
      this.need = 0;
      this.args = new Uint8Array(HEADER + 64);
      this.n = 0;
      this.bitrate = 500000;
      this.fd = true;
      this.dataBitrate = 2000000;
      this.listenOnly = false;
      this.open = false;
      this.pending = null; // {steps, at}
    }
    feed(b, out) {
      if (this.rx === 'idle') {
        if (b === START) this.rx = 'command';
        return null;
      }
      if (this.rx === 'command') return this.command(b, out);
      if (this.n < this.args.length) this.args[this.n++] = b;
      if (this.n < this.need) return null;
      this.rx = 'idle';
      return this.run(this.op, out);
    }
    answer(a, out) {
      if (!this.pending) return null;
      const { steps, at } = this.pending;
      this.pending = null;
      if (steps[at] === 'close') this.open = false;
      else if (steps[at] === 'open' && a === 'ok') this.open = true;
      const next = at + 1;
      if (a === 'refused' || next >= steps.length) return null;
      this.pending = { steps, at: next };
      return this.request(steps[next]);
    }
    event(ev, out) {
      if (ev.echo || !this.open || ev.frame.err) return false;
      out.push(...gvretFrameBytes(ev.frame, ev.timestampUs));
      return true;
    }
    command(b, out) {
      this.rx = 'idle';
      this.n = 0;
      let need;
      switch (b) {
        case START:
          this.rx = 'command';
          return null;
        case OP.BUILD_CAN_FRAME: case OP.BUILD_FD_FRAME: case OP.ECHO_CAN_FRAME: need = HEADER; break;
        case OP.SETUP_CANBUS: need = 8; break;
        case OP.SETUP_FD: need = 6; break;
        case OP.SET_EXT_BUSES: need = 12; break;
        case OP.SET_DIG_OUT: case OP.SET_SW_MODE: case OP.SET_SYSTYPE: need = 1; break;
        default:
          this.reply(b, out);
          return null;
      }
      this.rx = 'args';
      this.op = b;
      this.need = need;
      return null;
    }
    run(op, out) {
      if (op === OP.BUILD_CAN_FRAME || op === OP.BUILD_FD_FRAME || op === OP.ECHO_CAN_FRAME) {
        const len = this.args[5] & 0x7f;
        if (len > 64) return null;
        if (this.n < HEADER + len) {
          this.rx = 'args';
          this.op = op;
          this.need = HEADER + len;
          return null;
        }
        return this.frame(op, out);
      }
      if (op === OP.SETUP_CANBUS) return this.setupCanbus();
      if (op === OP.SETUP_FD) return this.setupFd();
      return null;
    }
    frame(op, out) {
      const a = this.args;
      const raw = rdU32(a, 0);
      const ext = (raw & 0x80000000) !== 0;
      const id = raw & 0x7fffffff;
      const len = a[5] & 0x7f;
      const fd = op === OP.BUILD_FD_FRAME || len > 8;
      const f = frame({ id, ext, fd, brs: op === OP.BUILD_FD_FRAME && (a[5] & 0x80) !== 0,
        dlc: fd ? lenToDlc(len) : len, data: a.slice(HEADER, HEADER + len) });
      const bus = a[4] & 3;
      if (id > (ext ? CAN_EFF_MASK : CAN_SFF_MASK)) return null;
      if (op === OP.ECHO_CAN_FRAME) {
        out.push(...gvretFrameBytes(f, this.nowUs));
        return null;
      }
      if (bus !== 0 || !this.open || this.listenOnly || (fd && !this.fd)) return null;
      return Req.send(f);
    }
    setupCanbus() {
      const v = rdU32(this.args, 0);
      let bitrate = this.bitrate, listenOnly = this.listenOnly, enable = false;
      if (v !== 0) {
        const speed = Math.min(v & 0xfffff, 1000000);
        if (speed !== 0) bitrate = speed;
        enable = true;
        if (v & 0x80000000) {
          enable = (v & 0x40000000) !== 0;
          listenOnly = (v & 0x20000000) !== 0;
        }
      }
      return this.apply(bitrate, this.fd, this.dataBitrate, listenOnly, enable);
    }
    setupFd() {
      const a = this.args;
      const rate = rdU32(a, 2);
      if (a[0] !== 0) return null;
      const fd = (a[1] & 0x02) !== 0;
      const data = rate >= 1000000 && rate <= 8000000 ? rate : this.dataBitrate;
      return this.apply(this.bitrate, fd, data, this.listenOnly, this.open);
    }
    apply(bitrate, fd, dataBitrate, listenOnly, enable) {
      const before = [this.bitrate, this.effectiveDataBitrate(), this.listenOnly].join();
      Object.assign(this, { bitrate, fd, dataBitrate, listenOnly });
      const changed = before !== [this.bitrate, this.effectiveDataBitrate(), this.listenOnly].join();
      let steps = [];
      if (this.open && !enable) steps = ['close'];
      else if (this.open && enable && changed) steps = ['close', 'bitrate', 'dataBitrate', 'open'];
      else if (!this.open && enable) steps = ['bitrate', 'dataBitrate', 'open'];
      if (!steps.length) return null;
      this.pending = { steps, at: 0 };
      return this.request(steps[0]);
    }
    effectiveDataBitrate() {
      return this.fd ? this.dataBitrate : 0;
    }
    request(step) {
      if (step === 'close') return Req.close();
      if (step === 'bitrate') return Req.bitrate(this.bitrate);
      if (step === 'dataBitrate') return Req.dataBitrate(this.effectiveDataBitrate());
      return Req.open(this.listenOnly);
    }
    reply(op, out) {
      const r = [START, op];
      const [major, minor, patch] = this.info.version;
      switch (op) {
        case OP.TIME_SYNC: r.push(...u32le(this.nowUs)); break;
        case OP.DIG_INPUTS: r.push(0, 0); break;
        case OP.ANA_INPUTS: r.push(...new Array(15).fill(0)); break;
        case OP.GET_CANBUS_PARAMS:
          r.push((this.open ? 1 : 0) | (this.listenOnly ? 0x10 : 0), ...u32le(this.bitrate), 0, 0, 0, 0, 0);
          break;
        case OP.GET_DEV_INFO: {
          const build = Math.min(0xffff, major * 100 + minor * 10 + patch);
          r.push(build & 0xff, build >> 8, 0x20, 0, 0, 0);
          break;
        }
        case OP.KEEPALIVE: r.push(0xde, 0xad); break;
        case OP.GET_NUMBUSES: r.push(1); break;
        case OP.GET_EXT_BUSES: r.push(...new Array(15).fill(0)); break;
        case OP.GET_FD: r.push(1, 0x01 | (this.fd ? 0x02 : 0), ...u32le(this.dataBitrate)); break;
        default: return;
      }
      out.push(...r);
    }
  }

  // ---------------------------------------------------------------- slcan (firmware/proto/src/slcan.rs)
  const SLCAN_BITRATES = [10000, 20000, 50000, 100000, 125000, 250000, 500000, 800000, 1000000, 83333];
  const LINE_MAX = 1 + 8 + 2 + 128;
  const CR = 0x0d, BEL = 0x07;
  const FLAG = { ERROR_WARNING: 0x04, DATA_OVERRUN: 0x08, ERROR_PASSIVE: 0x20, ARBITRATION_LOST: 0x40, BUS_ERROR: 0x80 };

  const isHex = (c) => (c >= 0x30 && c <= 0x39) || (c >= 0x41 && c <= 0x46) || (c >= 0x61 && c <= 0x66);
  const hexv = (c) => (isHex(c) ? parseInt(String.fromCharCode(c), 16) : null);
  function hexValue(s) {
    let v = 0;
    for (const c of s) {
      const d = hexv(c);
      if (d === null) return null;
      v = ((v << 4) | d) >>> 0;
    }
    return v;
  }
  function putHex(l, v, digits) {
    for (let i = digits - 1; i >= 0; i--) l.push(HEX.charCodeAt((v >>> (4 * i)) & 0xf));
  }
  const ascii = (s) => Array.from(s, (c) => c.charCodeAt(0));

  function btrBitrate(s) {
    const v = hexValue(s);
    if (v === null) return null;
    const btr0 = v >> 8, btr1 = v & 0xff;
    const brp = (btr0 & 0x3f) + 1;
    const tq = 1 + (btr1 & 0xf) + 1 + ((btr1 >> 4) & 7) + 1;
    const d = brp * tq;
    return Math.floor((8000000 + Math.floor(d / 2)) / d);
  }

  function slcanParseFrame(l) {
    const kind = l[0];
    if (kind === undefined) return null;
    const ext = kind >= 0x41 && kind <= 0x5a;
    const k = String.fromCharCode(kind).toLowerCase();
    const t = { t: [false, false, false], r: [true, false, false], d: [false, true, false], b: [false, true, true] }[k];
    if (!t) return null;
    const [rtr, fd, brs] = t;
    const idLen = ext ? 8 : 3;
    if (l.length < 1 + idLen) return null;
    const id = hexValue(l.slice(1, 1 + idLen));
    if (id === null || (!ext && id > CAN_SFF_MASK)) return null;
    const rest = l.slice(1 + idLen);
    let dlc, data;
    if (fd) {
      if (!rest.length) return null;
      const d = hexv(rest[0]);
      if (d === null) return null;
      if (rest.length === 1 + 2 * dlcToLen(d, true)) {
        dlc = d;
        data = rest.slice(1);
      } else {
        if (rest.length < 2) return null;
        const u = hexv(rest[1]);
        if (!(d === 1 && u !== null && u <= 5)) return null;
        dlc = 10 + u;
        if (rest.length !== 2 + 2 * dlcToLen(dlc, true)) return null;
        data = rest.slice(2);
      }
    } else {
      if (!rest.length) return null;
      const d = (rest[0] - 0x30) & 0xff;
      const need = rtr ? 0 : 2 * d;
      if (d > 8 || rest.length !== 1 + need) return null;
      dlc = d;
      data = rest.slice(1);
    }
    const bytes = [];
    for (let i = 0; i + 1 < data.length; i += 2) {
      const hi = hexv(data[i]), lo = hexv(data[i + 1]);
      if (hi === null || lo === null) return null;
      bytes.push((hi << 4) | lo);
    }
    return frame({ id: id & CAN_EFF_MASK, ext, rtr, fd, brs, dlc, data: bytes });
  }

  function statusFlags(f) {
    const id = f.id, crtl = f.data[1];
    let s = 0;
    if (id & ERR.CRTL) {
      if (crtl & (ERR.CRTL_RX_WARNING | ERR.CRTL_TX_WARNING)) s |= FLAG.ERROR_WARNING;
      if (crtl & (ERR.CRTL_RX_PASSIVE | ERR.CRTL_TX_PASSIVE)) s |= FLAG.ERROR_PASSIVE;
      if (crtl & (ERR.CRTL_RX_OVERFLOW | ERR.CRTL_TX_OVERFLOW)) s |= FLAG.DATA_OVERRUN;
    }
    if (id & (ERR.BUSOFF | ERR.PROT | ERR.BUSERROR | ERR.ACK)) s |= FLAG.BUS_ERROR;
    if (id & ERR.LOSTARB) s |= FLAG.ARBITRATION_LOST;
    return s;
  }

  function errorLines(f, out) {
    const id = f.id, crtl = f.data[1], prot = f.data[2], loc = f.data[3];
    const l = [];
    let state = null;
    if (id & ERR.BUSOFF) state = 'b';
    else if (id & ERR.RESTARTED) state = 'a';
    else if (!(id & ERR.CRTL)) state = null;
    else if (crtl & (ERR.CRTL_RX_PASSIVE | ERR.CRTL_TX_PASSIVE)) state = 'p';
    else if (crtl & (ERR.CRTL_RX_WARNING | ERR.CRTL_TX_WARNING)) state = 'w';
    else if (crtl & ERR.CRTL_ACTIVE) state = 'a';
    if (state) {
      const [tec, rec] = id & ERR.CNT ? [f.data[6], f.data[7]] : [0, 0];
      l.push(0x73, state.charCodeAt(0));
      for (const v of [rec, tec]) l.push(0x30 + Math.floor(v / 100), 0x30 + (Math.floor(v / 10) % 10), 0x30 + (v % 10));
      l.push(CR);
    }
    const codes = [];
    const p = (id & ERR.PROT) !== 0;
    const code = (c, on) => on && codes.push(c.charCodeAt(0));
    code('a', (id & ERR.ACK) !== 0);
    code('b', p && (prot & ERR.PROT_BIT0) !== 0);
    code('B', p && (prot & ERR.PROT_BIT1) !== 0);
    code('c', p && loc === ERR.PROT_LOC_CRC_SEQ);
    code('f', p && (prot & ERR.PROT_FORM) !== 0);
    code('s', p && (prot & ERR.PROT_STUFF) !== 0);
    code('o', (id & ERR.CRTL) !== 0 && (crtl & ERR.CRTL_RX_OVERFLOW) !== 0);
    code('O', (id & ERR.CRTL) !== 0 && (crtl & ERR.CRTL_TX_OVERFLOW) !== 0);
    if (codes.length) l.push(0x65, 0x30 + codes.length, ...codes, CR);
    out.push(...l);
    return l.length > 0;
  }

  class Slcan {
    constructor(info) {
      this.info = info;
      this.line = [];
      this.overflow = false;
      this.bitrate = 500000;
      this.dataBitrate = 2000000;
      this.silent = false;
      this.open = false;
      this.listenOnly = false;
      this.timestamps = false;
      this.status = 0;
      this.tsLast = null;
      this.tsExt = 0;
      this.pending = null;
    }
    feed(b, out) {
      const end = b === CR || (b === 0x0a && (this.line.length > 0 || this.overflow));
      if (end) {
        const req = this.overflow ? this.refuse(out) : this.command(out);
        this.line = [];
        this.overflow = false;
        return req;
      }
      if (b === 0x0a) return null;
      if (this.line.length < LINE_MAX) this.line.push(b);
      else this.overflow = true;
      return null;
    }
    answer(a, out) {
      const p = this.pending;
      this.pending = null;
      if (!p) return null;
      if (a === 'refused') return this.refuse(out);
      switch (p.kind) {
        case 'bitrate': this.bitrate = p.rate; return this.done(out);
        case 'dataBitrate': this.dataBitrate = p.rate; return this.done(out);
        case 'open':
          if (p.step === 0) {
            this.pending = { kind: 'open', listenOnly: p.listenOnly, step: 1 };
            return Req.dataBitrate(this.dataBitrate);
          }
          if (p.step === 1) {
            this.pending = { kind: 'open', listenOnly: p.listenOnly, step: 2 };
            return Req.open(p.listenOnly);
          }
          this.open = true;
          this.listenOnly = p.listenOnly;
          return this.done(out);
        case 'close': this.open = false; return this.done(out);
        case 'send': out.push(p.ext ? 0x5a : 0x7a, CR); return null;
      }
      return null;
    }
    event(ev, out) {
      const ms = this.ms(ev.timestampUs);
      const f = ev.frame;
      if (f.err) {
        this.status |= statusFlags(f);
        return this.open && errorLines(f, out);
      }
      if (ev.echo || !this.open) return false;
      const l = [];
      const kind = f.fd ? (f.brs ? 'b' : 'd') : f.rtr ? 'r' : 't';
      if (f.ext) {
        l.push(kind.toUpperCase().charCodeAt(0));
        putHex(l, f.id & CAN_EFF_MASK, 8);
      } else {
        l.push(kind.charCodeAt(0));
        putHex(l, f.id & CAN_SFF_MASK, 3);
      }
      const dlc = f.fd ? f.dlc & 0xf : Math.min(f.dlc, 8);
      l.push(HEX.charCodeAt(dlc));
      const len = frameLen(f);
      for (let i = 0; i < len; i++) putHex(l, f.data[i], 2);
      if (this.timestamps) putHex(l, ms, 4);
      l.push(CR);
      out.push(...l);
      return true;
    }
    done(out) {
      out.push(CR);
      return null;
    }
    refuse(out) {
      out.push(BEL);
      return null;
    }
    ask(p, r) {
      this.pending = p;
      return r;
    }
    command(out) {
      const l = this.line;
      const n = l.length;
      if (!n) return this.done(out);
      const c = String.fromCharCode(l[0]);
      const arg = n > 1 ? l[1] : 0;
      const argc = String.fromCharCode(arg);
      if (c === 'O' && n === 1) return this.ask({ kind: 'open', listenOnly: this.silent, step: 0 }, Req.bitrate(this.bitrate));
      if (c === 'L' && n === 1) return this.ask({ kind: 'open', listenOnly: true, step: 0 }, Req.bitrate(this.bitrate));
      if (c === 'C' && n === 1) return this.ask({ kind: 'close' }, Req.close());
      if (c === 'S' && n === 2) {
        const r = SLCAN_BITRATES[(arg - 0x30) & 0xff];
        return r ? this.ask({ kind: 'bitrate', rate: r }, Req.bitrate(r)) : this.refuse(out);
      }
      if (c === 's' && n === 5) {
        const r = btrBitrate(l.slice(1, 5));
        return r !== null ? this.ask({ kind: 'bitrate', rate: r }, Req.bitrate(r)) : this.refuse(out);
      }
      if (c === 'Y' && n === 2) {
        if (argc >= '1' && argc <= '8') {
          const r = (arg - 0x30) * 1000000;
          return this.ask({ kind: 'dataBitrate', rate: r }, Req.dataBitrate(r));
        }
        return this.refuse(out);
      }
      if (c === 'Z' && n === 2 && (argc === '0' || argc === '1')) {
        this.timestamps = argc === '1';
        return this.done(out);
      }
      if (c === 'F' && n === 1) {
        const s = this.status;
        this.status = 0;
        out.push(0x46, HEX.charCodeAt(s >> 4), HEX.charCodeAt(s & 0xf), CR);
        return null;
      }
      const [major, minor] = this.info.version;
      const digit = (v) => 0x30 + Math.min(v, 9);
      if (c === 'V' && n === 1) {
        out.push(0x56, 0x31, 0x30, digit(major), digit(minor), CR);
        return null;
      }
      if (c === 'v' && n === 1) {
        const M = Math.min(major, 99), m = Math.min(minor, 99);
        out.push(0x76, digit(Math.floor(M / 10)), digit(M % 10), digit(Math.floor(m / 10)), digit(m % 10), CR);
        return null;
      }
      if (c === 'N' && n === 1) {
        out.push(0x4e, ...ascii(this.info.serial.slice(0, 64)), CR);
        return null;
      }
      if (c === 'M' && n === 2 && (argc === '0' || argc === '1')) {
        this.silent = argc === '1';
        return this.done(out);
      }
      if ((c === 'M' || c === 'm') && n === 9 && l.slice(1).every(isHex)) return this.done(out);
      if (c === 'A' && n === 2 && argc === '1') return this.done(out);
      if (c === 'X' && n === 2 && (argc === '0' || argc === '1')) return this.done(out);
      if (c === 'U' && n === 2 && argc >= '0' && argc <= '9') return this.done(out);
      if ('tTrRdDbB'.includes(c)) {
        const f = slcanParseFrame(l);
        if (f && this.open && !this.listenOnly) return this.ask({ kind: 'send', ext: f.ext }, Req.send(f));
        return this.refuse(out);
      }
      return this.refuse(out);
    }
    ms(ts) {
      ts >>>= 0;
      const last = this.tsLast === null ? ts : this.tsLast;
      if (this.tsLast === null) this.tsExt = ts;
      const d = ((ts - last) | 0); // wrapping difference as i32
      const ext = this.tsExt + d;
      if (d >= 0) {
        this.tsExt = ext;
        this.tsLast = ts;
      }
      return Math.floor(ext / 1000) % 60000;
    }
  }

  // ---------------------------------------------------------------- the port (firmware/proto/src/serial.rs)
  class Port {
    constructor(info) {
      this.info = info;
      this.mode = 'slcan';
      this.slcan = new Slcan(info);
      this.gvret = new Gvret(info);
      this.e7 = false;
      this.closing = false;
    }
    reset() {
      const now = this.gvret.nowUs;
      Object.assign(this, new Port(this.info));
      this.gvret.nowUs = now;
    }
    setTime(us) {
      this.gvret.nowUs = us >>> 0;
    }
    feed(b, out) {
      if (this.mode === 'gvret') return this.gvret.feed(b, out);
      if (b === 0xe7 && this.e7) {
        const wasOpen = this.slcan.open;
        this.slcan = new Slcan(this.info);
        this.mode = 'gvret';
        this.e7 = false;
        this.closing = wasOpen;
        return wasOpen ? Req.close() : null;
      }
      this.e7 = b === 0xe7;
      return this.slcan.feed(b, out);
    }
    answer(a, out) {
      if (this.closing) {
        this.closing = false;
        return null;
      }
      return this.mode === 'slcan' ? this.slcan.answer(a, out) : this.gvret.answer(a, out);
    }
    event(ev, out) {
      return this.mode === 'slcan' ? this.slcan.event(ev, out) : this.gvret.event(ev, out);
    }
  }

  // ---------------------------------------------------------------- traffic
  // The Leaf AZE0 EV-CAN as resolve-hil's scenarios/leaf_bus.yaml has it (IDs, DBC lengths, periods): ~836 frames/s.
  const LEAF_TABLE = [
    [0x11a, 8, 10], [0x1d4, 8, 10], [0x1da, 8, 10], [0x1db, 8, 10], [0x1dc, 8, 10], [0x1f2, 8, 10], [0x284, 8, 20],
    [0x390, 8, 100], [0x393, 8, 100], [0x45e, 1, 100], [0x481, 2, 100], [0x50a, 8, 100], [0x50b, 7, 100],
    [0x50c, 6, 100], [0x539, 8, 100], [0x54a, 8, 100], [0x54b, 8, 100], [0x54c, 8, 100], [0x54f, 8, 500],
    [0x55a, 8, 100], [0x55b, 8, 100], [0x56e, 1, 100], [0x59e, 8, 500], [0x5a9, 8, 100], [0x5b9, 7, 100],
    [0x5bc, 8, 100], [0x5c0, 8, 500], [0x60d, 8, 100],
  ];

  /**
   * Traffic modes. Each makes a schedule: `next()` gives the next frame and its time on the bus (ms from the start).
   * Frames of the full-load modes carry a sequence number (data[0..3], little-endian) for loss checks.
   *   idle            nothing
   *   leaf            the Leaf table, 500 kbit/s classic, ~836 frames/s
   *   classic-full    rc1's measured full rate, classic 8 bytes at 1 Mbit/s: 8 000 frames/s (`rate` to change)
   *   fd64-full       rc1's measured full rate, FD 64 bytes with BRS at 500k/8M: 6 900 frames/s (`rate`)
   *   fd-mix          every FD length 0..64, BRS on/off, 11/29-bit IDs, classic and remote frames in between: 500/s
   *   errors          the Leaf table plus an error frame every 100 ms (warning, passive, bus-off, restart, ACK, overflow)
   */
  function makeTraffic(mode, o = {}) {
    if (mode === 'idle' || !mode) return { next: () => null, name: 'idle' };
    let seq = o.seqStart || 0;
    const seqData = (len) => {
      const d = new Uint8Array(len);
      const s = seq >>> 0;
      d.set(u32le(s).slice(0, Math.min(4, len)));
      for (let i = 4; i < len; i++) d[i] = (s + i) & 0xff;
      return d;
    };
    if (mode === 'classic-full' || mode === 'fd64-full') {
      const fd = mode === 'fd64-full';
      const rate = o.rate || (fd ? 6900 : 8000);
      const len = fd ? 64 : 8;
      const extEvery = o.extendedEvery || 0;
      return {
        name: mode, rate,
        next() {
          const t = (seq * 1000) / rate;
          const ext = extEvery > 0 && seq % extEvery === extEvery - 1;
          const f = frame({ id: ext ? 0x18db33f1 : 0x100 + (seq % 16), ext, fd, brs: fd, data: seqData(len) });
          f.seq = seq++;
          return { t, frame: f };
        },
      };
    }
    if (mode === 'leaf' || mode === 'errors') {
      // A merged periodic schedule: each message at k * period + a small fixed offset.
      const msgs = LEAF_TABLE.map(([id, len, period], i) => ({ id, len, period, k: 0, off: (i * 0.37) % period, cnt: 0 }));
      const errs = ['warning', 'passive', 'busoff', 'restarted', 'ack', 'overflow'];
      let errK = 0;
      return {
        name: mode,
        next() {
          let best = null;
          for (const m of msgs) {
            const t = m.off + m.k * m.period;
            if (!best || t < best.t) best = { t, m };
          }
          if (mode === 'errors') {
            const te = 50 + errK * 100;
            if (te < best.t) {
              const kind = errs[errK++ % errs.length];
              const f = ERROR_FRAMES[kind](97 + (errK % 50), 5);
              f.kind = kind;
              return { t: te, frame: f };
            }
          }
          const m = best.m;
          m.k++;
          const d = new Uint8Array(m.len);
          for (let i = 0; i < m.len; i++) d[i] = (m.cnt + i * 17) & 0xff;
          m.cnt++;
          const f = frame({ id: m.id, data: d });
          f.seq = seq++;
          return { t: best.t, frame: f };
        },
      };
    }
    if (mode === 'fd-mix') {
      const rate = o.rate || 500;
      return {
        name: mode, rate,
        next() {
          const t = (seq * 1000) / rate;
          const i = seq;
          const dlc = i % 16;
          const kind = Math.floor(i / 16) % 6; // fd, fd brs, fd ext, fd ext brs, classic, remote
          let f;
          if (kind < 4) {
            const d = new Uint8Array(dlcToLen(dlc, true)).map((_, j) => (j ^ i) & 0xff);
            f = frame({ id: kind >= 2 ? 0x1234567 + dlc : 0x700 + dlc, ext: kind >= 2, fd: true, brs: kind % 2 === 1, data: d });
          } else if (kind === 4) {
            f = frame({ id: 0x200 + (dlc % 9), data: new Uint8Array(dlc % 9).map((_, j) => j + i) });
          } else {
            f = frame({ id: 0x300 + (dlc % 9), rtr: true, dlc: dlc % 9 });
          }
          f.seq = seq++;
          return { t, frame: f };
        },
      };
    }
    throw new Error(`unknown traffic mode ${mode}`);
  }

  // ---------------------------------------------------------------- the device
  /**
   * rc1 with its CAN channel and a bus. Host side: `write(bytes)` (host -> device), `read(max)` (device -> host),
   * `setControl({dtr, rts, coding})` (the CDC-ACM control lines), `hostClosed()`. Bus side: `setTraffic(mode, opts)`,
   * `inject(frame)`, `advance(nowMs)` (generates the frames due by then). `stats()` for the tests.
   *
   * Options:
   *   info              {serial, version: [major, minor, patch]}
   *   bufferBytes       device -> host bytes held while the host doesn't read, before CAN frames are lost: the firmware's
   *                     1 KB pipe + its CAN ring + the host's tty buffer (default 8192); the Web Serial mock adds the
   *                     port's bufferSize (Chrome's data pipe)
   *   clockStartUs      the device's µs clock at start (u32, wraps; set near 2^32 to test the wrap)
   *   record            keep every frame sent to the host (for exact comparisons; not for long runs)
   *   refuseOpen        the CAN channel is owned by gs_usb: Bitrate/DataBitrate/Open refused (slcan BEL, GVRET stays off)
   *   now               a clock in ms (default performance.now / Date.now)
   */
  class Rc1Device {
    constructor(o = {}) {
      this.opts = o;
      this.info = Object.assign({}, DEFAULT_INFO, o.info || {});
      this.port = new Port(this.info);
      this.now = o.now || (typeof performance !== 'undefined' ? () => performance.now() : () => Date.now());
      this.t0 = this.now();
      this.clockStartUs = o.clockStartUs || 0;
      this.bufferBytes = o.bufferBytes || 8192;
      this.refuseOpen = !!o.refuseOpen;
      this.chan = { open: false, bitrate: 500000, dataBitrate: 0, fdMode: false, listenOnly: false };
      this.fdFlag = false;
      this.out = [];
      this.outLen = 0;
      this.control = { dtr: false, rts: false, coding: null };
      this.traffic = makeTraffic('idle');
      this.trafficStart = 0;
      this.pendingBus = null;
      this.hostFrames = [];
      this.requests = [];
      this.log = [];
      this.garbageEvery = 0;
      this.record = o.record ? [] : null;
      this.resetStats();
    }
    resetStats() {
      this.st = { bus: 0, busWhileClosed: 0, busFdOnClassic: 0, encoded: 0, lost: 0, errorFrames: 0, bytesToHost: 0,
        bytesFromHost: 0, sessions: 0, resets: 0, garbageBytes: 0, firstSeq: null, lastSeq: null, lostSeqs: 0 };
    }
    usNow() {
      return (this.clockStartUs + Math.round((this.now() - this.t0) * 1000)) >>> 0;
    }
    // host -> device ----------------------------------------------------------
    write(bytes) {
      this.st.bytesFromHost += bytes.length;
      this.port.setTime(this.usNow());
      for (const b of bytes) {
        const out = [];
        let next = this.port.feed(b, out);
        this.reply(out);
        while (next) {
          const a = this.execute(next);
          const o2 = [];
          next = this.port.answer(a, o2);
          this.reply(o2);
        }
      }
    }
    execute(req) {
      this.requests.push(req.kind === 'send' ? { kind: 'send' } : req);
      const c = this.chan;
      switch (req.kind) {
        case 'bitrate':
          if (this.refuseOpen || c.open || req.rate > 1000000 || req.rate <= 0) return 'refused';
          c.bitrate = req.rate;
          return 'ok';
        case 'dataBitrate':
          if (req.rate === 0) {
            if (c.open) return 'refused';
            this.fdFlag = false;
            return 'ok';
          }
          if (this.refuseOpen || c.open || req.rate < 1000000 || req.rate > 8000000) return 'refused';
          c.dataBitrate = req.rate;
          this.fdFlag = true;
          return 'ok';
        case 'open':
          if (this.refuseOpen || c.open) return 'refused';
          Object.assign(c, { open: true, listenOnly: req.listenOnly, fdMode: this.fdFlag });
          this.log.push(`open ${c.bitrate}${c.fdMode ? '/' + c.dataBitrate : ''}${c.listenOnly ? ' listen-only' : ''}`);
          return 'ok';
        case 'close':
          if (c.open) this.log.push('close');
          c.open = false;
          return 'ok';
        case 'send':
          if (!c.open) return 'refused';
          this.hostFrames.push(Object.assign({ tUs: this.usNow() }, req.frame));
          return 'ok';
      }
      return 'refused';
    }
    reply(bytes) {
      if (!bytes.length) return;
      this.out.push(Uint8Array.from(bytes));
      this.outLen += bytes.length;
    }
    /** The CDC-ACM control request (SET_CONTROL_LINE_STATE / SET_LINE_CODING), as control_task sees it. */
    setControl({ dtr, rts, coding }) {
      const c = this.control;
      const now = { dtr: !!dtr, rts: !!rts, coding: coding === undefined ? c.coding : coding };
      const repeatedLow = !c.dtr && !c.rts && !now.dtr && !now.rts && JSON.stringify(now.coding) === JSON.stringify(c.coding);
      if ((c.dtr && !now.dtr) || repeatedLow) this.endSession();
      this.control = now;
    }
    /** The host closed the port: Linux lowers DTR and RTS. */
    hostClosed() {
      this.setControl({ dtr: false, rts: false });
      this.out = [];
      this.outLen = 0;
    }
    endSession() {
      if (this.chan.open) this.log.push('close (session end)');
      this.chan.open = false;
      this.fdFlag = false;
      this.port.reset();
      this.st.resets++;
      this.out = [];
      this.outLen = 0;
    }
    // device -> host ----------------------------------------------------------
    available() {
      this.advance();
      return this.outLen;
    }
    /** Up to `max` bytes for the host (a Uint8Array, maybe empty). */
    read(max = Infinity) {
      this.advance();
      if (!this.outLen) return new Uint8Array(0);
      const n = Math.min(max, this.outLen);
      const r = new Uint8Array(n);
      let at = 0;
      while (at < n) {
        const c = this.out[0];
        const take = Math.min(c.length, n - at);
        r.set(c.subarray(0, take), at);
        at += take;
        if (take === c.length) this.out.shift();
        else this.out[0] = c.subarray(take);
      }
      this.outLen -= n;
      this.st.bytesToHost += n;
      return r;
    }
    // the bus -----------------------------------------------------------------
    setTraffic(mode, o = {}) {
      this.advance();
      this.traffic = makeTraffic(mode, o);
      this.trafficStart = this.now() + (o.delayMs || 0);
      this.trafficEnd = o.seconds ? this.trafficStart + o.seconds * 1000 : Infinity;
      this.garbageEvery = o.garbageEvery || 0;
      this.pendingBus = null;
      this.log.push(`traffic ${mode}`);
    }
    /** Time (ms) when the next bus frame is due, or null. */
    nextDue() {
      if (!this.pendingBus) this.pendingBus = this.traffic.next();
      if (!this.pendingBus) return null;
      const t = this.trafficStart + this.pendingBus.t;
      return t >= this.trafficEnd ? null : t;
    }
    advance(nowMs = this.now()) {
      for (;;) {
        const t = this.nextDue();
        if (t === null || t > nowMs) return;
        const f = this.pendingBus.frame;
        this.pendingBus = null;
        this.onBus(f, (this.clockStartUs + Math.round((t - this.t0) * 1000)) >>> 0);
      }
    }
    /** A frame on the bus now (or at `tsUs`), as another node sends it. */
    inject(f, tsUs) {
      this.onBus(f, tsUs === undefined ? this.usNow() : tsUs);
    }
    onBus(f, tsUs) {
      const st = this.st;
      if (f.err) st.errorFrames++;
      else st.bus++;
      if (!this.chan.open) {
        if (!f.err) st.busWhileClosed++;
        return;
      }
      if (f.fd && !this.chan.fdMode) {
        st.busFdOnClassic++;
        return;
      }
      const out = [];
      if (!this.port.event({ frame: f, timestampUs: tsUs, echo: false }, out)) return;
      if (this.outLen + out.length > this.bufferBytes + (this.hostBufferBytes || 0)) {
        st.lost++;
        if (f.seq !== undefined) st.lostSeqs++;
        return;
      }
      if (this.garbageEvery && this.port.mode === 'gvret' && !f.err && (st.encoded + 1) % this.garbageEvery === 0) {
        const junk = [0x00, 0x55, 0xaa, 0x13, 0x37]; // never 0xF1: frames stay countable
        this.reply(junk);
        st.garbageBytes += junk.length;
      }
      this.reply(out);
      if (!f.err) {
        st.encoded++;
        if (this.record) this.record.push({ id: f.id, ext: f.ext, fd: f.fd, brs: f.brs, rtr: f.rtr, data: Array.from(frameData(f)) });
        if (f.seq !== undefined) {
          if (st.firstSeq === null) st.firstSeq = f.seq;
          st.lastSeq = f.seq;
        }
      }
    }
    stats() {
      this.advance();
      return Object.assign({}, this.st, {
        mode: this.port.mode, channel: Object.assign({}, this.chan), queuedBytes: this.outLen,
        hostFrames: this.hostFrames.length, traffic: this.traffic.name,
      });
    }
  }

  // ---------------------------------------------------------------- Web Serial mock
  /**
   * Replaces navigator.serial with a mock backed by Rc1Device. Options:
   *   ports              how many rc1s are plugged in (default 1)
   *   granted            getPorts() already lists them (a returning visitor) (default false)
   *   requireGesture     requestPort() needs transient user activation, as in Chrome (default true)
   *   chooser            'first' (default): requestPort picks the first matching port, 'cancel': NotFoundError
   *   device             Rc1Device options; traffic {mode, ...opts}: started when the channel opens
   *   usbIntervalMs      the host's reads see new data at most this often (USB Full-Speed frames; default 1)
   *   disconnectAfterMs  unplug this long after open (the `disconnect` case)
   * The test reaches everything through `window.__rc1` ({ports, device, stats(), setTraffic(), unplug(), replug(), events}).
   */
  function installWebSerial(o = {}) {
    const g = globalThis;
    const events = [];
    const ev = (type, detail) => events.push(Object.assign({ type, t: Math.round(performance.now()) }, detail || {}));

    class MockSerial extends EventTarget {
      constructor() {
        super();
        this.onconnect = null;
        this.ondisconnect = null;
      }
      async getPorts() {
        ev('getPorts');
        return ports.filter((p) => p._granted && p._plugged);
      }
      async requestPort(opts = {}) {
        ev('requestPort', { filters: opts.filters || null });
        const activation = g.navigator.userActivation;
        if (o.requireGesture !== false && activation && !activation.isActive) {
          throw new DOMException("Failed to execute 'requestPort' on 'Serial': Must be handling a user gesture to show a permission request.", 'SecurityError');
        }
        const filters = opts.filters || [];
        const match = (p) => !filters.length || filters.some((f) =>
          (f.usbVendorId === undefined || f.usbVendorId === USB_VENDOR_ID) &&
          (f.usbProductId === undefined || f.usbProductId === USB_PRODUCT_ID) && f.bluetoothServiceClassId === undefined);
        const candidates = ports.filter((p) => p._plugged && match(p));
        if (o.chooser === 'cancel' || !candidates.length) {
          throw new DOMException("Failed to execute 'requestPort' on 'Serial': No port selected by the user.", 'NotFoundError');
        }
        const p = candidates[0];
        p._granted = true;
        return p;
      }
      _fire(type, port) {
        const e = new Event(type, { bubbles: true });
        Object.defineProperty(e, 'target', { value: port });
        port.dispatchEvent(new Event(type));
        this.dispatchEvent(e);
        const h = this['on' + type];
        if (typeof h === 'function') h.call(this, e);
      }
    }

    class MockSerialPort extends EventTarget {
      constructor(i) {
        super();
        this._i = i;
        this._granted = !!o.granted;
        this._plugged = true;
        this._dev = new Rc1Device(Object.assign({}, o.device || {}, i ? { info: { serial: DEFAULT_INFO.serial.slice(0, -1) + (i + 1) } } : {}));
        this._isOpen = false;
        this.readable = null;
        this.writable = null;
        this.onconnect = null;
        this.ondisconnect = null;
        this._signals = { dataTerminalReady: false, requestToSend: false, break: false };
        this._lastDelivery = 0;
        this._waiter = null;
      }
      get connected() {
        return this._plugged;
      }
      getInfo() {
        return { usbVendorId: USB_VENDOR_ID, usbProductId: USB_PRODUCT_ID };
      }
      async open(opt) {
        ev('open', { options: opt });
        if (!opt || typeof opt.baudRate !== 'number') {
          throw new TypeError("Failed to execute 'open' on 'SerialPort': Failed to read the 'baudRate' property from 'SerialOptions': Required member is undefined.");
        }
        if (opt.baudRate <= 0) throw new TypeError("Failed to execute 'open' on 'SerialPort': Requested baud rate must be non-zero.");
        if (opt.bufferSize !== undefined && (opt.bufferSize <= 0 || opt.bufferSize > 16 * 1024 * 1024)) {
          throw new TypeError("Failed to execute 'open' on 'SerialPort': Requested buffer size (" + opt.bufferSize + ' bytes) is greater than the maximum allowed (16777216 bytes).');
        }
        if (this._isOpen) throw new DOMException("Failed to execute 'open' on 'SerialPort': The port is already open.", 'InvalidStateError');
        if (!this._plugged) throw new DOMException("Failed to execute 'open' on 'SerialPort': Failed to open serial port.", 'NetworkError');
        this._opt = Object.assign({ dataBits: 8, stopBits: 1, parity: 'none', bufferSize: 255, flowControl: 'none' }, opt);
        this._isOpen = true;
        // Opening a tty raises DTR and RTS (HUPCL) and sets the line coding.
        this._signals = { dataTerminalReady: true, requestToSend: true, break: false };
        this._dev.setControl({ dtr: true, rts: true, coding: [this._opt.baudRate, this._opt.dataBits, this._opt.stopBits, this._opt.parity] });
        this._dev.st.sessions++;
        // Chrome holds up to bufferSize bytes in its data pipe while the page doesn't read: more room before rc1 loses frames.
        this._dev.hostBufferBytes = this._opt.bufferSize;
        this._makeStreams();
        if (o.disconnectAfterMs) setTimeout(() => this._unplug(), o.disconnectAfterMs);
        const t = (o.device || {}).traffic;
        if (t && !this._trafficStarted) {
          this._trafficStarted = true;
          this._dev.setTraffic(t.mode, t);
        }
      }
      _makeStreams() {
        const port = this;
        const dev = this._dev;
        const size = this._opt.bufferSize;
        const interval = o.usbIntervalMs === undefined ? 1 : o.usbIntervalMs;
        this._readErr = null;
        this.readable = new ReadableStream({
          type: 'bytes',
          pull(controller) {
            return new Promise((resolve) => {
              const attempt = () => {
                port._waiter = null;
                if (port._readErr) {
                  controller.error(port._readErr);
                  return resolve();
                }
                if (!port._isOpen) return resolve();
                const now = performance.now();
                const wait = port._lastDelivery + interval - now;
                if (wait > 0) {
                  port._waiter = setTimeout(attempt, wait);
                  return;
                }
                const chunk = dev.read(size);
                if (chunk.length) {
                  port._lastDelivery = now;
                  controller.enqueue(chunk);
                  return resolve();
                }
                const due = dev.nextDue();
                const delay = due === null ? 20 : Math.max(0, Math.min(20, due - performance.now()));
                port._waiter = setTimeout(attempt, delay);
              };
              port._kick = attempt;
              attempt();
            });
          },
          cancel() {
            if (port._waiter) clearTimeout(port._waiter);
            port._waiter = null;
            ev('readable.cancel');
            port.readable = null;
          },
        }, { highWaterMark: 0 });
        this.writable = new WritableStream({
          write(chunk) {
            if (port._readErr) throw port._readErr;
            if (!(chunk instanceof ArrayBuffer || ArrayBuffer.isView(chunk))) {
              throw new TypeError("Failed to execute 'write' on 'UnderlyingSinkBase': The provided value is not of type '(ArrayBuffer or ArrayBufferView)'.");
            }
            const b = chunk instanceof ArrayBuffer ? new Uint8Array(chunk)
              : new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength);
            dev.write(b);
            if (port._waiter && port._kick) {
              clearTimeout(port._waiter);
              port._kick();
            }
          },
          close() {
            ev('writable.close');
            port.writable = null;
          },
          abort() {
            ev('writable.abort');
            port.writable = null;
          },
        }, new CountQueuingStrategy({ highWaterMark: 1 }));
      }
      async setSignals(s = {}) {
        ev('setSignals', { signals: s });
        if (!this._isOpen) throw new DOMException("Failed to execute 'setSignals' on 'SerialPort': The port is closed.", 'InvalidStateError');
        if (!('dataTerminalReady' in s || 'requestToSend' in s || 'break' in s)) {
          throw new TypeError("Failed to execute 'setSignals' on 'SerialPort': Signals dictionary is empty.");
        }
        Object.assign(this._signals, s);
        this._dev.setControl({ dtr: this._signals.dataTerminalReady, rts: this._signals.requestToSend });
      }
      async getSignals() {
        if (!this._isOpen) throw new DOMException("Failed to execute 'getSignals' on 'SerialPort': The port is closed.", 'InvalidStateError');
        return { dataCarrierDetect: false, clearToSend: true, ringIndicator: false, dataSetReady: true };
      }
      async close() {
        ev('close');
        if (!this._isOpen) throw new DOMException("Failed to execute 'close' on 'SerialPort': The port is already closed.", 'InvalidStateError');
        if ((this.readable && this.readable.locked) || (this.writable && this.writable.locked)) {
          throw new TypeError("Failed to execute 'close' on 'SerialPort': Cannot cancel a locked stream");
        }
        if (this.readable) await this.readable.cancel();
        if (this.writable) await this.writable.close().catch(() => {});
        this._finishClose();
      }
      _finishClose() {
        if (this._waiter) clearTimeout(this._waiter);
        this._waiter = null;
        this._isOpen = false;
        this.readable = null;
        this.writable = null;
        this._dev.hostClosed();
      }
      async forget() {
        ev('forget');
        if (this._isOpen) await this.close().catch(() => this._finishClose());
        this._granted = false;
      }
      _unplug() {
        if (!this._plugged) return;
        ev('unplug');
        this._plugged = false;
        const err = new DOMException('The device has been lost.', 'NetworkError');
        this._readErr = err;
        if (this._isOpen) {
          if (this._kick) this._kick();
          this._isOpen = false; // a lost port is closed; streams error
          this._dev.hostClosed();
        }
        serial._fire('disconnect', this);
      }
      _replug() {
        if (this._plugged) return;
        ev('replug');
        this._plugged = true;
        this._readErr = null;
        this.readable = null;
        this.writable = null;
        this._trafficStarted = false;
        this._dev = new Rc1Device(o.device || {});
        serial._fire('connect', this);
      }
    }

    const serial = new MockSerial();
    const ports = Array.from({ length: o.ports === undefined ? 1 : o.ports }, (_, i) => new MockSerialPort(i));
    Object.defineProperty(g.navigator, 'serial', { value: serial, configurable: true });
    g.SerialPort = MockSerialPort;
    g.Serial = MockSerial;

    const api = {
      options: o,
      ports,
      events,
      get device() {
        return ports[0]._dev;
      },
      stats: (i = 0) => ports[i]._dev.stats(),
      setTraffic: (mode, opts, i = 0) => ports[i]._dev.setTraffic(mode, opts || {}),
      inject: (f, i = 0) => ports[i]._dev.inject(frame(f)),
      unplug: (i = 0) => ports[i]._unplug(),
      replug: (i = 0) => ports[i]._replug(),
      hostFrames: (i = 0) => ports[i]._dev.hostFrames.map((f) => ({ id: f.id, ext: f.ext, fd: f.fd, brs: f.brs, rtr: f.rtr,
        dlc: f.dlc, data: Array.from(frameData(f)) })),
      log: (i = 0) => ports[i]._dev.log.slice(),
      recorded: (i = 0, from = 0) => (ports[i]._dev.record || []).slice(from),
      requests: (i = 0) => ports[i]._dev.requests.slice(),
    };
    g.__rc1 = api;
    return api;
  }

  // ---------------------------------------------------------------- file pickers (logging to disk)
  /**
   * File System Access mocks for the logging tests, backed by the origin private file system (OPFS), so the test reads
   * back exactly what the app wrote: showSaveFilePicker() returns an OPFS file named by `suggestedName` (or `name`),
   * showDirectoryPicker() the OPFS root (or a sub-directory `dir`). `window.__pickers` lists the calls and reads files.
   */
  function installFilePickers(o = {}) {
    const g = globalThis;
    const calls = [];
    const root = async () => {
      let d = await g.navigator.storage.getDirectory();
      if (o.dir) d = await d.getDirectoryHandle(o.dir, { create: true });
      return d;
    };
    g.showSaveFilePicker = async (opts = {}) => {
      calls.push({ kind: 'save', opts: JSON.parse(JSON.stringify(opts)) });
      if (o.cancel) throw new DOMException('The user aborted a request.', 'AbortError');
      const name = o.name || opts.suggestedName || 'log.csv';
      return (await root()).getFileHandle(name, { create: true });
    };
    g.showDirectoryPicker = async (opts = {}) => {
      calls.push({ kind: 'directory', opts: JSON.parse(JSON.stringify(opts)) });
      if (o.cancel) throw new DOMException('The user aborted a request.', 'AbortError');
      return root();
    };
    g.__pickers = {
      calls,
      async list() {
        const out = [];
        for await (const [name, h] of (await root()).entries()) {
          if (h.kind === 'file') out.push({ name, size: (await h.getFile()).size });
        }
        return out;
      },
      async read(name) {
        return (await (await (await root()).getFileHandle(name)).getFile()).text();
      },
      async clear() {
        const d = await root();
        for await (const [name] of d.entries()) await d.removeEntry(name, { recursive: true });
      },
    };
    return g.__pickers;
  }

  // ---------------------------------------------------------------- event-loop probe
  /**
   * Measures how responsive the page is: a 10 ms timer and requestAnimationFrame, recording how late each fires.
   * `window.__probe.report()` -> {timer: {n, p50, p95, p99, max}, raf: {...}, longGaps: [ms of gaps > 200 ms]}.
   */
  function installProbe(o = {}) {
    const g = globalThis;
    const period = o.periodMs || 10;
    const timer = [], raf = [], longGaps = [];
    let last = performance.now();
    let running = true;
    const tick = () => {
      if (!running) return;
      const now = performance.now();
      const late = now - last - period;
      timer.push(Math.max(0, late));
      if (now - last > 200) longGaps.push(Math.round(now - last));
      last = now;
      setTimeout(tick, period);
    };
    setTimeout(tick, period);
    let lastRaf = null;
    const rafTick = (t) => {
      if (!running) return;
      if (lastRaf !== null) raf.push(t - lastRaf);
      lastRaf = t;
      if (g.requestAnimationFrame) g.requestAnimationFrame(rafTick);
    };
    if (g.requestAnimationFrame) g.requestAnimationFrame(rafTick);
    const summary = (a) => {
      if (!a.length) return { n: 0 };
      const s = a.slice().sort((x, y) => x - y);
      const q = (p) => Math.round(s[Math.min(s.length - 1, Math.floor(p * s.length))] * 10) / 10;
      return { n: s.length, p50: q(0.5), p95: q(0.95), p99: q(0.99), max: Math.round(s[s.length - 1]) };
    };
    g.__probe = {
      reset() {
        timer.length = 0;
        raf.length = 0;
        longGaps.length = 0;
      },
      stop() {
        running = false;
      },
      report: () => ({ timer: summary(timer), raf: summary(raf), longGaps: longGaps.slice() }),
    };
    return g.__probe;
  }

  // ---------------------------------------------------------------- host-side parsers (tests, self-test page)
  /** A GVRET stream parser as SavvyCAN's (frames 00/14 and the replies), for the self-test page and unit tests. */
  class GvretHostParser {
    constructor() {
      this.buf = [];
      this.frames = 0;
      this.replies = {};
      this.onFrame = null;
    }
    push(bytes) {
      const b = this.buf;
      for (const x of bytes) b.push(x);
      let i = 0;
      const sizes = { 1: 4, 2: 2, 3: 15, 6: 10, 7: 6, 9: 2, 12: 1, 13: 15, 22: 6 };
      while (i < b.length) {
        if (b[i] !== START) {
          i++;
          continue;
        }
        if (i + 1 >= b.length) break;
        const op = b[i + 1];
        if (op === 0 || op === 20) {
          const hdr = op === 0 ? 11 : 12;
          if (i + hdr > b.length) break;
          const len = b[i + 10] & (op === 0 ? 0x0f : 0x7f);
          const total = hdr + len + 1;
          if (i + total > b.length) break;
          const id = rdU32(b, i + 6);
          const f = { ts: rdU32(b, i + 2), id: id & 0x1fffffff, ext: (id & 0x80000000) !== 0, fd: op === 20,
            brs: op === 20 && (b[i + 10] & 0x80) !== 0, data: b.slice(i + hdr, i + hdr + len) };
          this.frames++;
          if (this.onFrame) this.onFrame(f);
          i += total;
          continue;
        }
        const n = sizes[op];
        if (n === undefined) {
          i += 2;
          continue;
        }
        if (i + 2 + n > b.length) break;
        this.replies[op] = b.slice(i + 2, i + 2 + n);
        i += 2 + n;
      }
      this.buf = b.slice(i);
    }
  }

  /** SavvyCAN's native GVRET CSV (framefileio.cpp nativeCSVRow) -> [{ts, id, ext, dir, bus, len, data, fd, brs, esi}]. */
  function parseSavvyCsv(text) {
    const lines = text.split(/\r?\n/).filter((l) => l.length);
    if (!lines.length) return { header: null, frames: [] };
    const header = lines[0];
    const frames = [];
    for (const l of lines.slice(1)) {
      const c = l.split(',');
      if (c[c.length - 1] === '') c.pop();
      const len = parseInt(c[5], 10);
      const flags = /^FD/.test(c[c.length - 1]) ? c.pop() : '';
      frames.push({ ts: Number(c[0]), id: parseInt(c[1], 16), ext: c[2] === 'true', dir: c[3], bus: parseInt(c[4], 10),
        len, data: c.slice(6, 6 + len).map((h) => parseInt(h, 16)), fd: flags.startsWith('FD'),
        brs: flags.includes('BRS'), esi: flags.includes('ESI') });
    }
    return { header, frames };
  }

  /** The sequence numbers in full-load frames (data[0..3] LE) -> {count, first, last, gaps, missing}. */
  function seqCheck(dataArrays) {
    let first = null, last = null, gaps = 0, missing = 0, dup = 0;
    for (const d of dataArrays) {
      const s = (d[0] | (d[1] << 8) | (d[2] << 16) | (d[3] << 24)) >>> 0;
      if (first === null) first = s;
      else if (s !== last + 1) {
        if (s <= last) dup++;
        else {
          gaps++;
          missing += s - last - 1;
        }
      }
      last = s;
    }
    return { count: dataArrays.length, first, last, gaps, missing, dup };
  }

  return { USB_VENDOR_ID, USB_PRODUCT_ID, PRODUCT, DEFAULT_INFO, DLC_LEN, dlcToLen, lenToDlc, frame, frameLen, frameData,
    ERR, ERROR_FRAMES, LEAF_TABLE, Gvret, Slcan, Port, Rc1Device, makeTraffic, gvretFrameBytes, slcanParseFrame,
    installWebSerial, installFilePickers, installProbe, GvretHostParser, parseSavvyCsv, seqCheck };
});
