// Regression coverage for plugin_layer render signatures.
// Run from repo root with: node tools/web_plugin_layer_signature_selftest.js

const fs = require("fs");
const path = require("path");
const vm = require("vm");

class FakeElement {
  constructor(tag) {
    this.tagName = String(tag || "div").toUpperCase();
    this.children = [];
    this.style = {
      display: "",
      setProperty(name, value) { this[name] = value; },
    };
    this.attributes = {};
    this._innerHTML = "";
    this.className = "";
    this.appendCount = 0;
  }
  appendChild(child) {
    this.children.push(child);
    this.appendCount += 1;
    if (child && child.id) document._byId[child.id] = child;
    return child;
  }
  remove() {
    this.removed = true;
  }
  addEventListener() {}
  setAttribute(name, value) {
    this.attributes[name] = String(value);
  }
  getAttribute(name) {
    return this.attributes[name] || "";
  }
  querySelectorAll() {
    return [];
  }
  set innerHTML(value) {
    this._innerHTML = String(value || "");
    this.children = [];
  }
  get innerHTML() {
    return this._innerHTML;
  }
}

const document = {
  readyState: "loading",
  _byId: {},
  head: new FakeElement("head"),
  body: new FakeElement("body"),
  documentElement: new FakeElement("html"),
  createElement(tag) { return new FakeElement(tag); },
  getElementById(id) { return this._byId[id] || null; },
  addEventListener(name, cb) { this._listener = { name, cb }; },
};

const warns = [];
const window = {
  __SAO_SURFACE__: "selftest",
  console: { warn(...args) { warns.push(args); }, log() {}, error() {} },
  addEventListener() {},
};

const context = {
  window,
  document,
  console: window.console,
  setInterval() { return 1; },
  Promise,
};

const root = path.resolve(__dirname, "..");
const script = fs.readFileSync(path.join(root, "web", "plugin_layer.js"), "utf8");
vm.runInNewContext(script, context, { filename: "plugin_layer.js" });

if (!window.PluginHooks || typeof window.PluginHooks.renderSpec !== "function") {
  throw new Error("PluginHooks.renderSpec was not installed");
}

const mount = new FakeElement("div");
const spec = {
  title: "Circular",
  nodes: [{ type: "text", text: "stable" }],
};
spec.self = spec;

window.PluginHooks.renderSpec(spec, mount, null);
const firstSig = mount.__splg_sig;
const firstAppendCount = mount.appendCount;
window.PluginHooks.renderSpec(spec, mount, null);

if (!firstSig || !String(firstSig).startsWith("[nonjson]")) {
  throw new Error("non-JSON spec did not use fallback signature");
}
if (mount.__splg_sig !== firstSig) {
  throw new Error("fallback signature changed for identical circular spec");
}
if (mount.appendCount !== firstAppendCount) {
  throw new Error("identical circular spec was rendered again");
}
if (warns.length !== 1) {
  throw new Error("fallback warning should be emitted once");
}

spec.nodes[0].text = "changed";
window.PluginHooks.renderSpec(spec, mount, null);
if (mount.__splg_sig === firstSig) {
  throw new Error("fallback signature did not track shallow visible spec changes");
}

console.log("web_plugin_layer_signature_selftest: ok");
