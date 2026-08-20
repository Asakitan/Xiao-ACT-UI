// Kernel Map Bridge — operator dashboard panel provider.
//
// A built-in webview panel that surfaces the sao::ai_editor::kernel_map
// bridge (activate / deactivate / status / enumerate / map / unmap) to the
// AI Editor operator.  The provider is passive metadata + a message
// dispatcher; it never talks to WebView2 directly.  Rendering happens in
// assets/ai_editor/kernel_map_panel/{index.html,panel.js,panel.css} which
// the provider inlines into a single setWebviewHtml payload during
// register_with_runtime().
//
// The peer agent owns include/sao/ai_editor/kernel_map_bridge.h — if that
// header is not yet on disk when this translation unit is compiled, the
// implementation guards the include with __has_include and reports the
// panel status as bridge-unavailable so the operator UI stays functional
// (buttons disabled, log records the reason).  Once the peer's header
// lands the provider auto-detects it at recompile time and delegates.
//
// Concurrency: the provider only touches its own std::mutex and the
// borrowed WebviewPanelRegistry snapshot; it never blocks on the peer's
// bridge for longer than the individual call.  Callers are expected to
// dispatch on whatever thread the extension host inbound loop uses.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "native_utils.h"
#include "webview_panel_registry.h"

namespace sao::ai_editor::native {

// Fixed panel identity — the runtime hook checks the inbound panelId
// against this constant to short-circuit into the provider.  Also the
// view type stored in the WebviewPanelRegistry so the sidebar UI can
// pick out the operator dashboard entry.
inline constexpr const char kKernelMapPanelId[] = "kernel-map-builtin";
inline constexpr const char kKernelMapViewType[] = "sao.kernel_map";
inline constexpr const char kKernelMapPanelTitle[] = "Kernel Map Bridge";

// Abstract interface over the peer's kernel_map::Bridge.  The
// production implementation forwards to sao::ai_editor::kernel_map::*
// declared in kernel_map_bridge.h when that header is available; tests
// install a mock via install_bridge() to avoid needing the peer's file.
class IKernelMapBridge {
public:
    struct ActivateConfig final {
        // Kernel VA of the shared "invoke result" slot used by the wire
        // layer to report DriverEntry return codes.  Operator explicitly
        // provides this; a 0 value is passed through and the proxy will
        // reject it with NOT_INITIALIZED, which surfaces as an error
        // reply in the panel log.
        uint64_t invoke_result_slot_va = 0;
        // Watchdog / idle timeout on the wire adapter.
        uint32_t idle_timeout_ms = 60000;
        // Pool tag seed forwarded to the kernel allocator.
        uint64_t pool_tag_seed = 0;
    };

    virtual ~IKernelMapBridge() = default;

    // Returns true iff the concrete bridge implementation is wired up
    // (i.e. the peer's header + code were present at compile / link
    // time).  A "false" answer causes the panel to disable action
    // buttons and surface the reason to the operator log.
    virtual bool is_available() const noexcept = 0;

    // status → JSON payload {active, mapped_count}. Payload contents
    // when !is_available() are unspecified; callers should check
    // is_available() first.
    virtual Json status() = 0;

    // enumerate → JSON payload {bases: ["0x...", ...]}.
    virtual Json enumerate() = 0;

    // activate / deactivate / unmap / map → JSON payload with
    // implementation-specific detail; the panel provider only inspects
    // the `ok` flag and passes payload verbatim to the UI.
    virtual Json activate(const ActivateConfig& config) = 0;
    virtual Json deactivate() = 0;
    // map() takes the raw PE bytes; the panel reads the file locally
    // before calling into the bridge, matching the peer's `driver_bytes,
    // driver_len` wire signature.
    virtual Json map(const std::vector<uint8_t>& driver_bytes) = 0;
    virtual Json unmap(uint64_t target_base) = 0;
};

// The default in-process bridge — forwards to sao::ai_editor::kernel_map
// when the peer header is on disk, otherwise reports unavailable.  Owns
// no resources; safe to instantiate anywhere.
class KernelMapDefaultBridge final : public IKernelMapBridge {
public:
    KernelMapDefaultBridge();
    ~KernelMapDefaultBridge() override = default;

