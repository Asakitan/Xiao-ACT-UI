#pragma once

#include "sao/plugins/csharp_host/cs_module_bridge.h"

#include <cstdint>

namespace sao::plugins::csharp_host {

struct managed_component_s;
struct sdk_bridge_session;

const cs_managed_sdk_table* cshost_sdk_bridge_table() noexcept;

int32_t cshost_sdk_session_create(void* runtime, void* loader_context, void* sdk_context,
                                  managed_component_s* component, bool retain_context,
                                  sdk_bridge_session** out_session) noexcept;
sdk_bridge_session* cshost_sdk_session_find(void* runtime) noexcept;
sdk_bridge_session* cshost_sdk_session_find_handle(cs_managed_sdk_session_t handle) noexcept;
cs_managed_sdk_session_t cshost_sdk_session_handle(sdk_bridge_session* session) noexcept;
int32_t cshost_sdk_session_quiesce(sdk_bridge_session* session) noexcept;
int32_t cshost_sdk_session_resume(sdk_bridge_session* session) noexcept;
int32_t cshost_sdk_session_release_callbacks(sdk_bridge_session* session) noexcept;
int32_t cshost_sdk_session_clear_sdk_context(sdk_bridge_session* session,
                                             void* sdk_context) noexcept;
int32_t cshost_sdk_session_set_binding(sdk_bridge_session* session, void* binding) noexcept;
int32_t cshost_sdk_session_finish(sdk_bridge_session* session) noexcept;
int32_t cshost_sdk_session_discard(sdk_bridge_session* session) noexcept;

int32_t cshost_sdk_wrap_callback(sdk_bridge_session* session,
                                 const cs_managed_callback_descriptor* descriptor,
                                 void** out_callback, void** out_user_data) noexcept;
int32_t cshost_sdk_wrap_callback_for_runtime(void* runtime,
                                             const cs_managed_callback_descriptor* descriptor,
                                             void** out_callback, void** out_user_data) noexcept;
void cshost_sdk_release_provider_callback(void* callback_user_data) noexcept;

int32_t cshost_sdk_bridge_test_start(void* runtime, void* loader_context, void* sdk_context,
                                     const cs_managed_sdk_table** out_table,
                                     cs_managed_sdk_session_t* out_session) noexcept;
int32_t cshost_sdk_bridge_test_finish(cs_managed_sdk_session_t session) noexcept;

} // namespace sao::plugins::csharp_host
