// S187 — C# WebView2 bridge shim.
//
// Exposes a small `window.bridge` API to all pages loaded into the
// WebView2 host. Mirrors Python's `inject_bridge.js` posture: parses
// the {type, name, payload} envelope coming from C# and dispatches to
// per-event subscribers; outbound calls wrap into the same envelope
// and `postMessage` to the host.
//
// Contract (matches BridgeMessage in C#):
//   inbound:  { type: 'event' | 'reply', name: string, payload: any }
//   outbound: { type: 'command', name: string, payload: any }
//
// Public surface:
//   window.bridge.on(name, cb)           — subscribe to events; returns unsub
//   window.bridge.off(name, cb)          — explicit unsub
//   window.bridge.cmd(name, payload)     — promise resolved with reply.payload
//   window.bridge.ready                  — Promise resolved once the shim has wired up
//   window.bridge.version                — string, sanity check from console
(function () {
    "use strict";

    if (window.bridge && window.bridge.version) {
        return; // already injected (defensive against double-inject)
    }

    var subscribers = Object.create(null); // name -> [cb]
    var pending = Object.create(null);     // name -> [resolve]
    var nextSeq = 1;

    var debug = window.__bridge_debug__ = { inboundCount: 0, lastEnvelope: null, lastError: null };

    function dispatch(envelope) {
        try {
            debug.inboundCount++;
            debug.lastEnvelope = envelope;
            var msg = typeof envelope === 'string' ? JSON.parse(envelope) : envelope;
            if (!msg || typeof msg !== 'object') return;
            var type = msg.type;
            var name = msg.name;
            var payload = msg.payload;
            if (type === 'event' || type === 'reply') {
                var list = subscribers[name];
                if (list) {
                    for (var i = 0; i < list.length; i++) {
                        try { list[i](payload, msg); } catch (e) { /* swallow */ }
                    }
                }
                if (type === 'reply') {
                    var awaiters = pending[name];
                    if (awaiters && awaiters.length) {
                        var resolve = awaiters.shift();
                        try { resolve(payload); } catch (e) { /* swallow */ }
                    }
                }
            }
        } catch (e) {
            debug.lastError = String(e);
            // Malformed envelope — log to console, don't crash the page.
            if (window.console) console.warn('[bridge] dispatch failed', e);
        }
    }

    function on(name, cb) {
        if (!subscribers[name]) subscribers[name] = [];
        subscribers[name].push(cb);
        return function () { off(name, cb); };
    }

    function off(name, cb) {
        var list = subscribers[name];
        if (!list) return;
        var idx = list.indexOf(cb);
        if (idx >= 0) list.splice(idx, 1);
    }

    function cmd(name, payload) {
        var envelope = { type: 'command', name: name, payload: payload || {} };
        try {
            window.chrome.webview.postMessage(envelope);
        } catch (e) {
            return Promise.reject(e);
        }
        return new Promise(function (resolve) {
            if (!pending[name]) pending[name] = [];
            pending[name].push(resolve);
        });
    }

    // Wire the inbound channel as soon as window.chrome.webview is available.
    function wire() {
        try {
            if (!window.chrome || !window.chrome.webview) {
                // Page loaded outside WebView2; the shim is a no-op.
                return false;
            }
            window.chrome.webview.addEventListener('message', function (e) {
                // CoreWebView2.PostWebMessageAsString delivers a string in
                // e.data; postMessageAsJson would deliver a parsed object.
                dispatch(e.data);
            });
            return true;
        } catch (err) {
            if (window.console) console.warn('[bridge] wire failed', err);
            return false;
        }
    }

    var wired = wire();
    var readyResolve;
    var readyPromise = new Promise(function (r) { readyResolve = r; });
    if (wired) {
        readyResolve(true);
    } else if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', function () {
            wire();
            readyResolve(!!(window.chrome && window.chrome.webview));
        });
    } else {
        readyResolve(false);
    }

    window.bridge = {
        version: 's187',
        on: on,
        off: off,
        cmd: cmd,
        ready: readyPromise,
        _wired: function () { return !!(window.chrome && window.chrome.webview); },
    };
})();
