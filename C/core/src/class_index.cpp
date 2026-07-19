#include "sao_core/class_index.h"

#include "logging.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr size_t kMaximumClassNameBytes = 255;
constexpr char kComponent[] = "core.class_index";

struct ClassRegistry {
    std::mutex mutex;
    std::vector<std::string> names;
};

struct ProviderState {
    SaoLegacyCoreClassMetadataProvider provider{};
    std::mutex mutex;
    std::condition_variable idle;
    size_t active_calls = 0;
    bool accepting = true;
    std::atomic_bool retained{false};
};

struct TokenRecord {
    sao_legacy_core_process_handle_t process = nullptr;
    uint64_t provider_token = 0;
    std::shared_ptr<ProviderState> owner;
    std::mutex mutex;
    std::condition_variable idle;
    size_t active_calls = 0;
    bool accepting = true;
    bool release_in_progress = false;
    bool released = false;
    std::shared_ptr<TokenRecord> quarantine_next;
};

struct MetadataRegistry {
    std::mutex lifecycle_mutex;
    std::mutex mutex;
    std::shared_ptr<ProviderState> provider;
    std::unordered_map<sao_legacy_core_class_token_t, std::shared_ptr<TokenRecord>> tokens;
    std::shared_ptr<TokenRecord> quarantined_tokens;
    std::shared_ptr<ProviderState> candidate_quarantine;
    sao_legacy_core_class_token_t next_token = uint64_t{1} << 63u;
};

thread_local ProviderState* g_callback_provider = nullptr;

int32_t fail(int32_t status, const char* message) noexcept {
    if (g_callback_provider == nullptr) {
        sao::legacy_core::emit_log(sao::legacy_core::kLogLevelError, kComponent, status, message);
    }
    return status;
}

ClassRegistry& registry() {
    static ClassRegistry value;
    return value;
}

MetadataRegistry& metadata_registry() {
    static MetadataRegistry value;
    return value;
}

class ProviderCallLease {
  public:
    ProviderCallLease() = default;

    explicit ProviderCallLease(std::shared_ptr<ProviderState> provider) {
        (void)acquire(std::move(provider));
    }

    bool acquire(std::shared_ptr<ProviderState> provider) noexcept {
        try {
            if (active_ || provider == nullptr) {
                return false;
            }
            std::lock_guard lock(provider->mutex);
            if (!provider->accepting) {
                return false;
            }
            ++provider->active_calls;
            provider_ = std::move(provider);
            active_ = true;
            return true;
        } catch (...) {
            return false;
        }
    }

    ~ProviderCallLease() {
        if (!active_) {
            return;
        }
        try {
            {
                std::lock_guard lock(provider_->mutex);
                --provider_->active_calls;
            }
            provider_->idle.notify_all();
        } catch (...) {
        }
    }

    ProviderCallLease(const ProviderCallLease&) = delete;
    ProviderCallLease& operator=(const ProviderCallLease&) = delete;

    explicit operator bool() const noexcept {
        return active_;
    }

    ProviderState* get() const noexcept {
        return provider_.get();
    }

  private:
    std::shared_ptr<ProviderState> provider_;
    bool active_ = false;
};

class TokenCallLease {
  public:
    TokenCallLease() = default;

    ~TokenCallLease() {
        if (!active_) {
            return;
        }
        try {
            {
                std::lock_guard lock(token_->mutex);
                --token_->active_calls;
            }
            token_->idle.notify_all();
        } catch (...) {
        }
    }

    TokenCallLease(const TokenCallLease&) = delete;
    TokenCallLease& operator=(const TokenCallLease&) = delete;

    bool acquire(std::shared_ptr<TokenRecord> token) noexcept {
        try {
            if (active_ || token == nullptr) {
                return false;
            }
            std::lock_guard lock(token->mutex);
            if (!token->accepting) {
                return false;
            }
            ++token->active_calls;
            token_ = std::move(token);
            active_ = true;
            return true;
        } catch (...) {
            return false;
        }
    }

  private:
    std::shared_ptr<TokenRecord> token_;
    bool active_ = false;
};

bool valid_provider(const SaoLegacyCoreClassMetadataProvider* provider) noexcept {
    return provider != nullptr && provider->struct_size >= sizeof(*provider) &&
           (provider->abi_version >> 16u) ==
               SAO_LEGACY_CORE_CLASS_METADATA_PROVIDER_ABI_VERSION_MAJOR &&
           provider->retain != nullptr && provider->release != nullptr &&
           provider->resolve_class != nullptr && provider->resolve_field_offset != nullptr &&
           provider->release_class != nullptr;
}

