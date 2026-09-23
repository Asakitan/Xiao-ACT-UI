// runtime_bridge.h — `ctx.load_local` dispatch + cross-language module proxies.
//
// Legacy semantic: ``ctx.load_local("helper.py")`` execs a bundled script file
// and returns a *module object* whose attributes are callable from the calling
// language (``candy_helper.backdrop_b64(scale)`` in lua, …).  Non-script files
// (``.json``, ``.ini``…) return the resolved absolute path, matching the v2
// ``sao_plugins_ctx_load_local`` path form.
//
// Dispatch: files route by extension through a registry of *engine providers*
// (`script_engine_ops`).  Hosts register the providers they implement;
// providers are tried in priority order; only an unsupported preflight advances.
// Missing runtimes remain `unsupported`; execution failures return a nonzero status.
//
// Script-module contract: a provider returns `script_module` (refcounted via
// shared_ptr).  The module bound ctx is the *caller's* plugin_context_t —
// helper modules act on behalf of the plugin that loaded them.  The module
// becomes invalid once that context is destroyed.
// Loader-owned retirement drains registrations before releasing mini helpers.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "sao/plugins/loader/plugin_context.h"
#include "sao_plugins/sao_status.h"

namespace sao::plugins::script_ctx {

// Neutral value for cross-language marshalling.  `bytes` carry raw octets in
// `text`; `function` carries a callable accepting positional args.
struct script_value;
using script_value_ptr = std::shared_ptr<script_value>;

struct script_value {
    enum class kind : uint8_t {
        null, boolean, integer, number, string, bytes, list, map, function
    };

    kind k = kind::null;
    bool boolean = false;
    int64_t integer = 0;
    double number = 0.0;
    std::string text;                                            // string/bytes payload
    std::vector<script_value_ptr> items;                         // list/tuple
    std::vector<std::pair<std::string, script_value_ptr>> object; // map (ordered keys)
    // kind == function: positional call → (out_value | out_error)
    std::function<int32_t(const std::vector<script_value_ptr>& args,
                          script_value_ptr* out_value,
                          std::string* out_error)> call;

    static script_value_ptr null_value() { return std::make_shared<script_value>(); }
    static script_value_ptr make_boolean(bool v) {
        auto p = std::make_shared<script_value>();
        p->k = kind::boolean;
        p->boolean = v;
        return p;
    }
    static script_value_ptr make_integer(int64_t v) {
        auto p = std::make_shared<script_value>();
        p->k = kind::integer;
        p->integer = v;
        return p;
    }
    static script_value_ptr make_number(double v) {
        auto p = std::make_shared<script_value>();
        p->k = kind::number;
        p->number = v;
        return p;
    }
    static script_value_ptr make_string(std::string v) {
        auto p = std::make_shared<script_value>();
        p->k = kind::string;
        p->text = std::move(v);
        return p;
    }
    static script_value_ptr make_bytes(std::string v) {
        auto p = std::make_shared<script_value>();
        p->k = kind::bytes;
        p->text = std::move(v);
        return p;
    }
    static script_value_ptr make_list(std::vector<script_value_ptr> v = {}) {
        auto p = std::make_shared<script_value>();
        p->k = kind::list;
        p->items = std::move(v);
        return p;
    }
    static script_value_ptr make_map(
        std::vector<std::pair<std::string, script_value_ptr>> v = {}) {
        auto p = std::make_shared<script_value>();
        p->k = kind::map;
        p->object = std::move(v);
        return p;
    }
    static script_value_ptr make_function(
        std::function<int32_t(const std::vector<script_value_ptr>&,
                              script_value_ptr*, std::string*)> fn) {
        auto p = std::make_shared<script_value>();
        p->k = kind::function;
        p->call = std::move(fn);
        return p;
    }
};

// Opaque module facade.  Providers implement; hosts wrap members per their
// language (e.g. lua: table of closures → call(), python: module object).
struct script_module {
    virtual ~script_module() = default;
    virtual const std::string& module_id() const noexcept = 0;
    virtual std::vector<std::string> member_names() const = 0;
    // get: read a member (may be non-callable data).
    virtual int32_t get(const std::string& name,
                        script_value_ptr* out_value,
                        std::string* out_error) = 0;
    // call: invoke member function `name` with positional args.
    virtual int32_t call(const std::string& name,
                         const std::vector<script_value_ptr>& args,
                         script_value_ptr* out_value,
                         std::string* out_error) = 0;
};

// Engine provider for one extension family.  `ops` storage must outlive the
// registration (static storage is expected).
struct script_engine_ops {
    const char* engine_name_utf8;                                   // "pymini", "lua", …
    uint32_t priority;                    // lower probes first (pymini=10)
    const char* const* extensions_utf8;   // {"py", nullptr} — no dot, lowercase
    // Only SAO_ERR_NOT_IMPLEMENTED permits fallback; other nonzero statuses are terminal.
    int32_t (*probe)(loader::plugin_context_t* ctx,
                  const wchar_t* abs_path,
                  std::string& note,
                  void* user_data) noexcept;
    // Execute the file as a module bound to `ctx`'s plugin session.
    // `logical_name` is the canonical module name ("act_plugin_<id>__<stem>").
    int32_t (*load_module)(loader::plugin_context_t* ctx,
                           const wchar_t* abs_path,
                           const std::string& logical_name,
                           std::shared_ptr<script_module>* out_module,
                           std::string* out_error,
                           void* user_data) noexcept;
    void* user_data;
};

// Provider registry — called by adapter register/unregister paths.
int32_t runtime_bridge_register(const script_engine_ops* ops) noexcept;
int32_t runtime_bridge_unregister(const script_engine_ops* ops) noexcept;

enum class load_local_result : int32_t {
    module = 0,       // *out_module holds a callable module
    path_only = 1,    // non-script payload → *out_abs_path
    missing = 2,      // contained file is absent → *out_diag
    unsupported = 3,  // script file but every provider declined → *out_diag
    failed = 4,       // nonzero status; no module was published
};

// Existing files resolve to an absolute path; only absence returns OK with exists=false.
int32_t runtime_bridge_resolve_local(const wchar_t* plugin_root_dir,
                                    const char* rel_path_utf8,
                                    std::wstring* out_abs_path,
                                    bool* out_exists,
                                    std::string* out_diag) noexcept;

// OK: module, non-script path_only, missing, or unsupported (no accepting provider).
// Invalid paths, I/O/internal errors and execution failures return nonzero + failed.
// A selected provider's load result is terminal, including NOT_IMPLEMENTED.
int32_t runtime_bridge_load_local(loader::plugin_context_t* ctx,
                                  const char* plugin_id_utf8,
                                  const wchar_t* plugin_root_dir,
                                  const char* rel_path_utf8,
                                  load_local_result* out_kind,
                                  std::shared_ptr<script_module>* out_module,
                                  std::wstring* out_abs_path,
                                  std::string* out_diag) noexcept;

// Built-in py/lua/emma/as/cs families remain scripts without registered providers.
bool runtime_bridge_is_script_extension(const std::string& ext_no_dot_lower) noexcept;

} // namespace sao::plugins::script_ctx
