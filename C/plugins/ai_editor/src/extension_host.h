#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "native_utils.h"
#include "node_runtime.h"

namespace sao::ai_editor::native {

class NativeRuntime;

enum class ExtensionOperation : uint8_t {
    idle,
    activating,
    quarantined,
    deactivating,
    unregistering,
};

// One installed extension.  This mirrors a slice of the VS Code
// `package.json` extension manifest — activated extensions run inside
// the shared NodeRuntime process, with the shim script routing every
// vscode.* API through NativeRuntime::dispatch_extension_call.
struct ExtensionRecord {
    std::string id;
    std::string name;
    std::string publisher;
    std::string version;
    std::string extension_path;
    std::string main_module;
    Json manifest;
    bool activated = false;
    Json activation_result;
    ExtensionOperation operation = ExtensionOperation::idle;
    uint64_t generation = 0;
    uint64_t operation_generation = 0;

    Json to_json() const;
};

class ExtensionHost final {
public:
    using NativeCommandHandler =
        std::function<int32_t(const Json& args, Json& out)>;

    struct NativeCommandRegistration final {
        NativeCommandHandler handler;
        uint64_t owner = 0;
    };

    explicit ExtensionHost(NativeRuntime& runtime) : runtime_(runtime) {}
    ~ExtensionHost();

    ExtensionHost(const ExtensionHost&) = delete;
    ExtensionHost& operator=(const ExtensionHost&) = delete;

    int32_t configure(const Json& params);
    int32_t list_extensions(Json& out) const;
    int32_t register_extension(const Json& params, Json& out);
    int32_t unregister_extension(std::string_view extension_id, Json& out);
    int32_t activate(std::string_view extension_id, uint32_t timeout_ms,
                     Json& out);
    int32_t deactivate(std::string_view extension_id, Json& out);
    int32_t execute_command(std::string_view command_id, const Json& args,
                            uint32_t timeout_ms, Json& out);
    int32_t register_native_command(std::string_view command_id,
                                    NativeCommandHandler handler,
                                    uint64_t owner = 0);
    int32_t unregister_native_command(std::string_view command_id,
                                      uint64_t owner = 0);
    std::optional<NativeCommandRegistration> snapshot_native_command(
        std::string_view command_id) const;
    int32_t restore_native_command(
        std::string_view command_id,
        const std::optional<NativeCommandRegistration>& prior,
        uint64_t owner);
    int32_t post_webview_message(const Json& params, Json& out);
    Json snapshot() const;

private:
    void deactivate_all();
    int32_t ensure_runtime(std::shared_ptr<NodeRuntime>& runtime);

    NativeRuntime& runtime_;
    mutable std::mutex mutex_;
    mutable std::shared_mutex runtime_mutex_;
    std::shared_ptr<NodeRuntime> node_runtime_;
    NodeRuntime::BootOptions boot_options_{};
    std::unordered_map<std::string, ExtensionRecord> extensions_;
    std::unordered_map<std::string, NativeCommandRegistration> native_commands_;
};

}  // namespace sao::ai_editor::native
