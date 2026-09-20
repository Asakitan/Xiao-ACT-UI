// csmini_host_internal.h — private host registry API shared between
// csmini_loader_adapter.cpp and csmini_bridge.cpp (mirrors pymini_host.h's
// internal role — the public surface lives in
// include/sao/plugins/csmini/csmini_host.h).
#pragma once

#include "csmini_interp.h"

#include "sao/plugins/loader/plugin_context.h"

#include <nlohmann/json.hpp>

namespace sao::plugins::csmini {

// nlohmann::json → CsRef (dict/array/scalars) — defined in csmini_stdlib2.cpp.
CsRef csmini_json_to_cs(const nlohmann::json& j);

// load returns: 0 ok / -1 bad args / -2 already loaded / -3 entry unreadable
// / -4 script error / -5 internal error
int csmini_host_load_plugin(loader::plugin_context_t* ctx,
                            const char* plugin_id,
                            const wchar_t* plugin_root,
                            const wchar_t* entry_rel,
                            const std::vector<std::wstring>& extra_dirs,
                            std::string* out_err);

// hook invoke — 0 ok / 1 hook absent / <0 raised.  `out_ret` variant captures
// the hook's return value (adapter on_unload needs the veto bool).
int csmini_host_call_hook(const char* plugin_id, const char* hook,
                          const char* payload_json, std::string* out_err);
int csmini_host_call_hook_ret(const char* plugin_id, const char* hook,
                              const char* payload_json, CsRef* out_ret,
                              std::string* out_err);

int csmini_host_unload_plugin(const char* plugin_id, std::string* out_err);
bool csmini_host_is_loaded(const char* plugin_id);
std::vector<std::string> csmini_host_loaded_ids();

// dev-fix probe — did the entry file change since load?
int csmini_host_entry_changed(const char* plugin_id, bool* changed);

// bridge exposure — ctx object + entry-global lookup without ownership.
int csmini_host_ctx_object(const char* plugin_id, CsRef* out);
int csmini_host_module_attr(const char* plugin_id, const char* name,
                            CsRef* out);

// sig_raise → "TypeName: message [file:line in fn]" (shared with bridge).
std::string csmini_describe_sig(const sig_raise& sig, interpreter* i);

// script_ctx provider registration — csmini_bridge.cpp.
int csmini_register_script_engine() noexcept;
int csmini_unregister_script_engine() noexcept;

} // namespace sao::plugins::csmini
