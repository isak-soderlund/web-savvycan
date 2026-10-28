/*
 * web-savvycan: continuous logging straight to a file on disk, in the browser.
 *
 * SavvyCAN's "Continuous Logging" in the WebAssembly build writes through this module
 * (C++ side: wasm/disklog/webdisklog.cpp). Rows arrive already formatted (SavvyCAN's native
 * GVRET CSV, made by FrameFileIO::nativeCSVRow); this module only gets them onto disk.
 *
 * Backends
 *   "fsa"      File System Access API (Chrome, Edge, Opera desktop). The file is written while
 *              logging runs and committed every few seconds: the writable stream is closed (the
 *              browser moves its swap file onto the real file) and opened again with
 *              keepExistingData, seeking to the end. A crash or a closed tab loses at most what
 *              came after the last commit.
 *                - mode "prompt": showSaveFilePicker, the user names the file;
 *                - mode "direct": a file named by the start time is created in a remembered folder
 *                  (showDirectoryPicker once, the handle kept in IndexedDB, permission re-checked).
 *   "download" Firefox and other browsers without the API: a streaming download through a service
 *              worker (disklog-sw.js, as StreamSaver.js does). The download grows while logging and
 *              is finished on stop; a crash leaves it unfinished (the browser discards it).
 *
 * API (globalThis.SavvyDiskLog):
 *   support()                     -> {fsa, download, opfs, backend}
 *   configure({swUrl, commitIntervalMs, filePrefix})
 *   start({mode, suggestedName, header, commitIntervalMs, splitBytes, directory}) -> Promise<{name, backend}>
 *                                    splitBytes (mode "direct"): continue in a new file (<name>_part002.csv,
 *                                    ..., each with the header) once a file has this size, since a commit
 *                                    copies the whole file (Chrome's swap file): bounded commit time
 *                                    mode: "prompt" | "direct" | "download"; rejects with an
 *                                    Error whose .code is "cancelled", "permission", "unsupported",
 *                                    "busy", "notfound", "quota" or "error"
 *   write(Uint8Array | string)    -> bool (false when no log is open)
 *   writeCopy(Uint8Array)         -> bool (copies first; for views into the wasm heap)
 *   commit()                      -> Promise (commit now)
 *   stop()                        -> Promise<{name, bytes}>
 *   status()                      -> plain object (state, name, backend, bytes written/committed, ...)
 *   setListener(fn)               fn(event): {type: "started"|"committed"|"rotated"|"error"|"stopped", ...}
 *   rememberedDirectory()         -> Promise<{name, permission} | null>
 *   chooseDirectory()             -> Promise<{name}> (needs a user gesture)
 *   forgetDirectory()             -> Promise
 *   startTimeName(date)           -> "SavvyCAN_2026-10-28_14-03-22.csv"
 */
