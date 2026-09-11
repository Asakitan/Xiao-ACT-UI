// SAO Auto 用户指南 — LINK START 开场连接动画。
// 阶段：环圈聚能 → 六边形锁定 → LINK START 逐字 → 光柱爆闪 → 淡出。
(function () {
  'use strict';

  var overlay = document.getElementById('linkstart-overlay');
  if (!overlay) { return; }

  var motionPreference = window.matchMedia('(prefers-reduced-motion: reduce)');
  var contrastPreference = window.matchMedia('(forced-colors: active)');
  var nativeIntroCompleted = window.location.hash === '#sao-native-intro-complete';
  var introSeen = false;
  try { introSeen = window.sessionStorage.getItem('sao-guide-intro-seen') === '1'; } catch (error) {}
  if (nativeIntroCompleted) {
    try { window.sessionStorage.setItem('sao-guide-intro-seen', '1'); } catch (error) {}
  }
  var timers = [];
  var disposed = false;
  var finishing = false;
  var removalTimer = 0;
  var pressedPointer = null;
  var isolatedNodes = [];
  var previousFocus = document.activeElement;
  var ownsFocus = false;

  function clearTimers() {
    for (var i = 0; i < timers.length; i++) { window.clearTimeout(timers[i]); }
    timers.length = 0;
    if (removalTimer) { window.clearTimeout(removalTimer); removalTimer = 0; }
  }
  function removeListeners() {
    overlay.removeEventListener('pointerdown', onPointerDown);
    overlay.removeEventListener('pointerup', skip);
    overlay.removeEventListener('pointercancel', onPointerCancel);
    overlay.removeEventListener('pointerleave', onPointerCancel);
    overlay.removeEventListener('lostpointercapture', onPointerCancel);
    window.removeEventListener('keydown', onKeydown, true);
    window.removeEventListener('hashchange', onHashChange);
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
    isolatedNodes.forEach(function (state) {
      if (!state.inert) { state.node.removeAttribute('inert'); }
    });
    isolatedNodes.length = 0;
    if (overlay.parentNode) { overlay.parentNode.removeChild(overlay); }
    if (ownsFocus && previousFocus && document.contains(previousFocus)) {
      previousFocus.focus({ preventScroll: true });
    }
  }
  function finish(skipped) {
    if (disposed || finishing) { return; }
    finishing = true;
    try { window.sessionStorage.setItem('sao-guide-intro-seen', '1'); } catch (error) {}
    clearTimers();
    var duration = skipped ? 180 : 900;
    overlay.style.transition = 'opacity ' + duration + 'ms ease, visibility 0s linear ' + duration + 'ms';
    overlay.style.pointerEvents = 'auto';
    overlay.classList.add('done');
    removalTimer = window.setTimeout(function () {
      removalTimer = 0;
      dispose();
    }, duration + 30);
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
  function onPointerDown(e) {
    if (!e.isPrimary || e.button !== 0) { return; }
    pressedPointer = e.pointerId;
    e.preventDefault();
  }
  function onPointerCancel(e) {
    if (e.pointerId === pressedPointer) { pressedPointer = null; }
  }
  function skip(e) {
    if (e.pointerId !== pressedPointer) { return; }
    pressedPointer = null;
    e.preventDefault();
    e.stopPropagation();
    if (!disposed) { finish(true); }
  }
  function onKeydown(e) {
    e.stopPropagation();
    if (e.key === 'Tab') { e.preventDefault(); }
    if (e.key === 'Escape' || e.key === 'Enter' || e.key === ' ') {
      e.preventDefault();
      finish(true);
    }
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
  function onHashChange() {
    if (window.location.hash !== '#sao-native-intro-complete') { return; }
    try { window.sessionStorage.setItem('sao-guide-intro-seen', '1'); } catch (error) {}
    dispose();
  }

  if (introSeen || nativeIntroCompleted || window.location.hash || motionPreference.matches ||
      document.hidden || contrastPreference.matches) {
    dispose();
    return;
  }

  document.body.classList.add('ls-locked');
  Array.prototype.forEach.call(document.body.children, function (node) {
    if (node === overlay || node.tagName === 'SCRIPT') { return; }
    isolatedNodes.push({ node: node, inert: node.hasAttribute('inert') });
    node.setAttribute('inert', '');
  });
  overlay.setAttribute('role', 'dialog');
  overlay.setAttribute('aria-modal', 'true');
  overlay.setAttribute('aria-label', 'Link Start 开场');
  overlay.setAttribute('tabindex', '-1');
  overlay.focus({ preventScroll: true });
  ownsFocus = true;
  overlay.addEventListener('pointerdown', onPointerDown);
  overlay.addEventListener('pointerup', skip);
  overlay.addEventListener('pointercancel', onPointerCancel);
  overlay.addEventListener('pointerleave', onPointerCancel);
  overlay.addEventListener('lostpointercapture', onPointerCancel);
  window.addEventListener('keydown', onKeydown, true);
  window.addEventListener('hashchange', onHashChange);
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