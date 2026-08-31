// SAO Auto 用户指南 — 局部高分辨率真实水波（无 shader）。
//
// 每个波域为 240px、240×240 采样。点击产生单个向外传播的波包：
// 中心只下陷并回弹一次，随后波峰/波谷离开中心；主波半径最大约 45px，
// 含自然尾部也只占鼠标周围约 75px。相近点击进入同一个波域，所有直达波与
// 边界镜像反射波使用同一个高度公式求和，所以会真实增强/相消。
//
// 高度公式：h=Σ A·sin(πq)e^(−q²)·(1−e^(−t/τᵣ))·e^(−t/τᵥ)/√(1+r/r₀)。
// 屏幕边界使用镜像源法加入反射项。R/G 位移取 ∂h/∂x、∂h/∂y；阴影取
// 同一梯度形成的法线与左上光源点积。原页面从不挂滤镜，避免跳动和裁剪。
//
// 每个活动域每帧约 58K 像素；数组与 ImageData 预分配，仅在有波时运行
// requestAnimationFrame；波尾靠黏性和几何扩散自然降到阈值后回收。
(function () {
  'use strict';

  var SVG_NS = 'http://www.w3.org/2000/svg';
  var XLINK_NS = 'http://www.w3.org/1999/xlink';
  var DOMAIN_SIZE = 240;
  var MAP_SIZE = 240;
  var MAX_LIFE_MS = 5000;
  var MIN_PHYSICAL_AMPLITUDE = 0.0025;
  var SPEED_PX_PER_MS = 0.009;
  var PULSE_WIDTH_PX = 6.5;
  var IMPACT_RISE_MS = 80;
  var VISCOSITY_MS = 720;
  var SPREADING_RADIUS_PX = 20;
  var MAX_WAVE_RADIUS = MAX_LIFE_MS * SPEED_PX_PER_MS;
  var MAX_SIGMA = PULSE_WIDTH_PX * (1 + MAX_LIFE_MS * 0.00018);
  var MAX_VISIBLE_RADIUS = MAX_WAVE_RADIUS + MAX_SIGMA * 3.2;
  var MERGE_SPAN = 56;
  var MAX_SOURCES = 6;
  var MAX_DOMAINS = 2;
  var REFLECTION = 0.52;
  var DISPLACEMENT_SCALE = 22;

  if (window.matchMedia('(prefers-reduced-motion: reduce)').matches) { return; }

  var defs = document.getElementById('sao-wave-runtime-defs');
  var supportsBackdropSvg =
    typeof CSS !== 'undefined' && CSS.supports &&
    CSS.supports('backdrop-filter', 'url(#sao-wave-probe)');
  if (!defs || !supportsBackdropSvg) { return; }

  var domains = [];
  var sequence = 0;
  var running = false;
  var frameRequest = 0;
  var frameCount = 0;
  var fpsWindowStart = 0;
  var measuredFps = 0;
  var mapFrames = 0;
  var diagnosticNow = 0;

  function clamp(value, low, high) {
    return Math.max(low, Math.min(high, value));
  }

  function makeSvgElement(name) {
    return document.createElementNS(SVG_NS, name);
  }

  function createDomain(x, y) {
    if (domains.length >= MAX_DOMAINS) { return null; }
    var id = 'sao-water-domain-' + (++sequence);
    var surface = document.createElement('div');
    surface.className = 'sao-water-domain';
    surface.setAttribute('aria-hidden', 'true');
    var lighting = document.createElement('canvas');
    lighting.className = 'sao-water-domain-lighting';
    lighting.width = MAP_SIZE;
    lighting.height = MAP_SIZE;
    surface.appendChild(lighting);

    var filter = makeSvgElement('filter');
    filter.setAttribute('id', id);
    filter.setAttribute('filterUnits', 'userSpaceOnUse');
    filter.setAttribute('primitiveUnits', 'userSpaceOnUse');
    filter.setAttribute('x', '0');
    filter.setAttribute('y', '0');
    filter.setAttribute('width', String(DOMAIN_SIZE));
    filter.setAttribute('height', String(DOMAIN_SIZE));
    filter.setAttribute('color-interpolation-filters', 'sRGB');

    var mapImage = makeSvgElement('feImage');
    mapImage.setAttribute('x', '0');
    mapImage.setAttribute('y', '0');
    mapImage.setAttribute('width', String(DOMAIN_SIZE));
    mapImage.setAttribute('height', String(DOMAIN_SIZE));
    mapImage.setAttribute('preserveAspectRatio', 'none');
    mapImage.setAttribute('result', 'water-map');

    var displacement = makeSvgElement('feDisplacementMap');
    displacement.setAttribute('in', 'SourceGraphic');
    displacement.setAttribute('in2', 'water-map');
    displacement.setAttribute('scale', String(DISPLACEMENT_SCALE));
    displacement.setAttribute('xChannelSelector', 'R');
    displacement.setAttribute('yChannelSelector', 'G');
    filter.appendChild(mapImage);
    filter.appendChild(displacement);
    defs.appendChild(filter);

    surface.style.backdropFilter = 'url(#' + id + ')';
    surface.style.webkitBackdropFilter = 'url(#' + id + ')';
    document.body.appendChild(surface);

    var mapCanvas = document.createElement('canvas');
    mapCanvas.width = MAP_SIZE;
    mapCanvas.height = MAP_SIZE;
    var mapCtx = mapCanvas.getContext('2d');
    var lightCtx = lighting.getContext('2d');

    var domain = {
      id: id,
      centerX: x,
      centerY: y,
      surface: surface,
      lighting: lighting,
      filter: filter,
      mapImage: mapImage,
      mapCanvas: mapCanvas,
      mapCtx: mapCtx,
      lightCtx: lightCtx,
      mapPixels: mapCtx.createImageData(MAP_SIZE, MAP_SIZE),
      lightPixels: lightCtx.createImageData(MAP_SIZE, MAP_SIZE),
      heights: new Float32Array(MAP_SIZE * MAP_SIZE),
      sources: [],
      energy: 0,
      retiring: false
    };
    positionDomain(domain);
    domains.push(domain);
    return domain;
  }

  function positionDomain(domain) {
    domain.surface.style.left = (domain.centerX - DOMAIN_SIZE * 0.5) + 'px';
    domain.surface.style.top = (domain.centerY - DOMAIN_SIZE * 0.5) + 'px';
  }

  function destroyDomain(domain) {
    var index = domains.indexOf(domain);
    if (index >= 0) { domains.splice(index, 1); }
    if (domain.surface.parentNode) { domain.surface.parentNode.removeChild(domain.surface); }
    if (domain.filter.parentNode) { domain.filter.parentNode.removeChild(domain.filter); }
  }

  function totalSourceCount() {
    var count = 0;
    for (var i = 0; i < domains.length; i++) { count += domains[i].sources.length; }
    return count;
  }

  function canMerge(domain, x, y) {
    var minX = x;
    var maxX = x;
    var minY = y;
    var maxY = y;
    for (var i = 0; i < domain.sources.length; i++) {
      minX = Math.min(minX, domain.sources[i].x);
      maxX = Math.max(maxX, domain.sources[i].x);
      minY = Math.min(minY, domain.sources[i].y);
      maxY = Math.max(maxY, domain.sources[i].y);
    }
    return maxX - minX <= MERGE_SPAN && maxY - minY <= MERGE_SPAN;
  }

  function recenterDomain(domain) {
    var minX = Infinity;
    var maxX = -Infinity;
    var minY = Infinity;
    var maxY = -Infinity;
    for (var i = 0; i < domain.sources.length; i++) {
      minX = Math.min(minX, domain.sources[i].x);
      maxX = Math.max(maxX, domain.sources[i].x);
      minY = Math.min(minY, domain.sources[i].y);
      maxY = Math.max(maxY, domain.sources[i].y);
    }
    domain.centerX = (minX + maxX) * 0.5;
    domain.centerY = (minY + maxY) * 0.5;
    positionDomain(domain);
  }

  function reflectedImages(x, y) {
    var width = Math.max(1, window.innerWidth);
    var height = Math.max(1, window.innerHeight);
    var range = MAX_VISIBLE_RADIUS;
    var images = [{ x: x, y: y, coefficient: 1 }];
    var nearLeft = x < range;
    var nearRight = width - x < range;
    var nearTop = y < range;
    var nearBottom = height - y < range;

    if (nearLeft) { images.push({ x: -x, y: y, coefficient: REFLECTION }); }
    if (nearRight) { images.push({ x: width * 2 - x, y: y, coefficient: REFLECTION }); }
    if (nearTop) { images.push({ x: x, y: -y, coefficient: REFLECTION }); }
    if (nearBottom) { images.push({ x: x, y: height * 2 - y, coefficient: REFLECTION }); }
    if (nearLeft && nearTop) {
      images.push({ x: -x, y: -y, coefficient: REFLECTION * REFLECTION });
    }
    if (nearLeft && nearBottom) {
      images.push({ x: -x, y: height * 2 - y, coefficient: REFLECTION * REFLECTION });
    }
    if (nearRight && nearTop) {
      images.push({ x: width * 2 - x, y: -y, coefficient: REFLECTION * REFLECTION });
    }
    if (nearRight && nearBottom) {
      images.push({ x: width * 2 - x, y: height * 2 - y, coefficient: REFLECTION * REFLECTION });
    }
    return images;
  }

  function inject(clientX, clientY) {
    if (totalSourceCount() >= MAX_SOURCES) { return; }
    var now = performance.now();
    var domain = null;
    for (var i = domains.length - 1; i >= 0; i--) {
      if (!domains[i].retiring && canMerge(domains[i], clientX, clientY)) {
        domain = domains[i];
        break;
      }
    }
    if (!domain) { domain = createDomain(clientX, clientY); }
    if (!domain) { return; }

    domain.sources.push({
      x: clientX,
      y: clientY,
      start: now,
      seed: sequence * 1.713 + domain.sources.length * 0.847,
      images: reflectedImages(clientX, clientY)
    });
    recenterDomain(domain);
    diagnosticNow = now;
    if (!running) { start(now); }
  }

  function evaluateDomain(domain, now) {
    var alive = [];
    domain.heights.fill(0);
    var scale = DOMAIN_SIZE / MAP_SIZE;

    for (var s = 0; s < domain.sources.length; s++) {
      var source = domain.sources[s];
      var age = now - source.start;
      if (age < 0 || age >= MAX_LIFE_MS) { continue; }
      var front = age * SPEED_PX_PER_MS;
      var sigma = PULSE_WIDTH_PX * (1 + age * 0.00018);
      var impactRise = 1 - Math.exp(-age / IMPACT_RISE_MS);
      var viscous = Math.exp(-age / VISCOSITY_MS);
      var dispersion = Math.sqrt(PULSE_WIDTH_PX / sigma);
      var baseAttenuation = impactRise * viscous * dispersion;
      var retirementAmplitude =
        baseAttenuation / Math.sqrt(1 + front / SPREADING_RADIUS_PX);
      if (age > IMPACT_RISE_MS * 2 &&
          retirementAmplitude < MIN_PHYSICAL_AMPLITUDE) {
        continue;
      }
      alive.push(source);
      var support = front + sigma * 3.2;

      for (var im = 0; im < source.images.length; im++) {
        var image = source.images[im];
        var localImageX = image.x - (domain.centerX - DOMAIN_SIZE * 0.5);
        var localImageY = image.y - (domain.centerY - DOMAIN_SIZE * 0.5);
        var minX = clamp(Math.floor((localImageX - support) / scale), 0, MAP_SIZE - 1);
        var maxX = clamp(Math.ceil((localImageX + support) / scale), 0, MAP_SIZE - 1);
        var minY = clamp(Math.floor((localImageY - support) / scale), 0, MAP_SIZE - 1);
        var maxY = clamp(Math.ceil((localImageY + support) / scale), 0, MAP_SIZE - 1);

        for (var gy = minY; gy <= maxY; gy++) {
          var worldY = domain.centerY - DOMAIN_SIZE * 0.5 + (gy + 0.5) * scale;
          var row = gy * MAP_SIZE;
          for (var gx = minX; gx <= maxX; gx++) {
            var worldX = domain.centerX - DOMAIN_SIZE * 0.5 + (gx + 0.5) * scale;
            var dx = worldX - image.x;
            var dy = worldY - image.y;
            var radius = Math.sqrt(dx * dx + dy * dy);
            var theta = Math.atan2(dy, dx);
            // 仅 0.8% 的低阶扰动：自然水面不是机械圆，也不会形成乱边。
            var organic = 1 +
              0.005 * Math.sin(theta * 3 + source.seed) +
              0.003 * Math.sin(theta * 5 - source.seed * 0.6);
            var q = (radius * organic - front) / sigma;
            if (Math.abs(q) > 3.2) { continue; }
            var q2 = q * q;
            // 单个高斯调制正弦包：t=0 时为零，冲击建立后产生一次凹陷、
            // 一次弱回弹并离开中心，不会周期性重复。
            var geometric = 1 / Math.sqrt(1 + radius / SPREADING_RADIUS_PX);
            var packet =
              Math.sin(Math.PI * q) * Math.exp(-q2) *
              baseAttenuation * geometric;
            domain.heights[row + gx] += packet * image.coefficient;
          }
        }
      }
    }
    domain.sources = alive;
    return alive.length;
  }

  function renderDomain(domain) {
    var dp = domain.mapPixels.data;
    var lp = domain.lightPixels.data;
    var heights = domain.heights;
    var energy = 0;

    for (var y = 0; y < MAP_SIZE; y++) {
      var row = y * MAP_SIZE;
      for (var x = 0; x < MAP_SIZE; x++) {
        var index = row + x;
        var output = index * 4;
        var left = heights[row + Math.max(0, x - 1)];
        var right = heights[row + Math.min(MAP_SIZE - 1, x + 1)];
        var up = heights[Math.max(0, y - 1) * MAP_SIZE + x];
        var down = heights[Math.min(MAP_SIZE - 1, y + 1) * MAP_SIZE + x];
        var gx = (right - left) * 0.5;
        var gy = (down - up) * 0.5;

        dp[output] = clamp(Math.round(128 + gx * 3.1 * 127), 0, 255);
        dp[output + 1] = clamp(Math.round(128 + gy * 3.1 * 127), 0, 255);
        dp[output + 2] = 128;
        dp[output + 3] = 255;

        var slopeX = -gx * 3.2;
        var slopeY = -gy * 3.2;
        var normalLength = Math.sqrt(slopeX * slopeX + slopeY * slopeY + 1);
        var light = (slopeX * -0.46 + slopeY * -0.56 + 0.68) / normalLength;
        var delta = clamp((light - 0.68) * 2.2, -1, 1);
        if (delta >= 0) {
          lp[output] = 196;
          lp[output + 1] = 244;
          lp[output + 2] = 255;
          lp[output + 3] = Math.round(delta * 118);
        } else {
          lp[output] = 0;
          lp[output + 1] = 24;
          lp[output + 2] = 38;
          lp[output + 3] = Math.round(-delta * 102);
        }
        if ((x & 3) === 0 && (y & 3) === 0) { energy += Math.abs(heights[index]); }
      }
    }

    domain.mapCtx.putImageData(domain.mapPixels, 0, 0);
    domain.lightCtx.putImageData(domain.lightPixels, 0, 0);
    var uri = domain.mapCanvas.toDataURL('image/png');
    domain.mapImage.setAttribute('href', uri);
    domain.mapImage.setAttributeNS(XLINK_NS, 'xlink:href', uri);
    domain.energy = energy;
    mapFrames++;
  }

  function update(now) {
    for (var i = domains.length - 1; i >= 0; i--) {
      var domain = domains[i];
      if (domain.retiring) {
        destroyDomain(domain);
        continue;
      }
      var alive = evaluateDomain(domain, now);
      renderDomain(domain);
      if (alive === 0) {
        // 保留至下一次 rAF：本回调返回后，浏览器能真正呈现中性位移帧。
        domain.retiring = true;
      }
    }
    return domains.length;
  }

  function start(now) {
    running = true;
    frameCount = 0;
    fpsWindowStart = now;
    frameRequest = requestAnimationFrame(frame);
  }

  function stop() {
    running = false;
    if (frameRequest) { cancelAnimationFrame(frameRequest); }
    frameRequest = 0;
    while (domains.length) { destroyDomain(domains[0]); }
  }

  function frame(now) {
    if (!running) { return; }
    if (update(now) === 0) {
      running = false;
      frameRequest = 0;
      return;
    }
    frameCount++;
    var elapsed = now - fpsWindowStart;
    if (elapsed >= 500) {
      measuredFps = frameCount * 1000 / elapsed;
      frameCount = 0;
      fpsWindowStart = now;
    }
    frameRequest = requestAnimationFrame(frame);
  }

  window.saoRipple = inject;
  window.__saoRippleStats = function () {
    var sourceCount = 0;
    var imageCount = 0;
    var retiringCount = 0;
    var energy = 0;
    for (var i = 0; i < domains.length; i++) {
      if (domains[i].retiring) { retiringCount++; }
      sourceCount += domains[i].sources.length;
      for (var s = 0; s < domains[i].sources.length; s++) {
        imageCount += domains[i].sources[s].images.length;
      }
      energy += domains[i].energy;
    }
    return {
      running: running,
      domains: domains.length,
      activeImpulses: sourceCount,
      waveTerms: imageCount,
      retiringDomains: retiringCount,
      mapResolution: MAP_SIZE + 'x' + MAP_SIZE,
      fps: Math.round(measuredFps * 10) / 10,
      energy: Math.round(energy * 10000) / 10000,
      mapFrames: mapFrames,
      maxRadiusPx: Math.round(MAX_WAVE_RADIUS),
      visibleRadiusPx: Math.round(MAX_VISIBLE_RADIUS)
    };
  };
  window.__saoRippleAdvance = function (steps) {
    var count = clamp(Math.floor(steps || 1), 1, 180);
    if (diagnosticNow === 0) { diagnosticNow = performance.now(); }
    for (var i = 0; i < count && domains.length; i++) {
      diagnosticNow += 1000 / 60;
      update(diagnosticNow);
      var neutralPending = false;
      for (var d = 0; d < domains.length; d++) {
        if (domains[d].retiring) { neutralPending = true; break; }
      }
      if (neutralPending) { break; }
    }
    if (domains.length === 0) { running = false; }
    return window.__saoRippleStats();
  };

  window.addEventListener('resize', function () { if (running) { stop(); } });
  window.addEventListener('scroll', function () { if (running) { stop(); } }, { passive: true });
  document.addEventListener('visibilitychange', function () {
    if (document.hidden && running) { stop(); }
  });
  window.addEventListener('pagehide', stop);
})();
