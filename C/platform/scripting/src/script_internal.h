#pragma once

#include "sao/scripting/script_engine.h"

namespace sao::scripting::internal {

constexpr size_t kLegacyVTableSize =
    offsetof(SaoScriptEngineVTable, struct_size);

bool has_provider_field(const SaoScriptEngineVTable& vtable,
                        size_t field_offset,
                        size_t field_size) noexcept;

sao_status_t acquire_provider(int32_t language,
                              SaoScriptEngineVTable* out_vtable) noexcept;

void release_provider(const SaoScriptEngineVTable& vtable) noexcept;

bool provider_callback_active(sao_script_engine_handle_t engine) noexcept;

void retry_deferred_context_cleanup(
    sao_script_engine_handle_t engine) noexcept;

void retry_deferred_engine_cleanup(
    sao_script_engine_handle_t engine) noexcept;

bool cleanup_engine_instance(
    sao_script_engine_handle_t engine,
    sao_script_instance_handle_t instance) noexcept;

void set_error(SaoScriptError* error,
               sao_status_t status,
               const char* message) noexcept;

}  // namespace sao::scripting::internal
