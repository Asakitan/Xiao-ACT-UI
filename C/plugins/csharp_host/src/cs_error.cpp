// cs_error.cpp — stub
#include "sao/plugins/csharp_host/cs_error.h"

#include "cs_component_internal.h"

#include <cstring>
#include <memory>

namespace sao::plugins::csharp_host {

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_cshost_take_error(char** out_utf8) {
    if (out_utf8 != nullptr)
        *out_utf8 = nullptr;
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_take_compile_errors(char** out_json_utf8) {
    if (out_json_utf8 != nullptr)
        *out_json_utf8 = nullptr;
    return SAO_ERR_NOT_IMPLEMENTED;
}

int32_t cshost_copy_string(const std::string& value, char** output) noexcept {
    if (output == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *output = nullptr;
    try {
        auto buffer = std::make_unique<char[]>(value.size() + 1);
        if (!value.empty())
            std::memcpy(buffer.get(), value.data(), value.size());
        buffer[value.size()] = '\0';
        *output = buffer.release();
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL sao_plugins_cshost_free_string(char* value) {
    delete[] value;
}

} // namespace sao::plugins::csharp_host
