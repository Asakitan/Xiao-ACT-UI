#!/usr/bin/env node
"use strict";

const childProcess = require("child_process");
const path = require("path");
const readline = require("readline");

const runnerPath = path.join(__dirname, "browser_smoke_python_bridge.py");

function pythonCandidates() {
  const explicit = String(process.env.SAO_BROWSER_SMOKE_PYTHON || "").trim();
  if (explicit) return [{ command: explicit, args: [], source: "SAO_BROWSER_SMOKE_PYTHON" }];
  const candidates = [];
  const configured = String(process.env.PYTHON || "").trim();
  if (configured) candidates.push({ command: configured, args: [], source: "PYTHON" });
  candidates.push({ command: "python", args: [], source: "PATH" });
  if (process.platform === "win32") candidates.push({ command: "py", args: ["-3"], source: "py launcher" });
  return candidates;
}

function discoverPythonRuntime() {
  const diagnostics = [];
  for (const candidate of pythonCandidates()) {
    const result = childProcess.spawnSync(
      candidate.command,
      [...candidate.args, runnerPath, "--check"],
      {
        cwd: __dirname,
        encoding: "utf8",
        timeout: 30000,
        windowsHide: true,
        env: { ...process.env, PYTHONUTF8: "1", PYTHONIOENCODING: "utf-8" }
      }
    );
    if (result.status === 0) {
      return candidate;
    }
    const detail = String(result.stderr || result.stdout || result.error || "unknown error").trim();
    diagnostics.push(candidate.source + ": " + detail.slice(0, 800));
  }
  throw new Error(
    "Python Playwright fallback is unavailable or could not launch Chromium/Edge. " +
    diagnostics.join(" | ")
  );
}

const pythonRuntime = discoverPythonRuntime();

function functionSource(value, operation) {
  if (typeof value !== "function") {
    throw new TypeError(operation + " requires a JavaScript function");
  }
  return value.toString();
}

class PythonBridgeClient {
  constructor(runtime) {
    this.runtime = runtime;
    this.nextId = 1;
    this.pending = new Map();
    this.stderr = "";
    this.closed = false;
    this.child = childProcess.spawn(
      runtime.command,
      [...runtime.args, runnerPath],
      {
        cwd: __dirname,
        windowsHide: true,
        stdio: ["pipe", "pipe", "pipe"],
        env: { ...process.env, PYTHONUTF8: "1", PYTHONIOENCODING: "utf-8" }
      }
    );
    this.lines = readline.createInterface({ input: this.child.stdout });
    this.lines.on("line", line => this._onLine(line));
    this.child.stderr.on("data", chunk => {
      this.stderr = (this.stderr + chunk.toString("utf8")).slice(-8000);
    });
    this.child.on("error", error => this._rejectAll(error));
    this.child.on("exit", code => {
      this.closed = true;
      if (this.pending.size) {
        const suffix = this.stderr.trim() ? ": " + this.stderr.trim() : "";
        this._rejectAll(new Error("Python Playwright bridge exited with code " + code + suffix));
      }
    });
  }

  _onLine(line) {
    let payload;
    try {
      payload = JSON.parse(line);
    } catch (error) {
      this._rejectAll(new Error("Invalid Python Playwright bridge response: " + line));
      return;
    }
    const pending = this.pending.get(payload.id);
    if (!pending) return;
    this.pending.delete(payload.id);
    if (payload.ok) {
      pending.resolve(payload.result);
      return;
    }
    const detail = payload.traceback ? "\n" + payload.traceback : "";
    pending.reject(new Error(String(payload.error || "Python Playwright bridge request failed") + detail));
  }

  _rejectAll(error) {
    for (const pending of this.pending.values()) pending.reject(error);
    this.pending.clear();
  }

  request(operation, params = {}) {
    if (this.closed) return Promise.reject(new Error("Python Playwright bridge is closed"));
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      const payload = JSON.stringify({ id, operation, params });
      this.child.stdin.write(payload + "\n", "utf8", error => {
        if (!error) return;
        this.pending.delete(id);
        reject(error);
      });
    });
  }

  dispose() {
    if (this.closed) return;
    this.closed = true;
    this.lines.close();
    this.child.stdin.end();
    this.child.kill();
    this._rejectAll(new Error("Python Playwright bridge disposed"));
  }
}

class PythonPage {
  constructor(client) {
    this.client = client;
  }

  on(eventName, _handler) {
    if (eventName !== "pageerror") {
      throw new Error("Unsupported page event in Python Playwright bridge: " + eventName);
    }
    return this;
  }

  addInitScript(script) {
    return this.client.request("add_init_script", { source: functionSource(script, "addInitScript") });
  }

  goto(url, options = {}) {
    return this.client.request("goto", { url: String(url), waitUntil: options.waitUntil || "load" });
  }

  waitForFunction(fn, arg, options = {}) {
    const params = { source: functionSource(fn, "waitForFunction"), timeout: options.timeout || 30000 };
    if (arg !== undefined) params.arg = arg;
    return this.client.request("wait_for_function", params);
  }

  evaluate(fn, arg) {
    const params = { source: functionSource(fn, "evaluate") };
    if (arg !== undefined) params.arg = arg;
    return this.client.request("evaluate", params);
  }

  waitForTimeout(timeout) {
    return this.client.request("wait_for_timeout", { timeout: Number(timeout) || 0 });
  }

  screenshot(options = {}) {
    return this.client.request("screenshot", {
      path: options.path,
      fullPage: Boolean(options.fullPage)
    });
  }
}

class PythonBrowser {
  constructor(client) {
    this.client = client;
  }

  async newPage(options = {}) {
    await this.client.request("new_page", { viewport: options.viewport });
    return new PythonPage(this.client);
  }

  async close() {
    try {
      await this.client.request("close");
    } finally {
      this.client.dispose();
    }
  }
}

async function launch(options = {}) {
  const client = new PythonBridgeClient(pythonRuntime);
  try {
    await client.request("launch", options);
    console.log(
      "INFO browser-smoke-runtime=python-playwright python-source=" + pythonRuntime.source
    );
    return new PythonBrowser(client);
  } catch (error) {
    client.dispose();
    throw error;
  }
}

module.exports = {
  chromium: { launch },
  _runtime: { kind: "python-playwright", source: pythonRuntime.source }
};
