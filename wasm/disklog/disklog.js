/*
 * web-savvycan: continuous logging in the browser (C++ side: wasm/disklog/webdisklog.cpp).
 *
 * The rows (SavvyCAN's native GVRET CSV, made by FrameFileIO::nativeCSVRow) are collected in the page as
 * Blobs, outside the wasm heap: small batches are folded into one Blob every few MB, so the JS heap holds
 * little and Chrome's blob storage can page big logs to disk. stop() starts one normal download
 * (<a download>) of the whole CSV. A crash or a closed tab loses the log (accepted for the MVP, RESOLVE.md);
 * closing the tab while logging asks first. Writing to disk while logging is on branch disklog-live.
 *
 * globalThis.SavvyDiskLog (all synchronous):
 *   start({header, suggestedName})  -> {name}  (default name: the start time, SavvyCAN_2026-10-28_14-03-22.csv)
 *   write(Uint8Array | string)      -> bool    (false when no log is open)
 *   writeCopy(Uint8Array)           -> bool    (copies first: for views into the wasm heap)
 *   stop({name})                    -> {name, bytes} of the download started, or null when no log was open
 *   status()                        -> {state: 'idle' | 'open', name, bytes}
 *   lastDownload()                  -> {name, bytes} of the last download, or null
 *   startTimeName(date), safeName(name)
 *   _blob()                         -> the running log as one Blob (tests)
 */
(function (global) {
    'use strict';
    if (global.SavvyDiskLog) return;

    const FOLD_BYTES = 4 << 20;      // small batches become one Blob at this size
    const REVOKE_AFTER_MS = 60000;   // the download's object URL is released after this

    let log = null;                  // {name, bytes, small: [Uint8Array], smallBytes, blobs: [Blob]}
    let lastDownload = null;
    const encoder = new TextEncoder();

    function pad(n) { return String(n).padStart(2, '0'); }

    function startTimeName(d) {
        d = d || new Date();
        return 'SavvyCAN_' + d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' + pad(d.getDate())
            + '_' + pad(d.getHours()) + '-' + pad(d.getMinutes()) + '-' + pad(d.getSeconds()) + '.csv';
    }

    // A download name: no path separators or characters Windows refuses; ".csv" added when missing.
    function safeName(name) {
        let n = String(name).replace(/[\\/:*?"<>|\u0000-\u001f]/g, '_').trim();
        if (!n || n === '.' || n === '..') n = startTimeName();
        if (!/\.csv$/i.test(n)) n += '.csv';
        return n;
    }

    function fold() {
        if (!log.small.length) return;
        log.blobs.push(new Blob(log.small));
        log.small = [];
        log.smallBytes = 0;
        if (log.blobs.length >= 256) log.blobs = [new Blob(log.blobs)];   // keep the list short
    }

    function wholeLog() {
        fold();
        return new Blob(log.blobs, { type: 'text/csv' });
    }

    function write(data) {
        if (!log) return false;
        const bytes = typeof data === 'string' ? encoder.encode(data) : data;
        if (!bytes.byteLength) return true;
        log.small.push(bytes);
        log.smallBytes += bytes.byteLength;
        log.bytes += bytes.byteLength;
        if (log.smallBytes >= FOLD_BYTES) fold();
        return true;
    }

    function start(opts) {
        opts = opts || {};
        if (log) return null;
        const name = opts.suggestedName ? safeName(opts.suggestedName) : startTimeName();
        log = { name, bytes: 0, small: [], smallBytes: 0, blobs: [] };
        if (opts.header) write(opts.header);
        return { name };
    }

    function stop(opts) {
        if (!log) return null;
        if (opts && opts.name) log.name = safeName(opts.name);
        const blob = wholeLog();
        const name = log.name;
        log = null;
        const url = global.URL.createObjectURL(blob);
        const a = global.document.createElement('a');
        a.href = url;
        a.download = name;
        a.style.display = 'none';
        global.document.body.appendChild(a);
        a.click();
        a.remove();
        // the browser has taken the Blob once the download has started; the URL can go later
        setTimeout(() => global.URL.revokeObjectURL(url), REVOKE_AFTER_MS);
        lastDownload = { name, bytes: blob.size };
        return lastDownload;
    }

    // Closing the tab while logging: ask first (the browser shows its own "leave site?" box).
    if (global.addEventListener) {
        global.addEventListener('beforeunload', (e) => {
            if (log) { e.preventDefault(); e.returnValue = ''; }
        });
    }

    global.SavvyDiskLog = {
        start,
        write,
        writeCopy(view) { return write(view.slice()); },
        stop,
        status() { return log ? { state: 'open', name: log.name, bytes: log.bytes } : { state: 'idle' }; },
        lastDownload() { return lastDownload; },
        startTimeName,
        safeName,
        _blob() { return log ? wholeLog() : null; },
    };
})(typeof globalThis !== 'undefined' ? globalThis : self);
