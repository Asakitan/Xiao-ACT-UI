(function () {
  'use strict';

  document.body.classList.add('js');

  var reducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  var sections = Array.prototype.slice.call(document.querySelectorAll('.manual-section[id]'));
  var tocLinks = document.querySelectorAll('.toc a[data-target]');
  var chapterSelect = document.getElementById('chapter-select');
  if (chapterSelect) {
    for (var chapterIndex = 0; chapterIndex < tocLinks.length; chapterIndex++) {
      var option = document.createElement('option');
      option.value = tocLinks[chapterIndex].getAttribute('data-target');
      option.textContent = tocLinks[chapterIndex].textContent.trim();
      chapterSelect.appendChild(option);
    }
    chapterSelect.addEventListener('change', function () {
      var target = document.getElementById(chapterSelect.value);
      if (!target || !target.classList.contains('manual-section')) { return; }
      if (window.location.hash === '#' + target.id) {
        target.scrollIntoView({ behavior: window.matchMedia('(prefers-reduced-motion: reduce)').matches ? 'auto' : 'smooth' });
      } else {
        window.location.hash = target.id;
      }
      target.setAttribute('tabindex', '-1');
      target.focus({ preventScroll: true });
    });
  }
  var sectionPositions = [];
  var frameId = 0;
  var activeSectionId = null;
  var lastHandledHash = null;
  var sectionTimers = typeof WeakMap === 'function' ? new WeakMap() : null;

  sections.forEach(function (section) { section.classList.add('reveal'); });

  var setActive = function (id) {
    if (activeSectionId === id) { return; }
    activeSectionId = id;
    if (chapterSelect) { chapterSelect.value = id; }
    for (var i = 0; i < tocLinks.length; i++) {
      var active = tocLinks[i].getAttribute('data-target') === id;
      tocLinks[i].classList.toggle('active', active);
      if (active) {
        tocLinks[i].setAttribute('aria-current', 'location');
      } else {
        tocLinks[i].removeAttribute('aria-current');
      }
    }
  };

  var scrollTop = function () {
    return window.scrollY || window.pageYOffset || document.documentElement.scrollTop || 0;
  };

  var recalculatePositions = function () {
    sectionPositions = sections.map(function (section) {
      var top = 0;
      for (var node = section; node; node = node.offsetParent) { top += node.offsetTop; }
      return { id: section.id, top: top };
    });
  };

  var fill = document.getElementById('progress-fill');

  var update = function () {
    if (fill) {
      var doc = document.documentElement;
      var max = doc.scrollHeight - window.innerHeight;
      var ratio = max > 0 ? scrollTop() / max : 0;
      fill.style.transform = 'scaleX(' + Math.min(1, Math.max(0, ratio)) + ')';
    }

    if (sectionPositions.length > 0 && tocLinks.length > 0) {
      var probe = scrollTop() + window.innerHeight * 0.25;
      var currentId = sectionPositions[0].id;
      for (var i = 0; i < sectionPositions.length; i++) {
        if (sectionPositions[i].top <= probe) {
          currentId = sectionPositions[i].id;
        } else {
          break;
        }
      }
      setActive(currentId);
    }
  };

  var scheduleUpdate = function () {
    if (frameId) { return; }
    frameId = window.requestAnimationFrame(function () {
      frameId = 0;
      update();
    });
  };

  var layoutChanged = function () {
    recalculatePositions();
    scheduleUpdate();
  };

  window.addEventListener('scroll', scheduleUpdate, { passive: true });
  window.addEventListener('resize', layoutChanged, { passive: true });

  if ('ResizeObserver' in window) {
    var layoutObserver = new ResizeObserver(layoutChanged);
    var content = document.querySelector('.manual-content');
    var header = document.querySelector('.site-header');
    if (content) { layoutObserver.observe(content); }
    if (header) { layoutObserver.observe(header); }
  }

  if (document.fonts) {
    if (document.fonts.ready) { document.fonts.ready.then(layoutChanged, layoutChanged); }
    if (document.fonts.addEventListener) {
      document.fonts.addEventListener('loadingdone', layoutChanged);
    }
  }

  recalculatePositions();
  update();

  var popupLabel = null;
  var popupTimer = 0;
  function showSectionPopup(section) {
    if (!popupLabel) {
      popupLabel = document.createElement('div');
      popupLabel.className = 'section-popup-label';
      popupLabel.setAttribute('aria-hidden', 'true');
      document.body.appendChild(popupLabel);
    }
    var num = section.querySelector('.section-number');
    var title = section.querySelector('.section-heading h2');
    while (popupLabel.firstChild) { popupLabel.removeChild(popupLabel.firstChild); }
    var numberNode = document.createElement('b');
    numberNode.textContent = num ? num.textContent : '';
    var titleNode = document.createElement('span');
    titleNode.textContent = title ? title.textContent : '';
    popupLabel.appendChild(numberNode);
    popupLabel.appendChild(titleNode);
    popupLabel.classList.remove('show');
    void popupLabel.offsetWidth;
    popupLabel.classList.add('show');
    window.clearTimeout(popupTimer);
    popupTimer = window.setTimeout(function () {
      popupLabel.classList.remove('show');
    }, 1600);
  }

  function animateSection(section) {
    section.classList.add('visible');
    if (window.matchMedia('(prefers-reduced-motion: reduce)').matches) { return; }
    if (sectionTimers) {
      window.clearTimeout(sectionTimers.get(section));
    }
    section.classList.remove('pop-target');
    void section.offsetWidth;
    section.classList.add('pop-target');
    var timer = window.setTimeout(function () {
      section.classList.remove('pop-target');
      if (sectionTimers) { sectionTimers.delete(section); }
    }, 700);
    if (sectionTimers) { sectionTimers.set(section, timer); }
  }

  function sectionFromHash() {
    var raw = window.location.hash.slice(1);
    if (!raw) { return null; }
    try {
      raw = decodeURIComponent(raw);
    } catch (error) {
      return null;
    }
    var section = document.getElementById(raw);
    return section && section.classList.contains('manual-section') ? section : null;
  }

  function syncHashTarget() {
    if (lastHandledHash === window.location.hash) { scheduleUpdate(); return; }
    lastHandledHash = window.location.hash;
    var section = sectionFromHash();
    if (section) {
      animateSection(section);
      showSectionPopup(section);
    }
    layoutChanged();
  }

  document.addEventListener('click', function (e) {
    if (e.defaultPrevented || e.button !== 0 || e.ctrlKey || e.metaKey || e.shiftKey || e.altKey) { return; }
    var a = e.target && e.target.closest ? e.target.closest('a') : null;
    if (!a) { return; }
    if (a.hasAttribute('download') || (a.target && a.target !== '_self')) { return; }
    var href = a.getAttribute('href') || '';
    if (href.charAt(0) !== '#') { return; }
    var id = href.slice(1);
    if (!id) { return; }
    var section = document.getElementById(id);
    if (!section || !section.classList || !section.classList.contains('manual-section')) { return; }
    if (window.location.hash === href) {
      animateSection(section);
      showSectionPopup(section);
    }
    scheduleUpdate();
  });

  window.addEventListener('hashchange', syncHashTarget);
  window.addEventListener('popstate', syncHashTarget);
  if (window.location.hash) {
    window.requestAnimationFrame(syncHashTarget);
  }

  var revealAll = function () {
    for (var i = 0; i < sections.length; i++) {
      sections[i].classList.add('visible');
    }
  };

  if (!reducedMotion && 'IntersectionObserver' in window) {
    var revealed = 0;
    var revealObserver = new IntersectionObserver(function (entries) {
      entries.forEach(function (entry) {
        if (entry.isIntersecting) {
          entry.target.classList.add('visible');
          revealed++;
          revealObserver.unobserve(entry.target);
        }
      });
    }, { rootMargin: '0px 0px -8% 0px', threshold: 0.05 });

    sections.forEach(function (section) {
      revealObserver.observe(section);
    });

    window.setTimeout(function () {
      if (revealed === 0) {
        revealAll();
      }
    }, 1600);
  } else {
    revealAll();
  }
})();