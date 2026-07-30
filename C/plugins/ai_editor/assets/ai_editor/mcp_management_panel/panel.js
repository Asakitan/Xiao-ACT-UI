(function () {
  "use strict";

  const vscode = typeof acquireVsCodeApi === "function" ? acquireVsCodeApi() : null;
  let sequence = 1;

  const summary = document.querySelector('[data-role="summary"]');
  const servers = document.querySelector('[data-role="servers"]');
  const tools = document.querySelector('[data-role="tools"]');

  function send(cmd) {
    const message = { cmd, requestId: "mcp-" + sequence++ };
    if (vscode) {
      vscode.postMessage(message);
    } else if (window.parent && window.parent !== window) {
      window.parent.postMessage(message, "*");
    }
  }

  function card(title, rows) {
    const node = document.createElement("article");
    const heading = document.createElement("h3");
    heading.textContent = title;
    node.appendChild(heading);
    for (const row of rows) {
      const line = document.createElement("p");
      line.textContent = row;
      node.appendChild(line);
    }
    return node;
  }

  function render(payload) {
    const serverItems = payload && Array.isArray(payload.servers) ? payload.servers : [];
    const toolItems = payload && Array.isArray(payload.tools) ? payload.tools : [];
    servers.replaceChildren();
    tools.replaceChildren();
    for (const server of serverItems) {
      const info = server.serverInfo || {};
      servers.appendChild(card(server.name || "unnamed", [
        "transport: " + (server.transport || "unknown"),
        "server: " + (info.name || "unknown"),
        "protocol: " + (server.protocolVersion || "unknown")
      ]));
    }
    for (const tool of toolItems) {
      const rate = tool.rate_limit || {};
      tools.appendChild(card(tool.name || "unnamed", [
        "server: " + (tool.server || "unknown"),
        tool.description || "No description",
        "confirm: " + String(Boolean(tool.requires_confirm)),
        "limit: " + String(rate.max_calls || "-") + " / " + String(rate.window_ms || "-") + " ms"
      ]));
    }
    summary.textContent = serverItems.length + " server(s), " + toolItems.length + " tool(s)";
  }

  window.addEventListener("message", function (event) {
    const data = event && event.data;
    if (!data || typeof data !== "object") {
      return;
    }
    if (data.status === "ok" && (data.cmd === "snapshot" || data.cmd === "refresh")) {
      render(data.payload || {});
    } else if (data.status === "error") {
      summary.textContent = data.reason || "MCP registry error";
    }
  });

  document.querySelector('[data-role="refresh"]').addEventListener("click", function () {
    send("refresh");
  });
  document.querySelector('[data-role="open-kernel-map"]').addEventListener("click", function () {
    send("open_kernel_map");
  });
  send("snapshot");
}());
