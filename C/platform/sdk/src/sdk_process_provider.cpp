#define SAO_SDK_BUILDING_DLL 1

#include "sdk_internal.h"

#include "sdk_callback_barrier.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

namespace sao_sdk_internal {
namespace {

template <typename Provider> struct ProcessProviderOwner {
    Provider provider{};
    bool retained = false;

    ~ProcessProviderOwner() {
        if (retained && provider.release != nullptr) {
            (void)invoke_void_callback_barrier([this] { provider.release(provider.user_data); });
        }
    }
};

struct ProcessProviderState {
    std::mutex mutex;
    std::shared_ptr<ProcessProviderOwner<SaoSdkMemoryProviderVTable>> memory;
    std::shared_ptr<ProcessProviderOwner<SaoSdkNetProviderVTable>> net;
};

ProcessProviderState& process_provider_state() {
    static ProcessProviderState state;
    return state;
}

struct ProcessBindingCandidates {
    std::shared_ptr<MemoryProviderSession> memory;
    std::shared_ptr<NetProviderSession> net;
    ProviderBindingCandidate platform;
};

class ProcessBindTransaction {
  public:
    explicit ProcessBindTransaction(ContextState* state) : state_(state) {
        if (state_ == nullptr) {
            status_ = SAO_SDK_ERR_HANDLE_INVALID;
            return;
        }
        if (state_->destroying.load(std::memory_order_acquire) ||
            state_->destroy_quarantined.load(std::memory_order_acquire) ||
            provider_callback_reentered(state_) || memory_callback_reentered(state_) ||
            net_callback_reentered(state_) || plugin_callback_reentered(state_) ||
            gpu_callback_reentered(state_)) {
            status_ = SAO_SDK_ERR_BUSY;
            return;
        }
        bool expected = false;
        if (!state_->provider_bind_transaction.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            status_ = SAO_SDK_ERR_BUSY;
            return;
        }
        active_ = true;
        try {
            std::lock_guard<std::mutex> lock(state_->mu);
            state_->memory_quarantine.reserve(state_->memory_quarantine.size() + 1u);
            state_->net_quarantine.reserve(state_->net_quarantine.size() + 1u);
            state_->provider_release_quarantine.reserve(
                state_->provider_release_quarantine.size() + 1u);
        } catch (...) {
            state_->provider_bind_transaction.store(false, std::memory_order_release);
            active_ = false;
            status_ = SAO_SDK_ERR_INTERNAL;
            return;
        }
        status_ = SAO_SDK_OK;
    }

    ~ProcessBindTransaction() {
        if (active_)
            state_->provider_bind_transaction.store(false, std::memory_order_release);
    }

    ProcessBindTransaction(const ProcessBindTransaction&) = delete;
    ProcessBindTransaction& operator=(const ProcessBindTransaction&) = delete;

    explicit operator bool() const noexcept {
        return active_;
    }

    sao_sdk_status_t status() const noexcept {
        return status_;
    }

  private:
    ContextState* state_ = nullptr;
    bool active_ = false;
    sao_sdk_status_t status_ = SAO_SDK_ERR_HANDLE_INVALID;
};

struct ProcessProviderSnapshot {
    std::shared_ptr<ProcessProviderOwner<SaoSdkMemoryProviderVTable>> memory;
    std::shared_ptr<ProcessProviderOwner<SaoSdkNetProviderVTable>> net;
};

ProcessProviderSnapshot process_provider_snapshot() {
    auto& owners = process_provider_state();
    std::lock_guard<std::mutex> lock(owners.mutex);
    return {owners.memory, owners.net};
}

sao_sdk_status_t preflight_existing_bindings(ContextState* state,
                                             const ProcessProviderSnapshot& owners,
                                             bool include_platform, bool* bind_memory,
                                             bool* bind_net, bool* bind_platform) {
    *bind_memory = false;
    *bind_net = false;
    *bind_platform = false;

    std::shared_ptr<MemoryProviderSession> current_memory;
    std::shared_ptr<NetProviderSession> current_net;
    bool provider_bound = false;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        if (!state->memory_quarantine.empty() || !state->net_quarantine.empty() ||
            !state->provider_release_quarantine.empty()) {
            return SAO_SDK_ERR_BUSY;
        }
        current_memory = state->memory_provider.published();
        current_net = state->net_provider.published();
        provider_bound = state->provider_bound;
    }

