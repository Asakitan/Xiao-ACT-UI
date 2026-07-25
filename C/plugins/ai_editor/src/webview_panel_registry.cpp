#include "webview_panel_registry.h"

#include <chrono>
#include <windows.h>

namespace sao::ai_editor::native {

int64_t WebviewPanelRegistry::now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string WebviewPanelRegistry::mint_id_locked() {
    // Registry mutex is held by the caller.
    const uint64_t seq = next_seq_++;
    const DWORD pid = GetCurrentProcessId();
    return "wvp-" + std::to_string(pid) + "-" + std::to_string(seq);
}

int32_t WebviewPanelRegistry::create(const std::string& panel_id_hint, const std::string& view_type,
                                     const std::string& title, const WebviewPanelOptions& options,
                                     WebviewPanelState& out_state) {
    return create(panel_id_hint, view_type, title, options, WebviewPanelOwner::extension_host,
                  out_state);
}

int32_t WebviewPanelRegistry::create(const std::string& panel_id_hint, const std::string& view_type,
                                     const std::string& title, const WebviewPanelOptions& options,
                                     WebviewPanelOwner owner, WebviewPanelState& out_state) {
    if (view_type.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    std::string panel_id = panel_id_hint;
    if (panel_id.empty()) {
        panel_id = mint_id_locked();
    } else if (panels_.find(panel_id) != panels_.end()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    WebviewPanelState state;
    state.panel_id = panel_id;
    state.view_type = view_type;
    state.title = title;
    state.options = options;
    state.owner = owner;
    state.visible = true;
    state.disposed = false;
    state.created_ms = now_ms();
    state.last_reveal_ms = state.created_ms;
    state.last_post_ms = 0;
    state.message_seq = 0;
    state.initial_state = Json::object();
    panels_[panel_id] = state;
    ++total_created_;
    out_state = state;
    return SAO_AI_EDITOR_OK;
}

int32_t WebviewPanelRegistry::reveal(const std::string& panel_id,
                                     int view_column,
                                     bool preserve_focus,
                                     WebviewPanelState& out_state) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = panels_.find(panel_id);
    if (it == panels_.end()) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    if (it->second.disposed) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
    it->second.visible = true;
    it->second.last_reveal_ms = now_ms();
    it->second.options.view_column = view_column;
    // preserve_focus is a hint stored on the extras bag for future WebView2
    // ShowWindow(SW_SHOWNOACTIVATE) plumbing; the current bridge focuses on
    // reveal unconditionally.
    it->second.options.extras["preserveFocus"] = preserve_focus;
    out_state = it->second;
    return SAO_AI_EDITOR_OK;
}

int32_t WebviewPanelRegistry::dispose(const std::string& panel_id,
                                      WebviewPanelState& out_state) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = panels_.find(panel_id);
    if (it == panels_.end()) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    const bool already = it->second.disposed;
    it->second.disposed = true;
    it->second.visible = false;
    out_state = it->second;
    out_state.options.extras["already"] = already;
    return SAO_AI_EDITOR_OK;
}

int32_t WebviewPanelRegistry::set_html(const std::string& panel_id,
                                       const std::string& html,
                                       WebviewPanelState& out_state) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = panels_.find(panel_id);
    if (it == panels_.end()) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    if (it->second.disposed) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
    it->second.html = html;
    it->second.visible = true;
    it->second.last_reveal_ms = now_ms();
    out_state = it->second;
    return SAO_AI_EDITOR_OK;
}

int32_t WebviewPanelRegistry::note_post_message(const std::string& panel_id,
                                                WebviewPanelState& out_state) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = panels_.find(panel_id);
    if (it == panels_.end()) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    if (it->second.disposed) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
    ++it->second.message_seq;
    it->second.last_post_ms = now_ms();
    out_state = it->second;
    return SAO_AI_EDITOR_OK;
}

int32_t WebviewPanelRegistry::set_state(const std::string& panel_id,
                                        const Json& state,
                                        WebviewPanelState& out_state) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = panels_.find(panel_id);
    if (it == panels_.end()) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    if (it->second.disposed) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
    it->second.initial_state = state;
    out_state = it->second;
    return SAO_AI_EDITOR_OK;
}

std::optional<WebviewPanelState> WebviewPanelRegistry::snapshot(
    const std::string& panel_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = panels_.find(panel_id);
    if (it == panels_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<WebviewPanelState> WebviewPanelRegistry::list_alive() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<WebviewPanelState> out;
    out.reserve(panels_.size());
    for (const auto& entry : panels_) {
        if (!entry.second.disposed) {
            out.push_back(entry.second);
        }
    }
    return out;
}

std::vector<WebviewPanelState> WebviewPanelRegistry::list_alive(WebviewPanelOwner owner) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<WebviewPanelState> out;
    out.reserve(panels_.size());
    for (const auto& entry : panels_) {
        if (!entry.second.disposed && entry.second.owner == owner) {
            out.push_back(entry.second);
        }
    }
    return out;
}

size_t WebviewPanelRegistry::total_created() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_created_;
}

size_t WebviewPanelRegistry::total_created(WebviewPanelOwner owner) const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t total = 0;
    for (const auto& entry : panels_) {
        if (entry.second.owner == owner) {
            ++total;
        }
    }
    return total;
}

void WebviewPanelRegistry::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    panels_.clear();
    next_seq_ = 1;
    total_created_ = 0;
}

}  // namespace sao::ai_editor::native
