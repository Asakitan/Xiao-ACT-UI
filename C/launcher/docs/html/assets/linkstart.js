// SAO Auto 用户指南 — LINK START 开场连接动画。
// 阶段：环圈聚能 → 六边形锁定 → LINK START 逐字 → 光柱爆闪 → 淡出。
(function () {
  'use strict';

  var overlay = document.getElementById('linkstart-overlay');
  if (!overlay) { return; }

  var motionPreference = window.matchMedia('(prefers-reduced-motion: reduce)');
  var contrastPreference = window.matchMedia('(forced-colors: active)');
  var introSeen = false;
  try { introSeen = window.sessionStorage.getItem('sao-guide-intro-seen') === '1'; } catch (error) {}
  var timers = [];
  var disposed = false;
  var finishing = false;
  var removalTimer = 0;

  function clearTimers() {
    for (var i = 0; i < timers.length; i++) { window.clearTimeout(timers[i]); }
    timers.length = 0;
    if (removalTimer) { window.clearTimeout(removalTimer); removalTimer = 0; }
  }
  function removeListeners() {
    window.removeEventListener('pointerdown', skip);
    window.removeEventListener('keydown', onKeydown);
    window.removeEventListener('pagehide', onPagehide);
    document.removeEventListener('visibilitychange', onVisibilityChange);
    if (motionPreference.removeEventListener) {
      motionPreference.removeEventListener('change', onPreferenceChange);
      contrastPreference.removeEventListener('change', onPreferenceChange);
    }
  }
  function dispose() {
    if (disposed) { return; }
    disposed = true;
    clearTimers();
    removeListeners();
    document.body.classList.remove('ls-locked');
    if (overlay.parentNode) { overlay.parentNode.removeChild(overlay); }
  }
  function finish() {
    if (disposed || finishing) { return; }
    finishing = true;
    try { window.sessionStorage.setItem('sao-guide-intro-seen', '1'); } catch (error) {}
    clearTimers();
    window.removeEventListener('pointerdown', skip);
    window.removeEventListener('keydown', onKeydown);
    overlay.classList.add('done');
    document.body.classList.remove('ls-locked');
    removalTimer = window.setTimeout(function () {
      removalTimer = 0;
      dispose();
    }, 1100);
  }
  function schedule(fn, ms) {
    if (disposed || finishing) { return; }
    var id = window.setTimeout(function () {
      var index = timers.indexOf(id);
      if (index >= 0) { timers.splice(index, 1); }
      if (disposed || finishing) { return; }
      if (!overlay.parentNode) { dispose(); return; }
      fn();
    }, ms);
    timers.push(id);
  }
  function skip() {
    if (!disposed) { finish(); }
  }
  function onKeydown(e) {
    if (e.key === 'Escape' || e.key === 'Enter' || e.key === ' ') { skip(); }
  }
  function onPagehide() {
    dispose();
  }
  function onVisibilityChange() {
    if (document.hidden) { dispose(); }
  }
  function onPreferenceChange() {
    if (motionPreference.matches || contrastPreference.matches) { dispose(); }
  }

  if (introSeen || window.location.hash || motionPreference.matches ||
      document.hidden || contrastPreference.matches) {
    dispose();
    return;
  }

  document.body.classList.add('ls-locked');
  window.addEventListener('pointerdown', skip);
  window.addEventListener('keydown', onKeydown);
  window.addEventListener('pagehide', onPagehide);
  document.addEventListener('visibilitychange', onVisibilityChange);
  if (motionPreference.addEventListener) {
    motionPreference.addEventListener('change', onPreferenceChange);
    contrastPreference.addEventListener('change', onPreferenceChange);
  }

  schedule(function () { overlay.classList.add('s1'); }, 80);
  schedule(function () { overlay.classList.add('s2'); }, 900);
  schedule(function () {
    overlay.classList.add('s3');
    var chars = overlay.querySelectorAll('.ls-title span');
    for (var i = 0; i < chars.length; i++) {
      (function (el, delay) {
        schedule(function () { el.classList.add('on'); }, delay);
      })(chars[i], i * 60);
    }
  }, 1500);
  schedule(function () { overlay.classList.add('s4'); }, 3000);
  schedule(function () { finish(); }, 3450);
  schedule(function () { finish(); }, 10000);
})();