    if (include_platform) {
        if (provider_bound && !platform_provider_bound(state))
            return SAO_SDK_ERR_BUSY;
        if (provider_bound) {
            const auto status = provider_status(state);
            if (status != SAO_SDK_OK)
                return status;
        }
        *bind_platform = !provider_bound;
    }

    if (owners.memory != nullptr) {
        if (current_memory == nullptr) {
            *bind_memory = true;
        } else if (state->process_memory_session.lock() != current_memory ||
                   state->process_memory_owner_tag != owners.memory.get()) {
            return SAO_SDK_ERR_BUSY;
        } else {
            const auto status = memory_provider_status(state);
            if (status != SAO_SDK_OK)
                return status;
        }
    }
    if (owners.net != nullptr) {
        if (current_net == nullptr) {
            *bind_net = true;
        } else if (state->process_net_session.lock() != current_net ||
                   state->process_net_owner_tag != owners.net.get()) {
            return SAO_SDK_ERR_BUSY;
        } else {
            const auto status = net_provider_status(state);
            if (status != SAO_SDK_OK)
                return status;
        }
    }
    return SAO_SDK_OK;
}

template <typename Session>
sao_sdk_status_t quarantine_process_candidate(ContextState* state,
                                              const std::shared_ptr<Session>& candidate);

template <>
sao_sdk_status_t quarantine_process_candidate(
    ContextState* state, const std::shared_ptr<MemoryProviderSession>& candidate) {
    if (candidate == nullptr)
        return SAO_SDK_OK;
    try {
        std::lock_guard<std::mutex> lock(state->mu);
        state->memory_quarantine.push_back(candidate);
        return SAO_SDK_OK;
    } catch (...) {
        return SAO_SDK_ERR_INTERNAL;
    }
}

template <>
sao_sdk_status_t quarantine_process_candidate(
    ContextState* state, const std::shared_ptr<NetProviderSession>& candidate) {
    if (candidate == nullptr)
        return SAO_SDK_OK;
    try {
        std::lock_guard<std::mutex> lock(state->mu);
        state->net_quarantine.push_back(candidate);
        return SAO_SDK_OK;
    } catch (...) {
        return SAO_SDK_ERR_INTERNAL;
    }
}

sao_sdk_status_t discard_memory_candidate(ContextState* state,
                                          std::shared_ptr<MemoryProviderSession>* candidate) {
    if (*candidate == nullptr)
        return SAO_SDK_OK;
    bool staged = false;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        staged = state->memory_provider.begin_staging(*candidate);
        if (staged)
            candidate->reset();
    }
    if (!staged)
        return quarantine_process_candidate(state, *candidate);
    sao_sdk_status_t status = SAO_SDK_ERR_INTERNAL;
    try {
        status = configure_memory_provider(state, nullptr);
    } catch (...) {
    }
    std::shared_ptr<MemoryProviderSession> residual;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        residual = state->memory_provider.take_staged();
    }
    const auto quarantine_status = quarantine_process_candidate(state, residual);
    return quarantine_status == SAO_SDK_OK ? status : quarantine_status;
}

sao_sdk_status_t discard_net_candidate(ContextState* state,
                                       std::shared_ptr<NetProviderSession>* candidate) {
    if (*candidate == nullptr)
        return SAO_SDK_OK;
    bool staged = false;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        staged = state->net_provider.begin_staging(*candidate);
        if (staged)
            candidate->reset();
    }
    if (!staged)
        return quarantine_process_candidate(state, *candidate);
    sao_sdk_status_t status = SAO_SDK_ERR_INTERNAL;
    try {
        status = configure_net_provider(state, nullptr);
    } catch (...) {
    }
    std::shared_ptr<NetProviderSession> residual;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        residual = state->net_provider.take_staged();
    }
    const auto quarantine_status = quarantine_process_candidate(state, residual);
    return quarantine_status == SAO_SDK_OK ? status : quarantine_status;
}

sao_sdk_status_t discard_candidates(ContextState* state,
                                    ProcessBindingCandidates* candidates,
                                    sao_sdk_status_t original_status) {
    sao_sdk_status_t cleanup_status = discard_net_candidate(state, &candidates->net);
    const auto memory_status = discard_memory_candidate(state, &candidates->memory);
    if (cleanup_status == SAO_SDK_OK)
        cleanup_status = memory_status;
    const auto platform_status =
        discard_provider_binding_candidate(state, &candidates->platform);
    if (cleanup_status == SAO_SDK_OK)
        cleanup_status = platform_status;
    return cleanup_status == SAO_SDK_OK ? original_status : cleanup_status;
}