(function (global) {
    'use strict';
    if (global.SavvyDiskLog) return;

    const DB_NAME = 'web-savvycan';
    const DB_STORE = 'handles';
    const DIR_KEY = 'disklog.directory';

    const config = {
        swUrl: null,                 // default: disklog-sw.js next to the page
        commitIntervalMs: 3000,      // owner: "every few seconds"
        maxCommitDutyCycle: 0.2,     // commits may take at most this share of the time (see commitDue)
        maxPendingBytes: 256 << 20,  // more than this waiting for the disk: the disk can't keep up
        filePrefix: 'SavvyCAN',
        pingMs: 10000,               // keeps the download service worker alive
        downloadStartTimeoutMs: 15000,
    };

    let listener = null;
    let session = null;

    function emit(ev) {
        if (!listener) return;
        try { listener(ev); } catch (e) { console.error('SavvyDiskLog listener', e); }
    }

    function makeError(code, message, cause) {
        const e = new Error(message);
        e.code = code;
        if (cause) e.cause = cause;
        return e;
    }

    // Map a DOMException from the file APIs onto a code the app shows.
    function classify(e) {
        if (e && e.code && typeof e.code === 'string') return e;
        const name = e && e.name;
        const msg = (e && e.message) || String(e);
        if (name === 'AbortError') return makeError('cancelled', 'Cancelled.', e);
        if (name === 'NotAllowedError' || name === 'SecurityError')
            return makeError('permission', 'The browser no longer allows writing the log file (' + msg + ').', e);
        if (name === 'NotFoundError')
            return makeError('notfound', 'The log file or its folder was removed or moved (' + msg + ').', e);
        if (name === 'QuotaExceededError')
            return makeError('quota', 'The disk is full (' + msg + ').', e);
        if (name === 'NoModificationAllowedError' || name === 'InvalidStateError')
            return makeError('permission', 'The log file is locked or can no longer be written (' + msg + ').', e);
        return makeError('error', msg, e);
    }

    function support() {
        const fsa = typeof global.showSaveFilePicker === 'function'
            && typeof global.showDirectoryPicker === 'function'
            && typeof global.FileSystemFileHandle === 'function'
            && 'createWritable' in global.FileSystemFileHandle.prototype;
        const download = !!(global.navigator && 'serviceWorker' in global.navigator)
            && !!global.isSecureContext && typeof global.ReadableStream === 'function';
        const opfs = !!(global.navigator && global.navigator.storage && global.navigator.storage.getDirectory);
        return { fsa, download, opfs, backend: fsa ? 'fsa' : (download ? 'download' : 'none') };
    }

    function pad(n, w) { return String(n).padStart(w || 2, '0'); }

    function startTimeName(d) {
        d = d || new Date();
        return config.filePrefix + '_' + d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' + pad(d.getDate())
            + '_' + pad(d.getHours()) + '-' + pad(d.getMinutes()) + '-' + pad(d.getSeconds()) + '.csv';
    }

    const encoder = new TextEncoder();
    function toBytes(data) {
        if (typeof data === 'string') return encoder.encode(data);
        if (data instanceof Uint8Array) return data;
        if (data instanceof ArrayBuffer) return new Uint8Array(data);
        if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
        throw makeError('error', 'SavvyDiskLog.write: bytes or a string expected');
    }

    function concat(chunks, total) {
        if (chunks.length === 1) return chunks[0];
        const out = new Uint8Array(total);
        let o = 0;
        for (const c of chunks) { out.set(c, o); o += c.byteLength; }
        return out;
    }

    // ---- IndexedDB: the remembered log folder -------------------------------------------------

    function idbOpen() {
        return new Promise((resolve, reject) => {
            const req = global.indexedDB.open(DB_NAME, 1);
            req.onupgradeneeded = () => req.result.createObjectStore(DB_STORE);
            req.onsuccess = () => resolve(req.result);
            req.onerror = () => reject(req.error);
        });
    }

    async function idb(mode, fn) {
        const db = await idbOpen();
        try {
            return await new Promise((resolve, reject) => {
                const tx = db.transaction(DB_STORE, mode);
                const req = fn(tx.objectStore(DB_STORE));
                tx.oncomplete = () => resolve(req && req.result);
                tx.onerror = () => reject(tx.error);
                tx.onabort = () => reject(tx.error);
            });
        } finally {
            db.close();
        }
    }

    const getStoredDirectory = () => idb('readonly', s => s.get(DIR_KEY)).catch(() => undefined);
    const storeDirectory = (h) => idb('readwrite', s => s.put(h, DIR_KEY));
    const forgetDirectory = () => idb('readwrite', s => s.delete(DIR_KEY));

    async function permissionOf(handle) {
        if (!handle.queryPermission) return 'granted';  // older API without permissions: assume yes
        try { return await handle.queryPermission({ mode: 'readwrite' }); } catch (e) { return 'denied'; }
    }

    async function rememberedDirectory() {
        const h = await getStoredDirectory();
        if (!h) return null;
        return { name: h.name, permission: await permissionOf(h) };
    }

    async function pickDirectory() {
        let h;
        try {
            h = await global.showDirectoryPicker({ id: 'savvycan-logs', mode: 'readwrite', startIn: 'documents' });
        } catch (e) { throw classify(e); }
        await storeDirectory(h);
        return h;
    }

    async function chooseDirectory() {
        if (!support().fsa) throw makeError('unsupported', 'This browser cannot write into a folder.');
        const h = await pickDirectory();
        return { name: h.name };
    }

    // The remembered folder with write permission, asking for the permission (or for a folder)
    // when needed. Both prompts need a user gesture: start() is called from a click.
    async function directoryForDirectMode() {
        let h = await getStoredDirectory();
        if (h) {
            let p = await permissionOf(h);
            if (p === 'prompt' && h.requestPermission) {
                try { p = await h.requestPermission({ mode: 'readwrite' }); } catch (e) { p = 'denied'; }
            }
            if (p === 'granted') return h;
            if (p === 'denied') {
                throw makeError('permission', 'Writing into the log folder "' + h.name
                    + '" is not allowed. Choose the folder again (Change folder), or allow it in the site settings.');
            }
        }
        return pickDirectory();
    }

    async function uniqueFileIn(dir, name) {
        const dot = name.lastIndexOf('.');
        const base = dot > 0 ? name.slice(0, dot) : name;
        const ext = dot > 0 ? name.slice(dot) : '';
        for (let i = 1; i < 1000; i++) {
            const candidate = i === 1 ? name : base + '_' + i + ext;
            try {
                await dir.getFileHandle(candidate);           // exists: try the next name
            } catch (e) {
                if (e && e.name === 'NotFoundError')
                    return dir.getFileHandle(candidate, { create: true });
                if (e && e.name === 'TypeMismatchError') continue;  // a folder of that name
                throw e;
            }
        }
        throw makeError('error', 'No free file name for ' + name);
    }

    // ---- Sinks ----------------------------------------------------------------------------------

    // File System Access: write into the swap file, commit by close + reopen (keepExistingData).
    class FsaSink {
        constructor(handle) {
            this.handle = handle;
            this.name = handle.name;
            this.backend = 'fsa';
            this.size = 0;
            this.w = null;
            this.crashSafe = true;
        }
        async open() {
            this.w = await this.handle.createWritable({ keepExistingData: false });
        }
        async write(bytes) {
            await this.w.write(bytes);
            this.size += bytes.byteLength;
        }
        async commit() {
            // Losing the permission (site settings, "forget") shows here first.
            if (this.handle.queryPermission) {
                const p = await this.handle.queryPermission({ mode: 'readwrite' });
                if (p !== 'granted') throw makeError('permission', 'Permission to write the log file was withdrawn.');
            }
            const w = this.w;
            this.w = null;
            await w.close();
            this.w = await this.handle.createWritable({ keepExistingData: true });
            await this.w.seek(this.size);
        }
        async close() {
            if (this.w) { const w = this.w; this.w = null; await w.close(); }
        }
        async abort() {
            if (this.w) { const w = this.w; this.w = null; try { await w.abort(); } catch (e) { /* gone */ } }
        }
    }

    // Streaming download through the service worker (Firefox): see disklog-sw.js.
    class DownloadSink {
        constructor(name) {
            this.name = name;
            this.backend = 'download';
            this.size = 0;
            this.crashSafe = false;
            this.port = null;
            this.iframe = null;
            this.reg = null;
            this.pingTimer = null;
        }
        async open() {
            const swUrl = new URL(config.swUrl || 'disklog-sw.js', global.document.baseURI).href;
            const scope = new URL('disklog-download/', swUrl).href;
            let reg;
            try {
                reg = await global.navigator.serviceWorker.register(swUrl, { scope });
            } catch (e) {
                throw makeError('unsupported', 'The download helper (service worker) could not start: ' + e.message, e);
            }
            const sw = reg.active || await new Promise((resolve, reject) => {
                const w = reg.installing || reg.waiting;
                if (!w) { reject(makeError('error', 'Service worker has no worker')); return; }
                w.addEventListener('statechange', () => {
                    if (w.state === 'activated') resolve(w);
                    if (w.state === 'redundant') reject(makeError('error', 'Service worker failed to install'));
                });
            });
            this.reg = reg;
            const id = Math.random().toString(36).slice(2) + Date.now().toString(36);
            const ch = new MessageChannel();
            this.port = ch.port1;
            const started = new Promise((resolve, reject) => {
                const t = setTimeout(() => reject(makeError('error', 'The download did not start.')),
                    config.downloadStartTimeoutMs);
                this.port.onmessage = (m) => {
                    if (m.data && m.data.type === 'pulled') { clearTimeout(t); resolve(); }
                    if (m.data && m.data.type === 'cancelled') {
                        if (this.onCancel) this.onCancel();
                    }
                };
            });
            const ready = new Promise((resolve) => {
                const prev = this.port.onmessage;
                this.port.onmessage = (m) => { if (m.data && m.data.type === 'ready') resolve(); prev(m); };
            });
            sw.postMessage({ type: 'disklog-start', id, name: this.name }, [ch.port2]);
            await ready;
            const url = new URL(id + '/' + encodeURIComponent(this.name), scope).href;
            const iframe = global.document.createElement('iframe');
            iframe.hidden = true;
            iframe.src = url;
            global.document.body.appendChild(iframe);
            this.iframe = iframe;
            this.pingTimer = setInterval(() => {
                if (reg.active) reg.active.postMessage({ type: 'disklog-ping' });
            }, config.pingMs);
            await started;
        }
        async write(bytes) {
            // Transfer a copy: the caller may reuse the buffer.
            const copy = bytes.byteOffset === 0 && bytes.byteLength === bytes.buffer.byteLength
                ? bytes : bytes.slice();
            this.port.postMessage({ type: 'data', bytes: copy }, [copy.buffer]);
            this.size += copy.byteLength;
        }
        async commit() { /* data is handed to the download as it comes */ }
        async close() {
            this.port.postMessage({ type: 'end' });
            this.cleanup(2000);
        }
        async abort() {
            try { this.port.postMessage({ type: 'abort' }); } catch (e) { /* closed */ }
            this.cleanup(0);
        }
        cleanup(delay) {
            clearInterval(this.pingTimer);
            const iframe = this.iframe;
            this.iframe = null;
            // The iframe must stay until the browser has taken the end of the stream.
            if (iframe) setTimeout(() => iframe.remove(), delay);
        }
    }

    // ---- Session: batching, periodic commit, stop -----------------------------------------------

    class Session {
        constructor(sink, opts) {
            this.sink = sink;
            this.state = 'open';
            this.pending = [];
            this.pendingBytes = 0;
            this.busy = false;
            this.stopRequested = false;
            this.commitRequested = false;
            this.bytesCommitted = 0;
            this.lastCommitAt = performance.now();
            this.lastCommitMs = 0;
            this.maxCommitMs = 0;
            this.commits = 0;
            this.startedAt = new Date();
            this.intervalMs = opts.commitIntervalMs || config.commitIntervalMs;
            this.waiters = [];
            // splitting into parts (direct mode): a commit copies the whole file, so a size limit keeps it short
            this.splitBytes = opts.splitBytes || 0;
            this.nextSink = opts.nextSink || null;
            this.header = opts.header ? toBytes(opts.header) : null;
            this.prevBytes = 0;          // bytes in the parts already closed
            this.parts = [sink.name];
            this.timer = setInterval(() => this.kick(), Math.min(1000, this.intervalMs));
        }

        get bytesWritten() { return this.prevBytes + this.sink.size + this.pendingBytes; }
        get bytesOnSink() { return this.prevBytes + this.sink.size; }

        // Close the full part (that commits it) and continue in a new file, which starts with the header.
        async rotate() {
            await this.sink.close();
            this.prevBytes += this.sink.size;
            this.bytesCommitted = this.prevBytes;
            this.sink = await this.nextSink(this.parts.length + 1);
            this.parts.push(this.sink.name);
            if (this.header) await this.sink.write(this.header);
            this.lastCommitAt = performance.now();
            this.lastCommitMs = 0;
            emit({ type: 'rotated', name: this.sink.name, part: this.parts.length, bytes: this.bytesCommitted });
        }

        write(bytes) {
            if (this.state !== 'open' || this.stopRequested) return false;
            if (!bytes.byteLength) return true;
            this.pending.push(bytes);
            this.pendingBytes += bytes.byteLength;
            if (this.pendingBytes > config.maxPendingBytes) {
                this.fail(makeError('error', 'The disk does not keep up with the log ('
                    + (this.pendingBytes >> 20) + ' MB waiting).'));
                return false;
            }
            this.kick();
            return true;
        }

        // Commit every intervalMs, but a commit copies the file (keepExistingData), so with a big
        // file it is stretched so that commits take at most maxCommitDutyCycle of the time.
        effectiveIntervalMs() {
            return Math.max(this.intervalMs, this.lastCommitMs / config.maxCommitDutyCycle);
        }

        commitDue() {
            if (this.bytesOnSink === this.bytesCommitted) return false;
            if (this.commitRequested) return true;
            return performance.now() - this.lastCommitAt >= this.effectiveIntervalMs();
        }

        kick() {
            if (!this.busy && this.state === 'open') this.run();
        }

        async run() {
            this.busy = true;
            try {
                for (;;) {
                    if (this.state !== 'open') break;
                    if (this.pending.length) {
                        if (this.splitBytes && this.nextSink && this.sink.size >= this.splitBytes) await this.rotate();
                        const chunks = this.pending, total = this.pendingBytes;
                        this.pending = [];
                        this.pendingBytes = 0;
                        await this.sink.write(concat(chunks, total));
                        continue;
                    }
                    if (this.stopRequested) {
                        await this.sink.close();
                        this.bytesCommitted = this.bytesOnSink;
                        this.finish('stopped');
                        break;
                    }
                    if (this.commitDue()) {
                        const t0 = performance.now();
                        await this.sink.commit();
                        const t1 = performance.now();
                        this.lastCommitMs = t1 - t0;
                        this.maxCommitMs = Math.max(this.maxCommitMs, this.lastCommitMs);
                        this.lastCommitAt = t1;
                        this.commits++;
                        this.bytesCommitted = this.bytesOnSink;
                        if (this.commitRequested) { this.commitRequested = false; this.resolveWaiters('commit'); }
                        emit({ type: 'committed', name: this.sink.name, bytes: this.bytesCommitted,
                               ms: this.lastCommitMs, intervalMs: this.effectiveIntervalMs() });
                        continue;
                    }
                    if (this.commitRequested) { this.commitRequested = false; this.resolveWaiters('commit'); }
                    break;
                }
            } catch (e) {
                this.fail(classify(e));
            } finally {
                this.busy = false;
            }
        }

        resolveWaiters(kind, err) {
            const keep = [];
            for (const w of this.waiters) {
                if (err) w.reject(err);
                else if (w.kind === kind || kind === 'stopped') w.resolve(this.result());
                else keep.push(w);
            }
            this.waiters = err ? [] : keep;
        }

        waitFor(kind) {
            return new Promise((resolve, reject) => this.waiters.push({ kind, resolve, reject }));
        }

        commit() {
            if (this.state !== 'open') return Promise.reject(makeError('error', 'No log is open.'));
            this.commitRequested = true;
            const p = this.waitFor('commit');
            this.kick();
            return p;
        }

        stop() {
            if (this.state !== 'open') return Promise.resolve(this.result());
            this.stopRequested = true;
            const p = this.waitFor('stopped');
            this.kick();
            return p;
        }

        result() {
            return { name: this.sink.name, backend: this.sink.backend, bytes: this.bytesCommitted, parts: this.parts.slice() };
        }

        finish(state) {
            this.state = state;
            clearInterval(this.timer);
            if (session === this) session = null;
            emit({ type: 'stopped', name: this.sink.name, bytes: this.bytesCommitted, parts: this.parts.length });
            this.resolveWaiters('stopped');
        }

        fail(err) {
            if (this.state !== 'open') return;
            this.state = 'failed';
            clearInterval(this.timer);
            this.pending = [];
            this.pendingBytes = 0;
            this.sink.abort();
            if (session === this) session = null;
            emit({ type: 'error', code: err.code || 'error', message: err.message,
                   name: this.sink.name, bytes: this.bytesCommitted, crashSafe: this.sink.crashSafe });
            this.resolveWaiters(null, err);
        }

        status() {
            return {
                state: this.state, name: this.sink.name, backend: this.sink.backend,
                crashSafe: this.sink.crashSafe, startedAt: this.startedAt.toISOString(),
                bytesWritten: this.bytesWritten, bytesCommitted: this.bytesCommitted,
                pendingBytes: this.pendingBytes, commits: this.commits, parts: this.parts.slice(),
                lastCommitMs: this.lastCommitMs, maxCommitMs: this.maxCommitMs,
                commitIntervalMs: this.effectiveIntervalMs(),
                secondsSinceCommit: (performance.now() - this.lastCommitAt) / 1000,
            };
        }
    }

    // ---- Public API ------------------------------------------------------------------------------

    let starting = false;

    async function start(opts) {
        opts = opts || {};
        if (session || starting) throw makeError('busy', 'A log is already running.');
        starting = true;
        try {
            const sup = support();
            let mode = opts.mode || (sup.fsa ? 'prompt' : 'download');
            const name = opts.suggestedName || startTimeName(new Date());
            let sink, nextSink = null;
            if (mode === 'prompt' || mode === 'direct') {
                if (!sup.fsa && !opts.directory) {
                    throw makeError('unsupported', 'This browser cannot write files on disk directly.');
                }
                let handle;
                if (mode === 'prompt') {
                    const pickerOpts = {
                        id: 'savvycan-log',
                        suggestedName: name,
                        types: [{ description: 'GVRET Logs (SavvyCAN CSV)', accept: { 'text/csv': ['.csv'] } }],
                    };
                    const dir = await getStoredDirectory();
                    if (dir) pickerOpts.startIn = dir;
                    try { handle = await global.showSaveFilePicker(pickerOpts); } catch (e) { throw classify(e); }
                } else {
                    const dir = opts.directory || await directoryForDirectMode();
                    handle = await uniqueFileIn(dir, name);
                    if (opts.splitBytes > 0) {
                        const dot = handle.name.lastIndexOf('.');
                        const base = dot > 0 ? handle.name.slice(0, dot) : handle.name;
                        const ext = dot > 0 ? handle.name.slice(dot) : '';
                        nextSink = async (n) => {
                            const sk = new FsaSink(await uniqueFileIn(dir, base + '_part' + String(n).padStart(3, '0') + ext));
                            await sk.open();
                            return sk;
                        };
                    }
                }
                sink = new FsaSink(handle);
            } else if (mode === 'download') {
                if (!sup.download) throw makeError('unsupported', 'This browser cannot stream a download (needs https).');
                sink = new DownloadSink(name);
            } else {
                throw makeError('error', 'Unknown mode ' + mode);
            }
            try { await sink.open(); } catch (e) { throw classify(e); }
            const s = new Session(sink, Object.assign({}, opts, { nextSink }));
            session = s;
            if (sink instanceof DownloadSink) {
                sink.onCancel = () => s.fail(makeError('cancelled', 'The download was cancelled in the browser.'));
            }
            if (s.header) s.write(s.header);
            emit({ type: 'started', name: sink.name, backend: sink.backend, crashSafe: sink.crashSafe,
                   intervalMs: s.intervalMs });
            return { name: sink.name, backend: sink.backend, crashSafe: sink.crashSafe };
        } catch (e) {
            const err = classify(e);
            throw err;
        } finally {
            starting = false;
        }
    }

    function write(data) {
        if (!session) return false;
        return session.write(toBytes(data));
    }

    function writeCopy(view) {
        if (!session) return false;
        return session.write(toBytes(view).slice());
    }

    function commit() {
        return session ? session.commit() : Promise.resolve(null);
    }

    function stop() {
        return session ? session.stop() : Promise.resolve(null);
    }

    function status() {
        return session ? session.status() : { state: 'idle' };
    }

    // Closing the tab while logging: ask first (the browser shows its own "leave site?" box).
    if (global.addEventListener) {
        global.addEventListener('beforeunload', (e) => {
            if (session && session.state === 'open') { e.preventDefault(); e.returnValue = ''; }
        });
    }

    global.SavvyDiskLog = {
        version: 1,
        support,
        configure(c) { Object.assign(config, c || {}); return Object.assign({}, config); },
        start,
        write,
        writeCopy,
        commit,
        stop,
        status,
        setListener(fn) { listener = typeof fn === 'function' ? fn : null; },
        rememberedDirectory,
        chooseDirectory,
        forgetDirectory,
        startTimeName,
        // tests: remember a folder without the picker (e.g. an origin-private one)
        _rememberDirectory: storeDirectory,
        // For the C++ side (webdisklog.cpp): fire and forget, every outcome comes as an event,
        // so C++ never handles a promise (no eval-made callbacks, the page's CSP stays strict).
        bridge: {
            start(opts) {
                start(opts).catch((e) => emit({ type: 'startfailed', code: e.code || 'error', message: e.message }));
            },
            stop() { stop().catch(() => { /* reported as an "error" event */ }); },
            queryDirectory() {
                rememberedDirectory().then(
                    (d) => emit({ type: 'directory', name: d ? d.name : '', permission: d ? d.permission : '' }),
                    () => emit({ type: 'directory', name: '', permission: '' }));
            },
            chooseDirectory() {
                chooseDirectory().then(
                    (d) => emit({ type: 'directory', name: d.name, permission: 'granted' }),
                    (e) => emit({ type: 'directoryfailed', code: e.code || 'error', message: e.message }));
            },
        },
    };
})(typeof globalThis !== 'undefined' ? globalThis : self);
