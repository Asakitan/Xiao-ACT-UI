// ctx_surface.cpp — per-language ctx capability table.
#include "sao/plugins/script_ctx/ctx_surface.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <set>

namespace sao::plugins::script_ctx {
namespace {

std::size_t language_slot(loader::engine_kind language) noexcept {
    switch (language) {
    case loader::engine_kind::python:      return 0;
    case loader::engine_kind::emma:        return 1;
    case loader::engine_kind::angelscript: return 2;
    case loader::engine_kind::lua:         return 3;
    case loader::engine_kind::csharp:      return 4;
    default:                               return 5;
    }
}

constexpr std::size_t kSlots = 6;
std::array<std::set<std::string>, kSlots> g_surface;
std::mutex g_surface_mutex;

} // namespace

void ctx_surface_note(loader::engine_kind language, const char* name) noexcept {
    try {
        if (name == nullptr || *name == '\0')
            return;
        std::lock_guard lock(g_surface_mutex);
        g_surface[language_slot(language)].emplace(name);
    } catch (...) {
    }
}

void ctx_surface_note_all(loader::engine_kind language,
                          const char* const* names) noexcept {
    if (names == nullptr)
        return;
    for (const char* const* it = names; *it != nullptr; ++it)
        ctx_surface_note(language, *it);
}

bool ctx_surface_has(loader::engine_kind language, const char* name) noexcept {
    try {
        if (name == nullptr || *name == '\0')
            return false;
        std::lock_guard lock(g_surface_mutex);
        return g_surface[language_slot(language)].contains(name);
    } catch (...) {
        return false;
    }
}

std::vector<std::string> ctx_surface_missing(
    loader::engine_kind language,
    const std::vector<std::string>& names) {
    std::vector<std::string> out;
    std::lock_guard lock(g_surface_mutex);
    const auto& bound = g_surface[language_slot(language)];
    for (const auto& n : names)
        if (!n.empty() && !bound.contains(n))
            out.push_back(n);
    return out;
}

std::vector<std::string> ctx_surface_report(loader::engine_kind language) {
    std::lock_guard lock(g_surface_mutex);
    const auto& bound = g_surface[language_slot(language)];
    return {bound.begin(), bound.end()};
}

void ctx_surface_advisory_check(loader::engine_kind language,
                                loader::plugin_context_t* ctx,
                                const loader::plugin_manifest* manifest) noexcept {
    try {
        if (manifest == nullptr)
            return;
        std::vector<std::string> wanted = manifest->platform_binds;
        for (const auto& requirement : manifest->requires_list) {
            if (requirement.rfind("runtime_feature:", 0) == 0)
                wanted.push_back(requirement.substr(16));
        }
        if (wanted.empty())
            return;
        const auto missing = ctx_surface_missing(language, wanted);
        if (missing.empty())
            return;
        std::string message = "ctx surface advisory — unmet binds:";
        for (const auto& name : missing) {
            message += ' ';
            message += name;
        }
        loader::sao_plugins_ctx_log(ctx, message.c_str());
    } catch (...) {
    }
}

} // namespace sao::plugins::script_ctx
