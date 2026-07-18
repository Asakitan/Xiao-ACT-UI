#include "auth_device_flow.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace sao::ai_editor::native {
namespace {

int64_t unix_milliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string url_encode(std::string_view value) {
    static const char* kHex = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size() * 3);
    for (unsigned char character : value) {
        const bool alpha = (character >= 'A' && character <= 'Z') ||
                           (character >= 'a' && character <= 'z');
        const bool digit = character >= '0' && character <= '9';
        const bool unreserved = character == '-' || character == '_' ||
                                character == '.' || character == '~';
        if (alpha || digit || unreserved) {
            encoded.push_back(static_cast<char>(character));
        } else {
            encoded.push_back('%');
            encoded.push_back(kHex[character >> 4]);
            encoded.push_back(kHex[character & 0x0F]);
        }
    }
    return encoded;
}

std::string next_flow_id() {
    static std::atomic<uint64_t> sequence{0};
    const uint64_t ticks = static_cast<uint64_t>(unix_milliseconds());
    const uint64_t current =
        sequence.fetch_add(1, std::memory_order_relaxed);
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "df-%llx-%llx",
                  static_cast<unsigned long long>(ticks),
                  static_cast<unsigned long long>(current));
    return buffer;
}

struct HttpResponse {
    DWORD status_code = 0;
    std::string body;
};

class InternetHandle {
public:
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET handle) noexcept : handle_(handle) {}
    ~InternetHandle() {
        if (handle_ != nullptr) {
            WinHttpCloseHandle(handle_);
        }
    }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    HINTERNET get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ != nullptr; }
private:
    HINTERNET handle_ = nullptr;
};

bool crack_url(std::string_view endpoint, std::wstring& host,
               std::wstring& path, INTERNET_PORT& port, bool& secure) {
    if (endpoint.empty() || endpoint.size() > 16'384 ||
        !valid_utf8(endpoint)) {
        return false;
    }
    const std::wstring wide = utf8_to_wide(endpoint);
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0,
                         &components)) {
        return false;
    }
    if (components.nScheme != INTERNET_SCHEME_HTTP &&
        components.nScheme != INTERNET_SCHEME_HTTPS) {
        return false;
    }
    host.assign(components.lpszHostName, components.dwHostNameLength);
    path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength > 0) {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (path.empty()) {
        path = L"/";
    }
    port = components.nPort;
    secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    return !host.empty();
}

int32_t post_form(std::string_view endpoint, std::string_view body,
                  HttpResponse& out) {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool secure = false;
    if (!crack_url(endpoint, host, path, port, secure)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    InternetHandle session(WinHttpOpen(
        L"SAO-AI-Editor/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        return SAO_AI_EDITOR_ERR_HTTP;
    }
    InternetHandle connection(
        WinHttpConnect(session.get(), host.c_str(), port, 0));
    if (!connection) {
        return SAO_AI_EDITOR_ERR_HTTP;
    }
    const DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    InternetHandle request(WinHttpOpenRequest(
        connection.get(), L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request) {
        return SAO_AI_EDITOR_ERR_HTTP;
    }
    const std::wstring headers =
        L"Content-Type: application/x-www-form-urlencoded\r\n"
        L"Accept: application/json\r\n";
    if (!WinHttpSendRequest(
            request.get(), headers.c_str(),
            static_cast<DWORD>(headers.size()),
            const_cast<char*>(body.data()),
            static_cast<DWORD>(body.size()),
            static_cast<DWORD>(body.size()), 0)) {
        return SAO_AI_EDITOR_ERR_HTTP;
    }
    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        return SAO_AI_EDITOR_ERR_HTTP;
    }
    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    if (!WinHttpQueryHeaders(request.get(),
                             WINHTTP_QUERY_STATUS_CODE |
                                 WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status_code,
                             &status_size, WINHTTP_NO_HEADER_INDEX)) {
        return SAO_AI_EDITOR_ERR_HTTP;
    }
    out.status_code = status_code;
    std::array<char, 16U * 1024U> buffer{};
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available) ||
            available == 0) {
            break;
        }
        while (available > 0) {
            const DWORD requested = std::min<DWORD>(
                available, static_cast<DWORD>(buffer.size()));
            DWORD read = 0;
            if (!WinHttpReadData(request.get(), buffer.data(), requested,
                                 &read) ||
                read == 0) {
                break;
            }
            out.body.append(buffer.data(), read);
            if (out.body.size() > 4U * 1024U * 1024U) {
                return SAO_AI_EDITOR_ERR_PROTOCOL;
            }
            available -= read;
        }
    }
    return SAO_AI_EDITOR_OK;
}

}  // namespace

