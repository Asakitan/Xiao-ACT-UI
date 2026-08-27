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
  var ticking = false;

  var update = function () {
    ticking = false;

    if (fill) {
      var doc = document.documentElement;
      var max = doc.scrollHeight - window.innerHeight;
      var ratio = max > 0 ? window.scrollY / max : 0;
      fill.style.transform = 'scaleX(' + Math.min(1, Math.max(0, ratio)) + ')';
    }

    if (sections.length > 0 && tocLinks.length > 0) {
      // 激活线：视口顶部往下 40% 处作为“当前阅读位置”
      var probe = window.scrollY + window.innerHeight * 0.4;
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

  window.addEventListener('scroll', function () {
    if (!ticking) {
      ticking = true;
      window.requestAnimationFrame(update);
    }
  }, { passive: true });
  window.addEventListener('resize', function () {
    if (!ticking) {
      ticking = true;
      window.requestAnimationFrame(update);
    }
  }, { passive: true });
  update();

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
