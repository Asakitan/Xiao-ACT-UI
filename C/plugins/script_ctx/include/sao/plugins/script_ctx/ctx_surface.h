// ctx_surface.h — per-language ctx capability table.
//
// Each host "notes" every ctx method/property name it binds (call sites sit in
// the host's ctx-binding code; recording the same name twice is fine — the
// table is a set).  Adapters then check a manifest's declared binds
// (`platform.binds` entries and `runtime_feature:<name>` requires tokens such
// as `ui.canvas`) against the table at load time and log an advisory list of
// unmet names — informational only, never a load failure (matches the legacy
// `runtime_feature:*` tokens being non-dependencies).
//
// Names use the canonical Python surface spelling: method `ui.rgba_frame`,
// property `plugin_id`, etc.  Hosts mark exactly what their ctx object binds.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sao/plugins/loader/plugin_manifest.h"

namespace sao::plugins::script_ctx {

// Record that `language`'s ctx binds `name` (e.g. "ui.canvas", "load_local",
// "plugin_id").  Idempotent, thread-safe.
void ctx_surface_note(loader::engine_kind language, const char* name) noexcept;

// Batch note from a null-terminated list.
void ctx_surface_note_all(loader::engine_kind language,
                          const char* const* names) noexcept;

// True iff `name` was noted for `language`.
bool ctx_surface_has(loader::engine_kind language, const char* name) noexcept;

// Collect every name in `names` not yet noted for `language`.
std::vector<std::string> ctx_surface_missing(
    loader::engine_kind language,
    const std::vector<std::string>& names);

// All noted names for `language` (sorted snapshot).
std::vector<std::string> ctx_surface_report(loader::engine_kind language);

} // namespace sao::plugins::script_ctx