int32_t AuthDeviceFlow::begin(const Json& params, DeviceFlowState& state) {
    if (!params.is_object()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::string provider_id = params.value("providerId", std::string{});
    const std::string device_authorization_url =
        params.value("deviceAuthorizationUrl", std::string{});
    const std::string token_url = params.value("tokenUrl", std::string{});
    const std::string client_id = params.value("clientId", std::string{});
    if (!valid_simple_id(provider_id) || device_authorization_url.empty() ||
        token_url.empty() || client_id.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::string scope = params.value("scope", std::string{});
    const std::string audience = params.value("audience", std::string{});
    const std::string client_secret =
        params.value("clientSecret", std::string{});
    std::string body =
        "client_id=" + url_encode(client_id);
    if (!scope.empty()) {
        body += "&scope=" + url_encode(scope);
    }
    if (!audience.empty()) {
        body += "&audience=" + url_encode(audience);
    }
    HttpResponse response;
    const int32_t http_status =
        post_form(device_authorization_url, body, response);
    if (http_status != SAO_AI_EDITOR_OK) {
        return http_status;
    }
    if (response.status_code < 200 || response.status_code >= 300) {
        return SAO_AI_EDITOR_ERR_HTTP;
    }
    Json payload = Json::parse(response.body, nullptr, false);
    if (!payload.is_object()) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
    state.flow_id = next_flow_id();
    state.provider_id = provider_id;
    state.token_endpoint = token_url;
    state.client_id = client_id;
    state.client_secret = client_secret;
    state.audience = audience;
    state.scope = scope;
    state.device_code = payload.value("device_code", std::string{});
    state.user_code = payload.value("user_code", std::string{});
    state.verification_uri = payload.value(
        "verification_uri",
        payload.value("verification_url", std::string{}));
    state.verification_uri_complete = payload.value(
        "verification_uri_complete",
        payload.value("verification_url_complete", std::string{}));
    const uint32_t interval = payload.value("interval", 5U);
    state.interval_seconds = std::clamp<uint32_t>(interval, 1U, 60U);
    const int64_t expires_in = payload.value("expires_in", int64_t{600});
    state.started_at_unix_ms = unix_milliseconds();
    state.expires_at_unix_ms =
        state.started_at_unix_ms + expires_in * 1000;
    if (state.device_code.empty() || state.user_code.empty() ||
        state.verification_uri.empty()) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
    state.status = "pending";
    {
        std::lock_guard<std::mutex> guard(mutex_);
        flows_[state.flow_id] = state;
    }
    return SAO_AI_EDITOR_OK;
}

void AuthDeviceFlow::write_state_locked(const DeviceFlowState& state,
                                        Json& out) const {
    out = Json{{"flowId", state.flow_id},
               {"providerId", state.provider_id},
               {"userCode", state.user_code},
               {"verificationUri", state.verification_uri},
               {"verificationUriComplete", state.verification_uri_complete},
               {"intervalSeconds", state.interval_seconds},
               {"expiresAtUnixMs", state.expires_at_unix_ms},
               {"status", state.status}};
}

int32_t AuthDeviceFlow::persist_token(const std::string& provider_id,
                                      const Json& token,
                                      int64_t expires_in_seconds) {
    if (store_ == nullptr) {
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    Json wrapped = token;
    wrapped["fetched_at_unix_ms"] = unix_milliseconds();
    if (expires_in_seconds > 0) {
        wrapped["expires_at_unix_ms"] =
            unix_milliseconds() + expires_in_seconds * 1000;
    }
    // If the caller already merged _sao_flow_config in, keep it — otherwise
    // fall back to the currently-cached blob so refresh metadata survives
    // partial responses (e.g. some IdPs omit refresh_token on refresh).
    if (!wrapped.contains("_sao_flow_config")) {
        std::string cached;
        if (store_->get("auth/" + provider_id + "/token", cached) ==
                SAO_AI_EDITOR_OK) {
            Json parsed = Json::parse(cached, nullptr, false);
            if (parsed.is_object() && parsed.contains("_sao_flow_config")) {
                wrapped["_sao_flow_config"] = parsed["_sao_flow_config"];
            }
        }
    }
    const std::string secret_key =
        "auth/" + provider_id + "/token";
    return store_->set(secret_key, wrapped.dump());
}

int32_t AuthDeviceFlow::poll(std::string_view flow_id, Json& out) {
    DeviceFlowState snapshot;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = flows_.find(std::string(flow_id));
        if (found == flows_.end()) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        snapshot = found->second;
    }
    if (snapshot.status == "success" || snapshot.status == "expired" ||
        snapshot.status == "denied" || snapshot.status == "cancelled") {
        std::lock_guard<std::mutex> guard(mutex_);
        write_state_locked(flows_[std::string(flow_id)], out);
        return SAO_AI_EDITOR_OK;
    }
    if (unix_milliseconds() > snapshot.expires_at_unix_ms) {
        std::lock_guard<std::mutex> guard(mutex_);
        auto& state = flows_[std::string(flow_id)];
        state.status = "expired";
        write_state_locked(state, out);
        return SAO_AI_EDITOR_OK;
    }
    std::string body =
        "grant_type=urn:ietf:params:oauth:grant-type:device_code";
    body += "&device_code=" + url_encode(snapshot.device_code);
    body += "&client_id=" + url_encode(snapshot.client_id);
    if (!snapshot.client_secret.empty()) {
        body += "&client_secret=" + url_encode(snapshot.client_secret);
    }
    HttpResponse response;
    const int32_t http_status = post_form(snapshot.token_endpoint, body,
                                          response);
    if (http_status != SAO_AI_EDITOR_OK) {
        return http_status;
    }
    Json payload = Json::parse(response.body, nullptr, false);
    if (!payload.is_object()) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    auto& state = flows_[std::string(flow_id)];
    state.last_poll_unix_ms = unix_milliseconds();
    if (payload.contains("access_token") &&
        payload["access_token"].is_string()) {
        state.status = "success";
        const int64_t expires_in = payload.value("expires_in", int64_t{0});
        Json enriched = payload;
        enriched["_sao_flow_config"] = Json{
            {"token_endpoint", state.token_endpoint},
            {"client_id", state.client_id},
            {"client_secret", state.client_secret}};
        const int32_t persist_status =
            persist_token(state.provider_id, enriched, expires_in);
        if (persist_status != SAO_AI_EDITOR_OK) {
            state.status = "failed";
            return persist_status;
        }
        write_state_locked(state, out);
        out["accessToken"] = payload["access_token"];
        if (payload.contains("refresh_token")) {
            out["refreshToken"] = payload["refresh_token"];
        }
        if (payload.contains("token_type")) {
            out["tokenType"] = payload["token_type"];
        }
        return SAO_AI_EDITOR_OK;
    }
    const std::string error = payload.value("error", std::string{});
    if (error == "authorization_pending") {
        state.status = "pending";
    } else if (error == "slow_down") {
        state.status = "pending";
        state.interval_seconds += 5;
    } else if (error == "access_denied") {
        state.status = "denied";
    } else if (error == "expired_token") {
        state.status = "expired";
    } else if (!error.empty()) {
        state.status = "failed";
    }
    write_state_locked(state, out);
    out["oauthError"] = error;
    if (payload.contains("error_description")) {
        out["oauthErrorDescription"] = payload["error_description"];
    }
    return SAO_AI_EDITOR_OK;
}

int32_t AuthDeviceFlow::status(std::string_view flow_id, Json& out) const {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto found = flows_.find(std::string(flow_id));
    if (found == flows_.end()) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    write_state_locked(found->second, out);
    return SAO_AI_EDITOR_OK;
}

int32_t AuthDeviceFlow::cancel(std::string_view flow_id, Json& out) {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto found = flows_.find(std::string(flow_id));
    if (found == flows_.end()) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    found->second.status = "cancelled";
    write_state_locked(found->second, out);
    return SAO_AI_EDITOR_OK;
}

int32_t AuthDeviceFlow::store_token(std::string_view provider_id,
                                    const Json& token, Json& out) {
    if (!valid_simple_id(provider_id) || !token.is_object() ||
        !token.contains("access_token") ||
        !token["access_token"].is_string()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    if (store_ == nullptr) {
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    const int64_t expires_in = token.value("expires_in", int64_t{0});
    const int32_t status = persist_token(std::string(provider_id), token,
                                          expires_in);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    out = Json{{"ok", true}, {"providerId", std::string(provider_id)}};
    return SAO_AI_EDITOR_OK;
}

int32_t AuthDeviceFlow::load_token(std::string_view provider_id,
                                   Json& out) const {
    if (!valid_simple_id(provider_id)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    if (store_ == nullptr) {
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    const std::string secret_key =
        "auth/" + std::string(provider_id) + "/token";
    std::string value;
    const int32_t status = store_->get(secret_key, value);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    Json parsed = Json::parse(value, nullptr, false);
    if (!parsed.is_object()) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
    out = std::move(parsed);
    return SAO_AI_EDITOR_OK;
}

int32_t AuthDeviceFlow::refresh(std::string_view provider_id, Json& out) {
    if (!valid_simple_id(provider_id)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    if (store_ == nullptr) {
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    Json existing;
    const int32_t load_status = load_token(provider_id, existing);
    if (load_status != SAO_AI_EDITOR_OK) {
        return load_status;
    }
    const std::string refresh_token = existing.value("refresh_token",
                                                      std::string{});
    if (refresh_token.empty()) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    const Json config = existing.value("_sao_flow_config", Json::object());
    const std::string token_endpoint = config.value("token_endpoint",
                                                     std::string{});
    const std::string client_id = config.value("client_id",
                                                std::string{});
    const std::string client_secret = config.value("client_secret",
                                                    std::string{});
    if (token_endpoint.empty() || client_id.empty()) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    std::string body = "grant_type=refresh_token";
    body += "&refresh_token=" + url_encode(refresh_token);
    body += "&client_id=" + url_encode(client_id);
    if (!client_secret.empty()) {
        body += "&client_secret=" + url_encode(client_secret);
    }
    HttpResponse response;
    const int32_t http_status = post_form(token_endpoint, body, response);
    if (http_status != SAO_AI_EDITOR_OK) {
        return http_status;
    }
    if (response.status_code < 200 || response.status_code >= 300) {
        return SAO_AI_EDITOR_ERR_HTTP;
    }
    Json payload = Json::parse(response.body, nullptr, false);
    if (!payload.is_object() || !payload.contains("access_token") ||
        !payload["access_token"].is_string()) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
    // Some IdPs only return a fresh access_token on refresh; carry the old
    // refresh_token forward when the response omits it.
    if (!payload.contains("refresh_token") ||
        !payload["refresh_token"].is_string()) {
        payload["refresh_token"] = refresh_token;
    }
    Json enriched = payload;
    enriched["_sao_flow_config"] = config;
    const int64_t expires_in = payload.value("expires_in", int64_t{0});
    const int32_t persist_status = persist_token(std::string(provider_id),
                                                  enriched, expires_in);
    if (persist_status != SAO_AI_EDITOR_OK) {
        return persist_status;
    }
    (void)load_token(provider_id, out);
    out["refreshed"] = true;
    return SAO_AI_EDITOR_OK;
}

int32_t AuthDeviceFlow::get_access_token(std::string_view provider_id,
                                         int64_t expiry_leeway_seconds,
                                         Json& out) {
    Json existing;
    const int32_t load_status = load_token(provider_id, existing);
    if (load_status != SAO_AI_EDITOR_OK) {
        return load_status;
    }
    const int64_t expires_at = existing.value("expires_at_unix_ms",
                                               int64_t{0});
    const int64_t leeway_ms = std::max<int64_t>(0, expiry_leeway_seconds) *
                              1000;
    const bool needs_refresh = expires_at > 0 &&
                               unix_milliseconds() + leeway_ms >= expires_at;
    if (!needs_refresh) {
        out = std::move(existing);
        out["refreshed"] = false;
        return SAO_AI_EDITOR_OK;
    }
    const int32_t refresh_status = refresh(provider_id, out);
    if (refresh_status == SAO_AI_EDITOR_OK) {
        return SAO_AI_EDITOR_OK;
    }
    // Refresh failed but we still have a stored access_token — hand it back
    // with a hint so the caller can decide whether to accept the risk.
    if (refresh_status == SAO_AI_EDITOR_ERR_NOT_FOUND ||
        refresh_status == SAO_AI_EDITOR_ERR_HTTP) {
        out = std::move(existing);
        out["refreshed"] = false;
        out["refreshError"] = refresh_status;
        return SAO_AI_EDITOR_OK;
    }
    return refresh_status;
}

int32_t AuthDeviceFlow::revoke(std::string_view provider_id, Json& out) {
    if (!valid_simple_id(provider_id)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    if (store_ == nullptr) {
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    const std::string secret_key =
        "auth/" + std::string(provider_id) + "/token";
    const int32_t status = store_->erase(secret_key);
    if (status != SAO_AI_EDITOR_OK &&
        status != SAO_AI_EDITOR_ERR_NOT_FOUND) {
        return status;
    }
    out = Json{{"ok", true}, {"providerId", std::string(provider_id)}};
    return SAO_AI_EDITOR_OK;
}

}  // namespace sao::ai_editor::native
