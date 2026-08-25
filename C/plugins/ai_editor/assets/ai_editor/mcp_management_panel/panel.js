(function () {
  "use strict";

  const TIMEOUT_MS = 10000;
  const MAX_EVENTS = 60;
  const vscode = typeof acquireVsCodeApi === "function" ? acquireVsCodeApi() : null;
  let requestSeq = 1;
  let generation = 1;
  let lastMessageSeq = 0;
  const pending = new Map();
  const state = { activeTab: "servers", loading: true, error: "", payload: {}, notifications: [] };

  const role = (name) => document.querySelector('[data-role="' + name + '"]');
  const summary = role("summary");
  const error = role("error");
  const errorText = role("error-text");
  const connection = role("connection-status");

  const el = (tag, className, text) => {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = text;
    return node;
  };

  const json = (value) => {
    try { return JSON.stringify(value === undefined ? {} : value, null, 2); }
    catch (_) { return String(value); }
  };

  function setError(message) {
    state.error = message || "";
    if (errorText) errorText.textContent = state.error;
    if (error) error.hidden = !state.error;
    if (connection) {
      connection.textContent = state.error ? "Error" : state.loading ? "Connecting" : "Connected";
      connection.classList.toggle("error", Boolean(state.error));
    }
  }

  function clearPending() {
    for (const request of pending.values()) window.clearTimeout(request.timer);
    pending.clear();
    syncPendingState();
  }

  function pendingFor(key) {
    for (const request of pending.values()) {
      if (request.pendingKey === key) return true;
    }
    return false;
  }

  function syncPendingState() {
    document.querySelectorAll("[data-pending-key]").forEach((button) => {
      if (!button.dataset.defaultLabel) button.dataset.defaultLabel = button.textContent;
      const busy = pendingFor(button.dataset.pendingKey);
      button.disabled = busy;
      button.setAttribute("aria-busy", String(busy));
      button.textContent = busy ? "Working..." : button.dataset.defaultLabel;
    });
  }

  function timeoutRequest(requestId) {
    const request = pending.get(requestId);
    if (!request) return;
    pending.delete(requestId);
    setError(request.cmd + " timed out");
    syncPendingState();
  }

  function send(cmd, args, meta) {
    const requestId = "mcp-" + requestSeq++;
    const requestGeneration = generation;
    const request = {
      requestId,
      generation: requestGeneration,
      cmd,
      pendingKey: meta && meta.pendingKey,
      startedAt: Date.now(),
      timer: window.setTimeout(() => timeoutRequest(requestId), TIMEOUT_MS)
    };
    const message = { cmd, requestId, generation: requestGeneration };
    if (args !== undefined) message.args = args;
    Object.assign(request, meta || {});
    pending.set(requestId, request);
    syncPendingState();
    if (vscode) vscode.postMessage(message);
    else if (window.__SAO_TEST_BRIDGE__ === true && window.parent && window.parent !== window) {
      window.parent.postMessage(message, window.__SAO_TEST_BRIDGE_ORIGIN__ || window.location.origin);
    }
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

  function list(key) { return Array.isArray(state.payload[key]) ? state.payload[key] : []; }

  function collection(container, items, empty, render) {
    container.replaceChildren();
    if (!items.length) {
      container.appendChild(el("div", "empty-state", empty));
      return;
    }
    for (const item of items) container.appendChild(render(item));
  }

  function skeleton(container, count) {
    container.replaceChildren();
    for (let index = 0; index < count; index += 1) container.appendChild(el("div", "skeleton-card"));
  }

  function serverConfig(server) {
    const config = { name: server.name || "", transport: server.transport || "stdio" };
    if (server.command) config.command = server.command;
    if (server.url) config.url = server.url;
    if (Array.isArray(server.args)) config.args = server.args;
    if (server.env && typeof server.env === "object" && !Object.values(server.env).some((value) => value === "<redacted>")) config.env = server.env;
    return config;
  }

  function count(name, value) {
    const node = role(name);
    if (node) node.textContent = String(value);
    const tabRoles = {
      "server-count": "tab-count-servers",
      "tool-count": "tab-count-tools",
      "prompt-count": "tab-count-prompts",
      "resource-count": "tab-count-resources",
      "notification-count": "tab-count-notifications"
    };
    const tab = tabRoles[name] ? role(tabRoles[name]) : null;
    if (tab) tab.textContent = String(value);
  }

  function renderServers() {
    const box = role("servers");
    const data = list("servers");
    count("server-count", data.length);
    if (state.loading) { skeleton(box, 3); return; }
    collection(box, data, "No MCP servers registered.", (server) => {
      const card = el("article");
      const title = el("div", "card-title");
      title.append(el("h3", "", server.name || "unnamed"), el("span", "badge", server.enabled === false ? "disabled" : server.status || "active"));
      card.append(title, el("p", "", "transport: " + (server.transport || "unknown")));
      const info = server.serverInfo || {};
      card.append(el("p", "", "server: " + (info.name || server.name || "unknown")), el("p", "", "protocol: " + (server.protocolVersion || "unknown")));
      if (server.url) card.append(el("p", "", "url: " + server.url));
      const actions = el("div", "card-actions");
      const enabled = server.enabled !== false;
      const buttons = enabled ? [["Disable", "disable"], ["Reconnect", "reconnect"], ["Close", "close_server"]] : [["Enable", "enable"], ["Close", "close_server"]];
      for (const pair of buttons) {
        const button = el("button", pair[1] === "close_server" ? "button-secondary" : "", pair[0]);
        button.type = "button";
        button.dataset.action = pair[1];
        button.dataset.serverName = server.name || "";
        button.dataset.serverConfig = json(serverConfig(server));
        button.dataset.pendingKey = "server:" + pair[1] + ":" + (server.name || "");
        actions.appendChild(button);
      }
      card.appendChild(actions);
      return card;
    });
  }

  function renderPlatform() {
    const box = role("platform-tools");
    if (!box) return;
    if (state.loading) { skeleton(box, 3); return; }
    const ready = state.payload.registration && (state.payload.registration.status === 0 || state.payload.registration.status === "ok" || state.payload.registration.status === "ready");
    const definitions = [["helperStatus", "Helper liveness and loaded engines.", ready ? "ready" : "offline"], ["engineSelect", "Switch the active engine after confirmation.", ready ? "available" : "offline"], ["memoryRead", "Bounded PID virtual-address reads.", ready ? "available" : "offline"], ["driverList", "Driver assets, state, and availability.", ready ? "available" : "offline"], ["hidSend", "Mouse or keyboard event after confirmation.", ready ? "available" : "offline"], ["vtStatus", "VT_ABSENT until driver_vt is installed.", "VT_ABSENT"]];
    box.replaceChildren();
    for (const item of definitions) {
      const card = el("div", "platform-card");
      const heading = el("div", "card-title");
      heading.append(el("strong", "", item[0]), el("span", "badge", item[2]));
      card.append(heading, el("p", "muted", item[1]));
      box.appendChild(card);
    }
  }

  function renderTools() {
    const box = role("tools");
    const data = list("tools");
    count("tool-count", data.length);
    if (state.loading) { skeleton(box, 3); return; }
    collection(box, data, "No MCP tools advertised.", (tool) => {
      const card = el("article");
      const title = el("div", "card-title");
      title.append(el("h3", "", tool.name || "unnamed"), el("span", "badge", tool.permission || tool.access || (tool.requires_confirm ? "confirm" : "allowed")));
      card.append(title, el("p", "", "server: " + (tool.server || "unknown")), el("p", "", tool.description || "No description"));
      card.append(el("p", "", "trust: " + (tool.trusted ? "trusted" : "untrusted") + " · permission: " + (tool.permission || tool.permissionSource || "unknown")));
      const rate = tool.rate_limit || tool.rateLimit || {};
      card.append(el("p", "", "rate: " + (rate.max_calls || rate.maxCalls || "-") + " / " + (rate.window_ms || rate.windowMs || "-") + " ms"), el("pre", "schema", json(tool.inputSchema || tool.input_schema || tool.schema || {})));
      const label = el("label", "inline-field", "Parameters JSON");
      const input = document.createElement("textarea");
      input.rows = 4;
      input.spellcheck = false;
      input.value = "{}";
      input.dataset.role = "tool-args";
      label.appendChild(input);
      card.appendChild(label);
      const call = el("button", "", "Call tool");
      call.type = "button";
      call.dataset.action = "call-tool";
      call.dataset.server = tool.server || "";
      call.dataset.toolName = tool.name || "";
      call.dataset.pendingKey = "tool:" + (tool.server || "") + ":" + (tool.name || "");
      const actions = el("div", "card-actions");
      actions.appendChild(call);
      card.appendChild(actions);
      return card;
    });
  }

  function promptArgs(prompt) {
    if (prompt.arguments && typeof prompt.arguments === "object" && !Array.isArray(prompt.arguments)) return prompt.arguments;
    const source = Array.isArray(prompt.arguments) ? prompt.arguments : Array.isArray(prompt.argumentsSchema) ? prompt.argumentsSchema : [];
    return source.reduce((out, argument) => { if (argument && argument.name) out[argument.name] = ""; return out; }, {});
  }

  function renderPrompts() {
    const box = role("prompts");
    const data = list("prompts");
    count("prompt-count", data.length);
    if (state.loading) { skeleton(box, 2); return; }
    collection(box, data, "No MCP prompts advertised.", (prompt) => {
      const card = el("article");
      card.append(el("h3", "", prompt.name || "unnamed"), el("p", "", "server: " + (prompt.server || "unknown")), el("p", "", prompt.description || "No description"));
      const label = el("label", "inline-field", "Arguments object");
      const input = document.createElement("textarea");
      input.rows = 4;
      input.spellcheck = false;
      input.value = json(promptArgs(prompt));
      input.dataset.role = "prompt-args";
      label.appendChild(input);
      card.appendChild(label);
      const button = el("button", "", "Preview");
      button.type = "button";
      button.dataset.action = "preview-prompt";
      button.dataset.server = prompt.server || "";
      button.dataset.promptName = prompt.name || "";
      button.dataset.pendingKey = "prompt:" + (prompt.server || "") + ":" + (prompt.name || "");
      const actions = el("div", "card-actions");
      actions.appendChild(button);
      card.appendChild(actions);
      return card;
    });
  }

  function renderResources() {
    const box = role("resources");
    const data = list("resources");
    count("resource-count", data.length);
    if (state.loading) { skeleton(box, 2); return; }
    collection(box, data, "No MCP resources advertised.", (resource) => {
      const card = el("article");
      const uri = resource.uri || resource.url || "";
      card.append(el("h3", "", resource.name || resource.title || uri || "unnamed"), el("p", "", "server: " + (resource.server || "unknown")), el("p", "", "URI: " + (uri || "unknown")), el("p", "", resource.description || resource.mimeType || "No description"));
      const button = el("button", "", "Read");
      button.type = "button";
      button.dataset.action = "read-resource";
      button.dataset.server = resource.server || "";
      button.dataset.uri = uri;
      button.dataset.pendingKey = "resource:" + uri;
      const actions = el("div", "card-actions");
      actions.appendChild(button);
      card.appendChild(actions);
      return card;
    });
  }

  function renderNotifications() {
    const box = role("notifications");
    if (!box) return;
    count("notification-count", state.notifications.length);
    if (!state.notifications.length) {
      box.replaceChildren(el("div", "empty-state", "Waiting for trusted MCP status events."));
      return;
    }
    box.replaceChildren();
    for (const entry of state.notifications) {
      const payload = entry.payload || {};
      const item = el("article", "feed-item");
      const heading = el("div", "card-title");
      heading.append(el("strong", "", payload.method || "notification"), el("span", "muted", entry.receivedAt));
      item.append(heading, el("p", "", "server: " + (payload.server || "unknown")), el("pre", "notification-details", json(payload.params || {})));
      box.appendChild(item);
    }
  }

  function renderAll() {
    renderServers();
    renderPlatform();
    renderTools();
    renderPrompts();
    renderResources();
    renderNotifications();
    if (summary) summary.textContent = state.loading ? "Loading registry..." : list("servers").length + " server(s), " + list("tools").length + " tool(s), " + list("prompts").length + " prompt(s), " + list("resources").length + " resource(s)";
    setError(state.error);
    syncPendingState();
  }

  function selectTab(tab, focus) {
    state.activeTab = tab;
    for (const button of document.querySelectorAll("[data-tab]")) {
      const selected = button.dataset.tab === tab;
      button.setAttribute("aria-selected", String(selected));
      button.tabIndex = selected ? 0 : -1;
      if (selected && focus) button.focus();
    }
    for (const panel of document.querySelectorAll("[data-panel]")) panel.hidden = panel.dataset.panel !== tab;
  }

  function parseJson(text, type, label) {
    let value;
    try { value = JSON.parse(text || (type === "array" ? "[]" : "{}")); }
    catch (_) { setError(label + " must be valid JSON"); return null; }
    if (type === "array" && !Array.isArray(value) || type === "object" && (!value || typeof value !== "object" || Array.isArray(value))) {
      setError(label + " must be a JSON " + type);
      return null;
    }
    return value;
  }

  function confirmAction(message) { return typeof window.confirm !== "function" || window.confirm(message); }

  function submitServer(form) {
    const data = new FormData(form);
    const name = String(data.get("name") || "").trim();
    const transport = String(data.get("transport") || "stdio");
    const command = String(data.get("command") || "").trim();
    const url = String(data.get("url") || "").trim();
    if (!name) { setError("Server name is required"); return; }
    if (transport === "stdio" && !command) { setError("stdio servers require a command"); return; }
    if (transport === "http" && !url) { setError("http servers require a URL"); return; }
    const args = parseJson(String(data.get("args") || "[]"), "array", "Args");
    const env = parseJson(String(data.get("env") || "{}"), "object", "Env");
    if (args === null || env === null || !confirmAction("Add MCP server '" + name + "'?")) return;
    const config = { name, transport, args, env, confirmed: true };
    if (command) config.command = command;
    if (url) config.url = url;
    setError("");
    send("add_server", config, { pendingKey: "add-server" });
  }

  function serverAction(button) {
    const action = button.dataset.action;
    const name = button.dataset.serverName || "";
    let config = {};
    try { config = JSON.parse(button.dataset.serverConfig || "{}"); } catch (_) { config = { name }; }
    config.name = name;
    config.confirmed = true;
    if (!confirmAction(action.replace("_", " ") + " MCP server '" + name + "'?")) return;
    setError("");
    send(action, config, { pendingKey: button.dataset.pendingKey });
  }

  function callTool(button) {
    const input = button.closest("article").querySelector('[data-role="tool-args"]');
    const args = parseJson(input.value, "object", "Tool parameters");
    const name = button.dataset.toolName || "";
    if (args === null || !confirmAction("Call MCP tool '" + name + "'?")) return;
    setError("");
    send("call_tool", { server: button.dataset.server || "", name, arguments: args, confirmed: true }, { pendingKey: button.dataset.pendingKey, kind: "tool", toolName: name });
  }

  function previewPrompt(button) {
    const input = button.closest("article").querySelector('[data-role="prompt-args"]');
    const args = parseJson(input.value, "object", "Prompt arguments");
    if (args === null) return;
    setError("");
    send("preview_prompt", { server: button.dataset.server || "", name: button.dataset.promptName || "", arguments: args }, { pendingKey: button.dataset.pendingKey, kind: "prompt", promptName: button.dataset.promptName || "" });
  }

  function readResource(button) {
    const uri = button.dataset.uri || "";
    if (!uri) { setError("Resource URI is required"); return; }
    setError("");
    send("read_resource", { server: button.dataset.server || "", uri }, { pendingKey: button.dataset.pendingKey, kind: "resource", uri });
  }

  function showResult(box, title, payload, elapsed) {
    box.replaceChildren();
    box.hidden = false;
    const heading = el("div", "card-title");
    heading.append(el("strong", "", title), el("span", "muted", "elapsed " + elapsed + " ms"));
    box.append(heading, el("pre", "result-content", json(payload)));
    const copy = el("button", "button-secondary", "Copy");
    copy.type = "button";
    copy.addEventListener("click", () => {
      const text = json(payload);
      if (navigator.clipboard && navigator.clipboard.writeText) navigator.clipboard.writeText(text).then(() => { copy.textContent = "Copied"; }, () => { copy.textContent = "Copy failed"; });
      else copy.textContent = "Clipboard unavailable";
    });
    box.appendChild(copy);
  }

  function reply(data, request) {
    const elapsed = Math.max(0, Date.now() - request.startedAt);
    if (data.status === "error") {
      state.loading = false;
      setError(data.reason || "MCP request failed");
      renderAll();
      if (request.kind === "tool") showResult(role("tool-result"), "Tool error", data.payload || { reason: data.reason }, elapsed);
      return;
    }
    setError("");
    if (data.cmd === "snapshot" || data.cmd === "refresh") {
      state.payload = data.payload && typeof data.payload === "object" ? data.payload : {};
      state.loading = false;
      renderAll();
      return;
    }
    if (request.kind === "tool") showResult(role("tool-result"), "Tool result", data.payload || {}, elapsed);
    if (request.kind === "prompt") showResult(role("prompt-result"), "Prompt preview", data.payload || {}, elapsed);
    if (request.kind === "resource") showResult(role("resource-viewer"), "Resource " + request.uri, data.payload || {}, elapsed);
    if (request.pendingKey === "add-server" || request.pendingKey && request.pendingKey.indexOf("server:") === 0) {
      state.loading = true;
      renderAll();
      send("snapshot", undefined, { pendingKey: "snapshot" });
    }
  }

  function asyncEvent(data) {
    if (data.generation !== undefined && data.generation !== generation) return;
    if (data.source !== undefined && data.source !== "native") return;
    if (data.origin !== undefined && data.origin !== "sao.native") return;
    if (!saneSeq(data)) return;
    const payload = data.payload && typeof data.payload === "object" ? data.payload : {};
    state.notifications.unshift({ payload, receivedAt: new Date().toLocaleTimeString() });
    state.notifications = state.notifications.slice(0, MAX_EVENTS);
    renderNotifications();
    if (summary) summary.textContent = "Live MCP notification received";
  }

  window.addEventListener("message", (event) => {
    if (!trusted(event)) return;
    const data = event.data;
    if (data.status === "event" && data.cmd === "notification") { asyncEvent(data); return; }
    if (!saneSeq(data)) return;
    const request = pending.get(data.requestId);
    if (!request || request.requestId !== data.requestId || request.generation !== generation || data.generation !== request.generation) return;
    window.clearTimeout(request.timer);
    pending.delete(data.requestId);
    syncPendingState();
    reply(data, request);
  });

  for (const tab of document.querySelectorAll("[data-tab]")) {
    tab.addEventListener("click", () => selectTab(tab.dataset.tab, false));
    tab.addEventListener("keydown", (event) => {
      const tabs = Array.from(document.querySelectorAll("[data-tab]"));
      const index = tabs.indexOf(tab);
      let next = index;
      if (event.key === "ArrowRight" || event.key === "ArrowDown") next = (index + 1) % tabs.length;
      if (event.key === "ArrowLeft" || event.key === "ArrowUp") next = (index - 1 + tabs.length) % tabs.length;
      if (event.key === "Home") next = 0;
      if (event.key === "End") next = tabs.length - 1;
      if (next === index) return;
      event.preventDefault();
      selectTab(tabs[next].dataset.tab, true);
    });
  }

  role("refresh").dataset.pendingKey = "refresh";
  role("open-kernel-map").dataset.pendingKey = "open-kernel-map";
  role("refresh").addEventListener("click", () => { state.loading = true; renderAll(); send("refresh", undefined, { pendingKey: "refresh" }); });
  role("retry").addEventListener("click", () => { clearPending(); generation += 1; state.loading = true; setError(""); renderAll(); send("snapshot", undefined, { pendingKey: "snapshot" }); });
  role("open-kernel-map").addEventListener("click", () => send("open_kernel_map", undefined, { pendingKey: "open-kernel-map" }));
  role("server-form").addEventListener("submit", (event) => { event.preventDefault(); submitServer(event.currentTarget); });
  document.addEventListener("click", (event) => {
    const button = event.target.closest("[data-action]");
    if (!button) return;
    switch (button.dataset.action) {
      case "call-tool": callTool(button); break;
      case "preview-prompt": previewPrompt(button); break;
      case "read-resource": readResource(button); break;
      case "enable":
      case "disable":
      case "close_server":
      case "reconnect": serverAction(button); break;
      default: break;
    }
  });

  selectTab(state.activeTab, false);
  renderAll();
  send("snapshot", undefined, { pendingKey: "snapshot" });
}());
