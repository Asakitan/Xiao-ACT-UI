#include "sao/core/error.h"

#include <cstring>

#include <windows.h>

namespace {

thread_local SaoErrorInfo tls_error_info{};

const char* source_basename(const char* source_file) {
    if (source_file == nullptr) {
        return "";
    }

    const char* basename = source_file;
    for (const char* cursor = source_file; *cursor != '\0'; ++cursor) {
        if (*cursor == '\\' || *cursor == '/') {
            basename = cursor + 1;
        }
    }
    return basename;
}

void copy_text(char* destination, size_t capacity, const char* source) {
    if (capacity == 0) {
        return;
    }
    if (source == nullptr) {
        destination[0] = '\0';
        return;
    }
    strncpy_s(destination, capacity, source, _TRUNCATE);
}

}  // namespace

extern "C" sao_status_t SAO_CORE_CALL sao_core_error_last(
    SaoErrorInfo* out_info) {
    if (out_info == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_info = tls_error_info;
    return SAO_STATUS_OK;
}

extern "C" void SAO_CORE_CALL sao_core_error_set(
    sao_status_t status,
    const char* category,
    const char* message_utf8,
    const char* source_file,
    uint32_t source_line) {
    std::memset(&tls_error_info, 0, sizeof(tls_error_info));
    tls_error_info.status = status;
    tls_error_info.os_error = GetLastError();
    tls_error_info.source_line = source_line;
    copy_text(tls_error_info.category, sizeof(tls_error_info.category), category);
    copy_text(tls_error_info.message_utf8, sizeof(tls_error_info.message_utf8), message_utf8);
    copy_text(tls_error_info.source_file,
              sizeof(tls_error_info.source_file),
              source_basename(source_file));
}

extern "C" void SAO_CORE_CALL sao_core_error_clear(void) {
    std::memset(&tls_error_info, 0, sizeof(tls_error_info));
}
