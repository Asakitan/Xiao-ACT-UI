// SAO Auto 用户指南 — LINK START 开场连接动画。
// 阶段：环圈聚能(0.6s) → 六边形锁定(0.8s) → LINK START 逐字(1.2s)
// → 光柱爆闪(0.45s) → 淡出。点击或 Esc 可跳过。
// prefers-reduced-motion 下直接跳过。
(function () {
  'use strict';

  var overlay = document.getElementById('linkstart-overlay');
  if (!overlay) { return; }

  var reducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;

  function finish() {
    if (!overlay.classList.contains('done')) {
      overlay.classList.add('done');
      document.body.classList.remove('ls-locked');
      window.setTimeout(function () {
        if (overlay.parentNode) { overlay.parentNode.removeChild(overlay); }
      }, 1100);
    }
  }

  if (reducedMotion) {
    if (overlay.parentNode) { overlay.parentNode.removeChild(overlay); }
    document.body.classList.remove('ls-locked');
    return;
  }

  document.body.classList.add('ls-locked');

  var skipped = false;
  var skip = function () {
    if (skipped) { return; }
    skipped = true;
    finish();
  };
  window.addEventListener('pointerdown', skip, { once: false });
  window.addEventListener('keydown', function (e) {
    if (e.key === 'Escape' || e.key === 'Enter' || e.key === ' ') { skip(); }
  });

  // 音效由软件侧 Link Start 统一播放，网页此处静默。
  var timers = [];
  var later = function (fn, ms) { timers.push(window.setTimeout(fn, ms)); };

  later(function () { overlay.classList.add('s1'); }, 80);   // 环圈聚能
  later(function () { overlay.classList.add('s2'); }, 900);  // 六边形锁定
  later(function () {
    overlay.classList.add('s3');
    // LINK START 逐字弹出：JS 驱动（transition 播形变，不依赖 CSS animation 时钟）
    var chars = overlay.querySelectorAll('.ls-title span');
    for (var i = 0; i < chars.length; i++) {
      (function (el, delay) {
        timers.push(window.setTimeout(function () { el.classList.add('on'); }, delay));
      })(chars[i], i * 60);
    }
  }, 1500); // LINK START 文字
  later(function () { overlay.classList.add('s4'); }, 3000); // 爆闪
  later(function () { finish(); }, 3450);

  // 安全网：10 秒内无论如何结束，避免极端环境卡死页面。
  later(function () { finish(); }, 10000);

  window.addEventListener('pagehide', function () {
    for (var i = 0; i < timers.length; i++) { window.clearTimeout(timers[i]); }
  });
})();