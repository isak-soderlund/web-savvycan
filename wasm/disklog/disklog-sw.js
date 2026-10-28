/*
 * web-savvycan: streaming-download service worker for continuous logging in browsers without the
 * File System Access API (Firefox). Same idea as StreamSaver.js: the page sends the log's bytes over
 * a MessagePort; a hidden iframe navigates to <scope>/<id>/<name>, which this worker answers with a
 * download whose body is a stream of those bytes. The download grows while logging runs and is
 * finished when the page sends "end". Not crash safe (the UI says so). Measured with Firefox 142
 * (test/web/tests/disklog-firefox.spec.mjs): a closed tab -> the download ends ~30 s later, when this
 * worker is stopped, as "finished" with what had been streamed; the browser killed -> a partial file
 * (cut mid-row) is left under the download's temporary name.
 *
 * Registered by disklog.js with the scope "<dir>/disklog-download/", so it never sees the app's own
 * requests.
 */
'use strict';

const streams = new Map();  // id -> {port, name}

self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', (event) => event.waitUntil(self.clients.claim()));

self.addEventListener('message', (event) => {
    const msg = event.data || {};
    if (msg.type === 'disklog-start' && event.ports[0]) {
        const port = event.ports[0];
        streams.set(msg.id, { port, name: msg.name });
        port.postMessage({ type: 'ready' });
    }
    // 'disklog-ping': every message event restarts the worker's idle timer; nothing else to do.
});

function contentDisposition(name) {
    const ascii = name.replace(/[^\x20-\x7e]/g, '_').replace(/["\\]/g, '_');
    return 'attachment; filename="' + ascii + '"; filename*=UTF-8\'\'' + encodeURIComponent(name);
}

self.addEventListener('fetch', (event) => {
    const url = new URL(event.request.url);
    const scope = new URL(self.registration.scope);
    if (!url.pathname.startsWith(scope.pathname)) return;
    const id = url.pathname.slice(scope.pathname.length).split('/')[0];
    const entry = streams.get(id);
    if (!entry) {
        event.respondWith(new Response('No such log stream (the page that started it is gone).',
            { status: 404, headers: { 'Content-Type': 'text/plain' } }));
        return;
    }
    streams.delete(id);
    const port = entry.port;
    const body = new ReadableStream({
        start(controller) {
            port.onmessage = (m) => {
                const d = m.data || {};
                if (d.type === 'data') controller.enqueue(new Uint8Array(d.bytes.buffer || d.bytes));
                else if (d.type === 'end') { controller.close(); port.close(); }
                else if (d.type === 'abort') { controller.error(new Error('aborted')); port.close(); }
            };
            port.postMessage({ type: 'pulled' });
        },
        cancel() {
            // The user cancelled the download in the browser.
            port.postMessage({ type: 'cancelled' });
        },
    });
    event.respondWith(new Response(body, {
        headers: {
            'Content-Type': 'text/csv; charset=utf-8',
            'Content-Disposition': contentDisposition(entry.name),
            'Content-Security-Policy': "default-src 'none'",
            'X-Content-Type-Options': 'nosniff',
            // pages served cross-origin isolated (COEP require-corp) may still load it
            'Cross-Origin-Resource-Policy': 'same-origin',
            'Cross-Origin-Embedder-Policy': 'require-corp',
        },
    }));
});
