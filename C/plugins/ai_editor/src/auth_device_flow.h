#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "native_secret_store.h"
#include "native_utils.h"

namespace sao::ai_editor::native {

// One in-flight OAuth 2.0 device authorization grant (RFC 8628).
struct DeviceFlowState {
    std::string flow_id;
    std::string provider_id;
    std::string token_endpoint;
    std::string client_id;
    std::string client_secret;
    std::string audience;
    std::string scope;
    std::string device_code;
    std::string user_code;
    std::string verification_uri;
    std::string verification_uri_complete;
    uint32_t interval_seconds = 5;
    int64_t expires_at_unix_ms = 0;
    int64_t started_at_unix_ms = 0;
    int64_t last_poll_unix_ms = 0;
    std::string status{"pending"};
};

class AuthDeviceFlow final {
public:
    explicit AuthDeviceFlow(SecretStore* store) noexcept : store_(store) {}

    // Kick off the device authorization request. On success `state` contains
    // the device/user code + verification URI + interval.
    int32_t begin(const Json& params, DeviceFlowState& state);

    // Poll the token endpoint once; caller is expected to space these out by
    // `interval_seconds`.
    int32_t poll(std::string_view flow_id, Json& out);

    // Return the last known flow state.
    int32_t status(std::string_view flow_id, Json& out) const;

    // Cancel + drop an in-flight flow.
    int32_t cancel(std::string_view flow_id, Json& out);

    // Store a token verbatim (used when the caller supplies its own OAuth
    // token acquired outside of the device flow).
    int32_t store_token(std::string_view provider_id,
                        const Json& token,
                        Json& out);

    // Load the stored token JSON blob for a provider.  Returns
    // SAO_AI_EDITOR_ERR_NOT_FOUND when no token exists.
    int32_t load_token(std::string_view provider_id, Json& out) const;

    // Erase any stored token for the provider.
    int32_t revoke(std::string_view provider_id, Json& out);

private:
    struct RefreshOutcome {
        bool refreshed = false;
        Json token;
    };

    void write_state_locked(const DeviceFlowState& state, Json& out) const;
    int32_t persist_token(const std::string& provider_id, const Json& token,
                          int64_t expires_in_seconds);

    SecretStore* store_ = nullptr;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, DeviceFlowState> flows_;
};

}  // namespace sao::ai_editor::native
