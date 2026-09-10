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
  const LONG_REQUEST_METHODS = new Set(["run_workflow", "retry_workflow_step"]);
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
    while (queuedLegacyEvents.length && typeof window._onEditorEvent === "function") {
      const item = queuedLegacyEvents.shift();
      callLegacyEvent(item.name, item.payload);
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
