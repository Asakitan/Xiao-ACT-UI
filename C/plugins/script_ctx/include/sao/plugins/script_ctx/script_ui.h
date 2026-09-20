// script_ui.h — shared `ctx.ui.*` spec builder for all script hosts.
//
// Mirror of the legacy Python `act_platform.ui_spec.UI` builder (24 static
// methods).  A host marshals the script call's arguments into a JSON array
// (positional) or JSON object (kwargs, canonical names below), calls
// `script_ui_build`, and marshals the emitted spec node back into its own
// value domain.  The node shape is the *builder output* (pre-normalization);
// `sao_engine_ui_spec_normalize` applies bounds downstream, exactly like the
// old platform did.
//
// Canonical positional signatures (from ui_spec.py):
//   panel(title="", children=None)
//   section(title="", children=None, accent="cyan")
//   card(title="", children=None)
//   row(children=None, align="left")
//   group(children=None)
//   text(text, style="value", align="left")
//   title(text)
//   kv(label, value, style="value")
//   bar(label="", pct=0.0, color="cyan", caption="")
//   slider(label="", id="", value=0.0, lo=0.0, hi=1.0, step=0.01, color="cyan")
//   badge(text, style="muted")
//   divider()
//   spacer(size=8)
//   button(label, action, style="default", payload=None, disabled=False)
//   input(id, value="", placeholder="", input_type="text", width=0)
//   table(columns=None, rows=None, highlight_key="", title="")
//   canvas(width, height, ops=None, bg="body", x=0, y=0, z=0, id="", draggable=False)
//   rgba_frame(id, width, height, frame_rgba_b64, x=0, y=0, z=0,
//              frame_key="", draggable=True, hit_test="alpha", premultiplied=False)
//   rect(x, y, w, h, fill="", outline="", width=0)
//   oval(x, y, w, h, fill="", outline="", width=0)
//   line(x1, y1, x2, y2, fill="value", width=1)
//   ctext(x, y, text, fill="value", size=10, anchor="nw", bold=false)
//
// `children`, `ops`, `columns`, `rows`, `payload` pass through as JSON
// (they are already spec-shaped values the host produced via other builders).
#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace sao::plugins::script_ctx {

// Build one `ctx.ui.*` spec node.
//   method: one of script_ui_methods() (case-sensitive, no `ui.` prefix).
//   args:   JSON array (positional) or JSON object (kwargs); extra keys are
//           ignored, missing keys fall back to the documented defaults.
//   out_node: emitted spec dict on success.
//   out_error: human-readable reason on failure (unknown method / bad args).
// Returns true on success. Never throws.
bool script_ui_build(const char* method,
                     const nlohmann::json& args,
                     nlohmann::json& out_node,
                     std::string& out_error) noexcept;

// The full method list this module implements (UTF-8, sorted).
const char* const* script_ui_methods(std::size_t* out_count) noexcept;

} // namespace sao::plugins::script_ctx
