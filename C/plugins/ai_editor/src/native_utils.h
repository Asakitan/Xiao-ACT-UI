#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "sao/ai_editor/ai_editor_status.h"

namespace sao::ai_editor::native {

using Json = nlohmann::json;

constexpr uint32_t kDefaultMaximumFileBytes = 4U * 1024U * 1024U;
constexpr uint32_t kMaximumJsonBytes = 16U * 1024U * 1024U;
constexpr uint32_t kDefaultSearchResults = 100U;

bool valid_utf8(std::string_view value) noexcept;
bool valid_simple_id(std::string_view value) noexcept;
std::wstring utf8_to_wide(std::string_view value);
std::string wide_to_utf8(std::wstring_view value);

bool normalize_root(std::string_view value,
                    std::filesystem::path& result,
                    bool create);
bool resolve_bounded_path(const std::filesystem::path& root,
                          std::string_view value,
                          bool for_write,
                          std::filesystem::path& result);

int32_t read_text_file(const std::filesystem::path& path,
                       uint32_t maximum_bytes,
                       std::string& result);
int32_t write_text_atomic(const std::filesystem::path& path,
                          std::string_view content);

int32_t copy_text_to_caller(std::string_view value,
                            char* output,
                            uint32_t capacity,
                            uint32_t* out_length);

Json rpc_error(const Json& id,
               int code,
               std::string_view message,
               const Json& data = Json());
Json rpc_result(const Json& id, const Json& result);
std::string dump_json(const Json& value);

}  // namespace sao::ai_editor::native