    bool is_available() const noexcept override { return available_; }
    Json status() override;
    Json enumerate() override;
    Json activate(const ActivateConfig& config) override;
    Json deactivate() override;
    Json map(const std::vector<uint8_t>& driver_bytes) override;
    Json unmap(uint64_t target_base) override;

private:
    bool available_ = false;
};

class SAO_AI_EDITOR_API KernelMapPanelProvider final : public NativePanelProvider {
public:
    // Delegate signature used to push messages back to the webview.
    // The runtime hook injects a lambda that runs the same
    // dispatch_webview_message_to_page path used by regular
    // vscode.window.postMessageToWebview calls.
    using PostToPage =
        std::function<bool(const std::string& panel_id, const Json& message)>;

    // File picker delegate — production wires this to whatever native
    // dialog the AI editor has once one lands.  A null delegate causes
    // the load_driver command to reply {"status": "not_implemented"}
    // rather than crash or open a phantom prompt.
    using FilePicker = std::function<std::string()>;

    KernelMapPanelProvider();
    ~KernelMapPanelProvider() override;

    KernelMapPanelProvider(const KernelMapPanelProvider&) = delete;
    KernelMapPanelProvider& operator=(const KernelMapPanelProvider&) = delete;

    // Idempotent: multiple register calls after the first one no-op.
    // Loads the HTML/JS/CSS bundle from assets_root and creates the
    // metadata entry inside the registry.  `assets_root` may be empty
    // in tests — the provider falls back to a minimal built-in HTML
    // stub that still surfaces the "bridge unavailable" state.
    int32_t register_with_runtime(WebviewPanelRegistry& registry,
                                  const std::string& assets_root) override;

    // Idempotent teardown counterpart.  Releases provider ownership while
    // keeping the live registry record reusable for same-provider adoption.
    int32_t unregister_from_runtime(WebviewPanelRegistry& registry) override;

    // Test / production seams — inject before register_with_runtime()
    // if you need a mock bridge or file picker.  Both take ownership of
    // the passed callable / instance for the provider's lifetime.
    void install_bridge(std::unique_ptr<IKernelMapBridge> bridge);
    void install_post_to_page(PostToPage sender);
    void install_file_picker(FilePicker picker);

    // Route an inbound { cmd, args, requestId? } message.  Returns
    // SAO_AI_EDITOR_OK when a reply was produced (even if the reply
    // itself is an error / not-implemented — the caller distinguishes
    // via `out_reply.status`).  Returns SAO_AI_EDITOR_ERR_INVALID_ARGUMENT
    // on malformed input.  Never throws.
    int32_t handle_message(const Json& message, Json& out_reply) override;

    std::string_view provider_panel_id() const noexcept override {
        return panel_id();
    }

    // Fixed identifiers — exposed as helpers so tests / runtime hooks
    // don't have to reach for the constants directly.
    static std::string_view panel_id() noexcept { return kKernelMapPanelId; }
    static std::string_view view_type() noexcept { return kKernelMapViewType; }

    // True after register_with_runtime() succeeded.  Idempotency guard.
    bool is_registered() const noexcept;

private:
    Json handle_status(const std::shared_ptr<IKernelMapBridge>& bridge);
    Json handle_enumerate(const std::shared_ptr<IKernelMapBridge>& bridge);
    Json handle_activate(const std::shared_ptr<IKernelMapBridge>& bridge,
                         const Json& args);
    Json handle_deactivate(const std::shared_ptr<IKernelMapBridge>& bridge);
    Json handle_unmap(const std::shared_ptr<IKernelMapBridge>& bridge,
                      const Json& args);
    Json handle_load_driver(const std::shared_ptr<IKernelMapBridge>& bridge,
                            const FilePicker& picker);
    Json handle_refresh();
    Json build_error(std::string_view cmd,
                     std::string_view reason,
                     const Json& request_id) const;
    Json build_reply(std::string_view cmd,
                     std::string_view status,
                     const Json& payload,
                     const Json& request_id) const;

    // Loads panel assets — returns the concatenated HTML with inline
    // <style> and <script> so a single setWebviewHtml call is enough.
    // Missing assets fall back to the built-in stub HTML.
    std::string load_bundled_html(const std::string& assets_root) const;

    mutable std::mutex mutex_;
    std::shared_ptr<IKernelMapBridge> bridge_;
    PostToPage post_to_page_;
    FilePicker file_picker_;
    bool registered_ = false;
};

}  // namespace sao::ai_editor::native
