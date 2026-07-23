// Kernel Map Bridge panel — front-end controller.
// Contract with the native provider (see kernel_map_panel_provider.cpp):
//   * Outbound: postMessage({ cmd, args?, requestId? }).
//   * Inbound: window "message" event with data shape:
//       { status: "ok" | "error" | "not_implemented", cmd, requestId?,
//         payload?: {...}, reason?: "..." }.
// The panel never invokes native APIs directly; every action is a cmd.

(function () {
  "use strict";

  const REFRESH_INTERVAL_MS = 5000;
  const LOG_LIMIT = 60;

  const vscode = (typeof acquireVsCodeApi === "function")
    ? acquireVsCodeApi()
    : null;

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
    rowTemplate: document.getElementById("image-row-template"),
  };

  let requestSeq = 1;
  let refreshTimer = null;
  let bridgeAvailable = null;

  function send(cmd, args) {
    const requestId = "km-" + (requestSeq++);
    const payload = { cmd, requestId };
    if (args !== undefined) {
      payload.args = args;
    }
    if (vscode) {
      vscode.postMessage(payload);
    } else if (window.parent && window.parent !== window) {
      // Fallback for smoke fixtures.
      window.parent.postMessage(payload, "*");
    }
    return requestId;
  }

  function pushLog(level, message) {
    if (!el.logEntries) { return; }
    const item = document.createElement("li");
    item.className = "log--" + (level || "info");
    const now = new Date();
    const stamp = now.toISOString().slice(11, 19);
    item.textContent = stamp + "  " + message;
    el.logEntries.appendChild(item);
    while (el.logEntries.childElementCount > LOG_LIMIT) {
      el.logEntries.removeChild(el.logEntries.firstElementChild);
    }
    el.logEntries.scrollTop = el.logEntries.scrollHeight;
  }

  function formatHexAddress(value) {
    if (typeof value === "string") {
      if (value.startsWith("0x") || value.startsWith("0X")) { return value; }
      return "0x" + value;
    }
    if (typeof value === "number") {
      return "0x" + value.toString(16).toUpperCase().padStart(16, "0");
    }
    return String(value);
  }

  function renderStatus(payload) {
    if (!payload || typeof payload !== "object") {
      el.statusBadge.textContent = "Unknown";
      el.statusBadge.className = "status-badge status-badge--unknown";
      el.mappedCount.textContent = "0";
      return;
    }
    const active = Boolean(payload.active);
    el.statusBadge.textContent = active ? "Active" : "Inactive";
    el.statusBadge.className =
      "status-badge " + (active ? "status-badge--active" : "status-badge--inactive");
    const count = (typeof payload.mapped_count === "number")
      ? payload.mapped_count
      : (Array.isArray(payload.bases) ? payload.bases.length : 0);
    el.mappedCount.textContent = String(count);
    el.lastRefresh.textContent = new Date().toISOString().slice(11, 19);
    el.btnActivate.disabled = active;
    el.btnDeactivate.disabled = !active;
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
      const btn = li.querySelector('button[data-cmd="unmap"]');
      btn.dataset.targetBase = addr;
      el.imageList.appendChild(node);
    }
  }

  function setBridgeAvailability(available, reason) {
    bridgeAvailable = Boolean(available);
    if (bridgeAvailable) {
      el.availability.textContent = "bridge: ready";
      el.availability.style.color = "";
      el.btnActivate.disabled = false;
      el.btnDeactivate.disabled = false;
      el.btnRefresh.disabled = false;
    } else {
      el.availability.textContent =
        "bridge: unavailable" + (reason ? " (" + reason + ")" : "");
      el.availability.style.color = "var(--km-warn)";
      el.btnActivate.disabled = true;
      el.btnDeactivate.disabled = true;
      // Refresh stays enabled — the user may want to re-probe.
    }
    el.btnLoadDriver.disabled = !bridgeAvailable;
  }

  function requestRefresh() {
    send("status");
    send("enumerate");
  }

  function bindClicks() {
    el.btnActivate.addEventListener("click", () => {
      // Slot VA / pool tag seed are 64-bit kernel-side values and must
      // travel as hex strings — JSON numbers cannot represent them
      // faithfully.  A 0x0 slot causes the peer to reply NOT_INITIALIZED,
      // which the operator sees as an error entry in the log.
      send("activate", {
        invoke_result_slot_va: "0x0000000000000000",
        idle_timeout_ms: 60000,
        pool_tag_seed: "0x00000000",
      });
      pushLog("info", "activate requested");
    });
    el.btnDeactivate.addEventListener("click", () => {
      send("deactivate");
      pushLog("info", "deactivate requested");
    });
    el.btnRefresh.addEventListener("click", () => {
      send("refresh");
      requestRefresh();
      pushLog("info", "manual refresh");
    });
    el.btnLoadDriver.addEventListener("click", () => {
      send("load_driver", {});
      pushLog("info", "load driver requested");
    });
    el.imageList.addEventListener("click", (event) => {
      const btn = event.target.closest('button[data-cmd="unmap"]');
      if (!btn) { return; }
      const target = btn.dataset.targetBase;
      send("unmap", { target_base: target });
      pushLog("info", "unmap requested for " + target);
    });
  }

  function handleInbound(event) {
    const data = event && event.data;
    if (!data || typeof data !== "object") { return; }
    // Native side sends {status, cmd, payload, reason?}
    const status = data.status;
    const cmd = data.cmd || data.command || "";
    const payload = data.payload || data.result || null;
    if (cmd === "availability") {
      setBridgeAvailability(status === "ok", data.reason);
      return;
    }
    if (status === "not_implemented") {
      pushLog("warn", cmd + ": not implemented" +
              (data.reason ? " — " + data.reason : ""));
      return;
    }
    if (status === "error") {
      pushLog("error", cmd + ": " + (data.reason || "error"));
      return;
    }
    if (status !== "ok") {
      // Non-status payloads (e.g. host push events) are logged verbatim.
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
      pushLog("ok", "unloaded " + (payload && payload.target_base
        ? formatHexAddress(payload.target_base) : ""));
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
    // First bootstrap sequence.
    requestRefresh();
    refreshTimer = window.setInterval(requestRefresh, REFRESH_INTERVAL_MS);
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", start, { once: true });
  } else {
    start();
  }
})();
