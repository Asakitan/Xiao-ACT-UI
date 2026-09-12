(function saoWorkbenchNativeBridge() {
  "use strict";

  const CHANNEL = "sao.workbench";
  const REQUEST_TIMEOUT_MS = 30000;
  const LONG_REQUEST_TIMEOUT_MS = 600000;
  const HANDSHAKE_TIMEOUT_MS = 4000;
  const LEGACY_EVENT_LIMIT = 128;
  const BLOCKED_METHODS = new Set([
    "memviewer_read",
    "memviewer_read_value",
    "memviewer_status"
  ]);
  const LONG_REQUEST_METHODS = new Set(["run_workflow", "retry_workflow_step",
    "open_text_file", "open_file_dialog", "save_file_dialog", "save_file_as"]);
  const state = {
    status: "connecting",
    connected: false,
    capabilities: [],
    sequence: 0,
    facadeInstalled: false,
    readyDispatched: false,
    helloSent: false,
    challenge: "",
    handshakeTimer: 0
  };
  const pending = new Map();
  const queuedLegacyEvents = [];
  const webview = window.chrome && window.chrome.webview;
  const transportAvailable = !!(webview && typeof webview.postMessage === "function" &&
    typeof webview.addEventListener === "function");

  class SaoWorkbenchBridgeError extends Error {
    constructor(code, message, method, data) {
      super(message);
      this.name = "SaoWorkbenchBridgeError";
      this.code = code;
      this.method = method || "";
      this.data = data === undefined ? null : data;
    }
  }

  function setDocumentState(status) {
    const root = document.documentElement;
    if (root) root.dataset.saoWorkbenchBridge = status;
    if (document.body) document.body.dataset.saoWorkbenchBridge = status;
  }

  function publish(type, detail) {
    try { window.dispatchEvent(new CustomEvent(type, { detail: detail })); } catch (_) {}
  }

  function normalizeNativeMessage(raw) {
    if (typeof raw === "string") {
      try { return JSON.parse(raw); } catch (_) { return null; }
    }
    return raw && typeof raw === "object" ? raw : null;
  }

  function errorFromNative(error, method) {
    const source = error && typeof error === "object" ? error : {};
    const code = typeof source.code === "string" && source.code ? source.code : "SAO_NATIVE_ERROR";
    const message = typeof source.message === "string" && source.message
      ? source.message
      : "Native workbench request failed.";
    return new SaoWorkbenchBridgeError(code, message, method, source.data);
  }

  function rejectOutstanding(error) {
    pending.forEach(function (entry) {
      clearTimeout(entry.timer);
      entry.reject(errorFromNative(error, entry.method));
    });
    pending.clear();
  }

  function request(method, args) {
    const methodName = String(method || "");
    if (BLOCKED_METHODS.has(methodName)) {
      return Promise.reject(new SaoWorkbenchBridgeError(
        "SAO_METHOD_UNAVAILABLE",
        "This workbench bridge does not expose memory tooling.",
        methodName));
    }
    if (state.status !== "online" || !state.connected) {
      return Promise.reject(new SaoWorkbenchBridgeError(
        "SAO_NATIVE_UNAVAILABLE",
        "Native workbench host is " + (state.status === "offline" ? "offline." : "not ready."),
        methodName));
    }
    const id = "sao-" + Date.now().toString(36) + "-" + (++state.sequence).toString(36);
    return new Promise(function (resolve, reject) {
      const timer = window.setTimeout(function () {
        pending.delete(id);
        reject(new SaoWorkbenchBridgeError(
          "SAO_NATIVE_TIMEOUT",
          "Native workbench request timed out.",
          methodName));
      }, LONG_REQUEST_METHODS.has(methodName) ? LONG_REQUEST_TIMEOUT_MS : REQUEST_TIMEOUT_MS);
      pending.set(id, { method: methodName, resolve: resolve, reject: reject, timer: timer });
      try {
        webview.postMessage({
          channel: CHANNEL,
          kind: "request",
          challenge: state.challenge,
          id: id,
          method: methodName,
          args: args || []
        });
      } catch (error) {
        clearTimeout(timer);
        pending.delete(id);
        reject(new SaoWorkbenchBridgeError(
          "SAO_NATIVE_TRANSPORT_ERROR",
          error && error.message ? error.message : "Native workbench transport failed.",
          methodName));
      }
    });
  }

  function createFacade() {
    return new Proxy(Object.create(null), {
      get: function (_, property) {
        if (property === "__saoWorkbenchBridge") return true;
        if (property === "__saoWorkbenchConnected") return state.connected;
        if (property === "then") return undefined;
        if (typeof property !== "string") return undefined;
        return function () { return request(property, Array.prototype.slice.call(arguments)); };
      },
      has: function (_, property) {
        return typeof property === "string" && property !== "then";
      }
    });
  }

  function installFacade() {
    if (state.facadeInstalled) return;
    window.pywebview = window.pywebview || {};
    window.pywebview.api = createFacade();
    state.facadeInstalled = true;
  }

  function callLegacyEvent(name, payload) {
    const eventName = name === "editor_event" && payload && typeof payload.event === "string"
      ? payload.event : name;
    const eventPayload = name === "editor_event" && payload ? payload.data : payload;
    const notificationLevels = {
      "vscode.window.InformationMessage": "info",
      "vscode.window.WarningMessage": "warning",
      "vscode.window.ErrorMessage": "error"
    };
    const level = Object.prototype.hasOwnProperty.call(notificationLevels, eventName)
      ? notificationLevels[eventName] : null;
    if (level) {
      const container = document.getElementById("toast-container");
      if (typeof window.showToast !== "function" || !container) return false;
      const message = eventPayload && typeof eventPayload.message === "string" ? eventPayload.message : "";
      if (message) {
        window.showToast(message.slice(0, 4096), level, level === "error" ? 8000 : 5000);
        while (container.children.length > 8) container.firstElementChild.remove();
      }
      return true;
    }
    if (typeof window._onEditorEvent !== "function") return false;
    try {
      if (name === "editor_event" && payload && typeof payload === "object" && typeof payload.event === "string") {
        window._onEditorEvent(payload.event, payload.data);
      } else {
        window._onEditorEvent(name, payload);
      }
      return true;
    } catch (error) {
      console.error("[sao.workbench] legacy event callback failed", error);
      return true;
    }
  }

  function flushLegacyEvents() {
    while (queuedLegacyEvents.length) {
      const item = queuedLegacyEvents[0];
      if (!callLegacyEvent(item.name, item.payload)) break;
      queuedLegacyEvents.shift();
    }
  }

  function dispatchNativeEvent(name, payload) {
    const eventName = typeof name === "string" && name ? name : "native_event";
    const detail = { name: eventName, payload: payload };
    publish("sao:workbench-event", detail);
    publish("sao:workbench:" + eventName, detail);
    if (!callLegacyEvent(eventName, payload)) {
      queuedLegacyEvents.push({ name: eventName, payload: payload });
      if (queuedLegacyEvents.length > LEGACY_EVENT_LIMIT) queuedLegacyEvents.shift();
    }
  }

  function finalizeReady(message) {
    if (state.readyDispatched) return;
    if (state.handshakeTimer) window.clearTimeout(state.handshakeTimer);
    state.handshakeTimer = 0;
    state.capabilities = Array.isArray(message.capabilities) ? message.capabilities.slice() : [];
    state.connected = message.connected === true;
    state.status = state.connected ? "online" : "offline";
    installFacade();
    setDocumentState(state.status);
    state.readyDispatched = true;
    const detail = {
      channel: CHANNEL,
      kind: "ready",
      connected: state.connected,
      capabilities: state.capabilities.slice()
    };
    publish("sao:workbench-ready", detail);
    try { window.dispatchEvent(new Event("pywebviewready")); } catch (_) {}
    flushLegacyEvents();
  }

  function enterOffline(reason) {
    const firstReady = !state.readyDispatched;
    if (state.handshakeTimer) window.clearTimeout(state.handshakeTimer);
    state.handshakeTimer = 0;
    state.connected = false;
    state.status = "offline";
    installFacade();
    setDocumentState("offline");
    state.readyDispatched = true;
    const detail = {
      channel: CHANNEL,
      kind: "ready",
      connected: false,
      capabilities: [],
      reason: reason || "Native workbench host is unavailable."
    };
    publish("sao:workbench-offline", detail);
    if (firstReady) {
      publish("sao:workbench-ready", detail);
      try { window.dispatchEvent(new Event("pywebviewready")); } catch (_) {}
    }
  }

  function receiveNativeMessage(event) {
    const message = normalizeNativeMessage(event && event.data);
    if (!message || message.channel !== CHANNEL || typeof message.kind !== "string") return;
    if (message.kind === "challenge") {
      const challenge = typeof message.challenge === "string" ? message.challenge : "";
      if (challenge.length < 16 || challenge.length > 128) return;
      if (state.challenge && state.challenge !== challenge) {
        rejectOutstanding({ code: "SAO_NAVIGATION_RESET", message: "Workbench document changed." });
        state.connected = false;
        state.status = "connecting";
        state.readyDispatched = false;
        state.helloSent = false;
        setDocumentState("connecting");
      }
      state.challenge = challenge;
      sendHello();
      armHandshakeTimeout("Native workbench host handshake timed out.");
      return;
    }
    if (!state.challenge || message.challenge !== state.challenge) return;
    if (message.kind === "ready") {
      finalizeReady(message);
      return;
    }
    if (message.kind === "reply") {
      const id = typeof message.id === "string" ? message.id : "";
      const entry = pending.get(id);
      if (!entry) return;
      clearTimeout(entry.timer);
      pending.delete(id);
      if (message.ok === true) entry.resolve(message.result);
      else entry.reject(errorFromNative(message.error, entry.method));
      return;
    }
    if (message.kind === "event") {
      if (message.name === "transport_failed") {
        const reason = message.payload && message.payload.error
          ? String(message.payload.error)
          : "Native workbench transport failed.";
        rejectOutstanding({ code: "SAO_NATIVE_UNAVAILABLE", message: reason });
        enterOffline(reason);
        return;
      }
      dispatchNativeEvent(message.name, message.payload);
    }
  }

  function sendHello() {
    if (!transportAvailable || !state.challenge || state.helloSent) return;
    state.helloSent = true;
    try {
      webview.postMessage({
        channel: CHANNEL,
        kind: "hello",
        challenge: state.challenge,
        capabilities: ["rpc", "events", "pywebview-api-compat"]
      });
    } catch (_) {
      enterOffline("Native workbench transport is unavailable.");
    }
  }

  function armHandshakeTimeout(reason) {
    if (state.readyDispatched) return;
    if (state.handshakeTimer) window.clearTimeout(state.handshakeTimer);
    state.handshakeTimer = window.setTimeout(function () {
      if (!state.readyDispatched) enterOffline(reason);
    }, HANDSHAKE_TIMEOUT_MS);
  }

  window.SaoWorkbenchBridge = Object.freeze({
    channel: CHANNEL,
    get status() { return state.status; },
    get connected() { return state.connected; },
    get capabilities() { return state.capabilities.slice(); },
    request: request
  });

  if (!transportAvailable) {
    enterOffline("Native workbench host is unavailable.");
    return;
  }

  webview.addEventListener("message", receiveNativeMessage);
  setDocumentState("connecting");
  document.addEventListener("DOMContentLoaded", flushLegacyEvents, { once: true });
  window.addEventListener("load", function () {
    flushLegacyEvents();
    if (!state.challenge)
      armHandshakeTimeout("Native workbench host did not issue a document challenge.");
  }, { once: true });
})();

