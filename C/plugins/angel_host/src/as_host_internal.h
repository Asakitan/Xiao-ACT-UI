#pragma once

#include "sao/plugins/angel_host/as_host.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(SAO_HAS_ANGELSCRIPT)
#include <angelscript.h>
#endif

namespace sao::plugins::angel_host {

#if defined(SAO_HAS_ANGELSCRIPT)

struct as_host_s {
    struct retained_message {
        std::string section;
        std::string message;
        int row = 0;
        int column = 0;
        int type = 0;
    };

    asIScriptEngine* engine = nullptr;
    void (*message_callback)(const char*, int, int, int, void*) = nullptr;
    void* user_data = nullptr;
    std::mutex engine_mutex;
    std::mutex message_mutex;
    std::vector<retained_message> messages;
    std::mutex lifecycle_mutex;
    size_t active_instances = 0;
    size_t active_callbacks = 0;
    size_t active_message_callbacks = 0;
    bool closing = false;
    bool finalizing = false;
    bool destroy_deferred = false;
};

using shared_host_state = std::shared_ptr<as_host_s>;

class engine_execution_guard {
  public:
    engine_execution_guard();
    ~engine_execution_guard();

    engine_execution_guard(const engine_execution_guard&) = delete;
    engine_execution_guard& operator=(const engine_execution_guard&) = delete;

  private:
    std::unique_lock<std::recursive_mutex> lock_;
};

bool engine_execution_active() noexcept;

void defer_host_finalize(const shared_host_state& host) noexcept;

shared_host_state acquire_host(as_host_handle_t host);
shared_host_state acquire_host_for_engine(asIScriptEngine* engine);
bool host_accepts_admission(const shared_host_state& host) noexcept;

class host_instance_lease {
  public:
    host_instance_lease() = default;
    ~host_instance_lease() noexcept;

    host_instance_lease(const host_instance_lease&) = delete;
    host_instance_lease& operator=(const host_instance_lease&) = delete;
    host_instance_lease(host_instance_lease&& other) noexcept;
    host_instance_lease& operator=(host_instance_lease&& other) noexcept;

    int32_t acquire(const shared_host_state& host) noexcept;
    void reset() noexcept;
    bool active() const noexcept;

  private:
    shared_host_state host_;
};

class host_callback_lease {
  public:
    host_callback_lease() = default;
    ~host_callback_lease() noexcept;

    host_callback_lease(const host_callback_lease&) = delete;
    host_callback_lease& operator=(const host_callback_lease&) = delete;
    host_callback_lease(host_callback_lease&& other) noexcept;
    host_callback_lease& operator=(host_callback_lease&& other) noexcept;

    int32_t acquire(const shared_host_state& host) noexcept;
    void reset() noexcept;
    bool active() const noexcept;

  private:
    shared_host_state host_;
};

int32_t request_host_destroy(const shared_host_state& host) noexcept;
void maybe_finalize_host(const shared_host_state& host) noexcept;

#endif

} // namespace sao::plugins::angel_host
