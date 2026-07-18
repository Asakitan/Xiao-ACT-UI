// script_sandbox_common.cpp — 支持库 impl
#include "sao/plugins/script_sandbox_common/script_sandbox_common.h"

#include <cstring>

namespace sao::plugins::script_sandbox_common {

// 语义化 magic: 'S','S','B','X' (Script Sandbox 共享层).
const uint32_t kScriptSandboxCommonTag = 0x53534258u;

bool denied_globals_contains(const char* const* names, size_t count,
                             const char* needle) noexcept {
    if (names == nullptr || count == 0 || needle == nullptr) return false;
    for (size_t i = 0; i < count; ++i) {
        const char* n = names[i];
        if (n && std::strcmp(n, needle) == 0) return true;
    }
    return false;
}

} // namespace sao::plugins::script_sandbox_common
