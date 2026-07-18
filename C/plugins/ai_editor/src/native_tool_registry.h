#pragma once

#include <cstdint>
#include <string_view>

#include "scope_store.h"

namespace sao::ai_editor::native {

class NativeToolRegistry final {
public:
    NativeToolRegistry(const ScopeStore& scopes,
                       uint32_t maximum_file_bytes,
                       uint32_t maximum_search_results) noexcept;

    Json describe(std::string_view mode) const;
    int32_t execute(std::string_view mode,
                    std::string_view name,
                    const Json& arguments,
                    Json& result) const;

private:
    int32_t read_file(const Json& arguments, Json& result) const;
    int32_t list_files(const Json& arguments, Json& result) const;
    int32_t search_files(const Json& arguments, Json& result) const;
    int32_t edit_file(const Json& arguments, Json& result) const;

    const ScopeStore& scopes_;
    uint32_t maximum_file_bytes_;
    uint32_t maximum_search_results_;
};

}  // namespace sao::ai_editor::native
