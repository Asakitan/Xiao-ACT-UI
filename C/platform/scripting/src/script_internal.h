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

void set_error(SaoScriptError* error,
               sao_status_t status,
               const char* message) noexcept;

}  // namespace sao::scripting::internal
