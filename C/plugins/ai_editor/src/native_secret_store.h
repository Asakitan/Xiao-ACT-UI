#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "sao/ai_editor/ai_editor_status.h"

namespace sao::ai_editor::native {

class SecretStore final {
public:
    explicit SecretStore(std::filesystem::path vault_path) noexcept;

    SecretStore(const SecretStore&) = delete;
    SecretStore& operator=(const SecretStore&) = delete;

    int32_t set(std::string_view key, std::string_view value);
    int32_t get(std::string_view key, std::string& value) const;
    int32_t erase(std::string_view key);
    bool has(std::string_view key) const;

    const std::filesystem::path& vault_path() const noexcept {
        return vault_path_;
    }

    static bool valid_key(std::string_view key) noexcept;

private:
    std::filesystem::path vault_path_;
};

}  // namespace sao::ai_editor::native
