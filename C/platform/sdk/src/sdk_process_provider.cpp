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
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    std::shared_ptr<ProcessProviderOwner<SaoSdkMemoryProviderVTable>> memory;
    std::shared_ptr<ProcessProviderOwner<SaoSdkNetProviderVTable>> net;
    {
        auto& owners = process_provider_state();
        std::lock_guard<std::mutex> lock(owners.mutex);
        memory = owners.memory;
        net = owners.net;
    }
    if (memory != nullptr) {
        const auto status = configure_memory_provider(state, &memory->provider);
        if (status != SAO_SDK_OK)
            return status;
    }
    if (net != nullptr) {
        const auto status = configure_net_provider(state, &net->provider);
        if (status != SAO_SDK_OK)
            return status;
    }
    return SAO_SDK_OK;
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