int32_t normalize_provider_status(int32_t status) noexcept {
    switch (status) {
    case SAO_OK:
    case SAO_ERR_INVALID_ARGUMENT:
    case SAO_ERR_NOT_INITIALIZED:
    case SAO_ERR_HANDLE_INVALID:
    case SAO_ERR_BUFFER_TOO_SMALL:
    case SAO_ERR_OS_CALL_FAILED:
    case SAO_ERR_NOT_IMPLEMENTED:
    case SAO_ERR_NOT_FOUND:
    case SAO_ERR_ACCESS_DENIED:
    case SAO_ERR_READ_FAULT:
    case SAO_ERR_UNKNOWN:
        return status;
    default:
        return SAO_ERR_UNKNOWN;
    }
}

using CallbackBarrierFn = int32_t (*)(void*) noexcept;

int32_t invoke_callback_barrier(CallbackBarrierFn callback, void* context) noexcept {
#if defined(_MSC_VER)
    __try {
        return callback(context);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return SAO_ERR_UNKNOWN;
    }
#else
    return callback(context);
#endif
}

template <typename Callback> int32_t invoke_status_cpp(void* context) noexcept {
    try {
        return normalize_provider_status((*static_cast<Callback*>(context))());
    } catch (...) {
        return SAO_ERR_UNKNOWN;
    }
}

template <typename Callback> int32_t invoke_void_cpp(void* context) noexcept {
    try {
        (*static_cast<Callback*>(context))();
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_UNKNOWN;
    }
}

template <typename Callback>
int32_t invoke_status_callback(ProviderState* provider, Callback&& callback) noexcept {
    using StoredCallback = std::remove_reference_t<Callback>;
    ProviderState* previous = g_callback_provider;
    g_callback_provider = provider;
    const int32_t status = invoke_callback_barrier(&invoke_status_cpp<StoredCallback>, &callback);
    g_callback_provider = previous;
    return status;
}

template <typename Callback>
int32_t invoke_void_callback(ProviderState* provider, Callback&& callback) noexcept {
    using StoredCallback = std::remove_reference_t<Callback>;
    ProviderState* previous = g_callback_provider;
    g_callback_provider = provider;
    const int32_t status = invoke_callback_barrier(&invoke_void_cpp<StoredCallback>, &callback);
    g_callback_provider = previous;
    return status;
}

int32_t release_provider_token(ProviderState* provider, uint64_t provider_token) noexcept {
    return invoke_void_callback(provider, [&] {
        provider->provider.release_class(provider->provider.user_data, provider_token);
    });
}

int32_t release_provider_owner(const std::shared_ptr<ProviderState>& provider) noexcept {
    if (provider == nullptr || !provider->retained.load(std::memory_order_acquire)) {
        return SAO_OK;
    }
    const int32_t status = invoke_void_callback(
        provider.get(), [&] { provider->provider.release(provider->provider.user_data); });
    if (status == SAO_OK) {
        provider->retained.store(false, std::memory_order_release);
    }
    return status;
}

int32_t wait_for_provider_calls(const std::shared_ptr<ProviderState>& provider) noexcept {
    if (provider == nullptr) {
        return SAO_OK;
    }
    try {
        std::unique_lock lock(provider->mutex);
        provider->idle.wait(lock, [&] { return provider->active_calls == 0; });
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_UNKNOWN;
    }
}

int32_t wait_for_token_calls(const std::shared_ptr<TokenRecord>& token) noexcept {
    if (token == nullptr) {
        return SAO_OK;
    }
    try {
        std::unique_lock lock(token->mutex);
        token->idle.wait(lock, [&] { return token->active_calls == 0; });
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_UNKNOWN;
    }
}

int32_t first_failure(int32_t current, int32_t candidate) noexcept {
    return current == SAO_OK && candidate != SAO_OK ? candidate : current;
}

