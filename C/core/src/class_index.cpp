#include "sao_core/class_index.h"

#include <windows.h>

#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr size_t kMaximumClassNameBytes = 255;

struct ClassRegistry {
    std::mutex mutex;
    std::vector<std::string> names;
};

ClassRegistry& registry() {
    static ClassRegistry value;
    return value;
}

bool valid_utf8_name(const char* value) noexcept {
    if (value == nullptr) {
        return false;
    }
    const size_t length = strnlen_s(value, kMaximumClassNameBytes + 1);
    if (length == 0 || length > kMaximumClassNameBytes ||
        length > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    return MultiByteToWideChar(
               CP_UTF8,
               MB_ERR_INVALID_CHARS,
               value,
               static_cast<int>(length),
               nullptr,
               0) > 0;
}

bool find_locked(const std::vector<std::string>& names, const char* name, uint32_t* out_index) {
    for (size_t index = 0; index < names.size(); ++index) {
        if (names[index] == name) {
            *out_index = static_cast<uint32_t>(index);
            return true;
        }
    }
    return false;
}

}  // namespace

extern "C" int32_t SAO_CORE_CALL sao_core_class_index_resolve(
    sao_core_process_handle_t handle, const char* class_name_utf8, uint64_t* out_class_ptr) {
    (void)handle;
    if (out_class_ptr != nullptr) {
        *out_class_ptr = 0;
    }
    if (out_class_ptr == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }

    uint32_t index = 0;
    const int32_t status = sao_core_class_index_register(class_name_utf8, &index);
    if (status != SAO_OK) {
        return status;
    }
    *out_class_ptr = static_cast<uint64_t>(index) + 1;
    return SAO_OK;
}

extern "C" int32_t SAO_CORE_CALL sao_core_class_index_resolve_field_offset(
    sao_core_process_handle_t handle,
    uint64_t class_ptr,
    const char* field_name_utf8,
    uint32_t* out_offset) {
    (void)handle;
    if (out_offset != nullptr) {
        *out_offset = 0;
    }
    if (out_offset == nullptr || class_ptr == 0 || !valid_utf8_name(field_name_utf8)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }

    try {
        auto& state = registry();
        std::lock_guard<std::mutex> lock(state.mutex);
        if (class_ptr > state.names.size()) {
            return SAO_ERR_NOT_FOUND;
        }
        return SAO_ERR_NOT_FOUND;
    } catch (...) {
        return SAO_ERR_UNKNOWN;
    }
}

extern "C" int32_t SAO_CORE_CALL sao_core_class_index_register(
    const char* class_name_utf8, uint32_t* out_index) {
    if (out_index != nullptr) {
        *out_index = 0;
    }
    if (out_index == nullptr || !valid_utf8_name(class_name_utf8)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }

    try {
        auto& state = registry();
        std::lock_guard<std::mutex> lock(state.mutex);
        if (find_locked(state.names, class_name_utf8, out_index)) {
            return SAO_OK;
        }
        if (state.names.size() >= std::numeric_limits<uint32_t>::max()) {
            return SAO_ERR_UNKNOWN;
        }
        state.names.emplace_back(class_name_utf8);
        *out_index = static_cast<uint32_t>(state.names.size() - 1);
        return SAO_OK;
    } catch (...) {
        *out_index = 0;
        return SAO_ERR_UNKNOWN;
    }
}

extern "C" int32_t SAO_CORE_CALL sao_core_class_index_find(
    const char* class_name_utf8, uint32_t* out_index) {
    if (out_index != nullptr) {
        *out_index = 0;
    }
    if (out_index == nullptr || !valid_utf8_name(class_name_utf8)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }

    try {
        auto& state = registry();
        std::lock_guard<std::mutex> lock(state.mutex);
        return find_locked(state.names, class_name_utf8, out_index) ? SAO_OK : SAO_ERR_NOT_FOUND;
    } catch (...) {
        *out_index = 0;
        return SAO_ERR_UNKNOWN;
    }
}

extern "C" int32_t SAO_CORE_CALL sao_core_class_index_get_name(
    uint32_t index,
    char* out_class_name_utf8,
    size_t class_name_capacity,
    size_t* out_required_size) {
    if (out_required_size != nullptr) {
        *out_required_size = 0;
    }
    if (out_class_name_utf8 != nullptr && class_name_capacity != 0) {
        out_class_name_utf8[0] = '\0';
    }
    if (out_required_size == nullptr ||
        (out_class_name_utf8 == nullptr && class_name_capacity != 0)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }

    try {
        auto& state = registry();
        std::lock_guard<std::mutex> lock(state.mutex);
        if (index >= state.names.size()) {
            return SAO_ERR_NOT_FOUND;
        }
        const std::string& name = state.names[index];
        *out_required_size = name.size() + 1;
        if (out_class_name_utf8 == nullptr) {
            return SAO_OK;
        }
        if (class_name_capacity < name.size() + 1) {
            return SAO_ERR_BUFFER_TOO_SMALL;
        }
        std::memcpy(out_class_name_utf8, name.c_str(), name.size() + 1);
        return SAO_OK;
    } catch (...) {
        if (out_class_name_utf8 != nullptr && class_name_capacity != 0) {
            out_class_name_utf8[0] = '\0';
        }
        *out_required_size = 0;
        return SAO_ERR_UNKNOWN;
    }
}

extern "C" int32_t SAO_CORE_CALL sao_core_class_index_count(size_t* out_count) {
    if (out_count != nullptr) {
        *out_count = 0;
    }
    if (out_count == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }

    try {
        auto& state = registry();
        std::lock_guard<std::mutex> lock(state.mutex);
        *out_count = state.names.size();
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_UNKNOWN;
    }
}
