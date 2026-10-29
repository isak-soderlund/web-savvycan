// web-savvycan: the browser half of the Web Serial QSerialPort stand-in: globalThis.resolveWebSerial. Linked with
// --pre-js, so it runs inside the Emscripten module (MODULARIZE: `Module` is this app instance). C++ calls it through
// EM_JS (webserialbackend_wasm.cpp); it answers through the embind functions Module.webSerial*. Tests read ports().
//
// One session per C++ handle. Per physical port, opens and closes run one after another (a promise chain), so closing
// and reopening at once (SavvyCAN does it when it retries) waits for the close to finish.

globalThis.resolveWebSerial = (function () {
  'use strict';

  // resolve-can rc1: the pid.codes test PID 1209:0001 (firmware/app/src/usb.rs; lab use only until it has its own).
  var RESOLVE_CAN_FILTERS = [{ usbVendorId: 0x1209, usbProductId: 0x0001 }];
  var KNOWN = { '1209:0001': 'resolve-can (rc1)' };
  // Chrome's default is 255 bytes. GVRET can't tell the host about frames rc1 lost, and rc1 loses them as soon as
  // its own buffers fill while the page doesn't read: the page has one thread, and a long Qt call (a dialog, a big
  // redraw) stalls this read loop too. So take Chrome's maximum, 16 MiB: ~23 s of a busy CAN FD bus over GVRET
  // (~0.7 MB/s, 9 000 FD-64 frames/s), minutes of a typical car. If Chrome refuses it, 1 MiB.
  var BUFFER_SIZE = 16 * 1024 * 1024;
  var BUFFER_SIZE_FALLBACK = 1 << 20;

  var serial = (typeof navigator !== 'undefined' && navigator.serial) ? navigator.serial : null;
  var entries = [];          // [{port, name, description, vid, pid, hasUsbIds, chain}], plugged in and granted
  var entryByPort = new Map();
  var sessions = new Map();  // handle -> session
  var ready = Promise.resolve();

  function hex4(n) { return ('0000' + n.toString(16)).slice(-4); }

  function call(name) {
    var fn = Module[name];
    if (typeof fn !== 'function') return; // C++ not started yet
    try { fn.apply(null, Array.prototype.slice.call(arguments, 1)); }
    catch (e) { console.error('webserial: ' + name + ' failed', e); }
  }

  // Names: "WebSerial VVVV:PPPP" (then " #2", " #3" for more with the same IDs), "WebSerial #n" without USB IDs.
  // No dots: SavvyCAN takes a GVRET "port" with a dot for a network address.
  function rename() {
    var seen = {};
    var plain = 0;
    entries.forEach(function (e) {
      if (e.hasUsbIds) {
        var key = hex4(e.vid) + ':' + hex4(e.pid);
        seen[key] = (seen[key] || 0) + 1;
        e.name = 'WebSerial ' + key + (seen[key] > 1 ? ' #' + seen[key] : '');
        e.description = KNOWN[key] || 'USB serial port';
      } else {
        plain += 1;
        e.name = 'WebSerial #' + plain;
        e.description = 'Serial port';
      }
    });
  }

  function entryFor(port) {
    var e = entryByPort.get(port);
    if (e) return e;
    var info = {};
    try { info = port.getInfo() || {}; } catch (err) { /* older mocks */ }
    e = {
      port: port, chain: Promise.resolve(),
      hasUsbIds: info.usbVendorId !== undefined && info.usbProductId !== undefined,
      vid: info.usbVendorId || 0, pid: info.usbProductId || 0, name: '', description: ''
    };
    entryByPort.set(port, e);
    return e;
  }

  function refresh() {
    if (!serial) return Promise.resolve();
    return serial.getPorts().then(function (ports) {
      var before = entries.map(function (e) { return e.name; }).join('|');
      entries = ports.map(entryFor);
      rename();
      if (entries.map(function (e) { return e.name; }).join('|') !== before) call('webSerialPortsChanged');
    }, function (err) {
      console.warn('webserial: getPorts failed', err);
    });
  }

  if (serial) {
    ready = refresh();
    serial.addEventListener('connect', function () { refresh(); });
    serial.addEventListener('disconnect', function (ev) {
      var port = ev.target || ev.port;
      sessions.forEach(function (s, handle) {
        if (s.entry.port === port && !s.closing) lost(handle, s, 'The device was unplugged');
      });
      refresh();
    });
  }

  // Qt's SerialPortError values (webserialport.h).
  var ERR = { DeviceNotFound: 1, Permission: 2, Open: 3, Write: 7, Resource: 9 };

  function openError(err) {
    var n = err && err.name;
    if (n === 'NotFoundError') return ERR.DeviceNotFound;
    if (n === 'SecurityError' || n === 'NotAllowedError') return ERR.Permission;
    return ERR.Open; // InvalidStateError (open elsewhere), NetworkError (busy: e.g. another program has it)
  }

  function text(err) { return err ? (err.name ? err.name + ': ' : '') + (err.message || String(err)) : ''; }

  function lost(handle, s, why) {
    if (s.reported) return;
    s.reported = true;
    call('webSerialError', handle, ERR.Resource, why);
  }

  // Reads as fast as the browser delivers: each chunk is copied into wasm memory and queued for Qt's event loop
  // (WebSerialHub), never waiting for SavvyCAN to take it.
  function readLoop(handle, s) {
    var port = s.entry.port;
    function next() {
      if (s.closing || !port.readable) {
        if (!s.closing) lost(handle, s, 'The serial port is gone');
        return Promise.resolve();
      }
      var reader;
      try { reader = port.readable.getReader(); } catch (e) { lost(handle, s, text(e)); return Promise.resolve(); }
      s.reader = reader;
      function pump() {
        return reader.read().then(function (r) {
          if (r.done) return 'done';
          if (r.value && r.value.length) call('webSerialData', handle, r.value);
          return pump();
        });
      }
      return pump().then(function () {
        try { reader.releaseLock(); } catch (e) { /* ignore */ }
        s.reader = null;
        if (!s.closing) lost(handle, s, 'The serial port stopped');
      }, function (err) {
        try { reader.releaseLock(); } catch (e) { /* ignore */ }
        s.reader = null;
        if (s.closing) return;
        var n = err && err.name;
        // Non-fatal: Web Serial gives a new readable stream after these; the bytes are lost, the stream goes on.
        if (n === 'BufferOverrunError' || n === 'BreakError' || n === 'FramingError' || n === 'ParityError') {
          console.warn('webserial: read error, data lost, reading on: ' + text(err));
          return next();
        }
        lost(handle, s, text(err)); // NetworkError: the device is gone
      });
    }
    s.reading = next();
  }

  function open(handle, name, options) {
    var s = { handle: handle, closing: false, opened: false, reported: false, writer: null, reader: null,
              reading: Promise.resolve(), signals: null, entry: null };
    sessions.set(handle, s);
    ready.then(function () {
      if (s.closing) return;
      var entry = entries.find(function (e) { return e.name === name; });
      if (!entry) {
        sessions.delete(handle);
        call('webSerialOpened', handle, false, ERR.DeviceNotFound,
             name + ' is not available. Plug the device in, or use "Choose device".');
        return;
      }
      s.entry = entry;
      entry.chain = entry.chain.then(function () {
        if (s.closing) return;
        var opts = { baudRate: options.baudRate, flowControl: options.flowControl, bufferSize: BUFFER_SIZE };
        return entry.port.open(opts).catch(function (err) {
          if (!(err instanceof TypeError) || opts.bufferSize === BUFFER_SIZE_FALLBACK) throw err;
          opts.bufferSize = BUFFER_SIZE_FALLBACK; // an older or stricter browser: smaller buffer
          return entry.port.open(opts);
        }).then(function () {
          if (s.closing) return entry.port.close().catch(function () {});
          s.opened = true;
          s.writer = entry.port.writable.getWriter();
          readLoop(handle, s);
          var signals = s.signals;
          s.signals = null;
          var applied = signals ? entry.port.setSignals(signals).catch(function (e) {
            console.warn('webserial: setSignals failed', e);
          }) : Promise.resolve();
          return applied.then(function () { call('webSerialOpened', handle, true, 0, ''); });
        }, function (err) {
          sessions.delete(handle);
          call('webSerialOpened', handle, false, openError(err), 'Could not open ' + name + ': ' + text(err));
        });
      });
    });
  }

  function write(handle, bytes) {
    var s = sessions.get(handle);
    if (!s || !s.writer || s.closing) return;
    s.writer.write(bytes).catch(function (err) {
      if (s.closing || s.reported) return;
      s.reported = true;
      call('webSerialError', handle, ERR.Write, 'Write failed: ' + text(err));
    });
  }

  function setSignals(handle, dtr, rts) {
    var s = sessions.get(handle);
    if (!s || s.closing) return;
    var signals = { dataTerminalReady: !!dtr, requestToSend: !!rts };
    if (!s.opened) { s.signals = signals; return; } // sent right after the open
    s.entry.port.setSignals(signals).catch(function (e) { console.warn('webserial: setSignals failed', e); });
  }

  function close(handle) {
    var s = sessions.get(handle);
    if (!s) return;
    sessions.delete(handle);
    s.closing = true;
    if (!s.entry) return;
    var port = s.entry.port;
    // Queued on the port's chain: a reopen right after waits for this.
    s.entry.chain = s.entry.chain.then(function () {
      if (!s.opened) return;
      var r = s.reader;
      var cancelled = r ? r.cancel().catch(function () {}) : Promise.resolve();
      return cancelled.then(function () { return s.reading; }).then(function () {
        if (!s.writer) return;
        var w = s.writer;
        s.writer = null;
        // close() lets queued writes finish; on a lost device it rejects: abort instead.
        return w.close().catch(function () { return w.abort().catch(function () {}); })
          .then(function () { try { w.releaseLock(); } catch (e) { /* ignore */ } });
      }).then(function () {
        return port.close().catch(function (e) { console.warn('webserial: close failed', e); });
      });
    }).catch(function (e) { console.warn('webserial: close', e); });
  }

  function requestPort(anyPort) {
    if (!serial) {
      call('webSerialPortChosen', '', 'This browser has no Web Serial. Use Chrome, Edge or Opera on a computer.');
      return Promise.resolve(null);
    }
    var options = anyPort ? {} : { filters: RESOLVE_CAN_FILTERS };
    return serial.requestPort(options).then(function (port) {
      var e = entryFor(port);
      return refresh().then(function () {
        // A port the browser granted but getPorts() does not list yet: add it.
        if (entries.indexOf(e) < 0) { entries.push(e); rename(); call('webSerialPortsChanged'); }
        call('webSerialPortChosen', e.name, '');
        return e.name;
      });
    }, function (err) {
      var n = err && err.name;
      if (n === 'NotFoundError') call('webSerialPortChosen', '', ''); // the user cancelled
      else if (n === 'SecurityError') call('webSerialPortChosen', '',
        'The browser only shows its port list right after a click: click "Choose device" again.');
      else call('webSerialPortChosen', '', text(err));
      return null;
    });
  }

  // C++ asks once it runs: the first getPorts() may have finished before (its portsChanged then went nowhere).
  function listPorts() {
    ready.then(function () { call('webSerialPortsChanged'); });
  }

  function ports() {
    return entries.map(function (e) { return { name: e.name, description: e.description }; });
  }

  return {
    supported: !!serial, ports: ports, open: open, write: write, setSignals: setSignals, close: close,
    requestPort: requestPort, listPorts: listPorts, ready: function () { return ready; }, sessions: function () { return sessions.size; }
  };
})();