int32_t release_token_record(const std::shared_ptr<TokenRecord>& token) noexcept {
    if (token == nullptr || token->owner == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        {
            std::lock_guard lock(token->mutex);
            if (token->released) {
                return SAO_OK;
            }
            if (token->release_in_progress) {
                return SAO_ERR_OS_CALL_FAILED;
            }
            token->accepting = false;
            token->release_in_progress = true;
        }

        const int32_t wait_status = wait_for_token_calls(token);
        if (wait_status != SAO_OK) {
            std::lock_guard lock(token->mutex);
            token->release_in_progress = false;
            return wait_status;
        }

        const int32_t status = release_provider_token(token->owner.get(), token->provider_token);
        {
            std::lock_guard lock(token->mutex);
            if (status == SAO_OK) {
                token->provider_token = 0;
                token->released = true;
            }
            token->release_in_progress = false;
        }
        return status;
    } catch (...) {
        try {
            std::lock_guard lock(token->mutex);
            token->release_in_progress = false;
        } catch (...) {
        }
        return SAO_ERR_UNKNOWN;
    }
}

bool quarantine_token(const std::shared_ptr<TokenRecord>& token) noexcept {
    try {
        auto& state = metadata_registry();
        std::lock_guard lock(state.mutex);
        for (auto current = state.quarantined_tokens; current != nullptr;
             current = current->quarantine_next) {
            if (current == token) {
                return true;
            }
        }
        token->quarantine_next = state.quarantined_tokens;
        state.quarantined_tokens = token;
        return true;
    } catch (...) {
        return false;
    }
}

void erase_quarantined_token(const std::shared_ptr<TokenRecord>& token) noexcept {
    try {
        auto& state = metadata_registry();
        std::lock_guard lock(state.mutex);
        auto* link = &state.quarantined_tokens;
        while (*link != nullptr) {
            if (*link == token) {
                auto next = std::move((*link)->quarantine_next);
                *link = std::move(next);
                return;
            }
            link = &(*link)->quarantine_next;
        }
    } catch (...) {
    }
}

int32_t release_quarantined_token(const std::shared_ptr<TokenRecord>& token) noexcept {
    const int32_t status = release_token_record(token);
    if (status == SAO_OK) {
        erase_quarantined_token(token);
    }
    return status;
}

int32_t cleanup_candidate_quarantine() noexcept {
    try {
        auto& state = metadata_registry();
        std::shared_ptr<ProviderState> candidate;
        {
            std::lock_guard lock(state.mutex);
            candidate = state.candidate_quarantine;
        }
        const int32_t status = release_provider_owner(candidate);
        if (status != SAO_OK) {
            return status;
        }
        std::lock_guard lock(state.mutex);
        if (state.candidate_quarantine == candidate) {
            state.candidate_quarantine.reset();
        }
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_UNKNOWN;
    }
}

int32_t release_candidate_or_quarantine(const std::shared_ptr<ProviderState>& candidate) noexcept {
    const int32_t status = release_provider_owner(candidate);
    if (status == SAO_OK || candidate == nullptr) {
        return status;
    }
    try {
        auto& state = metadata_registry();
        std::lock_guard lock(state.mutex);
        state.candidate_quarantine = candidate;
    } catch (...) {
        return SAO_ERR_UNKNOWN;
    }
    return status;
}

int32_t quarantine_and_release_provider_token(sao_legacy_core_process_handle_t process,
                                              const std::shared_ptr<ProviderState>& provider,
                                              uint64_t provider_token) noexcept {
    try {
        auto token = std::make_shared<TokenRecord>();
        token->process = process;
        token->provider_token = provider_token;
        token->owner = provider;
        token->accepting = false;
        if (!quarantine_token(token)) {
            return SAO_ERR_UNKNOWN;
        }
        return release_quarantined_token(token);
    } catch (...) {
        return SAO_ERR_UNKNOWN;
    }
}

