// SAO Auto 用户指南 — 滚动交互：阅读进度条 + 目录激活高亮 + 章节入场动画。
// 进度条和目录高亮用 scroll + rAF 实现；入场动画用 IntersectionObserver，
// 并带定时器兜底，保证任何环境下内容最终都会显示。
(function () {
  'use strict';

  document.body.classList.add('js');

  var reducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  var sections = Array.prototype.slice.call(document.querySelectorAll('.manual-section[id]'));
  var tocLinks = document.querySelectorAll('.toc a[data-target]');

  var setActive = function (id) {
    for (var i = 0; i < tocLinks.length; i++) {
      tocLinks[i].classList.toggle('active', tocLinks[i].getAttribute('data-target') === id);
    }
  };

  /* ── 滚动统一处理：进度条 + 目录高亮 ── */
  var fill = document.getElementById('progress-fill');

  var update = function () {
    if (fill) {
      var doc = document.documentElement;
      var max = doc.scrollHeight - window.innerHeight;
      var ratio = max > 0 ? window.scrollY / max : 0;
      fill.style.transform = 'scaleX(' + Math.min(1, Math.max(0, ratio)) + ')';
    }

    if (sections.length > 0 && tocLinks.length > 0) {
      // 激活线：视口顶部往下 25% 处作为“当前阅读位置”
      var probe = window.scrollY + window.innerHeight * 0.25;
      var currentId = sections[0].id;
      for (var i = 0; i < sections.length; i++) {
        if (sections[i].offsetTop <= probe) {
          currentId = sections[i].id;
        } else {
          break;
        }
      }
      setActive(currentId);
    }
  };

  // 直接响应 scroll/resize（函数很轻），不依赖 rAF 或定时器（部分 WebView 会节流或停派发）。
  window.addEventListener('scroll', update, { passive: true });
  window.addEventListener('resize', update, { passive: true });
  update();

  /* ── 章节 popup 跳转（点击锚点 → 弹出目标章节） ── */
  var popupLabel = null;
  var popupTimer = 0;
  function showSectionPopup(section) {
    // 漂浮章节标签
    if (!popupLabel) {
      popupLabel = document.createElement('div');
      popupLabel.className = 'section-popup-label';
      popupLabel.setAttribute('aria-hidden', 'true');
      document.body.appendChild(popupLabel);
    }
    var num = section.querySelector('.section-number');
    var title = section.querySelector('.section-heading h2');
    popupLabel.innerHTML =
      '<b>' + (num ? num.textContent : '') + '</b><span>' +
      (title ? title.textContent : '') + '</span>';
    popupLabel.classList.remove('show');
    void popupLabel.offsetWidth;
    popupLabel.classList.add('show');
    window.clearTimeout(popupTimer);
    popupTimer = window.setTimeout(function () {
      popupLabel.classList.remove('show');
    }, 1600);
  }

  document.addEventListener('click', function (e) {
    var a = e.target && e.target.closest ? e.target.closest('a[href^="#"]') : null;
    if (!a) { return; }
    var id = a.getAttribute('href').slice(1);
    if (!id) { return; }
    var section = document.getElementById(id);
    if (!section || !section.classList || !section.classList.contains('manual-section')) { return; }
    section.classList.remove('pop-target');
    void section.offsetWidth;
    section.classList.add('pop-target');
    showSectionPopup(section);
    window.setTimeout(function () {
      section.classList.remove('pop-target');
    }, 700);
  }, { passive: true });

  /* ── 章节入场动画 ── */
  var revealAll = function () {
    for (var i = 0; i < sections.length; i++) {
      sections[i].classList.add('in');
    }
  };

  if (!reducedMotion && 'IntersectionObserver' in window) {
    var revealed = 0;
    var revealObserver = new IntersectionObserver(function (entries) {
      entries.forEach(function (entry) {
        if (entry.isIntersecting) {
          entry.target.classList.add('in');
          revealed++;
          revealObserver.unobserve(entry.target);
        }
      });
    }, { rootMargin: '0px 0px -8% 0px', threshold: 0.05 });

    sections.forEach(function (section) {
      revealObserver.observe(section);
    });

    // 兜底：某些环境 IntersectionObserver 不派发回调，
    // 1.6 秒内一个都没触发就直接全部显示，内容优先。
    window.setTimeout(function () {
      if (revealed === 0) {
        revealAll();
      }
    }, 1600);
  } else {
    revealAll();
  }
})();
