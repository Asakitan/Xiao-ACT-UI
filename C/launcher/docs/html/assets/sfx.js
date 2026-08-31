// SAO Auto 用户指南 — SAO 音效引擎。
// 音效语义 1:1 对齐 python/utils/sao_sound.py 的 SAO_SOUNDS：
//   click       Feedback.SAO.Click    点击 / 悬停
//   menu_open   Popup.SAO.Launcher    打开主入口（hero CTA）
//   menu_close  Dismiss.SAO.Launcher  主题切换收合
//   submenu     Popup.SAO.Menu        目录 / 导航跳转
//   panel       Popup.SAO.Panel       FAQ 展开
//   alert_close Dismiss.SAO.Message   FAQ 收起 / 音效关闭
// LinkStart / NerveGear 由软件侧播放，网页不重复。
// 首次点击前不发声（浏览器 autoplay 策略）；开关持久化在 localStorage。
(function () {
  'use strict';

  var SFX = {
    click: 'assets/sounds/Feedback.SAO.Click.mp3',
    menu_open: 'assets/sounds/Popup.SAO.Launcher.mp3',
    menu_close: 'assets/sounds/Dismiss.SAO.Launcher.mp3',
    submenu: 'assets/sounds/Popup.SAO.Menu.mp3',
    panel: 'assets/sounds/Popup.SAO.Panel.mp3',
    alert_close: 'assets/sounds/Dismiss.SAO.Message.mp3'
  };

  var muted = false;
  try { muted = localStorage.getItem('sao-manual-sfx') === 'off'; } catch (e) { /* 私密模式忽略 */ }

  var toggle = document.getElementById('sound-toggle');
  var applyToggleUi = function () {
    if (!toggle) { return; }
    toggle.classList.toggle('muted', muted);
    toggle.setAttribute('aria-pressed', muted ? 'false' : 'true');
    toggle.setAttribute('aria-label', muted ? '打开音效' : '关闭音效');
    toggle.title = muted ? '音效已关，点一下打开' : '音效已开，点一下关闭';
  };
  applyToggleUi();

  // 简单音频池，避免反复 new Audio
  var pool = {};
  function obtain(path) {
    if (!pool[path]) { pool[path] = []; }
    var list = pool[path];
    var el = null;
    for (var i = 0; i < list.length; i++) {
      if (list[i].paused || list[i].ended) { el = list[i]; break; }
    }
    if (!el && list.length < 4) {
      el = new Audio(path);
      el.preload = 'auto';
      list.push(el);
    }
    return el || list[0];
  }

  var unlocked = false;
  function play(name, vol) {
    if (muted || !unlocked) { return; }
    var path = SFX[name];
    if (!path) { return; }
    var el = obtain(path);
    if (!el) { return; }
    try {
      el.volume = typeof vol === 'number' ? vol : 0.4;
      el.currentTime = 0;
      var p = el.play();
      if (p && p.catch) { p.catch(function () { /* 被策略拦截就静默 */ }); }
    } catch (e) { /* 静默 */ }
  }
  // 供 detail_popup 等其他脚本复用（面板开合音）
  window.saoSfxPlay = play;
  window.saoSfxUnlock = unlock;

  function unlock() {
    if (unlocked) { return; }
    unlocked = true;
    // LinkStart / NerveGear 音效由软件侧播放，这里解锁后不重复配音。
  }

  var lastHover = 0;

  /* 点击分发：按目标性质选音效 + 触发 GPU 水波 */
  document.addEventListener('click', function (e) {
    unlock();
    var t = e.target && e.target.closest ? e.target.closest('a, button, summary, [role="button"]') : null;
    if (toggle && t === toggle) { return; } // 音效开关自己处理
    if (t && t.classList && t.classList.contains('action-primary')) {
      play('menu_open', 0.4);
    } else if (t && t.closest && t.closest('.toc, .header-nav, .hero-actions')) {
      play('submenu', 0.42);
    } else {
      play('click', 0.32);
    }
    if (window.saoRipple) {
      window.saoRipple(e.clientX, e.clientY);
    }
  }, { passive: true });

  /* 悬停：轻触滴音（节流 90ms） */
  document.addEventListener('mouseover', function (e) {
    var t = e.target;
    if (!t || !t.closest) { return; }
    var hot = t.closest('a, button, summary, input, select, [role="button"]');
    if (!hot) { return; }
    var now = Date.now();
    if (now - lastHover < 90) { return; }
    lastHover = now;
    play('click', 0.14);
  }, { passive: true });

  /* FAQ 展开/收起 */
  document.addEventListener('toggle', function (e) {
    var d = e.target;
    if (!d || d.tagName !== 'DETAILS') { return; }
    if (d.open) { play('panel', 0.4); } else { play('alert_close', 0.4); }
  }, { capture: true, passive: true });

  /* 主题切换：收合音 */
  var themeBtn = document.getElementById('theme-toggle');
  if (themeBtn) {
    themeBtn.addEventListener('click', function () {
      play('menu_close', 0.38);
    });
  }

  /* 音效开关 */
  if (toggle) {
    toggle.addEventListener('click', function () {
      muted = !muted;
      applyToggleUi();
      try { localStorage.setItem('sao-manual-sfx', muted ? 'off' : 'on'); } catch (err) { /* 忽略 */ }
      if (!muted) { play('alert_close', 0.4); }
    });
  }
})();