// runtime_bridge.cpp — provider registry + `ctx.load_local` dispatch.
#include "sao/plugins/script_ctx/runtime_bridge.h"
#include "sao/plugins/loader/plugin_context_lifetime_internal.h"

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <vector>

namespace sao::plugins::script_ctx {
namespace {

std::mutex g_registry_mutex;
std::vector<const script_engine_ops*> g_providers;

std::string lower_ascii(std::string_view s) {
    std::string out(s);
    for (auto& c : out)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return out;
}

std::string extension_of(const std::filesystem::path& p) {
    const std::u8string e8 = p.extension().u8string();
    std::string ext(reinterpret_cast<const char*>(e8.data()), e8.size());
    if (!ext.empty() && ext.front() == '.')
        ext.erase(ext.begin());
    return lower_ascii(ext);
}

bool provider_has_extension(const script_engine_ops* ops, const std::string& ext) {
    if (ops == nullptr || ops->extensions_utf8 == nullptr)
        return false;
    for (const char* const* it = ops->extensions_utf8; *it != nullptr; ++it) {
        if (ext == lower_ascii(*it))
            return true;
    }
    return false;
}

bool known_script_extension(const std::string& ext) {
    return ext == "py" || ext == "lua" || ext == "emma" || ext == "as" || ext == "cs";
}

void set_diagnostic(std::string* out, const char* message) noexcept {
    if (out != nullptr) {
        try { *out = message; } catch (...) { out->clear(); }
    }
}

// u8path() overloads are deprecated in C++20; construct from u8string_view.
std::filesystem::path path_from_utf8(std::string_view utf8) {
    return std::filesystem::path(std::u8string_view(
        reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

// Resolve rel path inside root with containment (mirror of
// libs_vendor_bridge containment rules). Returns empty path on escape.
std::filesystem::path resolve_contained(const std::filesystem::path& root,
                                        const std::filesystem::path& rel) {
    if (rel.empty() || rel.has_root_path())
        return {};
    for (const auto& part : rel)
        if (part.native().find(L':') != std::wstring::npos)
            return {};
    auto base = std::filesystem::weakly_canonical(root);
    if (base.has_relative_path() && base.filename().empty())
        base = base.parent_path();
    const auto full = std::filesystem::weakly_canonical(base / rel);
    auto b = base.begin();
    auto f = full.begin();
    for (; b != base.end(); ++b, ++f)
        if (f == full.end() || *b != *f)
            return {};
    if (f == full.end())
        return {};
    return full;
}

std::string make_logical_name(const char* plugin_id,
                              const std::filesystem::path& file) {
    const std::u8string s8 = file.stem().u8string();
    std::string stem(reinterpret_cast<const char*>(s8.data()), s8.size());
    std::replace(stem.begin(), stem.end(), '-', '_');
    std::replace(stem.begin(), stem.end(), ' ', '_');
    std::string id = plugin_id != nullptr ? plugin_id : "plugin";
    for (auto& c : id)
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
            !(c >= '0' && c <= '9') && c != '_')
            c = '_';
    return "act_plugin_" + id + "__" + stem;
}

} // namespace

int32_t runtime_bridge_register(const script_engine_ops* ops) noexcept {
    try {
        if (ops == nullptr || ops->engine_name_utf8 == nullptr ||
            *ops->engine_name_utf8 == '\0' || ops->extensions_utf8 == nullptr ||
            ops->load_module == nullptr)
            return SAO_ERR_INVALID_ARGUMENT;
        std::lock_guard lock(g_registry_mutex);
        if (std::find(g_providers.begin(), g_providers.end(), ops) !=
            g_providers.end())
            return SAO_OK;
        g_providers.push_back(ops);
        std::sort(g_providers.begin(), g_providers.end(),
                  [](const script_engine_ops* a, const script_engine_ops* b) {
                      return a->priority < b->priority;
                  });
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t runtime_bridge_unregister(const script_engine_ops* ops) noexcept {
    try {
        std::lock_guard lock(g_registry_mutex);
        std::erase(g_providers, ops);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

bool runtime_bridge_is_script_extension(
    const std::string& ext_no_dot_lower) noexcept {
    try {
        if (known_script_extension(ext_no_dot_lower))
            return true;
        std::lock_guard lock(g_registry_mutex);
        for (const auto* ops : g_providers) {
            if (provider_has_extension(ops, ext_no_dot_lower))
                return true;
        }
    } catch (...) {
    }
    return false;
}

int32_t runtime_bridge_resolve_local(const wchar_t* plugin_root_dir,
                                    const char* rel_path_utf8,
                                    std::wstring* out_abs_path,
                                    bool* out_exists,
                                    std::string* out_diag) noexcept {
    if (out_abs_path != nullptr)
        out_abs_path->clear();
    if (out_exists != nullptr)
        *out_exists = false;
    if (out_diag != nullptr)
        out_diag->clear();
    try {
        if (out_abs_path == nullptr || out_exists == nullptr ||
            plugin_root_dir == nullptr || *plugin_root_dir == L'\0' ||
            rel_path_utf8 == nullptr || *rel_path_utf8 == '\0') {
            set_diagnostic(out_diag, "load_local: invalid argument");
            return SAO_ERR_INVALID_ARGUMENT;
        }
        const std::filesystem::path root(plugin_root_dir);
        const auto full = resolve_contained(root, path_from_utf8(rel_path_utf8));
        if (full.empty()) {
            set_diagnostic(out_diag, "load_local: path escapes plugin root or is not relative");
            return SAO_ERR_INVALID_ARGUMENT;
        }
        std::error_code ec;
        const auto status = std::filesystem::status(full, ec);
        if (status.type() == std::filesystem::file_type::not_found &&
            (!ec || ec == std::errc::no_such_file_or_directory)) {
            set_diagnostic(out_diag, "load_local: file not found");
            return SAO_OK;
        }
        if (ec) {
            if (out_diag != nullptr)
                *out_diag = "load_local: path inspection failed: " + ec.message();
            return SAO_ERR_OS_CALL_FAILED;
        }
        if (!std::filesystem::is_regular_file(status)) {
            set_diagnostic(out_diag, "load_local: path is not a regular file");
            return SAO_ERR_INVALID_ARGUMENT;
        }
        *out_abs_path = full.wstring();
        *out_exists = true;
        return SAO_OK;
    } catch (const std::filesystem::filesystem_error& e) {
        set_diagnostic(out_diag, e.what());
        return SAO_ERR_OS_CALL_FAILED;
    } catch (const std::exception& e) {
        set_diagnostic(out_diag, e.what());
        return SAO_ERR_OS_CALL_FAILED;
    } catch (...) {
        set_diagnostic(out_diag, "load_local: internal error");
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t runtime_bridge_load_local(loader::plugin_context_t* ctx,
                                  const char* plugin_id_utf8,
                                  const wchar_t* plugin_root_dir,
                                  const char* rel_path_utf8,
                                  load_local_result* out_kind,
                                  std::shared_ptr<script_module>* out_module,
                                  std::wstring* out_abs_path,
                                  std::string* out_diag) noexcept {
    try {
        if (out_kind != nullptr)
            *out_kind = load_local_result::failed;
        if (out_module != nullptr)
            out_module->reset();
        if (out_abs_path != nullptr)
            out_abs_path->clear();
        if (out_diag != nullptr)
            out_diag->clear();
        if (out_kind == nullptr || out_module == nullptr || ctx == nullptr) {
            if (out_diag != nullptr)
                *out_diag = "load_local: invalid argument";
            return SAO_ERR_INVALID_ARGUMENT;
        }

        loader::context_runtime_lease invocation(ctx);
        if (!invocation) {
            set_diagnostic(out_diag, "load_local: plugin context is retired");
            return SAO_ERR_HANDLE_INVALID;
        }

        std::wstring absolute;
        bool exists = false;
        const int32_t resolved = runtime_bridge_resolve_local(
            plugin_root_dir, rel_path_utf8, &absolute, &exists, out_diag);
        if (resolved != SAO_OK)
            return resolved;
        if (!exists) {
            *out_kind = load_local_result::missing;
            return SAO_OK;
        }
        const std::filesystem::path full(absolute);
        if (out_abs_path != nullptr)
            *out_abs_path = full.wstring();

        const std::string ext = extension_of(full);
        std::vector<const script_engine_ops*> candidates;
        {
            std::lock_guard lock(g_registry_mutex);
            for (const auto* ops : g_providers) {
                if (provider_has_extension(ops, ext))
                    candidates.push_back(ops);
            }
        }
        if (candidates.empty() && !known_script_extension(ext)) {
            *out_kind = load_local_result::path_only;
            return SAO_OK;
        }

        std::string notes;
        for (const auto* ops : candidates) {
            std::string note;
            const int32_t preflight = ops->probe == nullptr ? SAO_OK :
                ops->probe(ctx, full.c_str(), note, ops->user_data);
            if (preflight != SAO_OK) {
                if (preflight != SAO_ERR_NOT_IMPLEMENTED) {
                    if (out_diag != nullptr)
                        *out_diag = std::string(ops->engine_name_utf8) + ": " +
                            (note.empty() ? "preflight failed" : note);
                    return preflight;
                }
                if (!note.empty()) {
                    if (!notes.empty())
                        notes += "; ";
                    notes += std::string(ops->engine_name_utf8) + ": " + note;
                }
                continue;
            }
            std::shared_ptr<script_module> module;
            std::string error;
            const std::string logical =
                make_logical_name(plugin_id_utf8, full);
            const int32_t rc =
                ops->load_module(ctx, full.c_str(), logical, &module, &error,
                                 ops->user_data);
            if (rc == SAO_OK && module != nullptr) {
                if (out_module != nullptr)
                    *out_module = std::move(module);
                *out_kind = load_local_result::module;
                return SAO_OK;
            }
            if (out_diag != nullptr)
                *out_diag = std::string(ops->engine_name_utf8) + ": " +
                    (error.empty() ? "load returned no module" : error);
            return rc == SAO_OK ? SAO_ERR_OS_CALL_FAILED : rc;
        }

        *out_kind = load_local_result::unsupported;
        if (out_diag != nullptr) {
            *out_diag = "load_local: no engine could load '" +
                        std::string(rel_path_utf8) + "'";
            if (!notes.empty())
                *out_diag += " (" + notes + ")";
        }
        return SAO_OK;
    } catch (const std::exception& e) {
        if (out_kind != nullptr)
            *out_kind = load_local_result::failed;
        set_diagnostic(out_diag, e.what());
        return SAO_ERR_OS_CALL_FAILED;
    } catch (...) {
        if (out_kind != nullptr)
            *out_kind = load_local_result::failed;
        set_diagnostic(out_diag, "load_local: internal error");
        return SAO_ERR_OS_CALL_FAILED;
    }
}

} // namespace sao::plugins::script_ctx
