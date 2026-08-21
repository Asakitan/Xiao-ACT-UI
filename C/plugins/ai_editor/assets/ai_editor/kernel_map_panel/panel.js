// Kernel Map Bridge panel front-end controller.
// Outbound messages carry requestId + generation; inbound replies must match
// both values and the configured test bridge source/origin when applicable.
(function () {
  "use strict";

  const REFRESH_INTERVAL_MS = 5000;
  const LOG_LIMIT = 60;
  const vscode = (typeof acquireVsCodeApi === "function") ? acquireVsCodeApi() : null;

  const el = {
    availability: document.querySelector('[data-role="bridge-availability"]'),
    statusBadge: document.querySelector('[data-role="status-badge"]'),
    mappedCount: document.querySelector('[data-role="mapped-count"]'),
    lastRefresh: document.querySelector('[data-role="last-refresh"]'),
    imageList: document.querySelector('[data-role="image-list"]'),
    imagesHint: document.querySelector('[data-role="images-hint"]'),
    logEntries: document.querySelector('[data-role="log-entries"]'),
    btnActivate: document.querySelector('[data-role="btn-activate"]'),
    btnDeactivate: document.querySelector('[data-role="btn-deactivate"]'),
    btnRefresh: document.querySelector('[data-role="btn-refresh"]'),
    btnLoadDriver: document.querySelector('[data-role="btn-load-driver"]'),
    slot: document.querySelector('[data-role="activate-slot"]'),
    seed: document.querySelector('[data-role="activate-seed"]'),
    timeout: document.querySelector('[data-role="activate-timeout"]'),
    rowTemplate: document.getElementById("image-row-template"),
  };

  let requestSeq = 1;
  let refreshTimer = null;
  let bridgeAvailable = null;
  let bridgeActive = false;
  let generation = 1;
  const pending = new Map();

  function send(cmd, args) {
    const requestId = "km-" + (requestSeq++);
    const requestGeneration = generation;
    const payload = { cmd, requestId, generation: requestGeneration };
    pending.set(requestId, { generation: requestGeneration, timer: window.setTimeout(function () {
      if (!pending.has(requestId)) return;
      pending.delete(requestId);
      generation += 1;
      pushLog("error", cmd + " timed out");
    }, 10000) });
    if (args !== undefined) payload.args = args;
    if (vscode) {
      vscode.postMessage(payload);
    } else if (window.__SAO_TEST_BRIDGE__ === true && window.parent && window.parent !== window) {
      window.parent.postMessage(payload, window.__SAO_TEST_BRIDGE_ORIGIN__ || window.location.origin);
    }
    return requestId;
  }

  function pushLog(level, message) {
    if (!el.logEntries) return;
    const item = document.createElement("li");
    item.className = "log--" + (level || "info");
    item.textContent = new Date().toISOString().slice(11, 19) + "  " + message;
    el.logEntries.appendChild(item);
    while (el.logEntries.childElementCount > LOG_LIMIT) el.logEntries.removeChild(el.logEntries.firstElementChild);
    el.logEntries.scrollTop = el.logEntries.scrollHeight;
  }

  function formatHexAddress(value) {
    if (typeof value === "string") return (value.startsWith("0x") || value.startsWith("0X")) ? value : "0x" + value;
    if (typeof value === "number") return "0x" + value.toString(16).toUpperCase().padStart(16, "0");
    return String(value);
  }

  function nonZeroUnsignedText(value) {
    const text = value ? value.trim() : "";
    if (!/^(?:0[xX][0-9a-fA-F]+|[0-9]+)$/.test(text)) return false;
    try { return BigInt(text) > 0n; } catch (_) { return false; }
  }

  function activateConfig() {
    const slot = el.slot ? el.slot.value.trim() : "";
    const seed = el.seed ? el.seed.value.trim() : "";
    const timeoutText = el.timeout ? el.timeout.value.trim() : "";
    const timeout = Number(timeoutText);
    if (!nonZeroUnsignedText(slot) || !nonZeroUnsignedText(seed) || !Number.isInteger(timeout) || timeout < 100 || timeout > 600000) return null;
    return { invoke_result_slot_va: slot, idle_timeout_ms: timeout, pool_tag_seed: seed };
  }

  function updateActionButtons() {
    const configReady = activateConfig() !== null;
    el.btnActivate.disabled = bridgeAvailable !== true || bridgeActive || !configReady;
    el.btnDeactivate.disabled = bridgeAvailable !== true || !bridgeActive;
  }

  function renderStatus(payload) {
    if (!payload || typeof payload !== "object") {
      bridgeActive = false;
      el.statusBadge.textContent = "Unknown";
      el.statusBadge.className = "status-badge status-badge--unknown";
      el.mappedCount.textContent = "0";
      updateActionButtons();
      return;
    }
    if (payload.bridge && typeof payload.bridge.available === "boolean") bridgeAvailable = payload.bridge.available;
    else if (bridgeAvailable === null) bridgeAvailable = true;
    bridgeActive = Boolean(payload.active);
    el.statusBadge.textContent = bridgeActive ? "Active" : "Inactive";
    el.statusBadge.className = "status-badge " + (bridgeActive ? "status-badge--active" : "status-badge--inactive");
    const count = (typeof payload.mapped_count === "number") ? payload.mapped_count : (Array.isArray(payload.bases) ? payload.bases.length : 0);
    el.mappedCount.textContent = String(count);
    el.lastRefresh.textContent = new Date().toISOString().slice(11, 19);
    updateActionButtons();
  }

  function renderImages(payload) {
    const bases = (payload && Array.isArray(payload.bases)) ? payload.bases : [];
    el.imageList.innerHTML = "";
    if (bases.length === 0) {
      el.imagesHint.textContent = "no images loaded";
      return;
    }
    el.imagesHint.textContent = bases.length + " image" + (bases.length === 1 ? "" : "s");
    for (const raw of bases) {
      const addr = formatHexAddress(raw);
      const node = el.rowTemplate.content.cloneNode(true);
      const li = node.querySelector("li");
      li.querySelector(".image-address").textContent = addr;
      li.querySelector('button[data-cmd="unmap"]').dataset.targetBase = addr;
      el.imageList.appendChild(node);
    }
  }

  function setBridgeAvailability(available, reason) {
    bridgeAvailable = Boolean(available);
    if (bridgeAvailable) {
      el.availability.textContent = "bridge: ready";
      el.availability.style.color = "";
      el.btnRefresh.disabled = false;
    } else {
      el.availability.textContent = "bridge: unavailable" + (reason ? " (" + reason + ")" : "");
      el.availability.style.color = "var(--km-warn)";
      el.btnRefresh.disabled = false;
    }
    el.btnLoadDriver.disabled = !bridgeAvailable;
    updateActionButtons();
  }

  function requestRefresh() {
    send("status");
    send("enumerate");
  }

  function bindClicks() {
    [el.slot, el.seed, el.timeout].forEach(function (input) {
      if (input) input.addEventListener("input", updateActionButtons);
    });
    el.btnActivate.addEventListener("click", function () {
      const config = activateConfig();
      if (!config) return;
      send("activate", config);
      pushLog("info", "activate requested");
    });
    el.btnDeactivate.addEventListener("click", function () {
      if (!window.confirm("Deactivate the kernel map bridge?")) return;
      send("deactivate", { confirmed: true });
      pushLog("info", "deactivate requested");
    });
    el.btnRefresh.addEventListener("click", function () {
      send("refresh");
      requestRefresh();
      pushLog("info", "manual refresh");
    });
    el.btnLoadDriver.addEventListener("click", function () {
      if (!window.confirm("Open the file picker and load the selected driver image?")) return;
      send("load_driver", { confirmed: true });
      pushLog("info", "load driver requested");
    });
    el.imageList.addEventListener("click", function (event) {
      const btn = event.target.closest('button[data-cmd="unmap"]');
      if (!btn) return;
      const target = btn.dataset.targetBase;
      if (!window.confirm("Unload mapped image " + target + "?")) return;
      send("unmap", { target_base: target, confirmed: true });
      pushLog("info", "unmap requested for " + target);
    });
  }

  function handleInbound(event) {
    const data = event && event.data;
    if (!data || typeof data !== "object") return;
    if (!vscode && !(window.__SAO_TEST_BRIDGE__ === true && event.source === window.parent && event.origin === (window.__SAO_TEST_BRIDGE_ORIGIN__ || window.location.origin))) return;
    const request = pending.get(data.requestId);
    if (!request || request.generation !== generation || data.generation !== request.generation) return;
    window.clearTimeout(request.timer);
    pending.delete(data.requestId);
    const status = data.status;
    const cmd = data.cmd || data.command || "";
    const payload = data.payload || data.result || null;
    if (cmd === "availability") {
      setBridgeAvailability(status === "ok", data.reason);
      return;
    }
    if (status === "not_implemented") {
      pushLog("warn", cmd + ": not implemented" + (data.reason ? " - " + data.reason : ""));
      return;
    }
    if (status === "error") {
      pushLog("error", cmd + ": " + (data.reason || "error"));
      return;
    }
    if (status !== "ok") {
      pushLog("info", cmd + ": " + JSON.stringify(data));
      return;
    }
    if (cmd === "status") {
      renderStatus(payload);
      pushLog("ok", "status: " + (payload && payload.active ? "active" : "inactive"));
    } else if (cmd === "enumerate") {
      renderImages(payload);
      const count = (payload && Array.isArray(payload.bases)) ? payload.bases.length : 0;
      pushLog("ok", "enumerate: " + count + " image" + (count === 1 ? "" : "s"));
    } else if (cmd === "activate") {
      pushLog("ok", "activated");
      requestRefresh();
    } else if (cmd === "deactivate") {
      pushLog("ok", "deactivated");
      requestRefresh();
    } else if (cmd === "unmap") {
      pushLog("ok", "unloaded " + (payload && payload.target_base ? formatHexAddress(payload.target_base) : ""));
      requestRefresh();
    } else if (cmd === "refresh") {
      requestRefresh();
    } else {
      pushLog("info", cmd + ": " + JSON.stringify(payload));
    }
  }

  function start() {
    bindClicks();
    window.addEventListener("message", handleInbound);
    updateActionButtons();
    requestRefresh();
    refreshTimer = window.setInterval(requestRefresh, REFRESH_INTERVAL_MS);
  }

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", start, { once: true });
  else start();
})();