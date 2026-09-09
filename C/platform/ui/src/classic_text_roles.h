#pragma once

// Private DirectWrite text-role contract.  The scope is intentionally small:
// callers that already know a panel style can bracket its paint/measure pass
// with ScopedTextRole, while the renderer keeps Auto as the default.

#include <cstdint>

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

// Display uses the embedded SAO UI resource through a private DirectWrite collection.
// See assets/fonts/SOURCE.md for provenance and distribution review.
// Body/Monospace use system families; no machine-wide font registration occurs.
constexpr const char* kClassicDisplayFontFamily = "SAO UI";
constexpr const char* kClassicBodyFontFamily = "Microsoft YaHei UI";
constexpr const char* kClassicMonospaceFontFamily = "Consolas";

} // namespace sao::ui::detail
