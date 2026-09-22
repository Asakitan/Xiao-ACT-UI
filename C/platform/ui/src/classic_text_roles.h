#pragma once

// Private DirectWrite text-role contract.  The scope is intentionally small:
// callers that already know a panel style can bracket its paint/measure pass
// with ScopedTextRole, while the renderer keeps Auto as the default.

#include <cstdint>
#include <string_view>

#if defined(_WIN32)
struct IDWriteTextLayout;
#endif

namespace sao::ui::detail {

enum class ClassicTextRole : uint8_t {
    Auto = 0,
    Display,
    Body,
    Monospace,
};

enum class ClassicTextWeight : uint8_t {
    Normal = 0,
    SemiBold,
    Bold,
};

struct ClassicTextStyle final {
    ClassicTextRole role{ClassicTextRole::Auto};
    ClassicTextWeight weight{ClassicTextWeight::Normal};
};

inline ClassicTextStyle& active_classic_text_style() noexcept {
    static thread_local ClassicTextStyle style{};
    return style;
}

class ScopedTextRole final {
  public:
    explicit ScopedTextRole(ClassicTextRole role,
                            ClassicTextWeight weight = ClassicTextWeight::Normal) noexcept
        : previous_(active_classic_text_style()) {
        active_classic_text_style() = {role, weight};
    }

    ~ScopedTextRole() {
        active_classic_text_style() = previous_;
    }

    ScopedTextRole(const ScopedTextRole&) = delete;
    ScopedTextRole& operator=(const ScopedTextRole&) = delete;

  private:
    ClassicTextStyle previous_{};
};

// Every role uses the same private Latin/CJK collection, including technical values.
constexpr const char* kClassicDisplayFontFamily = "SAO UI";
constexpr const char* kClassicBodyFontFamily = "SAO UI";
constexpr const char* kClassicMonospaceFontFamily = "SAO UI";

#if defined(_WIN32)
bool apply_product_text_typography(IDWriteTextLayout* layout, std::wstring_view text,
                                  float size_px) noexcept;
#endif

} // namespace sao::ui::detail
