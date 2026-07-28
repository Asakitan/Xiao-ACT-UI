// script_path_sandbox.cpp — path-traversal check for plugin-provided scripts.
//
// Phase 14 (Python parity closure) — port of
// act_platform/scripting/base.py::resolve_local_script_path.

#include <filesystem>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace sao::plugins::loader::script_sandbox {

// Return true iff candidate resolves within plugin_base_dir (no ..).
extern "C" int sao_plugins_script_path_sandbox_is_allowed(
    const char* plugin_base_dir_utf8, const char* candidate_utf8) {
    if (plugin_base_dir_utf8 == nullptr || candidate_utf8 == nullptr) return 0;
    std::error_code ec;
    fs::path base = fs::weakly_canonical(fs::path(plugin_base_dir_utf8), ec);
    if (ec) return 0;
    fs::path cand = fs::weakly_canonical(fs::path(candidate_utf8), ec);
    if (ec) return 0;
    // Prefix match on canonical paths.
    auto b_str = base.generic_string();
    auto c_str = cand.generic_string();
    if (c_str.size() < b_str.size()) return 0;
    if (c_str.compare(0, b_str.size(), b_str) != 0) return 0;
    // Must be followed by end-of-string or path separator.
    if (c_str.size() > b_str.size() && c_str[b_str.size()] != '/' &&
        c_str[b_str.size()] != '\\')
        return 0;
    return 1;
}

} // namespace sao::plugins::loader::script_sandbox
