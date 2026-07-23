#pragma once

#include "sao/core/status.h"

#include <cstdint>
#include <memory>
#include <string>

namespace sao::launcher::tool_launch {

inline constexpr bool ai_editor_capability_available() noexcept {
#if defined(SAO_LAUNCHER_HAS_AI_EDITOR_ABI)
    return true;
#else
    return false;
#endif
}

enum class AiEditorLaunchPhase : std::uint8_t {
    idle,
    launching,
    started,
    failed,
};

struct AiEditorLaunchSnapshot {
    AiEditorLaunchPhase phase{AiEditorLaunchPhase::idle};
    sao_status_t last_status{SAO_STATUS_OK};
    std::uint32_t process_id{};
    std::uint32_t exit_code{};
    bool has_exit_code{};
};

// Asynchronous owner for the native headless SaoAiEditor subprocess. The
// dedicated launcher ABI handshake is the sole readiness authority; visible
// UI is owned by SaoAuto's compositor service.
class AiEditorProcessOwner final {
  public:
    struct State;

    explicit AiEditorProcessOwner(std::wstring base_dir);
    ~AiEditorProcessOwner();

    AiEditorProcessOwner(const AiEditorProcessOwner&) = delete;
    AiEditorProcessOwner& operator=(const AiEditorProcessOwner&) = delete;

    // Success means the asynchronous open request was accepted. Completion
    // and launch failures are observable through snapshot(). A successful
    // take_offline() is reversible: the next owner-thread open() starts a new
    // child. A failed teardown keeps its gate closed until take_offline()
    // succeeds on a later retry.
    sao_status_t open() noexcept;
    // Owner-thread service for the compositor-backed AI Editor panel. Call
    // from the SaoAuto UI tick after the platform compositor is bound.
    sao_status_t service_ui() noexcept;
    // Owner-thread, retryable teardown. The panel is retired before the
    // subprocess launcher is stopped or destroyed. When a panel exists, the
    // compositor owner-thread check is completed before teardown state is
    // mutated.
    sao_status_t take_offline() noexcept;
    // Observation only: updates the published launch/exit snapshot without
    // retiring panel or launcher ownership.
    sao_status_t snapshot(AiEditorLaunchSnapshot& out) const noexcept;

    // Destruction performs the same panel-first cleanup when called on the
    // compositor owner thread. Embedders destroying from another thread must
    // first complete take_offline() on the owner thread; the fallback path
    // stops the child but does not cross-thread destroy a live panel or its
    // borrowed launcher.

  private:
    std::shared_ptr<State> state_;
};

sao_status_t open_ai_editor(AiEditorProcessOwner* owner) noexcept;

} // namespace sao::launcher::tool_launch
