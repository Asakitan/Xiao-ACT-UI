#include "kernel_map_panel_provider.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// The peer agent owns kernel_map_bridge.h under
// include/sao/ai_editor/.  If that header hasn't landed yet the panel
// still builds (guarded by __has_include) and reports "bridge
// unavailable" via KernelMapDefaultBridge::is_available().
//
// Test builds define SAO_AI_EDITOR_KERNEL_MAP_PANEL_TEST_STUB=1 so the
// panel test target does not need to link against the peer's rt_io
// proxy (which the real header pulls in transitively).  In stub mode
// the default bridge always reports unavailable; tests inject a mock
// via install_bridge().
#if defined(SAO_AI_EDITOR_KERNEL_MAP_PANEL_TEST_STUB) && \
    SAO_AI_EDITOR_KERNEL_MAP_PANEL_TEST_STUB
#define SAO_AI_EDITOR_KERNEL_MAP_BRIDGE_AVAILABLE 0
#elif __has_include("sao/ai_editor/kernel_map_bridge.h")
#include "sao/ai_editor/kernel_map_bridge.h"
#define SAO_AI_EDITOR_KERNEL_MAP_BRIDGE_AVAILABLE 1
#else
#define SAO_AI_EDITOR_KERNEL_MAP_BRIDGE_AVAILABLE 0
#endif

namespace sao::ai_editor::native {

namespace {

// Parse "0xDEADBEEF" or "deadbeef" or plain integer strings into a
// 64-bit base address.  Returns false on empty / malformed input; the
// caller is expected to translate a false into an "invalid target_base"
// error reply.
bool parse_hex_address(const std::string& value, uint64_t& out) {
    std::string s = value;
    // Trim surrounding whitespace + a single 0x/0X prefix.
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    if (s.empty()) { return false; }
    int base = 16;
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s.erase(0, 2);
    }
    if (s.empty()) { return false; }
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    try {
        out = std::stoull(s, nullptr, base);
    } catch (...) {
        return false;
    }
    return true;
}

std::string format_hex_address(uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%016llX",
                  static_cast<unsigned long long>(value));
    return std::string(buffer);
}

// Best-effort asset loader.  Missing / unreadable files return an empty
// string, which triggers the built-in stub HTML.
std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) { return {}; }
    std::ostringstream out;
    out << stream.rdbuf();
    return out.str();
}

// A minimal HTML shell used when the real assets aren't reachable
// (unit tests, headless configurations, first-boot before assets have
// been installed).  Keeps the same panelId / cmd contract so a smoke
// test can still exercise the message dispatch even without the CSS/JS
// bundle.
std::string builtin_stub_html() {
    return R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>Kernel Map Bridge</title></head>
<body>
  <h1>Kernel Map Bridge</h1>
  <p data-role="bridge-availability">bridge: probing</p>
  <p>Operator dashboard assets missing — running in stub mode.</p>
</body></html>)HTML";
}

}  // namespace

// -----------------------------------------------------------------------------
// KernelMapDefaultBridge — production forwarder to the peer's bridge.
// -----------------------------------------------------------------------------

KernelMapDefaultBridge::KernelMapDefaultBridge() {
#if SAO_AI_EDITOR_KERNEL_MAP_BRIDGE_AVAILABLE
    available_ = true;
#else
    available_ = false;
#endif
}

#if SAO_AI_EDITOR_KERNEL_MAP_BRIDGE_AVAILABLE

Json KernelMapDefaultBridge::status() {
    sao::ai_editor::kernel_map::BridgeStatus reply{};
    const int32_t rc =
        sao::ai_editor::kernel_map::shared_bridge().status(reply);
    Json payload = Json::object();
    payload["active"] = reply.active;
    payload["mapped_count"] = static_cast<int64_t>(reply.map_count);
    payload["rc"] = rc;
    return payload;
}

