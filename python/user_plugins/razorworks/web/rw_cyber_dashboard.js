// Razorworks Cyber Menu — dashboard client.
// Renders from a schema (GROUPS/CONTROLS) driven purely by state pushed
// from the plugin process; every control writes back via send_action().

(function () {
  "use strict";

  var GROUPS = [
    { key: "overview", label: "总览", icon: "◎" },
    { key: "aim", label: "瞄准", icon: "◈", color: "gold" },
    { key: "rcs", label: "压枪", icon: "◫", color: "cyan" },
    { key: "trigger", label: "扳机", icon: "⚡", color: "warn" },
    { key: "esp", label: "视觉", icon: "◉", color: "cyan" },
    { key: "timeshift", label: "时移", icon: "◔", color: "purple" },
    { key: "proximity", label: "近战", icon: "✦", color: "gold" },
    { key: "nade", label: "投掷", icon: "●", color: "cyan" },
    { key: "radar", label: "雷达", icon: "◎", color: "blue" },
    { key: "cloak", label: "隐匿", icon: "◆", color: "bad" },
    { key: "system", label: "系统", icon: "⚙", color: "blue" },
  ];

  var BIND_PRESETS = {
    tk_pri: ["head", "head,chest", "head,neck,chest", "chest", "chest,stomach"],
    rt_pri: ["head", "head,chest", "head,neck,chest", "chest", "chest,stomach"],
    tk_sec: ["neck,chest", "neck,stomach", "chest,stomach,pelvis", "stomach", "neck"],
    rt_sec: ["neck,chest", "neck,stomach", "chest,stomach,pelvis", "stomach", "neck"],
    tk_vis: ["both", "spotted", "multipoint", "off"],
    rt_vis: ["both", "spotted", "multipoint", "off"],
    tk_strat: ["crosshair", "distance", "health"],
    tk_curve: ["ease_out", "linear", "ease_in", "overshoot"],
    vis_ec: [3, 5, 9],
  };

  var CONTROLS = {
    aim: [
      { type: "toggle", key: "tk_on", label: "启用瞄准" },
      { type: "slider", key: "tk_fov", label: "FOV", min: 0.5, max: 30, step: 0.5 },
      { type: "slider", key: "tk_smooth", label: "平滑", min: 0.01, max: 1, step: 0.01, digits: 2 },
      { type: "slider", key: "tk_spd", label: "速度", min: 0.1, max: 3, step: 0.1, digits: 1 },
      { type: "slider", key: "tk_jit", label: "抖动", min: 0, max: 0.5, step: 0.01, digits: 2 },
      { type: "slider", key: "tk_off", label: "偏移", min: 0, max: 2, step: 0.05, digits: 2 },
      { type: "slider", key: "tk_react", label: "反应 ms", min: 0, max: 500, step: 10, digits: 0 },
      { type: "slider", key: "tk_hr", label: "命中率", min: 0, max: 1, step: 0.01, digits: 2 },
      { type: "slider", key: "tk_over", label: "过冲", min: 0, max: 0.5, step: 0.01, digits: 2 },
      { type: "cycle", key: "tk_curve", label: "曲线" },
      { type: "slider", key: "tk_missj", label: "脱靶抖动", min: 0, max: 1, step: 0.01, digits: 2 },
      { type: "slider", key: "tk_maxd", label: "最大距离(0=无限)", min: 0, max: 4000, step: 50, digits: 0 },
      { type: "cycle", key: "tk_pri", label: "优先骨骼" },
      { type: "cycle", key: "tk_sec", label: "次要骨骼" },
      { type: "cycle", key: "tk_vis", label: "可见性" },
      { type: "cycle", key: "tk_strat", label: "目标策略" },
      { type: "bind", key: "tk_bind", modeKey: "tk_bind_mode", label: "绑定键" },
    ],
    rcs: [
      { type: "toggle", key: "cp_on", label: "启用压枪" },
      { type: "slider", key: "cp_str", label: "纵向强度", min: 0, max: 1.5, step: 0.05, digits: 2 },
      { type: "slider", key: "cp_hstr", label: "横向强度", min: 0, max: 1.5, step: 0.05, digits: 2 },
      { type: "slider", key: "cp_alpha", label: "EMA α", min: 0.05, max: 1, step: 0.05, digits: 2 },
      { type: "stepper", key: "cp_start", label: "起始弹", min: 1, max: 99, step: 1 },
      { type: "stepper", key: "cp_max", label: "最大弹(0=不限)", min: 0, max: 99, step: 1 },
      { type: "stepper", key: "cp_ramp", label: "渐入弹数", min: 0, max: 30, step: 1 },
      { type: "slider", key: "cp_hr", label: "命中率", min: 0, max: 1, step: 0.01, digits: 2 },
      { type: "slider", key: "cp_j", label: "抖动", min: 0, max: 1, step: 0.01, digits: 2 },
    ],
    trigger: [
      { type: "toggle", key: "rt_on", label: "启用扳机" },
      { type: "toggle", key: "rt_seed", label: "Hitchance 模式" },
      { type: "slider", key: "rt_dmin", label: "延迟 min ms", min: 0, max: 500, step: 10, digits: 0 },
      { type: "slider", key: "rt_dmax", label: "延迟 max ms", min: 50, max: 800, step: 10, digits: 0 },
      { type: "slider", key: "rt_cd", label: "冷却 ms", min: 0, max: 300, step: 10, digits: 0 },
      { type: "slider", key: "rt_hr", label: "命中率", min: 0, max: 1, step: 0.01, digits: 2 },
      { type: "slider", key: "rt_chance", label: "Hitchance ≥", min: 0, max: 1, step: 0.05, digits: 2 },
      { type: "slider", key: "rt_spread", label: "散布锥", min: 0, max: 3, step: 0.1, digits: 1 },
      { type: "stepper", key: "rt_bmax", label: "连发(0=不限)", min: 0, max: 30, step: 1 },
      { type: "slider", key: "rt_bcd", label: "连发冷却 ms", min: 0, max: 3000, step: 50, digits: 0 },
      { type: "cycle", key: "rt_pri", label: "优先骨骼" },
      { type: "cycle", key: "rt_sec", label: "次要骨骼" },
      { type: "cycle", key: "rt_vis", label: "可见性" },
      { type: "bind", key: "rt_bind", modeKey: "rt_bind_mode", label: "绑定键" },
    ],
    esp: [
      { type: "toggle", key: "show_health", label: "血量数字" },
      { type: "toggle", key: "show_hp_bar", label: "血条" },
      { type: "toggle", key: "show_skeleton", label: "骨骼" },
      { type: "toggle", key: "show_distance", label: "距离" },
      { type: "toggle", key: "show_head", label: "头点" },
      { type: "toggle", key: "show_box", label: "框体" },
      { type: "toggle", key: "show_snapline", label: "拉线" },
      { type: "toggle", key: "show_fov_circle", label: "FOV 圈" },
    ],
    timeshift: [
      { type: "toggle", key: "bt_on", label: "延迟补偿" },
      { type: "slider", key: "bt_win", label: "窗口 ms", min: 0, max: 500, step: 10, digits: 0 },
      { type: "toggle", key: "st_on", label: "自跟踪" },
      { type: "toggle", key: "ip_on", label: "插值" },
      { type: "slider", key: "ip_delay", label: "渲染延迟 ms", min: 0, max: 200, step: 5, digits: 0 },
      { type: "toggle", key: "xp_on", label: "外推" },
      { type: "slider", key: "xp_ahead", label: "外推提前 ms", min: 0, max: 200, step: 5, digits: 0 },
      { type: "slider", key: "xp_conf", label: "最小置信度", min: 0, max: 1, step: 0.05, digits: 2 },
    ],
    proximity: [
      { type: "toggle", key: "px_k", label: "近战 (刀)" },
      { type: "bind", key: "px_kb", modeKey: "px_kb_mode", label: "刀 绑定键" },
      { type: "toggle", key: "px_z", label: "宙斯电击枪" },
      { type: "bind", key: "px_zb", modeKey: "px_zb_mode", label: "宙斯 绑定键" },
    ],
    nade: [
      { type: "toggle", key: "gd_on", label: "投掷物引导" },
      { type: "toggle", key: "gd_auto", label: "自动模式" },
      { type: "slider", key: "gd_prox", label: "触发距离", min: 0, max: 500, step: 10, digits: 0 },
    ],
    radar: [
      { type: "toggle", key: "map_on", label: "雷达覆盖" },
      { type: "toggle", key: "map_override", label: "手动覆盖比例(默认自动读取游戏)" },
      { type: "slider", key: "map_hs", label: "HUD 比例", min: 0.1, max: 3, step: 0.05, digits: 2, disableUnless: "map_override" },
      { type: "slider", key: "map_rs", label: "缩放比例", min: 0.1, max: 3, step: 0.05, digits: 2, disableUnless: "map_override" },
    ],
    cloak: [
      { type: "toggle", key: "cloak_on", label: "内核级隐匿" },
    ],
    system: [
      { type: "slider", key: "sensitivity", label: "全局灵敏度", min: 0.1, max: 3, step: 0.05, digits: 2 },
      { type: "toggle", key: "vis_mp", label: "边缘扫描可见性" },
      { type: "cycle", key: "vis_ec", label: "边缘扫描点数" },
      { type: "toggle", key: "use_sig", label: "特征码扫描(否则用静态偏移)" },
    ],
  };

  var state = { running: false, overview: {}, groups: {}, capture_target: "" };
  var activeGroup = "overview";
  var dragThrottle = {};

  function api() {
    return window.pywebview && window.pywebview.api;
  }

  function sendAction(action) {
    var a = api();
    if (a && a.send_action) {
      try { a.send_action(action); } catch (e) { /* ignore */ }
    }
  }

  function fmt(v, digits) {
    if (typeof v !== "number") return String(v);
    return v.toFixed(digits == null ? 2 : digits);
  }

  function groupValue(group, key, dflt) {
    var g = state.groups[group] || {};
    var v = g[key];
    return v === undefined ? dflt : v;
  }

  // ── rail ──────────────────────────────────────────────────────────

  function renderRail() {
    var rail = document.getElementById("rw-rail");
    rail.innerHTML = "";
    GROUPS.forEach(function (g) {
      var btn = document.createElement("button");
      btn.className = "rw-rail-btn" + (g.key === activeGroup ? " active" : "");
      btn.textContent = g.icon;
      btn.title = g.label;
      btn.addEventListener("click", function () {
        activeGroup = g.key;
        renderRail();
        renderContent();
      });
      rail.appendChild(btn);
    });
  }

  // ── overview ──────────────────────────────────────────────────────

  function renderOverview(root) {
    var ov = state.overview || {};
    var acc = typeof ov.accuracy === "number" ? ov.accuracy : 0;

    var hero = document.createElement("div");
    hero.className = "rw-card rw-hero";
    hero.innerHTML =
      '<div class="rw-hero-ring-wrap">' +
        '<div class="rw-hero-ring" style="--pct:' + (acc * 100).toFixed(1) + '">' +
          '<div class="rw-hero-ring-label">' +
            '<div class="big">' + (acc * 100).toFixed(0) + '%</div>' +
            '<div class="small">命中率</div>' +
          '</div>' +
        '</div>' +
      '</div>';

    var stats = document.createElement("div");
    stats.className = "rw-hero-stats";
    stats.appendChild(statRow("命中率", "#6a8dff", (ov.history || {}).accuracy || [], 0, 1,
      ov.accuracy_display || (acc * 100).toFixed(0) + "%"));
    stats.appendChild(statRow("Tick", "#b06bff", (ov.history || {}).tick_ms || [], 0, 60,
      (ov.tick_ms == null ? "-" : ov.tick_ms) + "ms"));
    stats.appendChild(statRow("实体数", "#ff6bb0", (ov.history || {}).entity_count || [], 0, 12,
      String(ov.entity_count == null ? 0 : ov.entity_count)));
    hero.appendChild(stats);
    root.appendChild(hero);

    var info = document.createElement("div");
    info.className = "rw-card";
    info.innerHTML =
      '<div class="rw-card-title">运行状态</div>' +
      '<div class="rw-dotgrid">' +
        dotItem("状态", true, ov.status === "LIVE") +
        dotItem("输入模式: " + (ov.input_mode || "-"), true, !!ov.input_mode) +
      '</div>';
    root.appendChild(info);

    var grid = document.createElement("div");
    grid.className = "rw-card";
    grid.innerHTML = '<div class="rw-card-title">功能总览</div><div class="rw-dotgrid" id="rw-overview-dots"></div>';
    root.appendChild(grid);
    var dotgrid = grid.querySelector("#rw-overview-dots");
    GROUPS.filter(function (g) { return g.key !== "overview"; }).forEach(function (g) {
      var on = groupOnState(g.key);
      dotgrid.appendChild(dotElement(g.label, true, on));
    });
  }

  function groupOnState(groupKey) {
    var onKeyByGroup = {
      aim: "tk_on", rcs: "cp_on", trigger: "rt_on", esp: "show_box",
      timeshift: "bt_on", proximity: "px_k", nade: "gd_on",
      radar: "map_on", cloak: "cloak_on", system: "vis_mp",
    };
    var key = onKeyByGroup[groupKey];
    return key ? !!groupValue(groupKey, key, false) : false;
  }

  function dotItem(label, show, on) {
    return show ? dotElement(label, true, on).outerHTML : "";
  }

  function dotElement(label, show, on) {
    var el = document.createElement("div");
    el.className = "rw-dotgrid-item";
    el.innerHTML = '<span class="rw-dotgrid-dot' + (on ? " on" : "") + '" style="' +
      (on ? "background:#38c98f" : "") + '"></span><span>' + label + "</span>";
    return el;
  }

  function statRow(label, color, history, lo, hi, valueText) {
    var row = document.createElement("div");
    row.className = "rw-stat-row";
    var points = sparkPoints(history, lo, hi);
    row.innerHTML =
      '<span class="rw-stat-dot" style="background:' + color + '"></span>' +
      '<span class="rw-stat-label">' + label + '</span>' +
      '<svg class="rw-spark" viewBox="0 0 100 26" preserveAspectRatio="none">' +
        '<polyline points="' + points + '" style="stroke:' + color + '"></polyline>' +
      '</svg>' +
      '<span class="rw-stat-value">' + valueText + '</span>';
    return row;
  }

  function sparkPoints(history, lo, hi) {
    if (!history || !history.length) return "0,26 100,26";
    var n = history.length;
    var range = Math.max(1e-6, hi - lo);
    return history.map(function (v, i) {
      var x = (i / Math.max(1, n - 1)) * 100;
      var norm = Math.max(0, Math.min(1, (v - lo) / range));
      var y = 26 - norm * 24;
      return x.toFixed(1) + "," + y.toFixed(1);
    }).join(" ");
  }

  // ── group control cards ───────────────────────────────────────────

  function renderGroup(root, groupKey) {
    var meta = GROUPS.find(function (g) { return g.key === groupKey; });
    var controls = CONTROLS[groupKey] || [];
    var card = document.createElement("div");
    card.className = "rw-card";
    var on = groupOnState(groupKey);
    card.innerHTML =
      '<div class="rw-group-header">' +
        '<div class="rw-card-title">' + (meta ? meta.label : groupKey) + '</div>' +
        '<span class="rw-group-badge' + (on ? " on" : "") + '">' + (on ? "ON" : "OFF") + '</span>' +
      '</div>';
    controls.forEach(function (ctrl) {
      card.appendChild(renderControl(groupKey, ctrl));
    });
    root.appendChild(card);
  }

  function renderControl(groupKey, ctrl) {
    var wrap = document.createElement("div");
    wrap.className = "rw-control";
    var disabled = ctrl.disableUnless && !groupValue(groupKey, ctrl.disableUnless, false);
    if (disabled) wrap.style.opacity = "0.45";

    if (ctrl.type === "toggle") {
      var val = !!groupValue(groupKey, ctrl.key, false);
      wrap.innerHTML =
        '<div class="rw-control-row"><span class="rw-control-label">' + ctrl.label + '</span>' +
        '<button class="rw-toggle' + (val ? " on" : "") + '"></button></div>';
      wrap.querySelector(".rw-toggle").addEventListener("click", function () {
        sendAction({ kind: "toggle", key: ctrl.key });
      });
    } else if (ctrl.type === "slider") {
      var v = groupValue(groupKey, ctrl.key, ctrl.min);
      wrap.innerHTML =
        '<div class="rw-control-row"><span class="rw-control-label">' + ctrl.label + '</span>' +
        '<span class="rw-control-value">' + fmt(v, ctrl.digits) + '</span></div>' +
        '<input type="range" class="rw-slider" min="' + ctrl.min + '" max="' + ctrl.max +
        '" step="' + ctrl.step + '" value="' + v + '"' + (disabled ? " disabled" : "") + '>';
      var input = wrap.querySelector(".rw-slider");
      var valueEl = wrap.querySelector(".rw-control-value");
      input.addEventListener("input", function () {
        valueEl.textContent = fmt(parseFloat(input.value), ctrl.digits);
        throttledSet(ctrl.key, parseFloat(input.value));
      });
    } else if (ctrl.type === "stepper") {
      var iv = groupValue(groupKey, ctrl.key, ctrl.min);
      wrap.innerHTML =
        '<div class="rw-control-row"><span class="rw-control-label">' + ctrl.label + '</span>' +
        '<div class="rw-stepper">' +
          '<button class="rw-stepper-btn" data-dir="-1">−</button>' +
          '<span class="rw-control-value">' + iv + '</span>' +
          '<button class="rw-stepper-btn" data-dir="1">+</button>' +
        '</div></div>';
      wrap.querySelectorAll(".rw-stepper-btn").forEach(function (btn) {
        btn.addEventListener("click", function () {
          sendAction({ kind: "nudge", key: ctrl.key, dir: parseInt(btn.dataset.dir, 10) });
        });
      });
    } else if (ctrl.type === "cycle") {
      var cv = groupValue(groupKey, ctrl.key, "");
      wrap.innerHTML =
        '<div class="rw-control-row"><span class="rw-control-label">' + ctrl.label + '</span>' +
        '<button class="rw-cycle-btn">' + cv + ' ↻</button></div>';
      wrap.querySelector(".rw-cycle-btn").addEventListener("click", function () {
        sendAction({ kind: "cycle", key: ctrl.key });
      });
    } else if (ctrl.type === "bind") {
      var bindLabel = groupValue(groupKey, ctrl.key + "_label", "—");
      var mode = groupValue(groupKey, ctrl.modeKey, "hold");
      var capturing = state.capture_target === ctrl.key;
      wrap.innerHTML =
        '<div class="rw-control-row"><span class="rw-control-label">' + ctrl.label +
        ': ' + bindLabel + '</span></div>' +
        '<div class="rw-stepper">' +
          '<button class="rw-bind-btn' + (capturing ? " capturing" : "") + '">' +
            (capturing ? "▶ 按任意键..." : "点击采集") + '</button>' +
          '<button class="rw-bind-btn">清除</button>' +
          '<button class="rw-bind-btn">' + (mode === "hold" ? "按住" : "切换") + '</button>' +
        '</div>';
      var btns = wrap.querySelectorAll(".rw-bind-btn");
      btns[0].addEventListener("click", function () { sendAction({ kind: "capture_bind", key: ctrl.key }); });
      btns[1].addEventListener("click", function () { sendAction({ kind: "clear_bind", key: ctrl.key }); });
      btns[2].addEventListener("click", function () { sendAction({ kind: "cycle_bind_mode", key: ctrl.modeKey }); });
    }
    return wrap;
  }

  function throttledSet(key, value) {
    var now = Date.now();
    var last = dragThrottle[key] || 0;
    if (now - last < 45) return;
    dragThrottle[key] = now;
    sendAction({ kind: "set", key: key, value: value });
  }

  // ── top-level render ──────────────────────────────────────────────

  function renderContent() {
    var root = document.getElementById("rw-content");
    root.innerHTML = "";
    if (activeGroup === "overview") {
      renderOverview(root);
    } else {
      renderGroup(root, activeGroup);
    }
  }

  function renderStatusPill() {
    var pill = document.getElementById("rw-status-pill");
    var ov = state.overview || {};
    var status = ov.status || (state.running ? "LIVE" : "READY");
    pill.textContent = status;
    pill.className = "rw-status-pill " + (status === "LIVE" ? "live" : (status === "INCOMPLETE" ? "bad" : "ready"));
  }

  function renderFooter() {
    var main = document.getElementById("rw-cta-main");
    main.textContent = state.running ? "Stop" : "Start";
    main.className = "rw-cta rw-cta-main" + (state.running ? " running" : "");
  }

  function applyState(msg) {
    state.running = !!msg.running;
    state.overview = msg.overview || state.overview;
    state.groups = msg.groups || state.groups;
    state.capture_target = msg.capture_target || "";
    renderStatusPill();
    renderFooter();
    renderContent();
  }

  window.__rwApplyState = applyState;

  document.addEventListener("DOMContentLoaded", function () {
    renderRail();
    renderContent();
    renderStatusPill();
    renderFooter();

    document.getElementById("rw-cta-main").addEventListener("click", function () {
      sendAction({ kind: "command", name: state.running ? "stop" : "start" });
    });
    document.getElementById("rw-cta-save").addEventListener("click", function () {
      sendAction({ kind: "command", name: "save" });
    });
    document.querySelectorAll(".rw-mini-btn").forEach(function (btn) {
      btn.addEventListener("click", function () {
        sendAction({ kind: "command", name: btn.dataset.cmd });
      });
    });
  });
})();
