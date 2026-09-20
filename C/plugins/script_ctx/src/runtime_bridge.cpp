// runtime_bridge.cpp — provider registry + `ctx.load_local` dispatch.
#include "sao/plugins/script_ctx/runtime_bridge.h"

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

// u8path() overloads are deprecated in C++20; construct from u8string_view.
std::filesystem::path path_from_utf8(std::string_view utf8) {
    return std::filesystem::path(std::u8string_view(
        reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

// Resolve rel path inside root with containment (mirror of
// libs_vendor_bridge containment rules). Returns empty path on escape.
std::filesystem::path resolve_contained(const std::filesystem::path& root,
                                        const std::filesystem::path& rel) {
    if (rel.empty() || rel.is_absolute())
        return {};
    const auto base = std::filesystem::weakly_canonical(root);
    const auto full = std::filesystem::weakly_canonical(base / rel);
    const auto& bs = base.native();
    const auto& fs = full.native();
    if (bs.empty() || fs.size() < bs.size() + 1 ||
        fs.compare(0, bs.size(), bs) != 0 ||
        (fs[bs.size()] != L'\\' && fs[bs.size()] != L'/')) {
        return {};
    }
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
        if (ops == nullptr || ops->extensions_utf8 == nullptr ||
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
        std::lock_guard lock(g_registry_mutex);
        for (const auto* ops : g_providers) {
            if (provider_has_extension(ops, ext_no_dot_lower))
                return true;
        }
    } catch (...) {
    }
    return false;
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
        if (out_kind == nullptr)
            return SAO_ERR_INVALID_ARGUMENT;
        *out_kind = load_local_result::missing;
        if (out_module != nullptr)
            out_module->reset();
        if (out_abs_path != nullptr)
            out_abs_path->clear();
        if (out_diag != nullptr)
            out_diag->clear();
        if (ctx == nullptr || plugin_root_dir == nullptr ||
            rel_path_utf8 == nullptr || *rel_path_utf8 == '\0') {
            if (out_diag != nullptr)
                *out_diag = "load_local: invalid argument";
            *out_kind = load_local_result::missing;
            return SAO_OK;
        }

        std::filesystem::path root(plugin_root_dir);
        const auto rel = path_from_utf8(rel_path_utf8);
        const auto full = resolve_contained(root, rel);
        if (full.empty() || !std::filesystem::is_regular_file(full)) {
            *out_kind = load_local_result::missing;
            if (out_diag != nullptr)
                *out_diag = "load_local: not found or escapes plugin root: " +
                            std::string(rel_path_utf8);
            return SAO_OK;
        }
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
        if (candidates.empty()) {
            // Non-script payload → path form (v2 semantic). A script-family
            // extension with zero providers also lands here?  No: a provider
            // list is the definition of "script". Files like .json/.ini
            // always take this branch.
            *out_kind = load_local_result::path_only;
            return SAO_OK;
        }

        std::string notes;
        for (const auto* ops : candidates) {
            std::string note;
            bool ok = false;
            if (ops->probe == nullptr) {
                ok = true;
            } else {
                ok = ops->probe(ctx, full.c_str(), note, ops->user_data);
            }
            if (!ok) {
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
            if (!notes.empty())
                notes += "; ";
            notes += std::string(ops->engine_name_utf8) + ": " +
                     (error.empty() ? std::string("load failed") : error);
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
            *out_kind = load_local_result::missing;
        if (out_diag != nullptr)
            *out_diag = std::string("load_local: internal error: ") + e.what();
        return SAO_OK;
    } catch (...) {
        if (out_kind != nullptr)
            *out_kind = load_local_result::missing;
        if (out_diag != nullptr)
            *out_diag = "load_local: internal error";
        return SAO_OK;
    }
}

} // namespace sao::plugins::script_ctx
