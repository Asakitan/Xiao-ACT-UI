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

const aggregate = read("web/act_aggregate.html");
assert(aggregate.includes("var collapsedSections = {}"), "aggregate must persist collapsed section state");
assert(aggregate.includes("function setContentHtml(html, preserveScroll)"), "aggregate must centralize content updates");
assert(aggregate.includes("content.scrollTop"), "aggregate must preserve main content scrollTop");
assert(aggregate.includes("data-section-key"), "aggregate sections must carry stable collapse keys");
assert(aggregate.includes("collapsedSections[key] = true"), "aggregate must record collapsed sections");
assert(aggregate.includes("delete collapsedSections[key]"), "aggregate must clear reopened sections");
assert(aggregate.includes("rerender() { render(lastData, lastGraph, { preserveScroll: true })"), "aggregate row expansion should preserve scroll");
assert(aggregate.includes("refresh({ preserveScroll: false })"), "aggregate query/source/group changes should reset scroll");

const skillDrilldown = read("web/act_skill_drilldown.html");
assert(skillDrilldown.includes("expandedRefs: {}"), "skill drilldown must persist expanded timeline refs");
assert(skillDrilldown.includes("_setTimelineHtml(html, preserveScroll)"), "skill drilldown must centralize timeline updates");
assert(skillDrilldown.includes("scroller.scrollTop"), "skill drilldown must preserve timeline scrollTop");
assert(skillDrilldown.includes("data-key"), "skill drilldown refs must carry stable expansion keys");
assert(skillDrilldown.includes("this.expandedRefs[key] = true"), "skill drilldown must record opened refs");
assert(skillDrilldown.includes("delete this.expandedRefs[key]"), "skill drilldown must clear closed refs");
assert(skillDrilldown.includes("this.render(status, { preserveScroll: false })"), "skill drilldown filtering/back should reset scroll");
assert(skillDrilldown.includes("this.render(copied, { preserveScroll: true })"), "skill drilldown copy should preserve scroll");

const dps = read("web/dps.html");
assert(!dps.includes('data-uid="\' + Number(entity.uid || 0)'), "DPS rows must not coerce entity uid to Number for data-uid");
assert(!dps.includes('onclick="_openDetail(\' + Number(entity.uid || 0) + \')"'), "DPS row click must not coerce entity uid to Number");
assert(!dps.includes("_liveDetailCache[Number(data.uid || 0)] = data"), "DPS detail cache must not key by numeric uid");
assert(dps.includes("function _uidKey"), "DPS must provide a string uid key helper");
assert(dps.includes("onclick=\"_openDetail(' + _jsArg(uidKey) + ')"), "DPS row click must pass uid through escaped jsArg");
assert(dps.includes("_liveDetailCache[_uidKey(data.uid)] = data"), "DPS detail cache must key by string uid");
assert(!dps.includes("v = Number(v || 0);"), "DPS numeric formatting must not accept non-finite values");
assert(!dps.includes("return Math.round(Number(v || 0) * 100) + '%'"), "DPS percentages must clamp to 0..100");
assert(!dps.includes("Math.round(Number(v || 0))"), "DPS resize clamp must not pass NaN through Number()");
assert(!dps.includes("var barPct = maxVal > 0 ? Math.round(amount / maxVal * 100) : 0;"), "DPS list bars must use clamped widths");
assert(!dps.includes("Math.round(ratio * 100) + '%;background:"), "DPS skill bars must use clamped widths");
assert(dps.includes("function _finiteNum"), "DPS must provide finite numeric normalization");
assert(dps.includes("function _nonNegNum"), "DPS must clamp numeric totals/rates to non-negative finite values");
assert(dps.includes("function _clamp01"), "DPS must clamp percentages to 0..1");
assert(dps.includes("Math.round(_finiteNum(v, lo))"), "DPS resize clamp must use finite fallback before rounding");
assert(dps.includes("function _barPct"), "DPS must centralize bar width clamping");
assert(dps.includes("var barPct = _barPct(amount, maxVal);"), "DPS list rows must render bars through _barPct");
assert(dps.includes("style=\"width:' + _barPct(amount, maxVal) + '%;background:"), "DPS detail skill bars must render through _barPct");

console.log("web_act_render_state_selftest: ok");
