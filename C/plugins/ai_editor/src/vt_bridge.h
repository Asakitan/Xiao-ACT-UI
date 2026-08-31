#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include <nlohmann/json.hpp>

namespace sao::ai_editor::vt {

class Bridge final {
public:
    Bridge();
    ~Bridge();

    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;

    int32_t execute(std::string_view tool_name,
                    const nlohmann::json& arguments,
                    nlohmann::json& result);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sao::ai_editor::vt
