(function () {
  'use strict';

  const bridge = window.bridge || {};
  bridge.version = bridge.version || 'csharp-standalone-1';
  bridge.notify = bridge.notify || function notify(message) {
    try {
      if (window.chrome && window.chrome.webview) {
        window.chrome.webview.postMessage(String(message || ''));
      }
    } catch (_) {}
  };
  bridge.call = bridge.call || function call(command, payload) {
    try {
      if (window.chrome && window.chrome.webview) {
        window.chrome.webview.postMessage({ command, payload: payload || {} });
      }
    } catch (_) {}
    return Promise.resolve({ ok: true, standalone: true });
  };

  window.bridge = bridge;
})();