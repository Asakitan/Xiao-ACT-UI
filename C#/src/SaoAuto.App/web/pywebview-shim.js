(function () {
  'use strict';

  window.__pywebviewShim = { version: 'csharp-standalone-1' };
  window.pywebview = window.pywebview || {};
  window.pywebview.api = window.pywebview.api || new Proxy({}, {
    get: function (_, name) {
      return function () {
        return window.bridge && window.bridge.call
          ? window.bridge.call(String(name), Array.prototype.slice.call(arguments))
          : Promise.resolve({ ok: true, standalone: true });
      };
    }
  });
})();