// SAO Auto 用户指南 — 主题切换（浅色默认，深色可选，选择记住在 localStorage）。
// 在 <head> 中同步加载：首帧前把 <html> 的 .dark 类定好，避免闪烁。
(function () {
  'use strict';

  var STORAGE_KEY = 'sao-manual-theme';

  function syncGuideHidden() {
    document.documentElement.classList.toggle('guide-hidden', document.hidden);
  }

  syncGuideHidden();
  document.addEventListener('visibilitychange', syncGuideHidden);
  window.addEventListener('pagehide', function () {
    document.documentElement.classList.add('guide-hidden');
  });
  window.addEventListener('pageshow', syncGuideHidden);
  function applyTheme(dark) {
    var root = document.documentElement;
    if (dark) {
      root.classList.add('dark');
    } else {
      root.classList.remove('dark');
    }
    var toggle = document.getElementById('theme-toggle');
    if (toggle) {
      toggle.setAttribute('aria-pressed', dark ? 'true' : 'false');
      toggle.title = dark ? '切换到浅色模式' : '切换到深色模式';
    }
  }

  // 恢复用户上次选择；没有记录时默认浅色（不跟随系统）。
  var saved = null;
  try {
    saved = window.localStorage.getItem(STORAGE_KEY);
  } catch (e) {
    saved = null;
  }
  applyTheme(saved === 'dark');

  function closeGuide() {
    if (window.chrome && window.chrome.webview) {
      window.chrome.webview.postMessage('sao-guide-close');
    } else {
      window.close();
    }
  }

  window.addEventListener('keydown', function (event) {
    if (event.key !== 'Escape' || event.defaultPrevented || event.repeat) { return; }
    event.preventDefault();
    closeGuide();
  });

  document.addEventListener('DOMContentLoaded', function () {
    document.querySelectorAll('[data-guide-close]').forEach(function (close) {
      close.addEventListener('click', function (event) {
        event.preventDefault();
        event.stopPropagation();
        closeGuide();
      });
    });
    var toggle = document.getElementById('theme-toggle');
    if (!toggle) {
      return;
    }
    toggle.addEventListener('click', function () {
      var dark = !document.documentElement.classList.contains('dark');
      applyTheme(dark);
      try {
        window.localStorage.setItem(STORAGE_KEY, dark ? 'dark' : 'light');
      } catch (e) {
        // 无痕 / 禁用存储时忽略，仅本次会话生效。
      }
    });
  });
})();
