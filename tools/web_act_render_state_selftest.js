// Static regression checks for ACT WebView render-state preservation.
// Run from repo root with: node tools/web_act_render_state_selftest.js

const fs = require("fs");
const path = require("path");

const root = path.resolve(__dirname, "..");

function read(rel) {
  return fs.readFileSync(path.join(root, rel), "utf8");
}

function assert(cond, message) {
  if (!cond) throw new Error(message);
}

const actionLog = read("web/act_action_log.html");
assert(actionLog.includes("function setRowsHtml(html, preserveScroll)"), "action log must centralize list updates");
assert(actionLog.includes("rowsEl.scrollTop"), "action log must preserve main-list scrollTop");
assert(actionLog.includes("render(data, { preserveScroll: false })"), "action log semantic searches should reset scroll");
assert(actionLog.includes("ActionLog.refresh({ preserveScroll: false })"), "action log page navigation should reset scroll");
assert(actionLog.includes("render(data, { preserveScroll: true })"), "action log copy/refresh path should preserve scroll");

const timeline = read("web/act_timeline_vcr.html");
assert(timeline.includes("var expandedEvents = {}"), "timeline VCR must persist expanded event state");
assert(timeline.includes("function setListHtml(html, preserveScroll)"), "timeline VCR must centralize list updates");
assert(timeline.includes("list.scrollTop"), "timeline VCR must preserve list scrollTop");
assert(timeline.includes("data-key=\""), "timeline VCR event rows must carry stable expansion keys");
assert(timeline.includes("expandedEvents[key] = true"), "timeline VCR must record opened event payloads");
assert(timeline.includes("delete expandedEvents[key]"), "timeline VCR must clear closed event payloads");
assert(timeline.includes("render(data, { preserveScroll: false })"), "timeline VCR filtering should reset scroll");
assert(timeline.includes("render(data, { preserveScroll: true })"), "timeline VCR VCR controls should preserve scroll");

console.log("web_act_render_state_selftest: ok");