int32_t cleanup_current_provider() noexcept {
    try {
        auto& state = metadata_registry();
        std::shared_ptr<ProviderState> previous;
        {
            std::lock_guard registry_lock(state.mutex);
            previous = state.provider;
            if (previous == nullptr) {
                return SAO_OK;
            }
        }
        {
            std::lock_guard provider_lock(previous->mutex);
            previous->accepting = false;
        }
        {
            std::lock_guard registry_lock(state.mutex);
            for (const auto& [_, token] : state.tokens) {
                if (token->owner == previous) {
                    std::lock_guard token_lock(token->mutex);
                    token->accepting = false;
                }
            }
            for (auto token = state.quarantined_tokens; token != nullptr;
                 token = token->quarantine_next) {
                if (token->owner == previous) {
                    std::lock_guard token_lock(token->mutex);
                    token->accepting = false;
                }
            }
        }

        const int32_t wait_status = wait_for_provider_calls(previous);
        if (wait_status != SAO_OK) {
            return wait_status;
        }

        std::vector<std::pair<sao_legacy_core_class_token_t, std::shared_ptr<TokenRecord>>>
            public_tokens;
        std::vector<std::shared_ptr<TokenRecord>> quarantined_tokens;
        {
            std::lock_guard registry_lock(state.mutex);
            for (const auto& [class_token, token] : state.tokens) {
                if (token->owner == previous) {
                    public_tokens.emplace_back(class_token, token);
                }
            }
            for (auto token = state.quarantined_tokens; token != nullptr;
                 token = token->quarantine_next) {
                if (token->owner == previous) {
                    quarantined_tokens.push_back(token);
                }
            }
        }

        for (const auto& [class_token, token] : public_tokens) {
            const int32_t status = release_token_record(token);
            if (status != SAO_OK) {
                return status;
            }
            std::lock_guard registry_lock(state.mutex);
            const auto found = state.tokens.find(class_token);
            if (found != state.tokens.end() && found->second == token) {
                state.tokens.erase(found);
            }
        }
        for (const auto& token : quarantined_tokens) {
            const int32_t status = release_token_record(token);
            if (status != SAO_OK) {
                return status;
            }
            erase_quarantined_token(token);
        }

        const int32_t release_status = release_provider_owner(previous);
        if (release_status != SAO_OK) {
            return release_status;
        }

        std::lock_guard registry_lock(state.mutex);
        if (state.provider != previous) {
            return SAO_ERR_UNKNOWN;
        }
        state.provider.reset();
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_UNKNOWN;
    }
}

sao_legacy_core_class_token_t next_token_locked(MetadataRegistry& state) {
    constexpr sao_legacy_core_class_token_t kOpaqueTokenTag = uint64_t{1} << 63u;
    for (;;) {
        const sao_legacy_core_class_token_t candidate = state.next_token++;
        if (state.next_token < kOpaqueTokenTag) {
            state.next_token = kOpaqueTokenTag;
        }
        if (candidate != 0 && !state.tokens.contains(candidate)) {
            return candidate;
        }
    }
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
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, static_cast<int>(length),
                               nullptr, 0) > 0;
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

int32_t register_class_impl(const char* class_name_utf8, uint32_t* out_index) noexcept {
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

} // namespace

extern "C" int32_t SAO_LEGACY_CORE_CALL sao_legacy_core_class_index_resolve(
    sao_legacy_core_process_handle_t handle, const char* class_name_utf8, uint64_t* out_class_ptr) {
    if (out_class_ptr != nullptr) {
        *out_class_ptr = 0;
    }
    if (out_class_ptr == nullptr || !valid_utf8_name(class_name_utf8)) {
        return fail(SAO_ERR_INVALID_ARGUMENT, "class_index_resolve received invalid arguments");
    }
    if (handle == nullptr) {
        return fail(SAO_ERR_HANDLE_INVALID, "class_index_resolve received an invalid process");
    }

    std::unique_lock<std::mutex> lifecycle_lock;
    try {
        auto& state = metadata_registry();
        lifecycle_lock = std::unique_lock<std::mutex>(state.lifecycle_mutex);
        std::shared_ptr<ProviderState> provider;
        {
            std::lock_guard lock(state.mutex);
            provider = state.provider;
        }

        int32_t failure_status = SAO_OK;
        const char* failure_message = nullptr;
        uint64_t provider_token = 0;
        sao_legacy_core_class_token_t class_token = 0;
        if (provider == nullptr) {
            failure_status = SAO_ERR_NOT_INITIALIZED;
            failure_message = "class_index_resolve has no metadata provider";
        } else {
            ProviderCallLease lease(provider);
            if (!lease) {
                failure_status = SAO_ERR_NOT_INITIALIZED;
                failure_message = "class_index_resolve metadata provider is retiring";
            } else {
                int32_t status = invoke_status_callback(lease.get(), [&] {
                    return provider->provider.resolve_class(provider->provider.user_data, handle,
                                                            class_name_utf8, &provider_token);
                });
                if (status != SAO_OK || provider_token == 0) {
                    if (provider_token != 0) {
                        status = first_failure(status, quarantine_and_release_provider_token(
                                                           handle, provider, provider_token));
                    }
                    if (status == SAO_OK) {
                        status = SAO_ERR_HANDLE_INVALID;
                    }
                    failure_status = status;
                    failure_message = "class_index_resolve provider failed";
                }
            }
            if (failure_message == nullptr) {
                bool provider_retired = false;
                bool ownership_failed = false;
                try {
                    std::lock_guard registry_lock(state.mutex);
                    std::lock_guard provider_lock(provider->mutex);
                    provider_retired = !provider->accepting || state.provider != provider;
                    if (!provider_retired) {
                        class_token = next_token_locked(state);
                        auto token = std::make_shared<TokenRecord>();
                        token->process = handle;
                        token->provider_token = provider_token;
                        token->owner = provider;
                        state.tokens.emplace(class_token, std::move(token));
                    }
                } catch (...) {
                    ownership_failed = true;
                }

                if (ownership_failed || provider_retired) {
                    const int32_t release_status =
                        quarantine_and_release_provider_token(handle, provider, provider_token);
                    failure_status =
                        release_status == SAO_OK
                            ? (ownership_failed ? SAO_ERR_UNKNOWN : SAO_ERR_NOT_INITIALIZED)
                            : release_status;
                    failure_message =
                        ownership_failed ? "class_index_resolve failed to own the provider token"
                                         : "class_index_resolve provider retired during resolution";
                }
            }
        }

        lifecycle_lock.unlock();
        if (failure_message != nullptr) {
            return fail(failure_status, failure_message);
        }

        *out_class_ptr = class_token;
        return SAO_OK;
    } catch (...) {
        if (lifecycle_lock.owns_lock()) {
            lifecycle_lock.unlock();
        }
        return fail(SAO_ERR_UNKNOWN, "class_index_resolve failed unexpectedly");
    }
}

