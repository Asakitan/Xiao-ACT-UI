#include "kernel_map_panel_provider.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "kernel_map_commands.h"

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
using Json = nlohmann::json;

std::string format_hex_address(uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%016llX",
                  static_cast<unsigned long long>(value));
    return std::string(buffer);
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    std::ostringstream output;
    output << stream.rdbuf();
    return output.str();
}

std::string builtin_stub_html() {
    return R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>Kernel Map Bridge</title></head>
<body><h1>Kernel Map Bridge</h1><p>bridge: probing</p></body></html>)HTML";
}

int32_t read_driver_file_bounded(std::string_view path,
                                 std::vector<uint8_t>& out) {
    constexpr uint64_t kMaxBytes = 32ull * 1024ull * 1024ull;
    out.clear();
    const std::wstring wide_path = std::filesystem::u8path(std::string(path)).wstring();
    HANDLE handle = CreateFileW(wide_path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE |
                                    FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    struct HandleGuard final {
        HANDLE value = INVALID_HANDLE_VALUE;
        ~HandleGuard() {
            if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
        }
    } handle_guard{handle};

    const bool disk_file = GetFileType(handle) == FILE_TYPE_DISK;
    BY_HANDLE_FILE_INFORMATION information{};
    const bool has_information =
        GetFileInformationByHandle(handle, &information) != FALSE;
    LARGE_INTEGER size{};
    const bool has_size = GetFileSizeEx(handle, &size) != FALSE;
    if (!disk_file || !has_information || !has_size ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        size.QuadPart <= 0 ||
        static_cast<uint64_t>(size.QuadPart) > kMaxBytes) {
        return size.QuadPart > static_cast<LONGLONG>(kMaxBytes)
                   ? SAO_AI_EDITOR_ERR_INVALID_ARGUMENT
                   : SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }

    out.resize(static_cast<size_t>(size.QuadPart));
    size_t offset = 0;
    while (offset < out.size()) {
        const DWORD request = static_cast<DWORD>(
            std::min<size_t>(out.size() - offset, 1024u * 1024u));
        DWORD read = 0;
        if (ReadFile(handle, out.data() + offset, request, &read, nullptr) == FALSE ||
            read == 0) {
            out.clear();
            return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
        }
        offset += read;
    }
    return SAO_AI_EDITOR_OK;
}

}  // namespace

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
    const int32_t rc = sao::ai_editor::kernel_map::shared_bridge().status(reply);
    return Json{{"active", reply.active},
                {"mapped_count", static_cast<int64_t>(reply.map_count)},
                {"rc", rc}};
}

Json KernelMapDefaultBridge::enumerate() {
    std::vector<uint64_t> bases;
    const int32_t rc = sao::ai_editor::kernel_map::shared_bridge().enumerate(bases);
    Json values = Json::array();
    for (const uint64_t base : bases) values.push_back(format_hex_address(base));
    return Json{{"bases", std::move(values)}, {"rc", rc}};
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

Json KernelMapDefaultBridge::map(const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) {
        return Json{{"ok", false}, {"target_base", "0x0000000000000000"},
                    {"rc", SAO_AI_EDITOR_ERR_INVALID_ARGUMENT}};
    }
    sao::ai_editor::kernel_map::MapResult result{};
    const int32_t rc = sao::ai_editor::kernel_map::shared_bridge().map(
        bytes.data(), static_cast<uint32_t>(bytes.size()), result);
    return Json{{"ok", rc == SAO_AI_EDITOR_OK},
                {"rc", rc},
                {"target_base", format_hex_address(result.target_base)},
                {"entry_status", result.entry_status}};
}

Json KernelMapDefaultBridge::unmap(uint64_t target_base) {
    const int32_t rc = sao::ai_editor::kernel_map::shared_bridge().unmap(target_base);
    return Json{{"ok", rc == SAO_AI_EDITOR_OK},
                {"target_base", format_hex_address(target_base)}, {"rc", rc}};
}
#else
Json KernelMapDefaultBridge::status() { return Json{{"active", false}, {"mapped_count", 0}}; }
Json KernelMapDefaultBridge::enumerate() { return Json{{"bases", Json::array()}}; }
Json KernelMapDefaultBridge::activate(const ActivateConfig&) { return Json{{"ok", false}}; }
Json KernelMapDefaultBridge::deactivate() { return Json{{"ok", false}}; }
Json KernelMapDefaultBridge::map(const std::vector<uint8_t>&) {
    return Json{{"ok", false}, {"target_base", "0x0000000000000000"}};
}
Json KernelMapDefaultBridge::unmap(uint64_t) { return Json{{"ok", false}}; }
#endif

KernelMapPanelProvider::KernelMapPanelProvider()
    : bridge_(std::make_shared<KernelMapDefaultBridge>()) {}

KernelMapPanelProvider::~KernelMapPanelProvider() = default;

