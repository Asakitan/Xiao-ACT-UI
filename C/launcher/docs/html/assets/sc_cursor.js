// SAO Auto 用户指南 — SC1 风格动态光标。
// 仅在细指针、非高对比、非 reduced-motion 且完整初始化成功后接管。
(function () {
  'use strict';

  var root = document.documentElement;
  function fallback(cursor) {
    root.classList.remove('sc-cursor-ready');
    root.classList.add('no-sc-cursor');
    if (cursor && cursor.parentNode) { cursor.parentNode.removeChild(cursor); }
  }

  if (!window.matchMedia || !window.requestAnimationFrame) {
    root.classList.add('no-sc-cursor');
    return;
  }
  var fineQuery = window.matchMedia('(pointer: fine)');
  var motionQuery = window.matchMedia('(prefers-reduced-motion: reduce)');
  var contrastQuery = window.matchMedia('(forced-colors: active)');
  function canUseCursor() {
    return fineQuery.matches && !motionQuery.matches && !contrastQuery.matches && !document.hidden;
  }

  var C = { accent: '#47d7ff', dark: '#0b657f', pale: '#b9f0ff', white: '#ffffff' };
  var NORMAL_SVG =
    '<svg xmlns="http://www.w3.org/2000/svg" width="44" height="44" viewBox="0 0 44 44">' +
    '<defs><linearGradient id="scg1" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#14829f"/><stop offset="0.55" stop-color="' + C.accent + '"/><stop offset="1" stop-color="#8be8ff"/></linearGradient><radialGradient id="scg2" cx="0.5" cy="0.42" r="0.55"><stop offset="0" stop-color="rgba(71,215,255,0.35)"/><stop offset="1" stop-color="rgba(71,215,255,0)"/></radialGradient></defs>' +
    '<ellipse cx="20" cy="33.2" rx="8.6" ry="2.4" fill="' + C.dark + '" opacity="0.18"/>' +
    '<g class="sc-orbit-group"><g class="sc-orbit-ellipse"><ellipse cx="21" cy="17" rx="14.6" ry="6" fill="none" stroke="' + C.dark + '" stroke-width="1.8" opacity="0.7"/><ellipse cx="21" cy="17" rx="13.2" ry="5" fill="none" stroke="' + C.accent + '" stroke-width="1" opacity="0.95"/><circle cx="34.6" cy="17" r="2" fill="' + C.pale + '" stroke="' + C.dark + '" stroke-width="0.9"/><circle cx="7.4" cy="17" r="1.3" fill="' + C.accent + '" opacity="0.65"/></g></g>' +
    '<g class="sc-core"><circle cx="21" cy="17" r="7.6" fill="url(#scg2)"/><circle cx="21" cy="17" r="2.3" fill="' + C.pale + '" stroke="' + C.dark + '" stroke-width="1"/></g>' +
    '<g class="sc-arrow"><path d="M5.6 4.4 L23.4 14.9 17.9 16.6 24.2 24.6 19.4 27.0 7.0 12.6 Z" fill="url(#scg1)" stroke="#ffffff" stroke-width="1.3" stroke-linejoin="round"/><path d="M9.4 8.1 L19.4 15.2" stroke="rgba(255,255,255,0.85)" stroke-width="1.1" fill="none"/></g>' +
    '</svg>';
  var ACTIVE_SVG =
    '<svg xmlns="http://www.w3.org/2000/svg" width="44" height="44" viewBox="0 0 44 44">' +
    '<g class="sc-far-ring"><circle cx="22" cy="22" r="13.2" fill="none" stroke="' + C.accent + '" stroke-width="1.1" opacity="0.42"/><circle cx="22" cy="22" r="16.6" fill="none" stroke="' + C.dark + '" stroke-width="1.4" opacity="0.26"/></g>' +
    '<g class="sc-brackets" stroke="' + C.accent + '" stroke-width="2.4" fill="none" stroke-linecap="round"><path class="sc-bracket-nw" d="M6.6 16 v-4.4 a5 5 0 0 1 5 -5 h4.4"/><path class="sc-bracket-ne" d="M28 6.6 H32.4 a5 5 0 0 1 5 5 V16"/><path class="sc-bracket-se" d="M37.4 28 v4.4 a5 5 0 0 1 -5 5 H28"/><path class="sc-bracket-sw" d="M16 37.4 H11.6 a5 5 0 0 1 -5 -5 V28"/></g>' +
    '<g class="sc-near-ring"><circle cx="22" cy="22" r="11" fill="none" stroke="' + C.pale + '" stroke-width="1.9" opacity="0.95"/><circle cx="22" cy="22" r="11" fill="none" stroke="' + C.accent + '" stroke-width="0.7" opacity="0.5"/></g>' +
    '<g stroke="' + C.pale + '" stroke-width="1" opacity="0.9"><path d="M22 17.4v9.2M17.4 22h9.2"/></g><circle cx="22" cy="22" r="1.6" fill="' + C.white + '" stroke="' + C.dark + '" stroke-width="0.8"/>' +
    '</svg>';

  var cursor = null;
  try {
    cursor = document.createElement('div');
    cursor.className = 'sc-cursor';
    cursor.setAttribute('aria-hidden', 'true');
    cursor.innerHTML = '<div class="sc-normal">' + NORMAL_SVG + '</div><div class="sc-active">' + ACTIVE_SVG + '</div>';
    document.body.appendChild(cursor);

    var hot = false;
    var overSel = 'a, button, summary, input, select, textarea, [role="button"], kbd, .toc a';
    var pendingX = -22;
    var pendingY = -22;
    var moveFrame = 0;
    var pressTimer = 0;

    function flushPosition() {
      moveFrame = 0;
      if (!canUseCursor()) { return; }
      cursor.style.transform = 'translate3d(' + pendingX + 'px,' + pendingY + 'px,0)';
      cursor.classList.add('is-visible');
      if (!root.classList.contains('sc-cursor-ready')) {
        root.classList.remove('no-sc-cursor');
        root.classList.add('sc-cursor-ready');
      }
    }
    function queuePosition(e) {
      if (!canUseCursor() || (e.pointerType && e.pointerType !== 'mouse')) { return; }
      pendingX = e.clientX - 22;
      pendingY = e.clientY - 22;
      if (!moveFrame) { moveFrame = window.requestAnimationFrame(flushPosition); }
    }
    function isHot(target) {
      return !!(target && target.closest && target.closest(overSel));
    }
    function resetPress() {
      window.clearTimeout(pressTimer);
      pressTimer = 0;
      cursor.classList.remove('pressed');
    }
    function releaseCursor() {
      if (moveFrame) { window.cancelAnimationFrame(moveFrame); moveFrame = 0; }
      resetPress();
      hot = false;
      cursor.classList.remove('is-visible', 'on-hot');
      if (root.classList.contains('sc-cursor-ready')) { root.classList.remove('sc-cursor-ready'); }
      if (!root.classList.contains('no-sc-cursor')) { root.classList.add('no-sc-cursor'); }
    }

    document.addEventListener('pointermove', queuePosition, { passive: true });
    document.addEventListener('pointerover', function (e) {
      if (canUseCursor() && isHot(e.target)) { hot = true; cursor.classList.add('on-hot'); }
    }, { passive: true });
    document.addEventListener('pointerout', function (e) {
      if (hot && !isHot(e.relatedTarget)) { hot = false; cursor.classList.remove('on-hot'); }
    }, { passive: true });
    document.addEventListener('pointerdown', function () {
      if (!canUseCursor()) { return; }
      cursor.classList.add('pressed');
      window.clearTimeout(pressTimer);
      pressTimer = window.setTimeout(resetPress, 340);
    }, { passive: true });
    document.addEventListener('pointerup', resetPress, { passive: true });
    document.addEventListener('pointercancel', resetPress, { passive: true });
    window.addEventListener('blur', releaseCursor);
    window.addEventListener('pagehide', releaseCursor);
    document.documentElement.addEventListener('pointerleave', releaseCursor);
    document.addEventListener('visibilitychange', releaseCursor);
    [fineQuery, motionQuery, contrastQuery].forEach(function (query) {
      query.addEventListener('change', releaseCursor);
    });

    releaseCursor();
  } catch (e) {
    fallback(cursor);
  }
})();