extern "C" int32_t SAO_LEGACY_CORE_CALL sao_legacy_core_class_index_resolve_field_offset(
    sao_legacy_core_process_handle_t handle, uint64_t class_ptr, const char* field_name_utf8,
    uint32_t* out_offset) {
    if (out_offset != nullptr) {
        *out_offset = 0;
    }
    if (out_offset == nullptr || class_ptr == 0 || !valid_utf8_name(field_name_utf8)) {
        return fail(SAO_ERR_INVALID_ARGUMENT,
                    "class_index_resolve_field_offset received invalid arguments");
    }
    if (handle == nullptr) {
        return fail(SAO_ERR_HANDLE_INVALID,
                    "class_index_resolve_field_offset received an invalid process");
    }

    try {
        std::shared_ptr<TokenRecord> token;
        int32_t failure_status = SAO_OK;
        const char* failure_message = nullptr;
        {
            auto& state = metadata_registry();
            std::lock_guard lock(state.mutex);
            const auto found = state.tokens.find(class_ptr);
            if (found == state.tokens.end() || found->second->process != handle) {
                failure_status = SAO_ERR_HANDLE_INVALID;
                failure_message =
                    "class_index_resolve_field_offset received a stale or foreign token";
            } else {
                token = found->second;
            }
        }

        if (failure_message == nullptr) {
            {
                ProviderCallLease provider_lease;
                TokenCallLease token_lease;
                if (!provider_lease.acquire(token->owner) || !token_lease.acquire(token)) {
                    failure_status = SAO_ERR_HANDLE_INVALID;
                    failure_message = "class_index_resolve_field_offset token is retiring";
                } else {
                    failure_status = invoke_status_callback(provider_lease.get(), [&] {
                        return token->owner->provider.resolve_field_offset(
                            token->owner->provider.user_data, handle, token->provider_token,
                            field_name_utf8, out_offset);
                    });
                    if (failure_status != SAO_OK) {
                        failure_message = "class_index_resolve_field_offset provider failed";
                    }
                }
            }
        }

        if (failure_message != nullptr) {
            *out_offset = 0;
            return fail(failure_status, failure_message);
        }
        return SAO_OK;
    } catch (...) {
        *out_offset = 0;
        return fail(SAO_ERR_UNKNOWN, "class_index_resolve_field_offset failed unexpectedly");
    }
}

