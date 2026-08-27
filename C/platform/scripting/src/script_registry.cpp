#include "sao/scripting/script_registry.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "script_internal.h"

namespace {

struct RegistryEntry {
    std::string name;
    SaoScriptEngineVTable vtable{};
    size_t active_leases = 0;
};

std::recursive_mutex g_registry_mutex;
std::vector<std::unique_ptr<RegistryEntry>> g_entries;
std::vector<std::unique_ptr<RegistryEntry>> g_retired_entries;

bool valid_language(int32_t language) noexcept {
    return language >= SAO_SCRIPT_LANG_PYTHON &&
           language <= SAO_SCRIPT_LANG_CSHARP;
}

bool valid_vtable(const SaoScriptEngineVTable* vtable) noexcept {
    if (vtable == nullptr || vtable->name_utf8 == nullptr ||
        vtable->name_utf8[0] == '\0' || !valid_language(vtable->language) ||
        vtable->abi_version != SAO_SCRIPTING_ABI_VERSION) {
        return false;
    }
    const bool legacy = vtable->engine_init != nullptr &&
                        vtable->engine_shutdown != nullptr &&
                        vtable->context_create != nullptr &&
                        vtable->context_destroy != nullptr &&
                        vtable->context_run != nullptr &&
                        vtable->context_call != nullptr;
    const bool unified = vtable->struct_size >=
                             offsetof(SaoScriptEngineVTable, last_error) +
                                 sizeof(vtable->last_error) &&
                         vtable->create != nullptr &&
                         vtable->destroy != nullptr &&
                         vtable->load != nullptr &&
                         vtable->unload != nullptr &&
                         vtable->run != nullptr && vtable->invoke != nullptr;
    if (!legacy && !unified) return false;
    return vtable->struct_size == 0 ||
           vtable->struct_size >= sao::scripting::internal::kLegacyVTableSize;
}

SaoScriptEngineVTable copy_vtable(
    const SaoScriptEngineVTable& source) noexcept {
    SaoScriptEngineVTable copy{};
    const size_t copy_size = source.struct_size == 0
                                 ? sao::scripting::internal::kLegacyVTableSize
                                 : std::min<size_t>(source.struct_size,
                                                    sizeof(copy));
    std::memcpy(&copy, &source, copy_size);
    copy.struct_size = static_cast<uint32_t>(copy_size);
    return copy;
}

RegistryEntry* find_entry_locked(int32_t language) noexcept {
    const auto found = std::find_if(
        g_entries.begin(), g_entries.end(),
        [language](const std::unique_ptr<RegistryEntry>& entry) {
            return entry->vtable.language == language;
        });
    return found == g_entries.end() ? nullptr : found->get();
}

RegistryEntry* find_provider_entry_locked(
    const SaoScriptEngineVTable& vtable) noexcept {
    const auto matches = [&vtable](const std::unique_ptr<RegistryEntry>& entry) {
        return entry->vtable.name_utf8 == vtable.name_utf8 &&
               entry->vtable.language == vtable.language &&
               entry->vtable.user_data == vtable.user_data;
    };
    for (const auto& entry : g_entries) {
        if (matches(entry)) return entry.get();
    }
    for (const auto& entry : g_retired_entries) {
        if (matches(entry)) return entry.get();
    }
    return nullptr;
}

void reclaim_retired_locked() noexcept {
    g_retired_entries.erase(
        std::remove_if(g_retired_entries.begin(), g_retired_entries.end(),
                       [](const std::unique_ptr<RegistryEntry>& entry) {
                           return entry->active_leases == 0;
                       }),
        g_retired_entries.end());
}

void release_registry_lease(const SaoScriptEngineVTable& vtable) noexcept {
    try {
        std::lock_guard lock(g_registry_mutex);
        RegistryEntry* entry = find_provider_entry_locked(vtable);
        if (entry != nullptr && entry->active_leases != 0) {
            --entry->active_leases;
        }
        reclaim_retired_locked();
    } catch (...) {
    }
}

sao_status_t check_available(const SaoScriptEngineVTable& vtable) noexcept {
    if (!sao::scripting::internal::has_provider_field(
            vtable, offsetof(SaoScriptEngineVTable, available),
            sizeof(vtable.available)) ||
        vtable.available == nullptr) {
        return SAO_STATUS_OK;
    }
    try {
        return vtable.available(vtable.user_data)
                   ? SAO_STATUS_OK
                   : SAO_STATUS_ERR_SCRIPT_UNSUPPORTED;
    } catch (...) {
        return SAO_STATUS_ERR_SCRIPT_PROVIDER_EXCEPTION;
    }
}

}  // namespace

