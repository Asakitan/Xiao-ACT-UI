(function () {
  'use strict';

  const bridge = window.bridge || {};
  const pending = {};
  const subscribers = {};

  bridge.version = bridge.version || 'csharp-standalone-2';
  bridge.ready = bridge.ready || Promise.resolve(!!(window.chrome && window.chrome.webview));

  function normalizePayload(payload) {
    return payload && typeof payload === 'object' && !Array.isArray(payload)
      ? payload
      : { value: payload };
  }

  function post(message) {
    if (window.chrome && window.chrome.webview && window.chrome.webview.postMessage) {
      window.chrome.webview.postMessage(message);
      return true;
    }
    return false;
  }

  function remember(name, resolve) {
    const key = String(name || '');
    pending[key] = pending[key] || [];
    pending[key].push(resolve);
  }

  function resolvePending(name, payload) {
    const key = String(name || '');
    const queue = pending[key];
    if (!queue || !queue.length) return false;
    const resolve = queue.shift();
    resolve(payload || {});
    return true;
  }

  function emit(name, payload, message) {
    const key = String(name || '');
    const list = subscribers[key];
    if (list) {
      list.slice().forEach(function (callback) {
        try { callback(payload || {}, message || null); } catch (_) {}
      });
    }
    try {
      window.dispatchEvent(new CustomEvent('sao:bridge:' + key, { detail: payload || {} }));
    } catch (_) {}
  }

  function parseMessage(data) {
    if (!data) return null;
    if (typeof data === 'string') {
      try { return JSON.parse(data); } catch (_) { return null; }
    }
    if (typeof data === 'object') return data;
    return null;
  }

  function handleHostMessage(event) {
    const message = parseMessage(event && event.data);
    if (!message || !message.type || !message.name) return;
    if (message.type === 'reply') {
      emit(message.name, message.payload || {}, message);
      resolvePending(message.name, message.payload || {});
      return;
    }
    if (message.type === 'event') {
      emit(message.name, message.payload || {}, message);
    }
  }

  try {
    if (window.chrome && window.chrome.webview && window.chrome.webview.addEventListener) {
      window.chrome.webview.addEventListener('message', handleHostMessage);
    }
  } catch (_) {}

  bridge.notify = bridge.notify || function notify(message) {
    try {
      post({ type: 'event', name: 'ui.notify', payload: { message: String(message || '') } });
    } catch (_) {}
  };

  bridge.on = bridge.on || function on(name, callback) {
    const key = String(name || '');
    if (typeof callback !== 'function') return function () {};
    subscribers[key] = subscribers[key] || [];
    subscribers[key].push(callback);
    return function () { bridge.off(key, callback); };
  };

  bridge.off = bridge.off || function off(name, callback) {
    const key = String(name || '');
    const list = subscribers[key];
    if (!list) return;
    const index = list.indexOf(callback);
    if (index >= 0) list.splice(index, 1);
  };

  bridge.cmd = bridge.cmd || function cmd(command, payload) {
    const name = String(command || '');
    const body = normalizePayload(payload || {});
    return new Promise(function (resolve) {
      remember(name, resolve);
      if (!post({ type: 'command', name: name, payload: body })) {
        resolve({ ok: true, standalone: true, command: name, payload: body });
      }
    });
  };

  bridge.call = bridge.call || function call(command, payload) {
    return bridge.cmd(command, payload);
  };

  window.bridge = bridge;
})();