#pragma once

#include "sao/core/status.h"

#include <cstdint>
#include <memory>
#include <string>

namespace sao::launcher::tool_launch {

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

// Asynchronous owner for the native SaoAiEditor subprocess. The dedicated
// launcher ABI handshake is the readiness authority; a window is optional.
class AiEditorProcessOwner final {
public:
    struct State;

    explicit AiEditorProcessOwner(std::wstring base_dir);
    ~AiEditorProcessOwner();

    AiEditorProcessOwner(const AiEditorProcessOwner&) = delete;
    AiEditorProcessOwner& operator=(const AiEditorProcessOwner&) = delete;

    // Success means the asynchronous open request was accepted. Completion
    // and launch failures are observable through snapshot().
    sao_status_t open() noexcept;
    sao_status_t snapshot(AiEditorLaunchSnapshot& out) const noexcept;

private:
    std::shared_ptr<State> state_;
};

sao_status_t open_ai_editor(AiEditorProcessOwner* owner) noexcept;

} // namespace sao::launcher::tool_launch
