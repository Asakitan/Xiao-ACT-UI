// SAO Auto — platform/ui ABI export macros.

#pragma once

#include <cstdint>

#ifdef __cplusplus
#  include <memory>
#endif

#if defined(_WIN32)
#  if defined(SAO_UI_BUILDING_DLL)
#    define SAO_UI_API __declspec(dllexport)
#  elif defined(SAO_UI_USING_DLL)
#    define SAO_UI_API __declspec(dllimport)
#  else
#    define SAO_UI_API
#  endif
#else
#  define SAO_UI_API
#endif

#define SAO_UI_CALL __cdecl

#define SAO_UI_ABI_VERSION_MAJOR 1u
#define SAO_UI_ABI_VERSION_MINOR 5u
#define SAO_UI_ABI_VERSION \
    ((SAO_UI_ABI_VERSION_MAJOR << 16) | SAO_UI_ABI_VERSION_MINOR)

#ifdef __cplusplus
extern "C" {
#endif

SAO_UI_API uint32_t SAO_UI_CALL sao_ui_abi_version(void);

#ifdef __cplusplus
}  // extern "C"

namespace sao::ui::detail {

enum class WidgetHandleFamily : uint8_t {
    text,
    data,
    table,
};

struct WidgetHandleMetadata {
    WidgetHandleFamily family{};
    int32_t kind{-1};
    uint64_t generation{};
};

void* register_widget_handle(
    WidgetHandleFamily family, int32_t kind,
    std::shared_ptr<void> state) noexcept;

std::shared_ptr<void> acquire_widget_handle(
    void* handle, WidgetHandleFamily expected_family,
    int32_t expected_kind) noexcept;

std::shared_ptr<void> retire_widget_handle(
    void* handle, WidgetHandleFamily expected_family,
    WidgetHandleMetadata* out_metadata = nullptr) noexcept;

bool inspect_widget_handle(
    void* handle, WidgetHandleMetadata* out_metadata) noexcept;

}  // namespace sao::ui::detail
#endif