void KernelMapPanelProvider::install_bridge(
    std::unique_ptr<IKernelMapBridge> bridge) {
    if (bridge == nullptr) return;
    std::lock_guard<std::mutex> guard(mutex_);
    bridge_ = std::shared_ptr<IKernelMapBridge>(std::move(bridge));
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
    if (assets_root.empty()) return builtin_stub_html();
    const std::filesystem::path root(assets_root);
    std::string html = read_file(root / "index.html");
    if (html.empty()) return builtin_stub_html();
    const std::string css = read_file(root / "panel.css");
    const std::string js = read_file(root / "panel.js");
    if (!css.empty()) {
        const std::string tag = R"(<link rel="stylesheet" href="panel.css">)";
        const auto pos = html.find(tag);
        if (pos != std::string::npos) html.replace(pos, tag.size(), "<style>\n" + css + "\n</style>");
    }
    if (!js.empty()) {
        const std::string tag = R"(<script src="panel.js"></script>)";
        const auto pos = html.find(tag);
        if (pos != std::string::npos) html.replace(pos, tag.size(), "<script>\n" + js + "\n</script>");
    }
    return html;
}

int32_t KernelMapPanelProvider::register_with_runtime(
    WebviewPanelRegistry& registry, const std::string& assets_root) {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (registered_) return SAO_AI_EDITOR_OK;
    }

    WebviewPanelOptions options;
    options.enable_scripts = true;
    options.retain_context_when_hidden = true;
    options.view_column = 1;
    options.extras = Json{{"builtin", true}, {"kind", "operator-dashboard"}};

    WebviewPanelState state;
    bool created = false;
    int32_t status = registry.create(
        std::string{kKernelMapPanelId}, std::string{kKernelMapViewType},
        std::string{kKernelMapPanelTitle}, options,
        WebviewPanelOwner::native_runtime, state);
    if (status == SAO_AI_EDITOR_ERR_INVALID_ARGUMENT) {
        const auto existing = registry.snapshot(std::string{kKernelMapPanelId});
        if (!existing.has_value() || existing->disposed ||
            existing->owner != WebviewPanelOwner::native_runtime ||
            existing->view_type != kKernelMapViewType ||
            existing->title != kKernelMapPanelTitle) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        state = *existing;
    } else if (status != SAO_AI_EDITOR_OK) {
        return status;
    } else {
        created = true;
    }

    const std::string html = load_bundled_html(assets_root);
    status = registry.set_html(std::string{kKernelMapPanelId}, html, state);
    if (status != SAO_AI_EDITOR_OK) {
        if (created) {
            WebviewPanelState rollback;
            (void)registry.dispose(std::string{kKernelMapPanelId}, rollback);
        }
        return status;
    }
    {
        std::lock_guard<std::mutex> guard(mutex_);
        registered_ = true;
    }
    return SAO_AI_EDITOR_OK;
}

int32_t KernelMapPanelProvider::unregister_from_runtime(
    WebviewPanelRegistry& registry) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!registered_) return SAO_AI_EDITOR_OK;
    WebviewPanelState state;
    const int32_t status =
        registry.dispose(std::string{kKernelMapPanelId}, state);
    if (status != SAO_AI_EDITOR_OK && status != SAO_AI_EDITOR_ERR_NOT_FOUND)
        return status;
    registered_ = false;
    return SAO_AI_EDITOR_OK;
}

int32_t KernelMapPanelProvider::handle_message(const Json& message,
                                               Json& out_reply) {
    try {
        if (!message.is_object() || !message.contains("cmd") &&
            !message.contains("command")) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const Json& cmd_value = message.contains("cmd")
                                    ? message["cmd"] : message["command"];
        if (!cmd_value.is_string() || cmd_value.get<std::string>().empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string cmd = cmd_value.get<std::string>();
        const Json request_id = message.value("requestId", Json());
        const Json args = message.contains("args") ? message["args"] : Json::object();
        if (!args.is_object()) return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;

        std::shared_ptr<IKernelMapBridge> bridge;
        FilePicker picker;
        PostToPage post;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            bridge = bridge_;
            picker = file_picker_;
            post = post_to_page_;
        }

        if (cmd == "status") out_reply = handle_status(bridge);
        else if (cmd == "enumerate") out_reply = handle_enumerate(bridge);
        else if (cmd == "activate") out_reply = handle_activate(bridge, args);
        else if (cmd == "deactivate") out_reply = handle_deactivate(bridge);
        else if (cmd == "unmap") out_reply = handle_unmap(bridge, args);
        else if (cmd == "refresh") out_reply = handle_refresh();
        else if (cmd == "load_driver") out_reply = handle_load_driver(bridge, picker);
        else out_reply = build_error(cmd, "unknown command", request_id);

        out_reply["cmd"] = cmd;
        if (!request_id.is_null()) out_reply["requestId"] = request_id;
        static thread_local bool post_in_progress = false;
        if (post && !post_in_progress) {
            post_in_progress = true;
            try { (void)post(std::string{kKernelMapPanelId}, out_reply); }
            catch (...) {}
            post_in_progress = false;
        }
        return SAO_AI_EDITOR_OK;
    } catch (const std::exception& error) {
        out_reply = build_error("", error.what(), Json());
        return SAO_AI_EDITOR_OK;
    } catch (...) {
        out_reply = build_error("", "kernel map panel message failed", Json());
        return SAO_AI_EDITOR_OK;
    }
}

