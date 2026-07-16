#include "sao/scripting/script_error.h"

#include <algorithm>
#include <cstring>
#include <exception>

#include "script_internal.h"

namespace sao::scripting::internal {

void set_error(SaoScriptError* error,
               sao_status_t status,
               const char* message) noexcept {
    if (error == nullptr) return;
    std::memset(error, 0, sizeof(*error));
    error->status = status;
    if (message == nullptr) return;
    const size_t length = std::min(
        std::strlen(message), sizeof(error->message_utf8) - 1u);
    std::memcpy(error->message_utf8, message, length);
    error->message_utf8[length] = '\0';
}

}  // namespace sao::scripting::internal

extern "C" void SAO_SCRIPTING_CALL sao_scripting_error_clear(
    SaoScriptError* err) {
    if (err == nullptr) return;
    std::memset(err, 0, sizeof(*err));
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_scripting_provider_barrier(
    sao_script_provider_call_t call,
    void* user_data,
    SaoScriptError* out_error) {
    sao_scripting_error_clear(out_error);
    if (call == nullptr) {
        sao::scripting::internal::set_error(
            out_error, SAO_STATUS_ERR_INVALID_ARGUMENT,
            "provider call is null");
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        return call(user_data);
    } catch (const std::exception& error) {
        sao::scripting::internal::set_error(
            out_error, SAO_STATUS_ERR_SCRIPT_PROVIDER_EXCEPTION,
            error.what());
        return SAO_STATUS_ERR_SCRIPT_PROVIDER_EXCEPTION;
    } catch (...) {
        sao::scripting::internal::set_error(
            out_error, SAO_STATUS_ERR_SCRIPT_PROVIDER_EXCEPTION,
            "language provider crossed exception barrier");
        return SAO_STATUS_ERR_SCRIPT_PROVIDER_EXCEPTION;
    }
}
