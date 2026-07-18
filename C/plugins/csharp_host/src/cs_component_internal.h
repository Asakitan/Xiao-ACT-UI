#pragma once

#include "cs_host_internal.h"

#include "sao/plugins/csharp_host/cs_host.h"
#include "sao/plugins/csharp_host/cs_plugin_lifecycle.h"
#include "sao/plugins/loader/plugin_manifest.h"

#include <cstdint>
#include <string>

namespace sao::plugins::csharp_host {

struct managed_component_s;

enum class managed_hook : uint8_t {
    init_sdk,
    on_load,
    on_enable,
    on_disable,
    on_unload,
    on_tick,
    get_tick_count,
};

int32_t cshost_component_load(cs_host_handle_t host,
                              const sao::plugins::loader::plugin_manifest& manifest,
                              managed_component_s** out_component, std::string& out_error) noexcept;

int32_t cshost_component_initialize(managed_component_s* component, void* sdk_context,
                                    void* loader_context, std::string& out_error) noexcept;

int32_t cshost_component_on_load(managed_component_s* component, int32_t* out_managed_result,
                                 std::string& out_error) noexcept;

bool cshost_component_has_hook(managed_component_s* component, managed_hook hook) noexcept;

int32_t cshost_component_invoke(managed_component_s* component, managed_hook hook, void* argument,
                                int32_t argument_size, int32_t* out_managed_result,
                                std::string& out_error) noexcept;

int32_t cshost_component_close(managed_component_s* component, std::string& out_error) noexcept;
void cshost_component_abandon(managed_component_s* component) noexcept;

int32_t cshost_close_runtime_context(hostfxr_close_fn close, hostfxr_handle_t context,
                                     std::string& out_error) noexcept;

void cshost_reset_sdk_counters() noexcept;
int32_t cshost_get_sdk_counters(cs_sdk_counters* out_counters) noexcept;

int32_t cshost_copy_string(const std::string& value, char** output) noexcept;

} // namespace sao::plugins::csharp_host