sao_sdk_status_t prepare_memory_candidate(
    ContextState* state,
    const std::shared_ptr<ProcessProviderOwner<SaoSdkMemoryProviderVTable>>& owner,
    std::shared_ptr<MemoryProviderSession>* out_candidate) {
    {
        std::lock_guard<std::mutex> lock(state->mu);
        if (!state->memory_provider.begin_staging())
            return SAO_SDK_ERR_BUSY;
    }
    sao_sdk_status_t status = SAO_SDK_ERR_INTERNAL;
    try {
        status = configure_memory_provider(state, &owner->provider);
    } catch (...) {
    }
    {
        std::lock_guard<std::mutex> lock(state->mu);
        *out_candidate = state->memory_provider.take_staged();
    }
    return status;
}

sao_sdk_status_t prepare_net_candidate(
    ContextState* state,
    const std::shared_ptr<ProcessProviderOwner<SaoSdkNetProviderVTable>>& owner,
    std::shared_ptr<NetProviderSession>* out_candidate) {
    {
        std::lock_guard<std::mutex> lock(state->mu);
        if (!state->net_provider.begin_staging())
            return SAO_SDK_ERR_BUSY;
    }
    sao_sdk_status_t status = SAO_SDK_ERR_INTERNAL;
    try {
        status = configure_net_provider(state, &owner->provider);
    } catch (...) {
    }
    {
        std::lock_guard<std::mutex> lock(state->mu);
        *out_candidate = state->net_provider.take_staged();
    }
    return status;
}

sao_sdk_status_t bind_provider_set(ContextState* state, bool include_platform) {
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    ProcessBindTransaction transaction(state);
    if (!transaction)
        return transaction.status();

    std::lock_guard<std::recursive_mutex> provider_lock(state->provider_lifecycle_mutex);

    const auto owners = process_provider_snapshot();
    bool bind_memory = false;
    bool bind_net = false;
    bool bind_platform = false;
    const auto preflight_status = preflight_existing_bindings(
        state, owners, include_platform, &bind_memory, &bind_net, &bind_platform);
    if (preflight_status != SAO_SDK_OK)
        return preflight_status;

    ProcessBindingCandidates candidates;
    if (bind_platform) {
        const auto status = prepare_platform_provider_binding(state, &candidates.platform);
        if (status != SAO_SDK_OK)
            return discard_candidates(state, &candidates, status);
    }
    if (bind_memory) {
        const auto status = prepare_memory_candidate(state, owners.memory, &candidates.memory);
        if (status != SAO_SDK_OK)
            return discard_candidates(state, &candidates, status);
    }
    if (bind_net) {
        const auto status = prepare_net_candidate(state, owners.net, &candidates.net);
        if (status != SAO_SDK_OK)
            return discard_candidates(state, &candidates, status);
    }

    std::unique_lock<std::mutex> memory_lock(state->memory_lifecycle_mutex);
    std::unique_lock<std::mutex> net_lock(state->net_lifecycle_mutex);
    bool recheck_memory = false;
    bool recheck_net = false;
    bool recheck_platform = false;
    const auto recheck_status = preflight_existing_bindings(
        state, owners, include_platform, &recheck_memory, &recheck_net, &recheck_platform);
    if (recheck_status != SAO_SDK_OK || recheck_memory != bind_memory ||
        recheck_net != bind_net || recheck_platform != bind_platform) {
        net_lock.unlock();
        memory_lock.unlock();
        return discard_candidates(
            state, &candidates,
            recheck_status == SAO_SDK_OK ? SAO_SDK_ERR_BUSY : recheck_status);
    }

    if (bind_platform) {
        std::lock_guard<std::mutex> lock(state->provider_mutex);
        state->provider_accepting = true;
        state->provider_cleanup_status = SAO_SDK_OK;
        state->provider_retained = candidates.platform.retained;
    }
    {
        std::lock_guard<std::mutex> lock(state->mu);
        if (bind_platform) {
            state->provider = candidates.platform.provider;
            state->provider_bound = true;
            candidates.platform = {};
        }
        if (bind_memory) {
            state->memory_provider.publish(candidates.memory);
            state->process_memory_session = candidates.memory;
            state->process_memory_owner_tag = owners.memory.get();
            candidates.memory.reset();
        }
        if (bind_net) {
            state->net_provider.publish(candidates.net);
            state->process_net_session = candidates.net;
            state->process_net_owner_tag = owners.net.get();
            candidates.net.reset();
        }
    }
    return SAO_SDK_OK;
}