namespace sao::scripting::internal {

bool has_provider_field(const SaoScriptEngineVTable& vtable,
                        size_t field_offset,
                        size_t field_size) noexcept {
    return vtable.struct_size >= field_offset &&
           vtable.struct_size - field_offset >= field_size;
}

sao_status_t acquire_provider(int32_t language,
                              SaoScriptEngineVTable* out_vtable) noexcept {
    if (out_vtable == nullptr || !valid_language(language)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_vtable = {};
    {
        std::lock_guard lock(g_registry_mutex);
        RegistryEntry* entry = find_entry_locked(language);
        if (entry == nullptr) return SAO_STATUS_ERR_SCRIPT_UNSUPPORTED;
        *out_vtable = entry->vtable;
        ++entry->active_leases;
    }

    const auto rollback = [&] {
        release_registry_lease(*out_vtable);
        *out_vtable = {};
    };
    const sao_status_t available_status = check_available(*out_vtable);
    if (available_status != SAO_STATUS_OK) {
        rollback();
        return available_status;
    }
    if (has_provider_field(*out_vtable,
                           offsetof(SaoScriptEngineVTable, retain),
                           sizeof(out_vtable->retain)) &&
        out_vtable->retain != nullptr) {
        try {
            out_vtable->retain(out_vtable->user_data);
        } catch (...) {
            rollback();
            return SAO_STATUS_ERR_SCRIPT_PROVIDER_EXCEPTION;
        }
    }
    return SAO_STATUS_OK;
}

void release_provider(const SaoScriptEngineVTable& vtable) noexcept {
    const bool has_release =
        has_provider_field(vtable, offsetof(SaoScriptEngineVTable, release),
                           sizeof(vtable.release)) &&
        vtable.release != nullptr;
    try {
        if (has_release) vtable.release(vtable.user_data);
    } catch (...) {
    }
    release_registry_lease(vtable);
}

}  // namespace sao::scripting::internal

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_registry_register(
    const SaoScriptEngineVTable* vtable) {
    if (!valid_vtable(vtable)) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        auto replacement = std::make_unique<RegistryEntry>();
        replacement->name = vtable->name_utf8;
        replacement->vtable = copy_vtable(*vtable);
        replacement->vtable.name_utf8 = replacement->name.c_str();
        std::lock_guard lock(g_registry_mutex);
        reclaim_retired_locked();
        const auto found = std::find_if(
            g_entries.begin(), g_entries.end(),
            [vtable](const std::unique_ptr<RegistryEntry>& entry) {
                return entry->vtable.language == vtable->language;
            });
        if (found == g_entries.end()) {
            g_entries.push_back(std::move(replacement));
        } else {
            if (g_retired_entries.size() == g_retired_entries.max_size()) {
                return SAO_STATUS_ERR_UNKNOWN;
            }
            g_retired_entries.push_back(std::move(*found));
            *found = std::move(replacement);
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_registry_find(
    int32_t language, const SaoScriptEngineVTable** out_vtable) {
    if (out_vtable == nullptr || !valid_language(language)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_vtable = nullptr;
    thread_local SaoScriptEngineVTable snapshot{};
    thread_local std::string snapshot_name;
    {
        std::lock_guard lock(g_registry_mutex);
        RegistryEntry* entry = find_entry_locked(language);
        if (entry == nullptr) return SAO_STATUS_ERR_SCRIPT_UNSUPPORTED;
        snapshot = entry->vtable;
        snapshot_name = entry->name;
        snapshot.name_utf8 = snapshot_name.c_str();
    }
    const sao_status_t status = check_available(snapshot);
    if (status != SAO_STATUS_OK) return status;
    *out_vtable = &snapshot;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_registry_enumerate(
    const SaoScriptEngineVTable** out_vtables,
    size_t max_vtables,
    size_t* out_count) {
    if (out_count == nullptr || (out_vtables == nullptr && max_vtables != 0)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        std::vector<SaoScriptEngineVTable> snapshots;
        std::vector<std::string> names;
        {
            std::lock_guard lock(g_registry_mutex);
            *out_count = g_entries.size();
            if (out_vtables == nullptr || max_vtables < g_entries.size()) {
                return g_entries.empty() ? SAO_STATUS_OK
                                         : SAO_STATUS_ERR_BUFFER_TOO_SMALL;
            }
            snapshots.reserve(g_entries.size());
            names.reserve(g_entries.size());
            for (const auto& entry : g_entries) {
                snapshots.push_back(entry->vtable);
                names.push_back(entry->name);
            }
        }
        thread_local std::vector<SaoScriptEngineVTable> thread_vtables;
        thread_local std::vector<std::string> thread_names;
        thread_vtables = std::move(snapshots);
        thread_names = std::move(names);
        for (size_t index = 0; index < thread_vtables.size(); ++index) {
            thread_vtables[index].name_utf8 = thread_names[index].c_str();
            out_vtables[index] = &thread_vtables[index];
        }
        return SAO_STATUS_OK;
    } catch (...) {
        *out_count = 0;
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_registry_unregister(
    int32_t language) {
    if (!valid_language(language)) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard lock(g_registry_mutex);
        reclaim_retired_locked();
        const auto found = std::find_if(
            g_entries.begin(), g_entries.end(),
            [language](const std::unique_ptr<RegistryEntry>& entry) {
                return entry->vtable.language == language;
            });
        if (found == g_entries.end()) return SAO_STATUS_ERR_SCRIPT_UNSUPPORTED;
        if (g_retired_entries.size() == g_retired_entries.max_size()) {
            return SAO_STATUS_ERR_UNKNOWN;
        }
        g_retired_entries.reserve(g_retired_entries.size() + 1);
        g_retired_entries.push_back(std::move(*found));
        g_entries.erase(found);
        reclaim_retired_locked();
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_SCRIPTING_CALL
sao_script_registry_release_owner(const void* owner, size_t* out_removed) {
    if (owner == nullptr || out_removed == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_removed = 0;
    try {
        std::lock_guard lock(g_registry_mutex);
        reclaim_retired_locked();
        size_t matching = 0;
        for (const auto& entry : g_entries) {
            const auto& vtable = entry->vtable;
            const bool has_owner = sao::scripting::internal::has_provider_field(
                vtable, offsetof(SaoScriptEngineVTable, owner),
                sizeof(vtable.owner));
            if (has_owner && vtable.owner == owner) ++matching;
        }
        if (matching > g_retired_entries.max_size() - g_retired_entries.size()) {
            return SAO_STATUS_ERR_UNKNOWN;
        }
        g_retired_entries.reserve(g_retired_entries.size() + matching);
        for (auto iterator = g_entries.begin(); iterator != g_entries.end();) {
            const auto& vtable = (*iterator)->vtable;
            const bool has_owner = sao::scripting::internal::has_provider_field(
                vtable, offsetof(SaoScriptEngineVTable, owner),
                sizeof(vtable.owner));
            if (!has_owner || vtable.owner != owner) {
                ++iterator;
                continue;
            }
            g_retired_entries.push_back(std::move(*iterator));
            iterator = g_entries.erase(iterator);
            ++*out_removed;
        }
        reclaim_retired_locked();
        return SAO_STATUS_OK;
    } catch (...) {
        *out_removed = 0;
        return SAO_STATUS_ERR_UNKNOWN;
    }
}
