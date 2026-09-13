#pragma once

#include "sao/ai_editor/ai_editor_launcher.h"
#include "sao/core/status.h"
#include "sao/rt_io/proxy.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sao::ai_editor::workbench {

struct NativeAdapter;

struct NativeAdapterCompletion {
    std::string document_token;
    std::string request_id;
    bool ok{};
    nlohmann::json result;
    nlohmann::json error_data;
    std::string error_code;
    std::string error_message;
    std::uint32_t memory_pid{};
    std::uint64_t memory_start_time_100ns{};
    std::uint64_t memory_selection_generation{};
    std::uint64_t memory_binding_epoch{};
    bool memory_binding_captured{};
};

struct NativeAdapterEvent {
    std::string document_token;
    std::string name;
    nlohmann::json payload;
};

// launcher remains borrowed until native_adapter_try_destroy returns SAO_STATUS_OK.
sao_status_t native_adapter_create(sao_ai_editor_launcher_t launcher,
                                   NativeAdapter** out_adapter) noexcept;
sao_status_t native_adapter_set_document(NativeAdapter* adapter,
                                         std::string_view document_token) noexcept;
sao_status_t native_adapter_bind_memory_target(NativeAdapter* adapter,
                                               sao_rt_io_proxy_handle_t proxy, std::uint32_t pid,
                                               std::uint64_t start_time_100ns,
                                               std::uint64_t selection_generation) noexcept;
sao_status_t native_adapter_clear_memory_target(NativeAdapter* adapter) noexcept;
sao_status_t native_adapter_submit(NativeAdapter* adapter, std::string_view document_token,
                                   std::string_view request_id, std::string_view method,
                                   const nlohmann::json& args) noexcept;
sao_status_t native_adapter_drain(NativeAdapter* adapter,
                                  std::vector<NativeAdapterCompletion>* completions,
                                  std::vector<NativeAdapterEvent>* events) noexcept;
sao_status_t native_adapter_try_destroy(NativeAdapter* adapter) noexcept;

} // namespace sao::ai_editor::workbench
