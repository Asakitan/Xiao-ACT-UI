#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <type_traits>

#if defined(_MSC_VER) && defined(_WIN32)
#include <excpt.h>
#endif

#include "sao/sdk/sao_sdk_context.h"

namespace sao_sdk_internal {

class CallbackActivity {
  public:
    bool try_acquire() noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!accepting_)
                return false;
            ++active_calls_;
            return true;
        } catch (...) {
            return false;
        }
    }

    void release() noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_calls_ != 0)
                --active_calls_;
            if (active_calls_ == 0)
                idle_.notify_all();
        } catch (...) {
        }
    }

    void retire_and_wait() noexcept {
        try {
            std::unique_lock<std::mutex> lock(mutex_);
            accepting_ = false;
            idle_.wait(lock, [this] { return active_calls_ == 0; });
        } catch (...) {
        }
    }

    CallbackActivity() = default;
    CallbackActivity(const CallbackActivity&) = delete;
    CallbackActivity& operator=(const CallbackActivity&) = delete;

  private:
    std::mutex mutex_;
    std::condition_variable idle_;
    std::size_t active_calls_ = 0;
    bool accepting_ = true;
};

class CallbackActivityLease {
  public:
    explicit CallbackActivityLease(CallbackActivity* activity) noexcept : activity_(activity) {
        if (activity_ == nullptr || !activity_->try_acquire())
            activity_ = nullptr;
    }

    ~CallbackActivityLease() {
        if (activity_ != nullptr)
            activity_->release();
    }

    CallbackActivityLease(const CallbackActivityLease&) = delete;
    CallbackActivityLease& operator=(const CallbackActivityLease&) = delete;

    explicit operator bool() const noexcept {
        return activity_ != nullptr;
    }

  private:
    CallbackActivity* activity_ = nullptr;
};

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
