#pragma once

#include "sao/ai_editor/ai_editor_launcher.h"
#include "sao/core/status.h"

#include <nlohmann/json.hpp>

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
sao_status_t native_adapter_submit(NativeAdapter* adapter, std::string_view document_token,
                                   std::string_view request_id, std::string_view method,
                                   const nlohmann::json& args) noexcept;
sao_status_t native_adapter_drain(NativeAdapter* adapter,
                                  std::vector<NativeAdapterCompletion>* completions,
                                  std::vector<NativeAdapterEvent>* events) noexcept;
sao_status_t native_adapter_try_destroy(NativeAdapter* adapter) noexcept;

} // namespace sao::ai_editor::workbench
