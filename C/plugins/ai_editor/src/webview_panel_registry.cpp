#include "webview_panel_registry.h"

#include <chrono>
#include <windows.h>

namespace sao::ai_editor::native {

namespace {

constexpr uint64_t kMaximumSafeJsonInteger = 9007199254740991ULL;

} // namespace

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
    } else {
        const auto existing = panels_.find(panel_id);
        if (existing != panels_.end()) {
            // Adopt-on-view-type-match: a live record additionally requires
            // an owner match so a different lifecycle domain cannot hijack
            // an active panel id.  Disposed records adopt regardless of the
            // requesting owner — the stored owner wins, so a disposed
            // native_runtime builtin is revived while keeping its native
            // attribution (and its HTML, which dispose() preserves).
            if (existing->second.view_type != view_type) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            if (!existing->second.disposed && existing->second.owner != owner) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            existing->second.title = title;
            existing->second.options = options;
            for (auto& [id, panel] : panels_) {
                (void)id;
                panel.options.extras["active"] = false;
            }
            existing->second.options.extras["active"] = true;
            existing->second.visible = true;
            const bool was_disposed = existing->second.disposed;
            existing->second.disposed = false;
            existing->second.last_reveal_ms = now_ms();
            // A disposed extension-owned record restarts blank — its host
            // re-pushes HTML on create.  Live records (same-owner adopt)
            // and native-owned records keep their HTML so adoption never
            // blanks a rendered builtin dashboard.
            if (was_disposed && existing->second.owner != WebviewPanelOwner::native_runtime) {
                existing->second.html.clear();
            }
            existing->second.initial_state = Json::object();
            existing->second.last_post_ms = 0;
            out_state = existing->second;
            return SAO_AI_EDITOR_OK;
        }
    }
    WebviewPanelState state;
    state.panel_id = panel_id;
    state.view_type = view_type;
    state.title = title;
    state.options = options;
    state.options.extras["active"] = true;
    state.owner = owner;
    state.visible = true;
    state.disposed = false;
    state.created_ms = now_ms();
    state.last_reveal_ms = state.created_ms;
    state.last_post_ms = 0;
    state.message_seq = 0;
    state.initial_state = Json::object();
    for (auto& [id, panel] : panels_) {
        (void)id;
        panel.options.extras["active"] = false;
    }
    panels_[panel_id] = state;
    ++total_created_;
    out_state = state;
    return SAO_AI_EDITOR_OK;
}

int32_t WebviewPanelRegistry::reveal(const std::string& panel_id, int view_column,
                                     bool preserve_focus, WebviewPanelState& out_state) {
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
    if (!preserve_focus) {
        for (auto& [id, panel] : panels_) {
            (void)id;
            panel.options.extras["active"] = false;
        }
        it->second.options.extras["active"] = true;
    }
    out_state = it->second;
    return SAO_AI_EDITOR_OK;
}

int32_t WebviewPanelRegistry::set_view_state(const std::string& panel_id, bool active, bool visible,
                                             int view_column, WebviewPanelState& out_state) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = panels_.find(panel_id);
    if (it == panels_.end())
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    if (it->second.disposed)
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    if (active && !visible)
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    if (active) {
        for (auto& [id, panel] : panels_) {
            (void)id;
            panel.options.extras["active"] = false;
        }
    }
    it->second.visible = visible;
    it->second.options.view_column = view_column;
    it->second.options.extras["active"] = active;
    if (visible)
        it->second.last_reveal_ms = now_ms();
    out_state = it->second;
    return SAO_AI_EDITOR_OK;
}

int32_t WebviewPanelRegistry::dispose(const std::string& panel_id, WebviewPanelState& out_state) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = panels_.find(panel_id);
    if (it == panels_.end()) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    const bool already = it->second.disposed;
    it->second.disposed = true;
    it->second.visible = false;
    it->second.options.extras["active"] = false;
    // Native-runtime-owned panels (the kernel-map / mcp-management
    // builtins) push their HTML exactly once at register time and never
    // again, so clearing it here would leave a revived panel blank.  The
    // extension-host domain always re-pushes on create, so clearing that
    // HTML matches the documented dispose contract.
    if (it->second.owner != WebviewPanelOwner::native_runtime) {
        it->second.html.clear();
    }
    it->second.initial_state = Json::object();
    it->second.last_post_ms = 0;
    out_state = it->second;
    out_state.options.extras["already"] = already;
    return SAO_AI_EDITOR_OK;
}

int32_t WebviewPanelRegistry::set_html(const std::string& panel_id, const std::string& html,
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
    if (it->second.message_seq >= kMaximumSafeJsonInteger) {
        return SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL;
    }
    ++it->second.message_seq;
    it->second.last_post_ms = now_ms();
    out_state = it->second;
    return SAO_AI_EDITOR_OK;
}

int32_t WebviewPanelRegistry::set_state(const std::string& panel_id, const Json& state,
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

std::optional<WebviewPanelState> WebviewPanelRegistry::snapshot(const std::string& panel_id) const {
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

std::vector<WebviewPanelDescriptor> WebviewPanelRegistry::native_panel_descriptors() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<WebviewPanelDescriptor> out;
    out.reserve(panels_.size());
    for (const auto& entry : panels_) {
        const WebviewPanelState& state = entry.second;
        if (state.owner == WebviewPanelOwner::native_runtime && !state.disposed) {
            out.push_back(WebviewPanelDescriptor{state.panel_id, state.title,
                                                 state.view_type, state.owner,
                                                 state.visible});
        }
    }
    return out;
}

std::optional<WebviewPanelState> WebviewPanelRegistry::active_panel() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::optional<WebviewPanelState> active;
    for (const auto& [panel_id, state] : panels_) {
        (void)panel_id;
        if (state.disposed || !state.visible)
            continue;
        const auto active_value = state.options.extras.find("active");
        if (active_value != state.options.extras.end() && active_value->is_boolean() &&
            !active_value->get<bool>())
            continue;
        if (!active.has_value() || state.last_reveal_ms > active->last_reveal_ms ||
            (state.last_reveal_ms == active->last_reveal_ms &&
             state.created_ms > active->created_ms) ||
            (state.last_reveal_ms == active->last_reveal_ms &&
             state.created_ms == active->created_ms && state.panel_id > active->panel_id)) {
            active = state;
        }
    }
    return active;
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

} // namespace sao::ai_editor::native