Json KernelMapDefaultBridge::enumerate() {
    std::vector<uint64_t> bases;
    const int32_t rc =
        sao::ai_editor::kernel_map::shared_bridge().enumerate(bases);
    Json payload = Json::object();
    Json array = Json::array();
    for (uint64_t base : bases) {
        array.push_back(format_hex_address(base));
    }
    payload["bases"] = std::move(array);
    payload["rc"] = rc;
    return payload;
}

Json KernelMapDefaultBridge::activate(const ActivateConfig& config) {
    const int32_t rc = sao::ai_editor::kernel_map::shared_bridge().activate(
        config.invoke_result_slot_va, config.idle_timeout_ms,
        config.pool_tag_seed);
    return Json{{"ok", rc == SAO_AI_EDITOR_OK}, {"rc", rc}};
}

Json KernelMapDefaultBridge::deactivate() {
    const int32_t rc = sao::ai_editor::kernel_map::shared_bridge().deactivate();
    return Json{{"ok", rc == SAO_AI_EDITOR_OK}, {"rc", rc}};
}

Json KernelMapDefaultBridge::map(const std::vector<uint8_t>& driver_bytes) {
    if (driver_bytes.empty()) {
        return Json{{"ok", false},
                    {"target_base", "0x0000000000000000"},
                    {"rc", SAO_AI_EDITOR_ERR_INVALID_ARGUMENT}};
    }
    sao::ai_editor::kernel_map::MapResult reply{};
    const int32_t rc = sao::ai_editor::kernel_map::shared_bridge().map(
        driver_bytes.data(),
        static_cast<uint32_t>(driver_bytes.size()),
        reply);
    Json payload;
    payload["ok"] = rc == SAO_AI_EDITOR_OK;
    payload["rc"] = rc;
    payload["target_base"] = format_hex_address(reply.target_base);
    payload["entry_status"] = reply.entry_status;
    return payload;
}

Json KernelMapDefaultBridge::unmap(uint64_t target_base) {
    const int32_t rc =
        sao::ai_editor::kernel_map::shared_bridge().unmap(target_base);
    return Json{{"ok", rc == SAO_AI_EDITOR_OK},
                {"target_base", format_hex_address(target_base)},
                {"rc", rc}};
}

#else  // !SAO_AI_EDITOR_KERNEL_MAP_BRIDGE_AVAILABLE

Json KernelMapDefaultBridge::status() {
    return Json{{"active", false}, {"mapped_count", 0}};
}

Json KernelMapDefaultBridge::enumerate() {
    return Json{{"bases", Json::array()}};
}

Json KernelMapDefaultBridge::activate(const ActivateConfig&) {
    return Json{{"ok", false}};
}

Json KernelMapDefaultBridge::deactivate() {
    return Json{{"ok", false}};
}

Json KernelMapDefaultBridge::map(const std::vector<uint8_t>&) {
    return Json{{"ok", false}, {"target_base", "0x0000000000000000"}};
}

Json KernelMapDefaultBridge::unmap(uint64_t target_base) {
    (void)target_base;
    return Json{{"ok", false}};
}

#endif

// -----------------------------------------------------------------------------
// KernelMapPanelProvider.
// -----------------------------------------------------------------------------

KernelMapPanelProvider::KernelMapPanelProvider()
    : bridge_(std::make_unique<KernelMapDefaultBridge>()) {}

KernelMapPanelProvider::~KernelMapPanelProvider() = default;

void KernelMapPanelProvider::install_bridge(
    std::unique_ptr<IKernelMapBridge> bridge) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (bridge != nullptr) {
        bridge_ = std::move(bridge);
    }
}

void KernelMapPanelProvider::install_post_to_page(PostToPage sender) {
    std::lock_guard<std::mutex> guard(mutex_);
    post_to_page_ = std::move(sender);
}

void KernelMapPanelProvider::install_file_picker(FilePicker picker) {
    std::lock_guard<std::mutex> guard(mutex_);
    file_picker_ = std::move(picker);
}

