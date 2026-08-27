// SAO Auto 用户指南 — 主题切换（浅色默认，深色可选，选择记住在 localStorage）。
// 在 <head> 中同步加载：首帧前把 <html> 的 .dark 类定好，避免闪烁。
(function () {
  'use strict';

  var STORAGE_KEY = 'sao-manual-theme';

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

  document.addEventListener('DOMContentLoaded', function () {
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
