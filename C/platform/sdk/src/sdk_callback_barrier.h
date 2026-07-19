#pragma once

#include <type_traits>

#if defined(_MSC_VER) && defined(_WIN32)
#include <excpt.h>
#endif

#include "sao/sdk/sao_sdk_context.h"

namespace sao_sdk_internal {
namespace callback_barrier_detail {

using InvokeFn = sao_sdk_status_t (*)(void*) noexcept;

inline sao_sdk_status_t invoke_seh(InvokeFn callback, void* context) noexcept {
#if defined(_MSC_VER) && defined(_WIN32)
    __try {
        return callback(context);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return SAO_SDK_ERR_INTERNAL;
    }
#else
    return callback(context);
#endif
}

template <typename Callback> sao_sdk_status_t invoke_cpp(void* context) noexcept {
    try {
        return (*static_cast<Callback*>(context))();
    } catch (...) {
        return SAO_SDK_ERR_INTERNAL;
    }
}

} // namespace callback_barrier_detail

template <typename Callback>
sao_sdk_status_t invoke_callback_barrier(Callback&& callback) noexcept {
    using StoredCallback = std::remove_reference_t<Callback>;
    return callback_barrier_detail::invoke_seh(callback_barrier_detail::invoke_cpp<StoredCallback>,
                                               &callback);
}

template <typename Callback>
sao_sdk_status_t invoke_void_callback_barrier(Callback&& callback) noexcept {
    return invoke_callback_barrier([&callback]() -> sao_sdk_status_t {
        callback();
        return SAO_SDK_OK;
    });
}

} // namespace sao_sdk_internal
