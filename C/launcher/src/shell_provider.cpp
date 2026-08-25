#include "sao/launcher/init_pipeline.h"
#include "sao/launcher/provider_config.h"

#include "sao/shell/stub/entry.h"
#include "sao/shell/stub/runtime.h"
#include "sao_core/sao_status.h"

extern "C" sao_status_t sao_shell_verify_integrity(
    sao_shell_verify_result* out) {
    if (out == nullptr) return SAO_STATUS_INVALID_ARGUMENT;
    *out = {};
    try {
        const auto configuration =
            sao::launcher::launcherProviderConfigurationSnapshot().shell;
        (void)configuration;
        out->tampered = 1;
        strncpy_s(out->reason, sizeof(out->reason),
                  "external shell metadata provider unavailable: no pointer-free emitter or production key server",
                  _TRUNCATE);
        return SAO_STATUS_NOT_IMPLEMENTED;
    } catch (...) {
        out->tampered = 1;
        strncpy_s(out->reason, sizeof(out->reason),
                  "shell provider failed", _TRUNCATE);
        return SAO_STATUS_SHELL_TAMPERED;
    }
}

extern "C" sao_status_t sao_shell_shutdown(void) {
    sao_shell_stub_runtime_clear_provider();
    return SAO_STATUS_OK;
}