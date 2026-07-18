#include "sao/plugins/csharp_host/cs_sandbox.h"

#include "sao/plugins/csharp_host/cs_host.h"

#include <atomic>

namespace sao::plugins::csharp_host {
namespace {

enum class hostfxr_mock : int {
    use_real = 0,
    force_available = 1,
    force_missing = 2,
};

std::atomic<int> g_hostfxr_mock{static_cast<int>(hostfxr_mock::use_real)};

bool hostfxr_capability_present() noexcept {
    const int mode = g_hostfxr_mock.load();
    if (mode == static_cast<int>(hostfxr_mock::force_available))
        return true;
    if (mode == static_cast<int>(hostfxr_mock::force_missing))
        return false;
    bool available = false;
    return sao_plugins_cshost_is_available(&available) == SAO_OK && available;
}

} // namespace

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_sandbox_arm(cs_domain_handle_t domain, const cs_sandbox_config* cfg) {
    if (domain == nullptr || cfg == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    if (!hostfxr_capability_present())
        return SAO_ERR_NOT_INITIALIZED;
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_sandbox_set_hostfxr_mock(int mode) {
    if (mode < 0 || mode > 2)
        return SAO_ERR_INVALID_ARGUMENT;
    g_hostfxr_mock.store(mode);
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_cshost_sandbox_get_policy_snapshot(
    cs_domain_handle_t domain, uint64_t* out_alc_id, size_t* out_deny_count) {
    if (out_alc_id != nullptr)
        *out_alc_id = 0;
    if (out_deny_count != nullptr)
        *out_deny_count = 0;
    return domain == nullptr ? SAO_ERR_INVALID_ARGUMENT : SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_cshost_sandbox_is_api_denied(
    cs_domain_handle_t domain, const char* api_utf8, bool* out_denied) {
    if (out_denied != nullptr)
        *out_denied = false;
    if (domain == nullptr || api_utf8 == nullptr || out_denied == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_sandbox_release(cs_domain_handle_t domain) {
    return domain == nullptr ? SAO_ERR_INVALID_ARGUMENT : SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API size_t SAO_PLUGINS_CALL sao_plugins_cshost_sandbox_clear_all(void) {
    return 0;
}

} // namespace sao::plugins::csharp_host
