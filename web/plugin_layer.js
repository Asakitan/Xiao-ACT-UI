/* SAO plugin render layer — WebView twin of gui_modules/sao_plugin_ui_render.py.
 *
 * Loaded into every SAO WebView window (injected centrally by sao_webview.py).
 * Gives plugins two powers over the page, mirroring the Entity renderer:
 *
 *   1. OVERLAYS  — a declarative ui_spec drawn on top of native content.
 *      Works with ZERO page changes: this script polls the Python bridge for
 *      overlays on the window's surface and paints them into an injected layer.
 *
 *   2. HOOKS / TAKEOVER — a page opts in by routing its render payload through
 *      window.PluginHooks.tap(surface, payload, applyFn). Plugin render hooks
 *      (in Python) may mutate the payload (applyFn re-renders) or fully take the
 *      surface over with an override spec (painted by this layer).
 *
 * The same node vocabulary as ui_spec.py: panel/section/card/row/group + text/
 * kv/bar/badge/divider/spacer/button/table.
 */
(function () {
    "use strict";

    var SURFACE = String(window.__SAO_SURFACE__ || "unknown");
    var POLL_MS = 800;

    function api() { return window.pywebview && window.pywebview.api; }

    function parse(result) {
        if (result == null) return null;
        if (typeof result === "string") {
            try { return JSON.parse(result); } catch (_) { return null; }
        }
        return result;
    }

    // Call a Python bridge method by name; tolerate both pywebview + bridge.cmd.
    function call(name, args) {
        var a = api();
        if (a && typeof a[name] === "function") {
            try { return Promise.resolve(a[name].apply(a, args || [])).then(parse).catch(function () { return null; }); }
            catch (_) { return Promise.resolve(null); }
        }
        if (window.bridge && typeof window.bridge.cmd === "function") {
            var mapped = pluginBridgeCommand(name, args || []);
            return window.bridge.cmd(mapped.name, mapped.payload).then(parse).catch(function () { return null; });
        }
        return Promise.resolve(null);
    }

    function pluginPayloadArg(args, index) {
        if (!args || args.length <= index || args[index] == null) return "";
        return args[index];
    }

    function pluginBridgeCommand(name, args) {
        if (name === "invoke_ui_action") {
            return {
                name: "act.plugins.invoke_ui_action",
                payload: {
                    panel_id: String(args[0] || ""),
                    action_id: String(args[1] || ""),
                    payload: pluginPayloadArg(args, 2)
                }
            };
        }
        if (name === "act_render_overlays") {
            return { name: "act.render.overlays", payload: { surface: String(args[0] || "") } };
        }
        if (name === "act_render_apply_hooks") {
            return {
                name: "act.render.apply_hooks",
                payload: {
                    surface: String(args[0] || ""),
                    payload: pluginPayloadArg(args, 1)
                }
            };
        }
        if (name === "act_render_surfaces") {
            return { name: "act.render.surfaces", payload: {} };
        }
        return { name: "act." + name, payload: { args: args || [] } };
    }

    function esc(v) {
        return String(v == null ? "" : v)
            .replace(/&/g, "&amp;").replace(/</g, "&lt;")
            .replace(/>/g, "&gt;").replace(/"/g, "&quot;");
    }

    function isObjectValue(value) {
        return value && typeof value === "object" && !Array.isArray(value);
    }

    function objectValue(value) {
        return isObjectValue(value) ? value : {};
    }

    function listItems(value) {
        return Array.isArray(value) ? value : [];
    }

    function objectItems(value) {
        return listItems(value).filter(function (item) {
            return isObjectValue(item);
        });
    }

    function finiteNumber(value, fallback, lo, hi) {
        var number = Number(value);
        if (!isFinite(number)) number = Number(fallback || 0);
        if (!isFinite(number)) number = 0;
        if (lo != null) number = Math.max(Number(lo), number);
        if (hi != null) number = Math.min(Number(hi), number);
        return number;
    }

    function finiteInt(value, fallback, lo, hi) {
        return Math.round(finiteNumber(value, fallback, lo, hi));
    }

    // ── spec -> DOM ──────────────────────────────────────────────────────────
    function el(tag, cls, html) {
        var e = document.createElement(tag);
        if (cls) e.className = cls;
        if (html != null) e.innerHTML = html;
        return e;
    }

    function renderNode(node, onAction) {
        node = objectValue(node);
        var t = node.type;
        if (t === "panel" || t === "section" || t === "card" || t === "group" || t === "row") {
            var box = el("div", "splg-" + t + (node.title ? " splg-titled" : ""));
            if (node.accent) box.style.setProperty("--splg-acc", barColor(node.accent));
            if (node.title) {
                var head = el("div", "splg-head");
                head.appendChild(el("span", "splg-rail"));
                head.appendChild(el("span", "splg-title", esc(node.title)));
                box.appendChild(head);
            }
            var inner = el("div", "splg-body splg-" + t + "-body");
            if (t === "row" && node.align) {
                inner.style.justifyContent =
                    { left: "flex-start", center: "center", right: "flex-end" }[node.align] || "flex-start";
            }
            objectItems(node.children).forEach(function (c) {
                var ce = renderNode(c, onAction);
                if (ce) inner.appendChild(ce);
            });
            box.appendChild(inner);
            return box;
        }
        if (t === "text") return el("div", "splg-text splg-st-" + (node.style || "value") + " splg-al-" + (node.align || "left"), esc(node.text));
        if (t === "kv") {
            var kv = el("div", "splg-kv");
            kv.appendChild(el("span", "splg-k", esc(node.label)));
            kv.appendChild(el("span", "splg-v splg-st-" + (node.style || "value"), esc(node.value)));
            return kv;
        }
        if (t === "bar") {
            var wrap = el("div", "splg-barwrap");
            if (node.label || node.caption) {
                var top = el("div", "splg-barlabels");
                top.appendChild(el("span", "splg-k", esc(node.label || "")));
                top.appendChild(el("span", "splg-v", esc(node.caption || "")));
                wrap.appendChild(top);
            }
            var track = el("div", "splg-bar");
            var fill = el("div", "splg-fill");
            fill.style.width = (finiteNumber(node.pct, 0, 0, 1) * 100).toFixed(1) + "%";
            fill.style.background = barColor(node.color);
            track.appendChild(fill);
            wrap.appendChild(track);
            return wrap;
        }
        if (t === "badge") return el("span", "splg-badge splg-bg-" + (node.style || "muted"), esc(node.text));
        if (t === "divider") return el("div", "splg-divider");
        if (t === "spacer") {
            var s = el("div", "splg-spacer");
            s.style.height = finiteInt(node.size, 8, 0, 64) + "px";
            return s;
        }
        if (t === "button") {
            var b = el("button", "splg-btn splg-btn-" + (node.style || "default"), esc(node.label || node.action));
            if (node.disabled) b.disabled = true;
            else b.addEventListener("click", function () { if (onAction) onAction(node.action || "", node.payload || {}); });
            return b;
        }
        if (t === "input") {
            var inp = el("input", "splg-input");
            inp.type = node.input_type === "number" ? "number"
                     : (node.input_type === "password" ? "password" : "text");
            inp.setAttribute("data-splg-id", node.id || "");
            if (node.value != null) inp.value = node.value;
            if (node.placeholder) inp.placeholder = node.placeholder;
            var inputWidth = finiteInt(node.width, 0, 0, 2000);
            if (inputWidth > 0) inp.style.width = inputWidth + "px";
            return inp;
        }
        if (t === "table") return renderTable(node);
        if (t === "canvas") return renderCanvas(node);
        return null;
    }

    function canvasColor(c, dflt) {
        if (!c) return dflt || "";
        c = String(c);
        if (c.charAt(0) === "#") return c;
        var map = {
            white: "#f3f5f7", black: "#1b1f24", grid: "rgba(117,205,255,.20)",
            bg: "", body: "", border: "rgba(117,205,255,.34)", sep: "rgba(117,205,255,.20)",
            header: "rgba(18,31,47,.82)", transparent: "",
            title: "#ffd46f", subtitle: "#73d7ff", value: "#e8f6ff", label: "#9fc0d8",
            muted: "#9fc0d8", gold: "#ffd46f", accent: "#73d7ff", cyan: "#73d7ff",
            ok: "#7df2bf", heal: "#7df2bf", warn: "#ffd46f", bad: "#ff6b82", mono: "#e8f6ff"
        };
        return map.hasOwnProperty(c.toLowerCase()) ? map[c.toLowerCase()] : (dflt || "");
    }

    function renderCanvas(node) {
        node = objectValue(node);
        var w = finiteInt(node.width, 1, 1, 4096);
        var h = finiteInt(node.height, 1, 1, 4096);
        var wrap = el("div", "splg-canvaswrap");
        var cnv = document.createElement("canvas");
        cnv.width = w; cnv.height = h;
        cnv.style.maxWidth = "100%";
        var ctx = cnv.getContext("2d");
        var bg = canvasColor(node.bg, "");
        if (bg) { ctx.fillStyle = bg; ctx.fillRect(0, 0, w, h); }
        objectItems(node.ops).forEach(function (op) {
            var k = op.op, fill, outline;
            if (k === "rect") {
                fill = canvasColor(op.fill, ""); outline = canvasColor(op.outline, "");
                var x = finiteInt(op.x, 0), y = finiteInt(op.y, 0);
                var rw = finiteInt(op.w, 0, 0), rh = finiteInt(op.h, 0, 0);
                if (fill) { ctx.fillStyle = fill; ctx.fillRect(x, y, rw, rh); }
                if (outline) {
                    ctx.strokeStyle = outline;
                    ctx.lineWidth = finiteInt(op.width, 1, 0, 20);
                    ctx.strokeRect(x, y, rw, rh);
                }
            } else if (k === "oval") {
                fill = canvasColor(op.fill, ""); outline = canvasColor(op.outline, "");
                var ox = finiteInt(op.x, 0), oy = finiteInt(op.y, 0);
                var ow = finiteInt(op.w, 0, 0), oh = finiteInt(op.h, 0, 0);
                ctx.beginPath();
                ctx.ellipse(ox + ow / 2, oy + oh / 2, Math.max(0, ow / 2), Math.max(0, oh / 2), 0, 0, 2 * Math.PI);
                if (fill) { ctx.fillStyle = fill; ctx.fill(); }
                if (outline) {
                    ctx.strokeStyle = outline;
                    ctx.lineWidth = finiteInt(op.width, 1, 0, 20);
                    ctx.stroke();
                }
            } else if (k === "line") {
                ctx.strokeStyle = canvasColor(op.fill, "#e8f6ff");
                ctx.lineWidth = finiteInt(op.width, 1, 1, 20);
                ctx.beginPath();
                ctx.moveTo(finiteInt(op.x1, 0), finiteInt(op.y1, 0));
                ctx.lineTo(finiteInt(op.x2, 0), finiteInt(op.y2, 0));
                ctx.stroke();
            } else if (k === "text") {
                ctx.fillStyle = canvasColor(op.fill, "#e8f6ff");
                ctx.font = (op.bold ? "bold " : "") + finiteInt(op.size, 10, 6, 48) + "px 'Segoe UI',sans-serif";
                var a = op.anchor || "nw";
                ctx.textAlign = a.indexOf("e") >= 0 ? "right" : ((a === "n" || a === "s" || a === "center") ? "center" : "left");
                ctx.textBaseline = a.indexOf("s") >= 0 ? "bottom" : ((a === "center" || a === "w" || a === "e") ? "middle" : "top");
                ctx.fillText(String(op.text || ""), finiteInt(op.x, 0), finiteInt(op.y, 0));
            }
        });
        wrap.appendChild(cnv);
        return wrap;
    }

    function renderTable(node) {
        node = objectValue(node);
        var cols = objectItems(node.columns);
        var rows = objectItems(node.rows);
        if (!cols.length && rows.length) cols = Object.keys(rows[0]).map(function (k) { return { key: k, title: k, align: "left" }; });
        var html = "";
        if (node.title) html += '<div class="splg-tabtitle">' + esc(node.title) + "</div>";
        html += '<table class="splg-table"><thead><tr>';
        cols.forEach(function (c) { html += '<th class="splg-al-' + (c.align || "left") + '">' + esc(c.title || c.key) + "</th>"; });
        html += "</tr></thead><tbody>";
        var hk = node.highlight_key || "";
        rows.forEach(function (r) {
            var hi = hk && r[hk] ? " splg-row-hi" : "";
            html += '<tr class="' + hi + '">';
            cols.forEach(function (c) {
                var v = r[c.key];
                html += '<td class="splg-al-' + (c.align || "left") + '">' + esc(v == null ? "" : v) + "</td>";
            });
            html += "</tr>";
        });
        html += "</tbody></table>";
        return el("div", "splg-tablewrap", html);
    }

    function barColor(c) {
        return {
            cyan: "#73d7ff", accent: "#73d7ff", gold: "#ffd46f",
            ok: "#7df2bf", heal: "#7df2bf", warn: "#ffd46f", bad: "#ff6b82"
        }[c] || "#73d7ff";
    }

    function specSignature(value) {
        try { return JSON.stringify(value || {}); }
        catch (_) { return String(Date.now()); }
    }

    function hasRenderableSpec(spec) {
        spec = objectValue(spec);
        return !!(spec.title || objectItems(spec.nodes).length);
    }

    // Collect live text-input values within one panel mount into {id: value}.
    function collectInputs(root) {
        var out = {};
        if (!root || !root.querySelectorAll) return out;
        var list = root.querySelectorAll("input[data-splg-id]");
        for (var i = 0; i < list.length; i++) {
            var id = list[i].getAttribute("data-splg-id");
            if (id) out[id] = list[i].value;
        }
        return out;
    }

    function renderSpec(spec, mount, onAction) {
        spec = objectValue(spec);
        var nodes = objectItems(spec.nodes);
        if (!nodes.length && !spec.title) {
            spec = { nodes: [{ type: "text", text: "(empty)", style: "muted", align: "left" }] };
            nodes = objectItems(spec.nodes);
        }
        var sig = specSignature(spec);
        if (mount.__splg_sig === sig && mount.__splg_rendered
                && mount.__splg_on_action === onAction) return;
        // Snapshot live inputs: the innerHTML teardown below would otherwise drop
        // focus + typed text every poll/redraw (Tk reconciles in place; here we
        // must restore by hand to keep 1:1 parity with the Entity renderer).
        var prev = {};
        if (mount.querySelectorAll) {
            var existing = mount.querySelectorAll("input[data-splg-id]");
            for (var i = 0; i < existing.length; i++) {
                var pid = existing[i].getAttribute("data-splg-id");
                if (!pid) continue;
                prev[pid] = { v: existing[i].value, f: (document.activeElement === existing[i]),
                              s: existing[i].selectionStart, e: existing[i].selectionEnd };
            }
        }
        // Wrap onAction so a button fire carries the panel's input values — only
        // when inputs exist, so input-less panels keep their exact payload.
        var wrapped = onAction ? function (action, payload) {
            payload = payload || {};
            var inputs = collectInputs(mount);
            if (Object.keys(inputs).length) payload.inputs = inputs;
            onAction(action, payload);
        } : null;
        mount.innerHTML = "";
        if (spec.title) mount.appendChild(el("div", "splg-text splg-st-title", esc(spec.title)));
        nodes.forEach(function (n) {
            try { var e = renderNode(n, wrapped); if (e) mount.appendChild(e); }
            catch (_) { mount.appendChild(el("div", "splg-text splg-st-bad", "[render error]")); }
        });
        // Restore typed text / focus on inputs that survived the rebuild.
        var rebuilt = mount.querySelectorAll ? mount.querySelectorAll("input[data-splg-id]") : [];
        for (var j = 0; j < rebuilt.length; j++) {
            var rid = rebuilt[j].getAttribute("data-splg-id");
            var p = prev[rid];
            if (!p) continue;
            var seed = rebuilt[j].value;                   // server value from the new spec
            if (p.f || seed === "") rebuilt[j].value = p.v; // clobber guard: keep user text
            if (p.f) { rebuilt[j].focus(); try { rebuilt[j].setSelectionRange(p.s, p.e); } catch (_) {} }
        }
        mount.__splg_sig = sig;
        mount.__splg_on_action = onAction;
        mount.__splg_rendered = true;
    }

    // ── injected stylesheet (SAO cyan/gold theme, scoped to .splg-*) ─────────
    function injectStyle() {
        if (document.getElementById("sao-plugin-layer-style")) return;
        var css = [
            "#sao-plugin-layer{position:fixed;left:0;right:0;top:0;z-index:99990;pointer-events:none;padding:6px 8px;}",
            "#sao-plugin-layer .splg-card,#sao-plugin-layer .splg-section{pointer-events:auto;}",
            "#sao-plugin-override{position:fixed;inset:0;z-index:99995;overflow:auto;padding:14px;",
            "  background:linear-gradient(180deg,rgba(10,16,24,.96),rgba(8,12,20,.93));color:#e8f6ff;",
            "  font-family:'Segoe UI','Microsoft YaHei',sans-serif;}",
            ".splg-section,.splg-card{border:1px solid rgba(117,205,255,.34);border-left:3px solid var(--splg-acc,#73d7ff);",
            "  border-radius:9px;background:rgba(18,31,47,.82);box-shadow:inset 0 0 16px rgba(117,205,255,.06);",
            "  margin:6px 0;padding:8px 10px;color:#e8f6ff;}",
            ".splg-row-body{display:flex;gap:12px;flex-wrap:wrap;align-items:center;}",
            ".splg-head{display:flex;align-items:center;gap:6px;margin-bottom:4px;}",
            ".splg-rail{width:3px;height:14px;background:var(--splg-acc,#73d7ff);border-radius:2px;}",
            ".splg-title{color:#ffd46f;font-weight:700;font-size:13px;letter-spacing:.03em;}",
            ".splg-text{font-size:13px;line-height:1.5;margin:2px 0;}",
            ".splg-st-title{color:#ffd46f;font-weight:700;font-size:15px;}",
            ".splg-st-subtitle{color:#73d7ff;font-weight:600;}",
            ".splg-st-value{color:#e8f6ff;}.splg-st-label,.splg-st-muted{color:#9fc0d8;}",
            ".splg-st-ok{color:#7df2bf;}.splg-st-warn{color:#ffd46f;}.splg-st-bad{color:#ff6b82;}",
            ".splg-st-gold{color:#ffd46f;}.splg-st-accent{color:#73d7ff;}.splg-st-mono{font-family:Consolas,monospace;}",
            ".splg-al-left{text-align:left;}.splg-al-center{text-align:center;}.splg-al-right{text-align:right;}",
            ".splg-kv{display:flex;justify-content:space-between;gap:10px;font-size:12px;margin:2px 0;}",
            ".splg-k{color:#9fc0d8;}.splg-v{color:#e8f6ff;font-weight:600;}",
            ".splg-barwrap{margin:4px 0;}.splg-barlabels{display:flex;justify-content:space-between;font-size:11px;color:#9fc0d8;margin-bottom:2px;}",
            ".splg-bar{height:8px;border-radius:6px;background:rgba(120,150,170,.25);overflow:hidden;}",
            ".splg-fill{height:100%;border-radius:6px;transition:width .25s ease;}",
            ".splg-badge{display:inline-block;border:1px solid currentColor;border-radius:999px;padding:1px 7px;font-size:10px;font-weight:700;margin:2px 4px 2px 0;}",
            ".splg-bg-ok{color:#7df2bf;}.splg-bg-warn{color:#ffd46f;}.splg-bg-bad{color:#ff6b82;}.splg-bg-gold{color:#ffd46f;}.splg-bg-accent{color:#73d7ff;}.splg-bg-muted{color:#9fc0d8;}",
            ".splg-divider{height:1px;background:rgba(117,205,255,.22);margin:6px 0;}",
            ".splg-btn{pointer-events:auto;border:1px solid rgba(117,205,255,.5);background:linear-gradient(180deg,rgba(61,143,201,.42),rgba(19,48,74,.58));",
            "  color:#e8f6ff;border-radius:7px;padding:5px 10px;font-size:12px;font-weight:600;cursor:pointer;margin:4px 6px 4px 0;}",
            ".splg-btn:hover{box-shadow:0 0 12px rgba(107,214,255,.3);}",
            ".splg-btn-primary{border-color:rgba(255,214,117,.74);color:#fff8da;}.splg-btn-danger{border-color:rgba(255,107,130,.7);}",
            ".splg-btn:disabled{opacity:.5;cursor:default;}",
            ".splg-input{pointer-events:auto;border:1px solid rgba(117,205,255,.5);background:rgba(8,14,22,.55);",
            "  color:#e8f6ff;border-radius:7px;padding:5px 9px;font-size:12px;margin:4px 6px 4px 0;min-width:120px;}",
            ".splg-input:focus{border-color:rgba(255,214,117,.74);outline:none;box-shadow:0 0 10px rgba(107,214,255,.25);}",
            ".splg-input::placeholder{color:#9fc0d8;}",
            ".splg-table{width:100%;border-collapse:collapse;font-size:12px;}",
            ".splg-tablewrap{max-width:100%;overflow-x:auto;margin:4px 0;}",
            ".splg-table th{color:#9fc0d8;font-weight:700;text-align:left;padding:2px 6px;border-bottom:1px solid rgba(117,205,255,.2);}",
            ".splg-table td{color:#e8f6ff;padding:2px 6px;}.splg-row-hi td{color:#ffd46f;font-weight:700;}",
            ".splg-tabtitle{color:#9fc0d8;font-weight:700;font-size:12px;margin:4px 0 2px;}",
            ".splg-canvaswrap{margin:4px 0;overflow-x:auto;}.splg-canvaswrap canvas{display:block;border-radius:6px;}"
        ].join("\n");
        var st = el("style");
        st.id = "sao-plugin-layer-style";
        st.textContent = css;
        (document.head || document.documentElement).appendChild(st);
    }

    // ── overlay layer (universal, no page edits required) ────────────────────
    function overlayMount() {
        var m = document.getElementById("sao-plugin-layer");
        if (!m) { m = el("div"); m.id = "sao-plugin-layer"; document.body.appendChild(m); }
        return m;
    }

    function overlayAction(panelId) {
        return function (action, payload) {
            if (!panelId) return;
            call("invoke_ui_action", [panelId, action, JSON.stringify(payload || {})]).then(refreshOverlays);
        };
    }

    function refreshOverlays() {
        if (!document.body) return;
        return call("act_render_overlays", [SURFACE]).then(function (data) {
            var mount = overlayMount();
            var overlays = objectItems(objectValue(data).overlays);
            var sig = specSignature(overlays);
            if (!overlays.length) {
                if (mount.__splg_overlay_sig !== sig) mount.innerHTML = "";
                mount.__splg_overlay_sig = sig;
                mount.style.display = "none";
                return;
            }
            if (mount.__splg_overlay_sig === sig && mount.style.display !== "none") return;
            mount.__splg_overlay_sig = sig;
            mount.style.display = "block";
            mount.innerHTML = "";
            overlays.forEach(function (ov) {
                var card = el("div", "splg-section");
                renderSpec(objectValue(ov.spec), card, overlayAction(ov.panel_id || (objectValue(ov.spec).panel_id)));
                mount.appendChild(card);
            });
        });
    }

    // ── override layer (full takeover from a render hook) ────────────────────
    function showOverride(spec) {
        var ov = document.getElementById("sao-plugin-override");
        if (!hasRenderableSpec(spec)) {
            if (ov) {
                ov.__splg_override_sig = "";
                ov.remove();
            }
            return;
        }
        if (!ov) { ov = el("div"); ov.id = "sao-plugin-override"; document.body.appendChild(ov); }
        var sig = specSignature(spec);
        if (ov.__splg_override_sig === sig) return;
        ov.__splg_override_sig = sig;
        renderSpec(spec, ov, null);
    }

    // ── PluginHooks public API ───────────────────────────────────────────────
    var PluginHooks = {
        get surface() { return SURFACE; },
        setSurface: function (id) { SURFACE = String(id || SURFACE); },
        renderSpec: renderSpec,
        refresh: function () { refreshOverlays(); },

        // Async: resolve {payload, override} after running Python render hooks.
        process: function (surface, payload) {
            return call("act_render_apply_hooks", [surface || SURFACE, JSON.stringify(payload || {})])
                .then(function (data) {
                    data = objectValue(data);
                    if (!data || !data.ok) return { payload: payload, override: null };
                    return { payload: data.payload != null ? data.payload : payload, override: data.override || null };
                });
        },

        // Sync-friendly: return payload now; re-apply async if plugins change it.
        tap: function (surface, payload, applyFn) {
            PluginHooks.process(surface, payload).then(function (res) {
                if (res.override) { showOverride(res.override); }
                else { showOverride(null); }
                if (res.payload && applyFn && res.payload !== payload) {
                    try { applyFn(res.payload); } catch (_) {}
                }
            });
            return payload;
        }
    };
    window.PluginHooks = PluginHooks;

    // ── boot ─────────────────────────────────────────────────────────────────
    function boot() {
        injectStyle();
        refreshOverlays();
        setInterval(refreshOverlays, POLL_MS);
    }
    if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", boot);
    else boot();
    window.addEventListener("pywebviewready", refreshOverlays);
})();