Json KernelMapPanelProvider::build_reply(std::string_view, std::string_view status,
                                         const Json& payload, const Json&) const {
    Json reply{{"status", std::string(status)}};
    if (!payload.is_null()) reply["payload"] = payload;
    return reply;
}

Json KernelMapPanelProvider::build_error(std::string_view, std::string_view reason,
                                         const Json&) const {
    return Json{{"status", "error"}, {"reason", std::string(reason)}};
}

Json KernelMapPanelProvider::handle_status(
    const std::shared_ptr<IKernelMapBridge>& bridge) {
    if (bridge == nullptr || !bridge->is_available()) {
        Json reply = build_error("status", "bridge unavailable", Json());
        reply["payload"] = Json{{"active", false}, {"mapped_count", 0}};
        return reply;
    }
    return build_reply("status", "ok", bridge->status(), Json());
}

Json KernelMapPanelProvider::handle_enumerate(
    const std::shared_ptr<IKernelMapBridge>& bridge) {
    if (bridge == nullptr || !bridge->is_available()) {
        Json reply = build_error("enumerate", "bridge unavailable", Json());
        reply["payload"] = Json{{"bases", Json::array()}};
        return reply;
    }
    return build_reply("enumerate", "ok", bridge->enumerate(), Json());
}

Json KernelMapPanelProvider::handle_activate(
    const std::shared_ptr<IKernelMapBridge>& bridge, const Json& args) {
    if (bridge == nullptr || !bridge->is_available())
        return build_error("activate", "bridge unavailable", Json());
    IKernelMapBridge::ActivateConfig config{};
    if (args.contains("invoke_result_slot_va") &&
        !sao::ai_editor::kernel_map::parse_kernel_map_uint64(
            args["invoke_result_slot_va"], config.invoke_result_slot_va)) {
        return build_error("activate", "invalid invoke_result_slot_va", Json());
    }
    if (args.contains("pool_tag_seed") &&
        !sao::ai_editor::kernel_map::parse_kernel_map_uint64(
            args["pool_tag_seed"], config.pool_tag_seed)) {
        return build_error("activate", "invalid pool_tag_seed", Json());
    }
    if (args.contains("idle_timeout_ms")) {
        if (!sao::ai_editor::kernel_map::parse_kernel_map_uint32(
                args["idle_timeout_ms"], config.idle_timeout_ms)) {
            return build_error("activate", "invalid idle_timeout_ms", Json());
        }
    }
    const Json payload = bridge->activate(config);
    return build_reply("activate", payload.value("ok", false) ? "ok" : "error",
                       payload, Json());
}

Json KernelMapPanelProvider::handle_deactivate(
    const std::shared_ptr<IKernelMapBridge>& bridge) {
    if (bridge == nullptr || !bridge->is_available())
        return build_error("deactivate", "bridge unavailable", Json());
    const Json payload = bridge->deactivate();
    return build_reply("deactivate", payload.value("ok", false) ? "ok" : "error",
                       payload, Json());
}

Json KernelMapPanelProvider::handle_unmap(
    const std::shared_ptr<IKernelMapBridge>& bridge, const Json& args) {
    if (bridge == nullptr || !bridge->is_available())
        return build_error("unmap", "bridge unavailable", Json());
    uint64_t base = 0;
    if (!args.contains("target_base") ||
        !sao::ai_editor::kernel_map::parse_kernel_map_uint64(
            args["target_base"], base) || base == 0) {
        return build_error("unmap", "invalid target_base", Json());
    }
    const Json payload = bridge->unmap(base);
    return build_reply("unmap", payload.value("ok", false) ? "ok" : "error",
                       payload, Json());
}

Json KernelMapPanelProvider::handle_load_driver(
    const std::shared_ptr<IKernelMapBridge>& bridge, const FilePicker& picker) {
    if (bridge == nullptr || !bridge->is_available())
        return build_error("load_driver", "bridge unavailable", Json());
    if (!picker) return Json{{"status", "not_implemented"},
                             {"reason", "no file picker in current build"}};
    const std::string path = picker();
    if (path.empty()) return build_reply("load_driver", "cancelled", Json::object(), Json());
    std::vector<uint8_t> bytes;
    const int32_t status = read_driver_file_bounded(path, bytes);
    if (status != SAO_AI_EDITOR_OK) {
        return Json{{"status", "error"},
                    {"reason", status == SAO_AI_EDITOR_ERR_INVALID_ARGUMENT
                                    ? "driver image exceeds 32 MiB cap"
                                    : "unable to read driver image"},
                    {"payload", Json{{"path", path}, {"status", status}}}};
    }
    const Json payload = bridge->map(bytes);
    Json with_path = payload;
    with_path["path"] = path;
    return build_reply("load_driver", with_path.value("ok", false) ? "ok" : "error",
                       with_path, Json());
}

Json KernelMapPanelProvider::handle_refresh() {
    return build_reply("refresh", "ok", Json::object(), Json());
}

}  // namespace sao::ai_editor::native
