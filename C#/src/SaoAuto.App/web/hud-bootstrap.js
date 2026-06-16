(function () {
  'use strict';

  window.__hudBootstrap = { version: 'csharp-standalone-1' };

  function dispatch(name, detail) {
    try { window.dispatchEvent(new CustomEvent(name, { detail: detail || {} })); } catch (_) {}
  }

  window.updateHP = window.updateHP || function (payload) { dispatch('sao:hud:hp', payload); };
  window.updateSTA = window.updateSTA || function (payload) { dispatch('sao:hud:sta', payload); };
  window.updateBurst = window.updateBurst || function (payload) { dispatch('sao:hud:burst', payload); };
  window.updateBossHP = window.updateBossHP || function (payload) { dispatch('sao:hud:boss', payload); };
})();