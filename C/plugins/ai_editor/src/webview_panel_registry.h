#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "native_utils.h"
#include "sao/ai_editor/ai_editor_status.h"

// The registry stores metadata for every vscode.window.createWebviewPanel
// call routed through the dispatch layer.  It intentionally has *no*
// WebView2 dependency so the Node side of the extension host can still
// exercise create/postMessage/dispose semantics on hosts that ship without
// WebView2Loader.dll; post-message then reports an explicit bridge failure.
// The bridge glue in webview_bridge.cpp reads state from
// this registry under its own lock so the WebView2 controller thread can
// react to reveal/dispose deterministically.

namespace sao::ai_editor::native {

class WebviewPanelRegistry;

class NativePanelProvider {
public:
    virtual ~NativePanelProvider() = default;
    virtual int32_t register_with_runtime(WebviewPanelRegistry& registry,
                                          const std::string& assets_root) = 0;
    virtual int32_t unregister_from_runtime(WebviewPanelRegistry& registry) = 0;
    virtual int32_t handle_message(const Json& message, Json& out_reply) = 0;
    virtual std::string_view provider_panel_id() const noexcept = 0;
};

enum class WebviewPanelOwner : uint8_t {
    extension_host,
    native_runtime,
};

struct WebviewPanelOptions final {
    bool enable_scripts = true;
    bool retain_context_when_hidden = false;
    // ViewColumn hint (VSCode convention: Active=-1, Beside=-2, positive=explicit).
    // Stored verbatim; the single-window WebView2 host currently ignores splits.
    int view_column = 1;
    Json extras = Json::object();  // opaque forward-compat bag
};

struct WebviewPanelState final {
    std::string panel_id;
    std::string view_type;
    std::string title;
    std::string html;             // last setWebviewHtml payload
    WebviewPanelOptions options;
    WebviewPanelOwner owner = WebviewPanelOwner::extension_host;
    bool visible = false;         // reveal → true, hide/dispose → false
    bool disposed = false;
    int64_t created_ms = 0;
    int64_t last_reveal_ms = 0;
    int64_t last_post_ms = 0;
    uint64_t message_seq = 0;     // monotonic id assigned to native→web posts
    Json initial_state = Json::object();
};

class SAO_AI_EDITOR_API WebviewPanelRegistry final {
public:
    WebviewPanelRegistry() = default;

    WebviewPanelRegistry(const WebviewPanelRegistry&) = delete;
    WebviewPanelRegistry& operator=(const WebviewPanelRegistry&) = delete;

    // Create a fresh panel record and return its snapshot.  When
    // `panel_id` is empty the registry mints one (`wvp-<counter>-<pid>`).
    // Returns SAO_AI_EDITOR_ERR_INVALID_ARGUMENT if `view_type` is empty
    // or if a supplied `panel_id` already exists.
    int32_t create(const std::string& panel_id_hint,
                   const std::string& view_type,
                   const std::string& title,
                   const WebviewPanelOptions& options,
                   WebviewPanelState& out_state);

    int32_t create(const std::string& panel_id_hint, const std::string& view_type,
                   const std::string& title, const WebviewPanelOptions& options,
                   WebviewPanelOwner owner, WebviewPanelState& out_state);

    // Mark the panel visible and record the reveal timestamp.  Returns
    // NOT_FOUND when the id is missing, PROTOCOL when disposed.
    int32_t reveal(const std::string& panel_id,
                   int view_column,
                   bool preserve_focus,
                   WebviewPanelState& out_state);

    // Dispose the panel.  Idempotent: repeated calls succeed but flag
    // `already` in `out_state.extras.already` for the caller to log.
    int32_t dispose(const std::string& panel_id,
                    WebviewPanelState& out_state);

    // Update the last-known HTML payload and reset visibility to true
    // (VSCode contract: setting HTML implicitly reveals the panel).
    int32_t set_html(const std::string& panel_id,
                     const std::string& html,
                     WebviewPanelState& out_state);

    // Bookkeeping for postMessage: returns the message seq assigned to
    // this post (monotonic, 1-based) so the bridge can correlate acks.
    int32_t note_post_message(const std::string& panel_id,
                              WebviewPanelState& out_state);

    // Persist a small opaque state blob so acquireVsCodeApi().setState()
    // survives navigations without a real disk backing.
    int32_t set_state(const std::string& panel_id,
                      const Json& state,
                      WebviewPanelState& out_state);

    // Read-only snapshot for tests / debug endpoints.
    std::optional<WebviewPanelState> snapshot(const std::string& panel_id) const;

    // List every non-disposed panel — used by extensions.snapshot to
    // surface the current WebView2 fleet.
    std::vector<WebviewPanelState> list_alive() const;

    // List only non-disposed panels owned by one lifecycle domain.
    std::vector<WebviewPanelState> list_alive(WebviewPanelOwner owner) const;

    // Total create-count (including disposed) for diagnostics.
    size_t total_created() const;

    // Total create-count (including disposed) for one lifecycle domain.
    size_t total_created(WebviewPanelOwner owner) const;

    // Wipe everything.  Used by tests and by native runtime shutdown.
    void clear();

private:
    static int64_t now_ms() noexcept;
    std::string mint_id_locked();

    mutable std::mutex mutex_;
    std::unordered_map<std::string, WebviewPanelState> panels_;
    uint64_t next_seq_ = 1;
    size_t total_created_ = 0;
};

}  // namespace sao::ai_editor::native
