#include <sao_core/sao_core.h>

#include <sao/core/abi.h>
#include <sao/core/logging.h>

#include <Windows.h>

#include <cstdint>

namespace {

using legacy_abi_fn = uint32_t(SAO_LEGACY_CORE_CALL*)();

} // namespace

int main() {
    if (sao_core_abi_version() != SAO_CORE_ABI_VERSION)
        return 1;
    if (sao_legacy_core_abi_version() != SAO_LEGACY_CORE_ABI_VERSION)
        return 2;

    const HMODULE legacy_module = GetModuleHandleW(L"sao_core.dll");
    if (legacy_module == nullptr)
        return 3;
    const auto legacy_abi =
        reinterpret_cast<legacy_abi_fn>(GetProcAddress(legacy_module, "sao_core_abi_version"));
    if (legacy_abi == nullptr || legacy_abi() != SAO_LEGACY_CORE_ABI_VERSION)
        return 4;
    return 0;
}
