(function () {
  "use strict";

  const REFRESH_INTERVAL_MS = 5000;
  const TIMEOUT_MS = 10000;
  const LOG_LIMIT = 60;
  const vscode = typeof acquireVsCodeApi === "function" ? acquireVsCodeApi() : null;
  const el = {
    availability: document.querySelector('[data-role="bridge-availability"]'),
    statusPill: document.querySelector('[data-role="status-pill"]'),
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
    btnRetry: document.querySelector('[data-role="btn-retry"]'),
    error: document.querySelector('[data-role="error"]'),
    errorText: document.querySelector('[data-role="error-text"]'),
    loadingSkeleton: document.querySelector('[data-role="loading-skeleton"]'),
    slot: document.querySelector('[data-role="activate-slot"]'),
    seed: document.querySelector('[data-role="activate-seed"]'),
    timeout: document.querySelector('[data-role="activate-timeout"]'),
    rowTemplate: document.getElementById("image-row-template")
  };

  let requestSeq = 1;
  let refreshTimer = null;
  let bridgeAvailable = null;
  let bridgeActive = false;
  let generation = 1;
  let lastMessageSeq = 0;
  let loading = true;
  let autoRefreshPaused = false;
  const pending = new Map();

  function pendingKey(cmd, args) {
    if (cmd === "unmap") return cmd + ":" + String(args && args.target_base || "");
    return cmd;
  }

  function hasPending(key) {
    for (const request of pending.values()) if (request.key === key) return true;
    return false;
  }

  function clearPending() {
    for (const request of pending.values()) window.clearTimeout(request.timer);
    pending.clear();
    syncPendingState();
  }

  function advanceGeneration() {
    clearPending();
    generation += 1;
  }

  function setError(message) {
    const text = message || "";
    if (el.errorText) el.errorText.textContent = text;
    if (el.error) el.error.hidden = !text;
    if (el.statusPill) {
      el.statusPill.textContent = text ? "Attention" : bridgeActive ? "Active" : "Ready";
      el.statusPill.classList.toggle("error", Boolean(text));
    }
  }

  function syncPendingState() {
    document.querySelectorAll("[data-cmd]").forEach((button) => {
      if (!button.dataset.defaultLabel) button.dataset.defaultLabel = button.textContent;
      const key = button.dataset.pendingKey || button.dataset.cmd;
      const busy = hasPending(key);
      button.disabled = busy || button.dataset.disabled === "true";
      button.setAttribute("aria-busy", String(busy));
      button.textContent = busy ? "Working..." : button.dataset.defaultLabel;
    });
    if (el.btnRetry) el.btnRetry.disabled = hasPending("status") || hasPending("enumerate");
  }

  function send(cmd, args) {
    const requestId = "km-" + requestSeq++;
    const requestGeneration = generation;
    const key = pendingKey(cmd, args);
    if (hasPending(key)) return null;
    const payload = { cmd, requestId, generation: requestGeneration };
    const request = {
      requestId,
      generation: requestGeneration,
      key,
      timer: window.setTimeout(() => {
        const current = pending.get(requestId);
        if (!current || current.generation !== requestGeneration) return;
        advanceGeneration();
        loading = false;
        autoRefreshPaused = true;
        setError(cmd + " timed out; stale requests were discarded");
        pushLog("error", cmd + " timed out; pending requests cleared");
        renderUnavailable("Unable to load bridge state. Retry when the runtime is ready.");
      }, TIMEOUT_MS)
    };
    pending.set(requestId, request);
    if (args !== undefined) payload.args = args;
    if (vscode) vscode.postMessage(payload);
    else if (window.__SAO_TEST_BRIDGE__ === true && window.parent && window.parent !== window) window.parent.postMessage(payload, window.__SAO_TEST_BRIDGE_ORIGIN__ || window.location.origin);
    syncPendingState();
    return requestId;
  }

  function trusted(event) {
    if (!event || !event.data || typeof event.data !== "object") return false;
    if (vscode) {
      const sourceOk = event.source === null || event.source === window;
      const origin = event.origin || "";
      const originOk = origin === "" || origin === window.location.origin || origin.indexOf("vscode-webview://") === 0;
      return sourceOk && originOk;
    }
    return window.__SAO_TEST_BRIDGE__ === true && window.parent && event.source === window.parent && event.origin === (window.__SAO_TEST_BRIDGE_ORIGIN__ || window.location.origin);
  }

  function saneSeq(data) {
    if (!Object.prototype.hasOwnProperty.call(data, "messageSeq")) return true;
    const value = Number(data.messageSeq);
    if (!Number.isSafeInteger(value) || value <= lastMessageSeq) return false;
    lastMessageSeq = value;
    return true;
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
    if (typeof value === "string") return value.startsWith("0x") || value.startsWith("0X") ? value : "0x" + value;
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
    el.btnActivate.dataset.disabled = String(bridgeAvailable !== true || bridgeActive || !configReady);
    el.btnDeactivate.dataset.disabled = String(bridgeAvailable !== true || !bridgeActive);
    el.btnLoadDriver.dataset.disabled = String(bridgeAvailable !== true);
    el.btnRefresh.dataset.disabled = "false";
    syncPendingState();
  }

  function renderLoading() {
    if (el.loadingSkeleton) el.loadingSkeleton.hidden = !loading;
    if (!loading) return;
    el.imageList.replaceChildren();
    const item = document.createElement("li");
    item.className = "empty-state";
    item.textContent = "Loading bridge state...";
    el.imageList.appendChild(item);
  }

  function renderUnavailable(message) {
    loading = false;
    if (el.loadingSkeleton) el.loadingSkeleton.hidden = true;
    if (el.imagesHint) el.imagesHint.textContent = "state unavailable";
    if (!el.imageList) return;
    el.imageList.replaceChildren();
    const item = document.createElement("li");
    item.className = "empty-state empty-state--error";
    item.textContent = message;
    el.imageList.appendChild(item);
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
    const count = typeof payload.mapped_count === "number" ? payload.mapped_count : Array.isArray(payload.bases) ? payload.bases.length : 0;
    el.mappedCount.textContent = String(count);
    el.lastRefresh.textContent = new Date().toISOString().slice(11, 19);
    if (el.statusPill) {
      el.statusPill.textContent = bridgeActive ? "Active" : "Ready";
      el.statusPill.classList.remove("error");
    }
    updateActionButtons();
  }

  function renderImages(payload) {
    const bases = payload && Array.isArray(payload.bases) ? payload.bases : [];
    el.imageList.replaceChildren();
    if (bases.length === 0) {
      el.imagesHint.textContent = "no images loaded";
      const empty = document.createElement("li");
      empty.className = "empty-state";
      empty.textContent = "No mapped images are currently published.";
      el.imageList.appendChild(empty);
      return;
    }
    el.imagesHint.textContent = bases.length + " image" + (bases.length === 1 ? "" : "s");
    for (const raw of bases) {
      const addr = formatHexAddress(raw);
      const node = el.rowTemplate.content.cloneNode(true);
      const li = node.querySelector("li");
      li.querySelector(".image-address").textContent = addr;
      const button = li.querySelector('button[data-cmd="unmap"]');
      button.dataset.targetBase = addr;
      button.dataset.pendingKey = "unmap:" + addr;
      el.imageList.appendChild(node);
    }
    syncPendingState();
  }

  function setBridgeAvailability(available, reason) {
    bridgeAvailable = Boolean(available);
    if (bridgeAvailable) {
      el.availability.textContent = "bridge: ready";
      el.availability.classList.remove("availability--error");
    } else {
      el.availability.textContent = "bridge: unavailable" + (reason ? " (" + reason + ")" : "");
      el.availability.classList.add("availability--error");
    }
    el.btnLoadDriver.dataset.disabled = String(!bridgeAvailable);
    updateActionButtons();
  }

  function requestRefresh(force) {
    if (autoRefreshPaused && force !== true) return;
    send("status");
    send("enumerate");
  }

  function bindClicks() {
    [el.slot, el.seed, el.timeout].forEach((input) => { if (input) input.addEventListener("input", updateActionButtons); });
    el.btnActivate.addEventListener("click", () => {
      const config = activateConfig();
      if (!config) return;
      send("activate", config);
      pushLog("info", "activate requested");
    });
    el.btnDeactivate.addEventListener("click", () => {
      if (!window.confirm("Deactivate the kernel map bridge?")) return;
      send("deactivate", { confirmed: true });
      pushLog("info", "deactivate requested");
    });
    el.btnRefresh.addEventListener("click", () => {
      autoRefreshPaused = false;
      send("refresh");
      requestRefresh(true);
      pushLog("info", "manual refresh");
    });
    el.btnLoadDriver.addEventListener("click", () => {
      if (!window.confirm("Open the file picker and load the selected driver image?")) return;
      send("load_driver", { confirmed: true });
      pushLog("info", "load driver requested");
    });
    el.btnRetry.addEventListener("click", () => {
      advanceGeneration();
      loading = true;
      autoRefreshPaused = false;
      setError("");
      renderLoading();
      requestRefresh(true);
      pushLog("info", "retry requested");
    });
    el.imageList.addEventListener("click", (event) => {
      const button = event.target.closest('button[data-cmd="unmap"]');
      if (!button) return;
      const target = button.dataset.targetBase;
      if (!window.confirm("Unload mapped image " + target + "?")) return;
      send("unmap", { target_base: target, confirmed: true });
      pushLog("info", "unmap requested for " + target);
    });
  }

  function handleInbound(event) {
    if (!trusted(event)) return;
    const data = event.data;
    if (!saneSeq(data)) return;
    const request = pending.get(data.requestId);
    if (!request || request.generation !== generation || data.generation !== request.generation) return;
    window.clearTimeout(request.timer);
    pending.delete(data.requestId);
    autoRefreshPaused = false;
    const status = data.status;
    const cmd = data.cmd || data.command || "";
    const payload = data.payload || data.result || null;
    syncPendingState();
    if (cmd === "availability") {
      setBridgeAvailability(status === "ok", data.reason);
      return;
    }
    if (status === "not_implemented") {
      loading = false;
      renderUnavailable(cmd + " is not available in this runtime.");
      setError(cmd + ": not implemented" + (data.reason ? " - " + data.reason : ""));
      pushLog("warn", cmd + ": not implemented" + (data.reason ? " - " + data.reason : ""));
      return;
    }
    if (status === "error") {
      loading = false;
      renderUnavailable("Unable to load bridge state. Use Retry to reconnect.");
      if (payload) renderStatus(payload);
      setError(cmd + ": " + (data.reason || "error"));
      pushLog("error", cmd + ": " + (data.reason || "error"));
      return;
    }
    if (status !== "ok") {
      pushLog("info", cmd + ": " + JSON.stringify(data));
      return;
    }
    if (cmd === "status") {
      loading = false;
      renderLoading();
      renderStatus(payload);
      if (payload && payload.bridge && payload.bridge.available === false) setError("Kernel map bridge is unavailable");
      pushLog("ok", "status: " + (payload && payload.active ? "active" : "inactive"));
    } else if (cmd === "enumerate") {
      loading = false;
      renderLoading();
      renderImages(payload);
      const count = payload && Array.isArray(payload.bases) ? payload.bases.length : 0;
      pushLog("ok", "enumerate: " + count + " image" + (count === 1 ? "" : "s"));
    } else if (cmd === "activate") {
      setError("");
      pushLog("ok", "activated");
      requestRefresh();
    } else if (cmd === "deactivate") {
      setError("");
      pushLog("ok", "deactivated");
      requestRefresh();
    } else if (cmd === "unmap") {
      setError("");
      pushLog("ok", "unloaded " + (payload && payload.target_base ? formatHexAddress(payload.target_base) : ""));
      requestRefresh();
    } else if (cmd === "load_driver") {
      setError("");
      pushLog("ok", "driver load completed");
      requestRefresh();
    } else if (cmd === "refresh") {
      setError("");
      requestRefresh();
    } else {
      pushLog("info", cmd + ": " + JSON.stringify(payload));
    }
    updateActionButtons();
  }

  function start() {
    bindClicks();
    window.addEventListener("message", handleInbound);
    renderLoading();
    updateActionButtons();
    requestRefresh(true);
    refreshTimer = window.setInterval(requestRefresh, REFRESH_INTERVAL_MS);
  }

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", start, { once: true });
  else start();
}());