bool KernelMapPanelProvider::is_registered() const noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    return registered_;
}

std::string KernelMapPanelProvider::load_bundled_html(
    const std::string& assets_root) const {
    if (assets_root.empty()) {
        return builtin_stub_html();
    }
    const std::filesystem::path root(assets_root);
    const std::filesystem::path html_path = root / "index.html";
    const std::filesystem::path css_path = root / "panel.css";
    const std::filesystem::path js_path = root / "panel.js";
    std::string html = read_file(html_path);
    if (html.empty()) { return builtin_stub_html(); }
    std::string css = read_file(css_path);
    std::string js = read_file(js_path);
    // Inline the sibling assets so a single setWebviewHtml call is
    // enough — the panel record doesn't need to expose localResource
    // roots this way.  Fall back to the raw HTML (which already
    // <link>s / <script>s the assets by relative path) if inlining
    // isn't possible.
    if (!css.empty()) {
        const std::string link = R"(<link rel="stylesheet" href="panel.css">)";
        const std::string replacement =
            "<style>\n" + css + "\n</style>";
        const auto pos = html.find(link);
        if (pos != std::string::npos) {
            html.replace(pos, link.size(), replacement);
        }
    }
    if (!js.empty()) {
        const std::string script_tag = R"(<script src="panel.js"></script>)";
        const std::string replacement =
            "<script>\n" + js + "\n</script>";
        const auto pos = html.find(script_tag);
        if (pos != std::string::npos) {
            html.replace(pos, script_tag.size(), replacement);
        }
    }
    return html;
}

int32_t KernelMapPanelProvider::register_with_runtime(
    WebviewPanelRegistry& registry, const std::string& assets_root) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (registered_) {
        return SAO_AI_EDITOR_OK;
    }
    WebviewPanelOptions options;
    options.enable_scripts = true;
    options.retain_context_when_hidden = true;
    options.view_column = 1;
    options.extras = Json{{"builtin", true},
                          {"kind", "operator-dashboard"}};

    WebviewPanelState state;
    int32_t status = registry.create(
        std::string{kKernelMapPanelId}, std::string{kKernelMapViewType},
        std::string{kKernelMapPanelTitle}, options, WebviewPanelOwner::native_runtime, state);
    if (status == SAO_AI_EDITOR_ERR_INVALID_ARGUMENT) {
        // Registry rejects duplicate ids — treat as "already registered"
        // by another instance and simply reuse the entry.
        auto existing = registry.snapshot(std::string{kKernelMapPanelId});
        if (!existing.has_value()) {
            return status;
        }
        state = existing.value();
    } else if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    const std::string html = load_bundled_html(assets_root);
    status = registry.set_html(std::string{kKernelMapPanelId}, html, state);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    registered_ = true;
    return SAO_AI_EDITOR_OK;
}

int32_t KernelMapPanelProvider::unregister_from_runtime(
    WebviewPanelRegistry& registry) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!registered_) {
        return SAO_AI_EDITOR_OK;
    }
    WebviewPanelState state;
    const int32_t status =
        registry.dispose(std::string{kKernelMapPanelId}, state);
    if (status != SAO_AI_EDITOR_OK &&
        status != SAO_AI_EDITOR_ERR_NOT_FOUND) {
        return status;
    }
    registered_ = false;
    return SAO_AI_EDITOR_OK;
}