(function installEditorTools() {
  "use strict";
  let loading = false;
  let actionQueue = Promise.resolve();
  let actionCount = 0;
  let actionRevision = 0;
  let toolsSelected = false;
  let lastDocument = "";
  let refreshTimer = 0;
  function panel() { return document.getElementById("rp-native-tools"); }
  function render(model) {
    const host = panel();
    if (!host) return;
    const content = host.querySelector(".native-tools-content");
    const signature = JSON.stringify(model);
    if (signature === lastDocument || content.contains(document.activeElement) &&
        /^(INPUT|TEXTAREA|SELECT)$/.test(document.activeElement.tagName)) return;
    const oldFocus = document.activeElement && document.activeElement.dataset.nativeId;
    const scroll = content.scrollTop;
    let count = 0;
    function node(spec, depth) {
      if (!spec || typeof spec !== "object" || depth > 40 || ++count > 16000) return document.createTextNode("");
      const kind = String(spec.type || "section").toLowerCase();
      const element = document.createElement(kind === "button" ? "button" :
        kind === "input" ? (spec.multiline || spec.input_type === "multiline" ? "textarea" : "input") :
        kind === "text" || kind === "label" ? "p" : kind === "badge" ? "span" : "section");
      element.className = "native-tool-node native-tool-" + kind.replace(/[^a-z-]/g, "");
      const style = String(spec.style || spec.accent || "").replace(/[^a-z-]/g, "");
      if (style) element.classList.add("native-tool-" + style);
      if (spec.id) element.dataset.nativeId = String(spec.id);
      if (spec.title) {
        const title = document.createElement("h3"); title.textContent = String(spec.title); element.appendChild(title);
      }
      if (kind === "button") {
        element.type = "button"; element.textContent = String(spec.label || spec.text || "操作");
        element.disabled = !!spec.disabled;
        element.addEventListener("click", function () { act(spec.action, spec.payload || {}); });
      } else if (kind === "input") {
        element.value = String(spec.value || ""); element.disabled = !!spec.disabled;
        element.setAttribute("aria-label", String(spec.label || spec.id || "工具参数"));
        element.addEventListener("change", function () {
          act(spec.action, Object.assign({}, spec.payload || {}, { text: element.value, value: element.value }));
        });
      } else if (spec.text !== undefined) element.textContent = String(spec.text);
      (Array.isArray(spec.children) ? spec.children : Array.isArray(spec.nodes) ? spec.nodes : [])
        .forEach(function (child) { element.appendChild(node(child, depth + 1)); });
      return element;
    }
    content.replaceChildren(node(model, 0));
    content.scrollTop = scroll;
    if (oldFocus) {
      const target = Array.from(content.querySelectorAll("[data-native-id]"))
        .find(function (entry) { return entry.dataset.nativeId === oldFocus; });
      if (target) target.focus({ preventScroll: true });
    }
    lastDocument = signature;
    host.querySelector(".native-tools-status").textContent = "与原生编辑器状态同步";
  }
  function act(action, payload) {
    if (!action) return;
    ++actionCount;
    ++actionRevision;
    actionQueue = actionQueue.then(async function () {
      if (action === "settings.open" && typeof window.openSettings === "function") {
        window.openSettings();
        return;
      }
      if (action === "history.load" && typeof window.loadHistoryConv === "function") {
        await window.loadHistoryConv(payload.id);
        return;
      }
      if ((action === "control.use_agent" || action === "select.agent") &&
          typeof window.selectAgentFromPopup === "function") {
        await window.selectAgentFromPopup(payload.id || payload.value || "");
        return;
      }
      if (action === "select.model" && typeof window.selectModelFromPopup === "function") {
        await window.selectModelFromPopup(payload.value);
        return;
      }
      if (action === "control.use_prompt" && typeof window.setChatInputValue === "function") {
        window.setChatInputValue("Use prompt " + payload.id + " in the next request.", { focus: true });
        return;
      }
      const model = await window.SaoWorkbenchBridge.request("native_panel_action", [String(action), payload]);
      lastDocument = ""; render(model);
    }).catch(showError).finally(function () { --actionCount; });
    return actionQueue;
  }
  function showError(error) {
    const host = panel();
    if (host) host.querySelector(".native-tools-status").textContent =
      error && error.message ? error.message : "工具暂不可用，请刷新重试。";
  }
  async function refresh() {
    const host = panel();
    if (loading || actionCount || !host || !host.classList.contains("active") || document.hidden) return;
    loading = true;
    const revision = actionRevision;
    try {
      const model = await window.SaoWorkbenchBridge.request("native_panel_snapshot", []);
      if (revision === actionRevision && !actionCount) render(model);
    }
    catch (error) { showError(error); }
    finally { loading = false; }
  }
  function install() {
    const tabs = document.getElementById("right-tabs"), panels = document.getElementById("right-panels");
    if (!tabs || !panels) return;
    if (!panel()) {
      lastDocument = "";
      const host = document.createElement("div"); host.id = "rp-native-tools";
      host.className = "right-panel native-tools"; host.setAttribute("role", "tabpanel");
      host.setAttribute("aria-label", "编辑器工具与状态");
      const toolbar = document.createElement("header"); toolbar.className = "native-tools-toolbar";
      const heading = document.createElement("strong"); heading.textContent = "工具与状态";
      const reload = document.createElement("button"); reload.type = "button"; reload.textContent = "刷新";
      reload.addEventListener("click", refresh); toolbar.append(heading, reload);
      const status = document.createElement("p"); status.className = "native-tools-status";
      status.setAttribute("role", "status"); status.textContent = "打开后同步已有检查器、历史、日志和工具。";
      const content = document.createElement("div"); content.className = "native-tools-content";
      host.append(toolbar, status, content); panels.appendChild(host);
    }
    if (!document.getElementById("native-tools-tab")) {
      const button = document.createElement("button"); button.id = "native-tools-tab";
      button.type = "button"; button.className = "rtab"; button.dataset.rtab = "native-tools";
      button.textContent = "工具与状态"; button.setAttribute("aria-controls", "rp-native-tools");
      button.addEventListener("click", function (event) {
        event.stopPropagation();
        toolsSelected = true;
        document.querySelectorAll(".right-panel,.rtab").forEach(function (entry) { entry.classList.remove("active"); });
        panel().classList.add("active"); button.classList.add("active"); refresh();
      });
      tabs.appendChild(button);
    }
    if (toolsSelected && !panel().classList.contains("active")) {
      document.querySelectorAll(".right-panel,.rtab").forEach(function (entry) { entry.classList.remove("active"); });
      panel().classList.add("active");
      document.getElementById("native-tools-tab").classList.add("active");
    }
  }
  document.addEventListener("DOMContentLoaded", function () {
    install();
    const tabs = document.getElementById("right-tabs");
    const panels = document.getElementById("right-panels");
    const observer = new MutationObserver(install);
    if (tabs) observer.observe(tabs, { childList: true });
    if (panels) observer.observe(panels, { childList: true });
    document.addEventListener("click", function (event) {
      const tab = event.target.closest && event.target.closest(".rtab");
      if (tab && tab.id !== "native-tools-tab") toolsSelected = false;
    }, true);
    refreshTimer = window.setInterval(refresh, 1000);
  });
  window.addEventListener("pagehide", function () { window.clearInterval(refreshTimer); });
})();
