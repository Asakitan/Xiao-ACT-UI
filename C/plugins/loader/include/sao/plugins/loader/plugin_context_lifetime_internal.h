#pragma once

#include "sao/plugins/loader/plugin_context.h"

#include <memory>

namespace sao::plugins::loader {

// In-tree C++ ownership only; this is not part of the plugin C ABI.
struct context_runtime_state;
struct context_runtime_resource {
    virtual ~context_runtime_resource() = default;
    virtual void retire() noexcept = 0;
};

class context_runtime_lease {
  public:
    explicit context_runtime_lease(plugin_context_t* ctx) noexcept;
    explicit context_runtime_lease(std::shared_ptr<context_runtime_state> state) noexcept;
    ~context_runtime_lease();
    context_runtime_lease(const context_runtime_lease&) = delete;
    context_runtime_lease& operator=(const context_runtime_lease&) = delete;

    explicit operator bool() const noexcept { return state_ != nullptr; }
    const std::shared_ptr<context_runtime_state>& state() const noexcept { return state_; }
    std::shared_ptr<context_runtime_resource> resource(
        const void* key, std::shared_ptr<context_runtime_resource> candidate = {}) const;

  private:
    std::shared_ptr<context_runtime_state> state_;
};

bool plugin_context_runtime_busy(plugin_context_t* ctx) noexcept;

} // namespace sao::plugins::loader
