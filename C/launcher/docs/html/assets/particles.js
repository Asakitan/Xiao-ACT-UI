// SAO Auto 用户指南 — SAO 式全息粒子层 + 光标辉光。
// 全部只读画布动画：粒子缓慢上浮 + 轻微漂移，鼠标附近带轻微扰动；
// 颜色跟随主题（html.dark），prefers-reduced-motion 下完全关闭。
(function () {
  'use strict';

  var reducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  if (reducedMotion) { return; }

  function readPalette() {
    var cs = getComputedStyle(document.documentElement);
    var strip = function (v) {
      return (v || '#0ea5c4').trim();
    };
    return [
      strip(cs.getPropertyValue('--accent')),
      strip(cs.getPropertyValue('--violet')),
      strip(cs.getPropertyValue('--gold')),
      strip(cs.getPropertyValue('--accent-strong'))
    ];
  }

  /* ── 粒子层 ── */
  var canvas = document.createElement('canvas');
  canvas.className = 'sao-particles';
  canvas.setAttribute('aria-hidden', 'true');
  document.body.appendChild(canvas);

  var ctx = canvas.getContext('2d');
  var dpr = Math.min(window.devicePixelRatio || 1, 2);
  var width = 0, height = 0;
  var palette = readPalette();
  var particles = [];
  var particleCount = 0;
  var mouse = { x: -9999, y: -9999 };

  function sizeCanvas() {
    width = window.innerWidth;
    height = window.innerHeight;
    if (width <= 0 || height <= 0) { width = 0; height = 0; } // 嵌入环境首帧可能为 0
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
  paletteObserver.observe(document.documentElement, { attributes: true, attributeFilter: ['class'] });

  var tick = 0;
  var last = Date.now();

  function frame() {
    if (width === 0) { sizeCanvas(); } // 自救：尺寸曾被 0 挡住
    var now = Date.now();
    tick += (now - last) * 0.001;
    last = now;
    if (width === 0) { return; }
    ctx.clearRect(0, 0, width, height);

    for (var j = 0; j < particles.length; j++) {
      var p = particles[j];
      p.y -= p.speedY;
      p.x += p.drift + Math.sin(tick * p.twinkle + p.phase) * 0.12;

      // 鼠标扰动：近处粒子轻轻让开
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

      // 大粒子带一点光晕
      if (p.size > 1.6) {
        ctx.globalAlpha = p.alpha * tw * 0.22;
        ctx.beginPath();
        ctx.arc(p.x, p.y, p.size * 3.2, 0, Math.PI * 2);
        ctx.fill();
      }
    }
    ctx.globalAlpha = 1;
  }
  // 用 setInterval 驱动（部分 WebView 不派发 rAF 回调）。
  window.setInterval(frame, 33);
  frame();

  /* ── 光标辉光 ── */
  var glow = document.createElement('div');
  glow.className = 'cursor-glow';
  glow.setAttribute('aria-hidden', 'true');
  document.body.appendChild(glow);

  var dimTimer = 0;
  document.addEventListener('mousemove', function (e) {
    mouse.x = e.clientX;
    mouse.y = e.clientY;
    glow.style.transform = 'translate(' + (e.clientX - 260) + 'px,' + (e.clientY - 260) + 'px)';
    glow.classList.add('on');
    window.clearTimeout(dimTimer);
    dimTimer = window.setTimeout(function () {
      glow.classList.remove('on');
    }, 2600);
  }, { passive: true });

  window.addEventListener('resize', sizeCanvas);
})();