int32_t KernelMapPanelProvider::handle_message(const Json& message,
                                               Json& out_reply) {
    if (!message.is_object()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    // Accept `cmd` or `command` — the JS UI uses cmd, but stray VSCode-
    // shaped commands sometimes come through with `command`.
    std::string cmd = message.value("cmd", std::string{});
    if (cmd.empty()) {
        cmd = message.value("command", std::string{});
    }
    if (cmd.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const Json request_id = message.value("requestId", Json());
    const Json args = message.value("args", Json::object());

    std::lock_guard<std::mutex> guard(mutex_);
    if (cmd == "status") {
        out_reply = handle_status_locked();
    } else if (cmd == "enumerate") {
        out_reply = handle_enumerate_locked();
    } else if (cmd == "activate") {
        out_reply = handle_activate_locked(args);
    } else if (cmd == "deactivate") {
        out_reply = handle_deactivate_locked();
    } else if (cmd == "unmap") {
        out_reply = handle_unmap_locked(args);
    } else if (cmd == "refresh") {
        out_reply = handle_refresh_locked();
    } else if (cmd == "load_driver") {
        out_reply = handle_load_driver_locked();
    } else {
        out_reply = build_error(cmd, "unknown command", request_id);
        // Inject the request id + cmd into the error envelope so the
        // page can correlate correctly.
        out_reply["cmd"] = cmd;
        if (!request_id.is_null()) {
            out_reply["requestId"] = request_id;
        }
        return SAO_AI_EDITOR_OK;
    }
    out_reply["cmd"] = cmd;
    if (!request_id.is_null()) {
        out_reply["requestId"] = request_id;
    }
    return SAO_AI_EDITOR_OK;
}

Json KernelMapPanelProvider::build_reply(std::string_view /*cmd*/,
                                         std::string_view status,
                                         const Json& payload,
                                         const Json& /*request_id*/) const {
    Json reply = Json::object();
    reply["status"] = std::string(status);
    if (!payload.is_null()) {
        reply["payload"] = payload;
    }
    return reply;
}

Json KernelMapPanelProvider::build_error(std::string_view /*cmd*/,
                                         std::string_view reason,
                                         const Json& /*request_id*/) const {
    Json reply = Json::object();
    reply["status"] = "error";
    reply["reason"] = std::string(reason);
    return reply;
}

Json KernelMapPanelProvider::handle_status_locked() {
    if (bridge_ == nullptr || !bridge_->is_available()) {
        Json reply = build_error("status", "bridge unavailable", Json());
        reply["payload"] = Json{{"active", false}, {"mapped_count", 0}};
        return reply;
    }
    return build_reply("status", "ok", bridge_->status(), Json());
}

Json KernelMapPanelProvider::handle_enumerate_locked() {
    if (bridge_ == nullptr || !bridge_->is_available()) {
        Json reply = build_error("enumerate", "bridge unavailable", Json());
        reply["payload"] = Json{{"bases", Json::array()}};
        return reply;
    }
    return build_reply("enumerate", "ok", bridge_->enumerate(), Json());
}

Json KernelMapPanelProvider::handle_activate_locked(const Json& args) {
    if (bridge_ == nullptr || !bridge_->is_available()) {
        return build_error("activate", "bridge unavailable", Json());
    }
    IKernelMapBridge::ActivateConfig config{};
    if (args.is_object()) {
        // slot_va is passed either as a hex string ("0x...") or a raw
        // integer.  Hex strings arrive from the JS panel because JSON
        // numbers cannot faithfully represent 64-bit kernel VAs.
        if (args.contains("invoke_result_slot_va")) {
            const Json& slot = args["invoke_result_slot_va"];
            if (slot.is_number_unsigned()) {
                config.invoke_result_slot_va = slot.get<uint64_t>();
            } else if (slot.is_string()) {
                (void)parse_hex_address(slot.get<std::string>(),
                                        config.invoke_result_slot_va);
            }
        }
        config.idle_timeout_ms = args.value("idle_timeout_ms", 60000U);
        if (args.contains("pool_tag_seed")) {
            const Json& tag = args["pool_tag_seed"];
            if (tag.is_number_unsigned()) {
                config.pool_tag_seed = tag.get<uint64_t>();
            } else if (tag.is_string()) {
                (void)parse_hex_address(tag.get<std::string>(),
                                        config.pool_tag_seed);
            }
        }
    }
    Json payload = bridge_->activate(config);
    const bool ok = payload.value("ok", false);
    return build_reply("activate", ok ? "ok" : "error", payload, Json());
}

Json KernelMapPanelProvider::handle_deactivate_locked() {
    if (bridge_ == nullptr || !bridge_->is_available()) {
        return build_error("deactivate", "bridge unavailable", Json());
    }
    Json payload = bridge_->deactivate();
    const bool ok = payload.value("ok", false);
    return build_reply("deactivate", ok ? "ok" : "error", payload, Json());
}

Json KernelMapPanelProvider::handle_unmap_locked(const Json& args) {
    if (bridge_ == nullptr || !bridge_->is_available()) {
        return build_error("unmap", "bridge unavailable", Json());
    }
    const std::string target =
        args.is_object() ? args.value("target_base", std::string{}) : std::string{};
    if (target.empty()) {
        return build_error("unmap", "target_base is required", Json());
    }
    uint64_t base = 0;
    if (!parse_hex_address(target, base)) {
        Json reply = build_error("unmap", "invalid target_base", Json());
        reply["payload"] = Json{{"target_base", target}};
        return reply;
    }
    Json payload = bridge_->unmap(base);
    const bool ok = payload.value("ok", false);
    return build_reply("unmap", ok ? "ok" : "error", payload, Json());
}

Json KernelMapPanelProvider::handle_load_driver_locked() {
    if (bridge_ == nullptr || !bridge_->is_available()) {
        return build_error("load_driver", "bridge unavailable", Json());
    }
    if (!file_picker_) {
        Json reply;
        reply["status"] = "not_implemented";
        reply["reason"] = "no file picker in current build";
        return reply;
    }
    const std::string image_path = file_picker_();
    if (image_path.empty()) {
        return build_error("load_driver", "cancelled", Json());
    }
    // Peer's Bridge::map() takes raw PE bytes, not a path.  Read the
    // file locally under a strict 32 MiB cap that mirrors the wire
    // layer's own limit.  Files that exceed the cap are refused before
    // we even touch the bridge.
    constexpr std::uintmax_t kMaxDriverBytes = 32ull * 1024ull * 1024ull;
    std::error_code file_ec;
    const auto file_size =
        std::filesystem::file_size(image_path, file_ec);
    if (file_ec) {
        Json reply = build_error("load_driver", "unable to stat driver image",
                                 Json());
        reply["payload"] = Json{{"path", image_path},
                                {"error", file_ec.message()}};
        return reply;
    }
    if (file_size == 0) {
        return build_error("load_driver", "driver image is empty", Json());
    }
    if (file_size > kMaxDriverBytes) {
        Json reply = build_error("load_driver", "driver image exceeds 32 MiB cap",
                                 Json());
        reply["payload"] = Json{{"path", image_path},
                                {"size", static_cast<int64_t>(file_size)}};
        return reply;
    }
    std::ifstream stream(image_path, std::ios::binary);
    if (!stream) {
        Json reply = build_error("load_driver", "unable to open driver image",
                                 Json());
        reply["payload"] = Json{{"path", image_path}};
        return reply;
    }
    std::vector<uint8_t> driver_bytes(static_cast<size_t>(file_size));
    stream.read(reinterpret_cast<char*>(driver_bytes.data()),
                static_cast<std::streamsize>(driver_bytes.size()));
    if (!stream) {
        Json reply = build_error("load_driver", "short read on driver image",
                                 Json());
        reply["payload"] = Json{{"path", image_path}};
        return reply;
    }
    Json payload = bridge_->map(driver_bytes);
    payload["path"] = image_path;
    const bool ok = payload.value("ok", false);
    return build_reply("load_driver", ok ? "ok" : "error", payload, Json());
}

Json KernelMapPanelProvider::handle_refresh_locked() {
    // Refresh is a no-op on the native side — the page auto-polls
    // status/enumerate.  We echo an ok reply so the log gets a
    // completion marker.
    return build_reply("refresh", "ok", Json::object(), Json());
}

}  // namespace sao::ai_editor::native