extern "C" int32_t SAO_LEGACY_CORE_CALL
sao_legacy_core_class_index_configure_provider(const SaoLegacyCoreClassMetadataProvider* provider) {
    if (g_callback_provider != nullptr) {
        return SAO_ERR_OS_CALL_FAILED;
    }

    auto& state = metadata_registry();
    std::unique_lock<std::mutex> lifecycle_lock;
    try {
        lifecycle_lock = std::unique_lock<std::mutex>(state.lifecycle_mutex);
        const int32_t quarantine_status = cleanup_candidate_quarantine();
        if (quarantine_status != SAO_OK) {
            lifecycle_lock.unlock();
            return fail(quarantine_status,
                        "class_index_configure_provider candidate cleanup remains quarantined");
        }

        std::shared_ptr<ProviderState> candidate;
        if (provider != nullptr) {
            if (!valid_provider(provider)) {
                lifecycle_lock.unlock();
                return fail(SAO_ERR_INVALID_ARGUMENT,
                            "class_index_configure_provider received an invalid provider");
            }
            try {
                candidate = std::make_shared<ProviderState>();
                std::memcpy(&candidate->provider, provider,
                            std::min<size_t>(provider->struct_size, sizeof(candidate->provider)));
            } catch (...) {
                lifecycle_lock.unlock();
                return fail(SAO_ERR_UNKNOWN,
                            "class_index_configure_provider failed to copy the provider");
            }
            candidate->retained.store(true, std::memory_order_release);
            const int32_t retain_status = invoke_void_callback(candidate.get(), [&] {
                candidate->provider.retain(candidate->provider.user_data);
            });
            if (retain_status != SAO_OK) {
                const int32_t rollback_status = release_candidate_or_quarantine(candidate);
                const int32_t status = first_failure(retain_status, rollback_status);
                lifecycle_lock.unlock();
                return fail(status,
                            "class_index_configure_provider retain failed; rollback attempted");
            }
        }

        const int32_t cleanup_status = cleanup_current_provider();
        if (cleanup_status != SAO_OK) {
            const int32_t rollback_status = release_candidate_or_quarantine(candidate);
            const int32_t status = first_failure(cleanup_status, rollback_status);
            lifecycle_lock.unlock();
            return fail(status,
                        "class_index_configure_provider cleanup failed; ownership quarantined");
        }

        bool install_collision = false;
        try {
            std::lock_guard registry_lock(state.mutex);
            if (state.provider != nullptr) {
                install_collision = true;
            } else {
                state.provider = candidate;
            }
        } catch (...) {
            const int32_t rollback_status = release_candidate_or_quarantine(candidate);
            const int32_t status = rollback_status == SAO_OK ? SAO_ERR_UNKNOWN : rollback_status;
            lifecycle_lock.unlock();
            return fail(status, "class_index_configure_provider failed to install the provider");
        }
        if (install_collision) {
            const int32_t rollback_status = release_candidate_or_quarantine(candidate);
            const int32_t status = rollback_status == SAO_OK ? SAO_ERR_UNKNOWN : rollback_status;
            lifecycle_lock.unlock();
            return fail(status,
                        "class_index_configure_provider cleanup did not retire the provider");
        }
        return SAO_OK;
    } catch (...) {
        if (lifecycle_lock.owns_lock()) {
            lifecycle_lock.unlock();
        }
        return fail(SAO_ERR_UNKNOWN, "class_index_configure_provider failed unexpectedly");
    }
}

extern "C" int32_t SAO_LEGACY_CORE_CALL sao_legacy_core_class_index_release(
    sao_legacy_core_process_handle_t handle, sao_legacy_core_class_token_t class_token) {
    if (g_callback_provider != nullptr) {
        return SAO_ERR_OS_CALL_FAILED;
    }
    if (class_token == 0) {
        return fail(SAO_ERR_INVALID_ARGUMENT, "class_index_release received invalid arguments");
    }
    if (handle == nullptr) {
        return fail(SAO_ERR_HANDLE_INVALID, "class_index_release received an invalid process");
    }

    std::unique_lock<std::mutex> lifecycle_lock;
    try {
        auto& state = metadata_registry();
        lifecycle_lock = std::unique_lock<std::mutex>(state.lifecycle_mutex);
        std::shared_ptr<TokenRecord> token;
        int32_t failure_status = SAO_OK;
        const char* failure_message = nullptr;
        {
            std::lock_guard registry_lock(state.mutex);
            const auto found = state.tokens.find(class_token);
            if (found == state.tokens.end() || found->second->process != handle) {
                failure_status = SAO_ERR_HANDLE_INVALID;
                failure_message = "class_index_release received a stale or foreign token";
            } else {
                token = found->second;
                {
                    std::lock_guard token_lock(token->mutex);
                    token->accepting = false;
                }
            }
        }

        if (failure_message == nullptr) {
            failure_status = release_token_record(token);
            if (failure_status != SAO_OK) {
                failure_message =
                    "class_index_release provider failed; ownership quarantined for retry";
            }
        }

        if (failure_message != nullptr) {
            lifecycle_lock.unlock();
            return fail(failure_status, failure_message);
        }
        {
            std::lock_guard registry_lock(state.mutex);
            const auto found = state.tokens.find(class_token);
            if (found != state.tokens.end() && found->second == token) {
                state.tokens.erase(found);
            }
        }
        return SAO_OK;
    } catch (...) {
        if (lifecycle_lock.owns_lock()) {
            lifecycle_lock.unlock();
        }
        return fail(SAO_ERR_UNKNOWN, "class_index_release failed unexpectedly");
    }
}

