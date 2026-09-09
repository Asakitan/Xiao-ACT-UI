// SAO Auto 用户指南 — SAO 式全息粒子层 + 光标辉光。
// 粒子只在可见页面运行；隐藏、BFCache 与 reduced-motion 会停用装饰层。
(function () {
  'use strict';

  var root = document.documentElement;
  var motionQuery = window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)');
  var contrastQuery = window.matchMedia && window.matchMedia('(forced-colors: active)');

  function readPalette() {
    var cs = getComputedStyle(root);
    var strip = function (v) { return (v || '#0ea5c4').trim(); };
    return [
      strip(cs.getPropertyValue('--accent')),
      strip(cs.getPropertyValue('--violet')),
      strip(cs.getPropertyValue('--gold')),
      strip(cs.getPropertyValue('--accent-strong'))
    ];
  }

  var canvas = document.createElement('canvas');
  canvas.className = 'sao-particles';
  canvas.setAttribute('aria-hidden', 'true');
  document.body.appendChild(canvas);

  var ctx = canvas.getContext('2d');
  if (!ctx) { if (canvas.parentNode) { canvas.parentNode.removeChild(canvas); } return; }
  var dpr = Math.min(window.devicePixelRatio || 1, 2);
  var width = 0, height = 0;
  var palette = readPalette();
  var particles = [];
  var particleCount = 0;
  var mouse = { x: -9999, y: -9999 };

  function sizeCanvas() {
    width = window.innerWidth;
    height = window.innerHeight;
    if (width <= 0 || height <= 0) { width = 0; height = 0; }
    canvas.width = Math.floor(width * dpr);
    canvas.height = Math.floor(height * dpr);
    canvas.style.width = width + 'px';
    canvas.style.height = height + 'px';
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  }
  sizeCanvas();

  particleCount = Math.min(70, Math.max(30, Math.floor((width * height) / 26000)));

  function spawn(initial) {
    return {
      x: Math.random() * width,
      y: initial ? Math.random() * height : height + 8,
      size: 0.7 + Math.random() * 1.9,
      speedY: 0.10 + Math.random() * 0.28,
      drift: (Math.random() - 0.5) * 0.22,
      phase: Math.random() * Math.PI * 2,
      color: palette[(Math.random() * palette.length) | 0],
      alpha: 0.10 + Math.random() * 0.42,
      twinkle: 0.4 + Math.random() * 1.6
    };
  }

  var i;
  for (i = 0; i < particleCount; i++) { particles.push(spawn(true)); }

  var paletteObserver = new MutationObserver(function () {
    palette = readPalette();
  });
  paletteObserver.observe(root, { attributes: true, attributeFilter: ['class'] });

  var tick = 0;
  var last = Date.now();
  var scheduleId = 0;
  var running = false;
  var suspended = document.hidden || root.classList.contains('guide-hidden');

  function canRun() {
    return !suspended && !document.hidden && !root.classList.contains('guide-hidden') &&
      !(motionQuery && motionQuery.matches) && !(contrastQuery && contrastQuery.matches);
  }

  function frame() {
    if (width === 0) { sizeCanvas(); }
    var now = Date.now();
    tick += (now - last) * 0.001;
    last = now;
    if (width === 0) { return; }
    ctx.clearRect(0, 0, width, height);

    for (var j = 0; j < particles.length; j++) {
      var p = particles[j];
      p.y -= p.speedY;
      p.x += p.drift + Math.sin(tick * p.twinkle + p.phase) * 0.12;

      var dx = p.x - mouse.x;
      var dy = p.y - mouse.y;
      var dist2 = dx * dx + dy * dy;
      if (dist2 < 110 * 110) {
        var dist = Math.sqrt(dist2) || 1;
        var push = (110 - dist) / 110 * 0.5;
        p.x += (dx / dist) * push;
        p.y += (dy / dist) * push;
      }

      if (p.y < -10 || p.x < -20 || p.x > width + 20) {
        particles[j] = spawn(false);
        continue;
      }

      var tw = 0.55 + 0.45 * Math.sin(tick * p.twinkle + p.phase);
      ctx.globalAlpha = p.alpha * tw;
      ctx.fillStyle = p.color;
      ctx.beginPath();
      ctx.arc(p.x, p.y, p.size, 0, Math.PI * 2);
      ctx.fill();

      if (p.size > 1.6) {
        ctx.globalAlpha = p.alpha * tw * 0.22;
        ctx.beginPath();
        ctx.arc(p.x, p.y, p.size * 3.2, 0, Math.PI * 2);
        ctx.fill();
      }
    }
    ctx.globalAlpha = 1;
  }

  function schedule() {
    if (!running || !canRun() || scheduleId) { return; }
    scheduleId = window.setTimeout(function () {
      scheduleId = 0;
      if (!running || !canRun()) { return; }
      frame();
      schedule();
    }, 33);
  }

  function start() {
    if (running || !canRun()) { return; }
    running = true;
    last = Date.now();
    frame();
    schedule();
  }

  function pause() {
    running = false;
    if (scheduleId) { window.clearTimeout(scheduleId); }
    scheduleId = 0;
    last = Date.now();
    window.clearTimeout(dimTimer);
    dimTimer = 0;
    if (glow) { glow.classList.remove('on'); }
  }

  function syncSuspension() {
    var shouldPause = document.hidden || root.classList.contains('guide-hidden') ||
      (motionQuery && motionQuery.matches) || (contrastQuery && contrastQuery.matches);
    suspended = shouldPause;
    if (shouldPause) { pause(); } else { start(); }
  }

  var glow = document.createElement('div');
  glow.className = 'cursor-glow';
  glow.setAttribute('aria-hidden', 'true');
  document.body.appendChild(glow);

  var dimTimer = 0;
  document.addEventListener('mousemove', function (e) {
    if (!canRun()) { return; }
    mouse.x = e.clientX;
    mouse.y = e.clientY;
    glow.style.transform = 'translate(' + (e.clientX - 260) + 'px,' + (e.clientY - 260) + 'px)';
    glow.classList.add('on');
    window.clearTimeout(dimTimer);
    dimTimer = window.setTimeout(function () {
      glow.classList.remove('on');
    }, 2600);
  }, { passive: true });

  new MutationObserver(syncSuspension).observe(root, { attributes: true, attributeFilter: ['class'] });
  document.addEventListener('visibilitychange', syncSuspension);
  [motionQuery, contrastQuery].forEach(function (query) {
    if (query && query.addEventListener) { query.addEventListener('change', syncSuspension); }
  });
  window.addEventListener('pagehide', function () {
    suspended = true;
    pause();
    window.clearTimeout(dimTimer);
    dimTimer = 0;
    glow.classList.remove('on');
  });
  window.addEventListener('pageshow', function () {
    syncSuspension();
  });
  window.addEventListener('resize', sizeCanvas);

  syncSuspension();
})();