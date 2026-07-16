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
        const RegistryEntry* entry = find_entry_locked(language);
        if (entry == nullptr) return SAO_STATUS_ERR_SCRIPT_UNSUPPORTED;
        *out_vtable = entry->vtable;
        const sao_status_t available_status = check_available(*out_vtable);
        if (available_status != SAO_STATUS_OK) {
            *out_vtable = {};
            return available_status;
        }
        if (has_provider_field(*out_vtable,
                               offsetof(SaoScriptEngineVTable, retain),
                               sizeof(out_vtable->retain)) &&
            out_vtable->retain != nullptr) {
            try {
                out_vtable->retain(out_vtable->user_data);
            } catch (...) {
                *out_vtable = {};
                return SAO_STATUS_ERR_SCRIPT_PROVIDER_EXCEPTION;
            }
        }
    }
    return SAO_STATUS_OK;
}

void release_provider(const SaoScriptEngineVTable& vtable) noexcept {
    if (!has_provider_field(vtable,
                            offsetof(SaoScriptEngineVTable, release),
                            sizeof(vtable.release)) ||
        vtable.release == nullptr) {
        return;
    }
    try {
        vtable.release(vtable.user_data);
    } catch (...) {
    }
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
        const auto found = std::find_if(
            g_entries.begin(), g_entries.end(),
            [vtable](const std::unique_ptr<RegistryEntry>& entry) {
                return entry->vtable.language == vtable->language;
            });
        if (found == g_entries.end()) {
            g_entries.push_back(std::move(replacement));
        } else {
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
    SaoScriptEngineVTable snapshot{};
    {
        std::lock_guard lock(g_registry_mutex);
        RegistryEntry* entry = find_entry_locked(language);
        if (entry == nullptr) return SAO_STATUS_ERR_SCRIPT_UNSUPPORTED;
        snapshot = entry->vtable;
        *out_vtable = &entry->vtable;
    }
    const sao_status_t status = check_available(snapshot);
    if (status != SAO_STATUS_OK) *out_vtable = nullptr;
    return status;
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_registry_enumerate(
    const SaoScriptEngineVTable** out_vtables,
    size_t max_vtables,
    size_t* out_count) {
    if (out_count == nullptr || (out_vtables == nullptr && max_vtables != 0)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard lock(g_registry_mutex);
        *out_count = g_entries.size();
        if (out_vtables == nullptr || max_vtables < g_entries.size()) {
            return g_entries.empty() ? SAO_STATUS_OK
                                     : SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        }
        for (size_t index = 0; index < g_entries.size(); ++index) {
            out_vtables[index] = &g_entries[index]->vtable;
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
    std::lock_guard lock(g_registry_mutex);
    const auto found = std::find_if(
        g_entries.begin(), g_entries.end(),
        [language](const std::unique_ptr<RegistryEntry>& entry) {
            return entry->vtable.language == language;
        });
    if (found == g_entries.end()) return SAO_STATUS_ERR_SCRIPT_UNSUPPORTED;
    g_retired_entries.push_back(std::move(*found));
    g_entries.erase(found);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_SCRIPTING_CALL
sao_script_registry_release_owner(const void* owner, size_t* out_removed) {
    if (owner == nullptr || out_removed == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_removed = 0;
    std::lock_guard lock(g_registry_mutex);
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
    return SAO_STATUS_OK;
}