template <typename Provider>
sao_sdk_status_t validate_provider(const Provider* provider, uint32_t major, size_t required_size) {
    if (provider == nullptr)
        return SAO_SDK_OK;
    if ((provider->abi_version >> 16) != major || provider->struct_size < required_size)
        return SAO_SDK_ERR_ABI_MISMATCH;
    if (provider->retain == nullptr || provider->release == nullptr ||
        provider->open_session == nullptr || provider->close_session == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    if constexpr (std::is_same_v<Provider, SaoSdkMemoryProviderVTable>) {
        if (provider->attach == nullptr || provider->detach == nullptr ||
            provider->read == nullptr || provider->enumerate_modules == nullptr) {
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        }
    } else {
        if (provider->capture_start == nullptr || provider->capture_stop == nullptr ||
            provider->parse_packet == nullptr) {
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        }
    }
    return SAO_SDK_OK;
}

template <typename Provider>
sao_sdk_status_t make_owner(const Provider* provider, uint32_t major, size_t required_size,
                            std::shared_ptr<ProcessProviderOwner<Provider>>& out_owner) {
    const auto validation = validate_provider(provider, major, required_size);
    if (validation != SAO_SDK_OK || provider == nullptr)
        return validation;
    try {
        auto owner = std::make_shared<ProcessProviderOwner<Provider>>();
        std::memcpy(&owner->provider, provider,
                    std::min<size_t>(provider->struct_size, sizeof(owner->provider)));
        const auto retain_status = invoke_void_callback_barrier(
            [&owner] { owner->provider.retain(owner->provider.user_data); });
        if (retain_status != SAO_SDK_OK)
            return retain_status;
        owner->retained = true;
        out_owner = std::move(owner);
        return SAO_SDK_OK;
    } catch (...) {
        return SAO_SDK_ERR_INTERNAL;
    }
}

template <typename Owner>
void replace_owner(std::shared_ptr<Owner>& destination, std::shared_ptr<Owner> replacement) {
    std::shared_ptr<Owner> previous;
    {
        auto& state = process_provider_state();
        std::lock_guard<std::mutex> lock(state.mutex);
        previous = std::exchange(destination, std::move(replacement));
    }
    previous.reset();
}

} // namespace

sao_sdk_status_t configure_process_memory_provider(const SaoSdkMemoryProviderVTable* provider) {
    std::shared_ptr<ProcessProviderOwner<SaoSdkMemoryProviderVTable>> replacement;
    const auto status = make_owner(provider, SAO_SDK_MEMORY_PROVIDER_ABI_VERSION_MAJOR,
                                   SAO_SDK_MEMORY_PROVIDER_REQUIRED_SIZE, replacement);
    if (status != SAO_SDK_OK)
        return status;
    replace_owner(process_provider_state().memory, std::move(replacement));
    return SAO_SDK_OK;
}

sao_sdk_status_t configure_process_net_provider(const SaoSdkNetProviderVTable* provider) {
    std::shared_ptr<ProcessProviderOwner<SaoSdkNetProviderVTable>> replacement;
    const auto status = make_owner(provider, SAO_SDK_NET_PROVIDER_ABI_VERSION_MAJOR,
                                   SAO_SDK_NET_PROVIDER_REQUIRED_SIZE, replacement);
    if (status != SAO_SDK_OK)
        return status;
    replace_owner(process_provider_state().net, std::move(replacement));
    return SAO_SDK_OK;
}

sao_sdk_status_t bind_process_providers(ContextState* state) {
    return bind_provider_set(state, false);
}

sao_sdk_status_t bind_platform_services(ContextState* state) {
    return bind_provider_set(state, true);
}

} // namespace sao_sdk_internal

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_platform_memory_configure_provider(const SaoSdkMemoryProviderVTable* provider) {
    return sao_sdk_internal::configure_process_memory_provider(provider);
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_platform_net_configure_provider(const SaoSdkNetProviderVTable* provider) {
    return sao_sdk_internal::configure_process_net_provider(provider);
}