extern "C" int32_t SAO_LEGACY_CORE_CALL
sao_legacy_core_class_index_register(const char* class_name_utf8, uint32_t* out_index) {
    const int32_t status = register_class_impl(class_name_utf8, out_index);
    if (status != SAO_OK) {
        return fail(status, "class_index_register failed");
    }
    return SAO_OK;
}

extern "C" int32_t SAO_LEGACY_CORE_CALL
sao_legacy_core_class_index_find(const char* class_name_utf8, uint32_t* out_index) {
    if (out_index != nullptr) {
        *out_index = 0;
    }
    if (out_index == nullptr || !valid_utf8_name(class_name_utf8)) {
        return fail(SAO_ERR_INVALID_ARGUMENT, "class_index_find received invalid arguments");
    }

    try {
        bool found = false;
        {
            auto& state = registry();
            std::lock_guard<std::mutex> lock(state.mutex);
            found = find_locked(state.names, class_name_utf8, out_index);
        }
        return found ? SAO_OK
                     : fail(SAO_ERR_NOT_FOUND, "class_index_find found no registered class");
    } catch (...) {
        *out_index = 0;
        return fail(SAO_ERR_UNKNOWN, "class_index_find failed unexpectedly");
    }
}

extern "C" int32_t SAO_LEGACY_CORE_CALL
sao_legacy_core_class_index_get_name(uint32_t index, char* out_class_name_utf8,
                                     size_t class_name_capacity, size_t* out_required_size) {
    if (out_required_size != nullptr) {
        *out_required_size = 0;
    }
    if (out_class_name_utf8 != nullptr && class_name_capacity != 0) {
        out_class_name_utf8[0] = '\0';
    }
    if (out_required_size == nullptr ||
        (out_class_name_utf8 == nullptr && class_name_capacity != 0)) {
        return fail(SAO_ERR_INVALID_ARGUMENT,
                    "class_index_get_name received invalid output arguments");
    }

    try {
        int32_t status = SAO_OK;
        {
            auto& state = registry();
            std::lock_guard<std::mutex> lock(state.mutex);
            if (index >= state.names.size()) {
                status = SAO_ERR_NOT_FOUND;
            } else {
                const std::string& name = state.names[index];
                *out_required_size = name.size() + 1;
                if (out_class_name_utf8 != nullptr) {
                    if (class_name_capacity < name.size() + 1) {
                        status = SAO_ERR_BUFFER_TOO_SMALL;
                    } else {
                        std::memcpy(out_class_name_utf8, name.c_str(), name.size() + 1);
                    }
                }
            }
        }
        if (status != SAO_OK) {
            return fail(status, "class_index_get_name failed");
        }
        return SAO_OK;
    } catch (...) {
        if (out_class_name_utf8 != nullptr && class_name_capacity != 0) {
            out_class_name_utf8[0] = '\0';
        }
        *out_required_size = 0;
        return fail(SAO_ERR_UNKNOWN, "class_index_get_name failed unexpectedly");
    }
}

extern "C" int32_t SAO_LEGACY_CORE_CALL sao_legacy_core_class_index_count(size_t* out_count) {
    if (out_count != nullptr) {
        *out_count = 0;
    }
    if (out_count == nullptr) {
        return fail(SAO_ERR_INVALID_ARGUMENT, "class_index_count requires an output pointer");
    }

    try {
        auto& state = registry();
        std::lock_guard<std::mutex> lock(state.mutex);
        *out_count = state.names.size();
        return SAO_OK;
    } catch (...) {
        return fail(SAO_ERR_UNKNOWN, "class_index_count failed unexpectedly");
    }
}
