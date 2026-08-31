// SAO Auto 用户指南 — SC1 风格动态光标（青色主题，顶点精确对准热点）。
// 常态：优雅青箭头（尖端=热点）+ 3D 倾斜椭圆轨道环（绕箭头尾部旋转）+ 呼吸光核。
// 点击态：四角对焦括号收夹 + 前后双环纵深 + 细十字，中心=热点。
// 两态切换带双向形变动画（JS 只驱动 left/top，transform 交给 CSS 过渡/动画）。
// 触屏 / reduced-motion 不接管。
(function () {
  'use strict';

  var fine = window.matchMedia('(pointer: fine)').matches;
  var reducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  if (!fine || reducedMotion) {
    document.documentElement.classList.add('no-sc-cursor');
    return;
  }

  var C = { accent: '#47d7ff', dark: '#0b657f', pale: '#b9f0ff', white: '#ffffff' };

  // 常态：箭头尖端在 viewBox (6,5)，热点偏移由 CSS translate(15.5px,16.5px) 补偿
  var NORMAL_SVG =
    '<svg xmlns="http://www.w3.org/2000/svg" width="44" height="44" viewBox="0 0 44 44">' +
    '<defs><linearGradient id="scg1" x1="0" y1="0" x2="1" y2="1">' +
    '<stop offset="0" stop-color="#14829f"/><stop offset="0.55" stop-color="' + C.accent + '"/>' +
    '<stop offset="1" stop-color="#8be8ff"/></linearGradient>' +
    '<radialGradient id="scg2" cx="0.5" cy="0.42" r="0.55">' +
    '<stop offset="0" stop-color="rgba(71,215,255,0.35)"/><stop offset="1" stop-color="rgba(71,215,255,0)"/>' +
    '</radialGradient></defs>' +
    // 地面投影（最底）
    '<ellipse cx="20" cy="33.2" rx="8.6" ry="2.4" fill="' + C.dark + '" opacity="0.18"/>' +
    // 3D 倾斜轨道环（绕尾部 21,17，在箭头下层）
    '<g class="sc-orbit-group">' +
    '<g class="sc-orbit-ellipse">' +
    '<ellipse cx="21" cy="17" rx="14.6" ry="6" fill="none" stroke="' + C.dark + '" stroke-width="1.8" opacity="0.7"/>' +
    '<ellipse cx="21" cy="17" rx="13.2" ry="5" fill="none" stroke="' + C.accent + '" stroke-width="1" opacity="0.95"/>' +
    '<circle cx="34.6" cy="17" r="2" fill="' + C.pale + '" stroke="' + C.dark + '" stroke-width="0.9"/>' +
    '<circle cx="7.4" cy="17" r="1.3" fill="' + C.accent + '" opacity="0.65"/>' +
    '</g>' +
    '</g>' +
    // 呼吸光核
    '<g class="sc-core">' +
    '<circle cx="21" cy="17" r="7.6" fill="url(#scg2)"/>' +
    '<circle cx="21" cy="17" r="2.3" fill="' + C.pale + '" stroke="' + C.dark + '" stroke-width="1"/>' +
    '</g>' +
    // 箭头（最上层，覆盖旋转环）
    '<g class="sc-arrow">' +
    '<path d="M5.6 4.4 L23.4 14.9 17.9 16.6 24.2 24.6 19.4 27.0 7.0 12.6 Z" ' +
    'fill="url(#scg1)" stroke="#ffffff" stroke-width="1.3" stroke-linejoin="round"/>' +
    '<path d="M9.4 8.1 L19.4 15.2" stroke="rgba(255,255,255,0.85)" stroke-width="1.1" fill="none"/>' +
    '</g>' +
    '</svg>';

  // 点击态：中心 (22,22) = 热点
  var ACTIVE_SVG =
    '<svg xmlns="http://www.w3.org/2000/svg" width="44" height="44" viewBox="0 0 44 44">' +
    // 后层远环（小、半透明 → 纵深）
    '<g class="sc-far-ring">' +
    '<circle cx="22" cy="22" r="13.2" fill="none" stroke="' + C.accent + '" stroke-width="1.1" opacity="0.42"/>' +
    '<circle cx="22" cy="22" r="16.6" fill="none" stroke="' + C.dark + '" stroke-width="1.4" opacity="0.26"/>' +
    '</g>' +
    // 四角括号（云形收夹）
    '<g class="sc-brackets" stroke="' + C.accent + '" stroke-width="2.4" fill="none" stroke-linecap="round">' +
    '<path class="sc-bracket-nw" d="M6.6 16 v-4.4 a5 5 0 0 1 5 -5 h4.4"/>' +
    '<path class="sc-bracket-ne" d="M28 6.6 H32.4 a5 5 0 0 1 5 5 V16"/>' +
    '<path class="sc-bracket-se" d="M37.4 28 v4.4 a5 5 0 0 1 -5 5 H28"/>' +
    '<path class="sc-bracket-sw" d="M16 37.4 H11.6 a5 5 0 0 1 -5 -5 V28"/>' +
    '</g>' +
    // 近环（前层、亮）
    '<g class="sc-near-ring">' +
    '<circle cx="22" cy="22" r="11" fill="none" stroke="' + C.pale + '" stroke-width="1.9" opacity="0.95"/>' +
    '<circle cx="22" cy="22" r="11" fill="none" stroke="' + C.accent + '" stroke-width="0.7" opacity="0.5"/>' +
    '</g>' +
    // 细十字 + 白点
    '<g stroke="' + C.pale + '" stroke-width="1" opacity="0.9">' +
    '<path d="M22 17.4v9.2M17.4 22h9.2"/>' +
    '</g>' +
    '<circle cx="22" cy="22" r="1.6" fill="' + C.white + '" stroke="' + C.dark + '" stroke-width="0.8"/>' +
    '</svg>';

  var cursor = document.createElement('div');
  cursor.className = 'sc-cursor';
  cursor.setAttribute('aria-hidden', 'true');
  cursor.innerHTML =
    '<div class="sc-normal">' + NORMAL_SVG + '</div>' +
    '<div class="sc-active">' + ACTIVE_SVG + '</div>';
  document.body.appendChild(cursor);

  var hot = false;
  var overSel = 'a, button, summary, input, select, textarea, [role="button"], kbd, .toc a';

  function place(px, py) {
    cursor.style.left = (px - 22) + 'px';
    cursor.style.top = (py - 22) + 'px';
  }

  document.addEventListener('mousemove', function (e) {
    place(e.clientX, e.clientY);
    if (cursor.style.visibility !== 'visible') { cursor.style.visibility = 'visible'; }
  }, { passive: true });

  document.addEventListener('mouseover', function (e) {
    if (e.target && e.target.closest && e.target.closest(overSel)) {
      hot = true;
      cursor.classList.add('on-hot');
    }
  }, { passive: true });
  document.addEventListener('mouseout', function (e) {
    if (hot && (!e.relatedTarget || !e.relatedTarget.closest ||
        !e.relatedTarget.closest(overSel))) {
      hot = false;
      cursor.classList.remove('on-hot');
    }
  }, { passive: true });

  // 按压切换（双向过渡由 CSS transition 处理）
  var pressTimer = 0;
  document.addEventListener('pointerdown', function () {
    cursor.classList.add('pressed');
    window.clearTimeout(pressTimer);
    pressTimer = window.setTimeout(function () {
      cursor.classList.remove('pressed');
    }, 340);
  });
  document.addEventListener('pointerup', function () {
    window.clearTimeout(pressTimer);
    cursor.classList.remove('pressed');
  });
})();