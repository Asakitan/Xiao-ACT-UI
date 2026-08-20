#include "extension_host.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <new>
#include <utility>
#include <vector>

#include "native_runtime_internal.h"

namespace sao::ai_editor::native {
namespace {

std::filesystem::path module_directory() {
    HMODULE module_handle = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&module_directory),
            &module_handle) ||
        module_handle == nullptr) {
        return {};
    }
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD written = GetModuleFileNameW(
            module_handle, buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return {};
        }
        if (written < buffer.size()) {
            std::filesystem::path path(buffer.data());
            return path.parent_path();
        }
        buffer.resize(buffer.size() * 2);
        if (buffer.size() > 32768) {
            return {};
        }
    }
}

std::string default_shim_path_utf8() {
    const std::filesystem::path anchor = module_directory();
    if (anchor.empty()) {
        return {};
    }
    static const wchar_t* kRelativeCandidates[] = {
        L"assets/ai_editor/extension_host_shim.js",
        L"../assets/ai_editor/extension_host_shim.js",
        L"../../assets/ai_editor/extension_host_shim.js",
        L"../plugins/ai_editor/assets/extension_host_shim.js",
        L"../../plugins/ai_editor/assets/extension_host_shim.js",
    };
    for (const auto* rel : kRelativeCandidates) {
        std::filesystem::path candidate = anchor / rel;
        std::error_code error;
        candidate = std::filesystem::weakly_canonical(candidate, error);
        if (error) {
            continue;
        }
        if (std::filesystem::exists(candidate, error) && !error) {
            return wide_to_utf8(candidate.native());
        }
    }
    return {};
}

}  // namespace
}  // namespace sao::ai_editor::native

namespace sao::ai_editor::native {

namespace {

const char* operation_name(ExtensionOperation operation) noexcept {
    switch (operation) {
        case ExtensionOperation::activating:
            return "activating";
        case ExtensionOperation::deactivating:
            return "deactivating";
        case ExtensionOperation::unregistering:
            return "unregistering";
        case ExtensionOperation::idle:
        default:
            return "idle";
    }
}

bool has_inflight_operation_locked(
    const std::unordered_map<std::string, ExtensionRecord>& extensions) {
    return std::any_of(
        extensions.begin(), extensions.end(),
        [](const auto& entry) {
            return entry.second.operation != ExtensionOperation::idle;
        });
}

}  // namespace

Json ExtensionRecord::to_json() const {
    return Json{{"id", id},
                {"name", name},
                {"publisher", publisher},
                {"version", version},
                {"extensionPath", extension_path},
                {"main", main_module},
                {"activated", activated},
                {"operation", operation_name(operation)},
                {"generation", generation},
                {"operationGeneration", operation_generation},
                {"manifest", manifest},
                {"activationResult", activation_result}};
}

ExtensionHost::~ExtensionHost() { deactivate_all(); }

int32_t ExtensionHost::configure(const Json& params) {
    try {
        std::lock_guard<std::mutex> runtime_guard(runtime_mutex_);
        std::lock_guard<std::mutex> guard(mutex_);
        if (has_inflight_operation_locked(extensions_) ||
            (node_runtime_ && node_runtime_->alive())) {
            return SAO_AI_EDITOR_ERR_BUSY;
        }
        if (!params.is_object() || !params.contains("nodeExecutable") ||
            !params["nodeExecutable"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }

        NodeRuntime::BootOptions candidate;
        candidate.node_executable =
            params["nodeExecutable"].get<std::string>();
        if (candidate.node_executable.empty() ||
            !valid_utf8(candidate.node_executable)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }

        if (params.contains("entryScript")) {
            if (!params["entryScript"].is_string()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            candidate.entry_script =
                params["entryScript"].get<std::string>();
            if (!valid_utf8(candidate.entry_script)) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
        }
        if (candidate.entry_script.empty()) {
            candidate.entry_script = default_shim_path_utf8();
            if (candidate.entry_script.empty()) {
                return SAO_AI_EDITOR_ERR_NOT_FOUND;
            }
        }

        const auto parse_string_array = [](const Json& value,
                                           std::vector<std::string>& target) {
            if (!value.is_array()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            target.reserve(value.size());
            for (const auto& item : value) {
                if (!item.is_string()) {
                    return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                }
                const std::string string = item.get<std::string>();
                if (!valid_utf8(string)) {
                    return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                }
                target.push_back(string);
            }
            return SAO_AI_EDITOR_OK;
        };

        if (params.contains("nodeArgs") &&
            parse_string_array(params["nodeArgs"], candidate.node_args) !=
                SAO_AI_EDITOR_OK) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        if (params.contains("extraArgs") &&
            parse_string_array(params["extraArgs"], candidate.extra_args) !=
                SAO_AI_EDITOR_OK) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        if (params.contains("environment")) {
            if (!params["environment"].is_object()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            candidate.environment.reserve(params["environment"].size());
            for (const auto& [key, value] : params["environment"].items()) {
                if (key.empty() || !valid_utf8(key) || !value.is_string()) {
                    return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                }
                const std::string string = value.get<std::string>();
                if (!valid_utf8(string)) {
                    return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                }
                candidate.environment.emplace_back(key, string);
            }
        }
        if (params.contains("workingDirectory")) {
            if (!params["workingDirectory"].is_string()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            candidate.working_directory =
                params["workingDirectory"].get<std::string>();
            if (!valid_utf8(candidate.working_directory)) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
        }
        if (params.contains("startupMs")) {
            if (!params["startupMs"].is_number_unsigned() &&
                !params["startupMs"].is_number_integer()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            uint64_t startup_ms = 0;
            if (params["startupMs"].is_number_unsigned()) {
                startup_ms = params["startupMs"].get<uint64_t>();
            } else {
                const int64_t signed_startup_ms =
                    params["startupMs"].get<int64_t>();
                if (signed_startup_ms < 0) {
                    return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                }
                startup_ms = static_cast<uint64_t>(signed_startup_ms);
            }
            if (startup_ms > std::numeric_limits<uint32_t>::max()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            candidate.startup_ms = static_cast<uint32_t>(startup_ms);
        }

        boot_options_ = std::move(candidate);
        return SAO_AI_EDITOR_OK;
    } catch (const std::bad_alloc&) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    } catch (const Json::exception&) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    } catch (...) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

int32_t ExtensionHost::ensure_runtime(
    std::shared_ptr<NodeRuntime>& runtime) {
    std::lock_guard<std::mutex> runtime_guard(runtime_mutex_);
    std::shared_ptr<NodeRuntime> stale_runtime;
    NodeRuntime::BootOptions options;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (boot_options_.node_executable.empty() ||
            boot_options_.entry_script.empty()) {
            return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
        }
        if (node_runtime_ && node_runtime_->alive()) {
            runtime = node_runtime_;
            return SAO_AI_EDITOR_OK;
        }
        stale_runtime = std::move(node_runtime_);
        options = boot_options_;
    }
    if (stale_runtime) {
        stale_runtime->shutdown();
    }
    auto candidate = std::make_shared<NodeRuntime>();
    candidate->set_native_runtime(&runtime_);
    const int32_t status = candidate->boot(options);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (node_runtime_ && node_runtime_->alive()) {
            runtime = node_runtime_;
        } else {
            node_runtime_ = candidate;
            runtime = std::move(candidate);
        }
    }
    if (candidate) {
        candidate->shutdown();
    }
    return SAO_AI_EDITOR_OK;
}

int32_t ExtensionHost::list_extensions(Json& out) const {
    std::lock_guard<std::mutex> guard(mutex_);
    Json items = Json::array();
    for (const auto& [id, record] : extensions_) {
        (void)id;
        items.push_back(record.to_json());
    }
    out = Json{{"items", std::move(items)},
               {"total", extensions_.size()},
               {"nodeExecutable", boot_options_.node_executable},
               {"nodeAlive", node_runtime_ && node_runtime_->alive()}};
    return SAO_AI_EDITOR_OK;
}

int32_t ExtensionHost::register_extension(const Json& params, Json& out) {
    if (!params.is_object() || !params.contains("manifest") ||
        !params["manifest"].is_object()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    ExtensionRecord record;
    record.manifest = params["manifest"];
    record.extension_path = params.value("extensionPath", std::string{});
    record.name = record.manifest.value("name", std::string{});
    record.publisher = record.manifest.value("publisher", std::string{});
    record.version = record.manifest.value("version", std::string{});
    record.main_module = record.manifest.value("main", std::string{});
    if (record.name.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    record.id = record.publisher.empty()
        ? record.name
        : record.publisher + "." + record.name;
    if (!valid_utf8(record.id)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    const auto existing = extensions_.find(record.id);
    if (existing != extensions_.end()) {
        if (existing->second.operation != ExtensionOperation::idle ||
            existing->second.activated) {
            return SAO_AI_EDITOR_ERR_BUSY;
        }
        record.generation = existing->second.generation + 1;
    }
    const auto [inserted, _] = extensions_.insert_or_assign(record.id, record);
    out = inserted->second.to_json();
    return SAO_AI_EDITOR_OK;
}

int32_t ExtensionHost::unregister_extension(std::string_view extension_id,
                                            Json& out) {
    const std::string id(extension_id);
    std::shared_ptr<NodeRuntime> node;
    uint64_t operation_generation = 0;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = extensions_.find(id);
        if (found == extensions_.end()) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        if (found->second.operation != ExtensionOperation::idle) {
            return SAO_AI_EDITOR_ERR_BUSY;
        }
        if (!found->second.activated) {
            extensions_.erase(found);
            out = Json{{"ok", true}, {"extensionId", id}};
            return SAO_AI_EDITOR_OK;
        }
        found->second.operation = ExtensionOperation::unregistering;
        operation_generation = ++found->second.generation;
        found->second.operation_generation = operation_generation;
        node = node_runtime_;
    }

    if (!node || !node->alive()) {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = extensions_.find(id);
        if (found != extensions_.end() &&
            found->second.operation_generation == operation_generation) {
            found->second.operation = ExtensionOperation::idle;
        }
        out = Json{{"message", "extension host is not running"},
                   {"extensionId", id}};
        return SAO_AI_EDITOR_ERR_IPC_CLOSED;
    }
    Json deactivate_result;
    const int32_t status = node->request(
        "host.deactivate", Json{{"extensionId", id}}, 5000,
        deactivate_result);
    if (status != SAO_AI_EDITOR_OK) {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = extensions_.find(id);
        if (found != extensions_.end() &&
            found->second.operation_generation == operation_generation) {
            found->second.operation = ExtensionOperation::idle;
        }
        out = std::move(deactivate_result);
        return status;
    }

    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = extensions_.find(id);
        if (found == extensions_.end() ||
            found->second.operation != ExtensionOperation::unregistering ||
            found->second.operation_generation != operation_generation) {
            return SAO_AI_EDITOR_ERR_BUSY;
        }
        extensions_.erase(found);
    }
    out = Json{{"ok", true}, {"extensionId", id}};
    return SAO_AI_EDITOR_OK;
}

int32_t ExtensionHost::activate(std::string_view extension_id,
                                uint32_t timeout_ms, Json& out) {
    const std::string id(extension_id);
    ExtensionRecord record;
    uint64_t operation_generation = 0;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = extensions_.find(id);
        if (found == extensions_.end()) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        if (found->second.operation != ExtensionOperation::idle) {
            return SAO_AI_EDITOR_ERR_BUSY;
        }
        if (found->second.activated && node_runtime_ &&
            node_runtime_->alive()) {
            return SAO_AI_EDITOR_ERR_BUSY;
        }
        found->second.operation = ExtensionOperation::activating;
        operation_generation = ++found->second.generation;
        found->second.operation_generation = operation_generation;
        record = found->second;
    }
    std::shared_ptr<NodeRuntime> node;
    const int32_t runtime_status = ensure_runtime(node);
    if (runtime_status != SAO_AI_EDITOR_OK) {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = extensions_.find(id);
        if (found != extensions_.end() &&
            found->second.operation_generation == operation_generation) {
            found->second.operation = ExtensionOperation::idle;
        }
        return runtime_status;
    }
    Json params{{"extensionId", id},
                {"extensionPath", record.extension_path},
                {"main", record.main_module},
                {"manifest", record.manifest}};
    Json result;
    const int32_t status = node->request(
        "host.activate", params, timeout_ms == 0 ? 15000 : timeout_ms, result);
    if (status != SAO_AI_EDITOR_OK) {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = extensions_.find(id);
        if (found != extensions_.end() &&
            found->second.operation_generation == operation_generation) {
            found->second.operation = ExtensionOperation::idle;
        }
        out = std::move(result);
        return status;
    }
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = extensions_.find(id);
        if (found == extensions_.end() ||
            found->second.operation != ExtensionOperation::activating ||
            found->second.operation_generation != operation_generation) {
            return SAO_AI_EDITOR_ERR_BUSY;
        }
        found->second.activated = true;
        found->second.activation_result = std::move(result);
        found->second.operation = ExtensionOperation::idle;
        out = found->second.to_json();
    }
    return SAO_AI_EDITOR_OK;
}

int32_t ExtensionHost::deactivate(std::string_view extension_id, Json& out) {
    const std::string id(extension_id);
    std::shared_ptr<NodeRuntime> node;
    uint64_t operation_generation = 0;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = extensions_.find(id);
        if (found == extensions_.end()) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        if (found->second.operation != ExtensionOperation::idle) {
            return SAO_AI_EDITOR_ERR_BUSY;
        }
        if (!found->second.activated) {
            out = Json{{"ok", true}, {"already", true}};
            return SAO_AI_EDITOR_OK;
        }
        found->second.operation = ExtensionOperation::deactivating;
        operation_generation = ++found->second.generation;
        found->second.operation_generation = operation_generation;
        node = node_runtime_;
    }

    if (!node || !node->alive()) {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = extensions_.find(id);
        if (found != extensions_.end() &&
            found->second.operation_generation == operation_generation) {
            found->second.operation = ExtensionOperation::idle;
        }
        out = Json{{"message", "extension host is not running"},
                   {"extensionId", id}};
        return SAO_AI_EDITOR_ERR_IPC_CLOSED;
    }
    Json deactivate_result;
    const int32_t status = node->request(
        "host.deactivate", Json{{"extensionId", id}}, 5000,
        deactivate_result);
    if (status != SAO_AI_EDITOR_OK) {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = extensions_.find(id);
        if (found != extensions_.end() &&
            found->second.operation_generation == operation_generation) {
            found->second.operation = ExtensionOperation::idle;
        }
        out = std::move(deactivate_result);
        return status;
    }

    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = extensions_.find(id);
        if (found == extensions_.end() ||
            found->second.operation != ExtensionOperation::deactivating ||
            found->second.operation_generation != operation_generation) {
            return SAO_AI_EDITOR_ERR_BUSY;
        }
        found->second.activated = false;
        found->second.activation_result = Json::object();
        found->second.operation = ExtensionOperation::idle;
    }
    out = Json{{"ok", true}, {"extensionId", id}};
    return SAO_AI_EDITOR_OK;
}

int32_t ExtensionHost::execute_command(std::string_view command_id,
                                       const Json& args, uint32_t timeout_ms,
                                       Json& out) {
    NativeCommandHandler native_handler;
    std::shared_ptr<NodeRuntime> node;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto native = native_commands_.find(std::string(command_id));
        if (native != native_commands_.end()) {
            native_handler = native->second.handler;
        }
        node = node_runtime_;
    }
    if (native_handler) {
        return native_handler(args, out);
    }
    if (!node || !node->alive()) {
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    Json params{{"command", std::string(command_id)}, {"arguments", args}};
    return node->request("commands.execute", params,
                         timeout_ms == 0 ? 15000 : timeout_ms, out);
}

int32_t ExtensionHost::register_native_command(
    std::string_view command_id, NativeCommandHandler handler,
    uint64_t owner) {
    if (command_id.empty() || !valid_utf8(command_id) || !handler) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    const std::string id(command_id);
    const auto found = native_commands_.find(id);
    if (found != native_commands_.end() && found->second.owner != owner &&
        (found->second.owner != 0 || owner != 0)) {
        return SAO_AI_EDITOR_ERR_BUSY;
    }
    native_commands_.insert_or_assign(
        id, NativeCommandRegistration{std::move(handler), owner});
    return SAO_AI_EDITOR_OK;
}

int32_t ExtensionHost::unregister_native_command(
    std::string_view command_id, uint64_t owner) {
    if (command_id.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    const auto found = native_commands_.find(std::string(command_id));
    if (found == native_commands_.end()) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    if (found->second.owner != owner &&
        (found->second.owner != 0 || owner != 0)) {
        return SAO_AI_EDITOR_ERR_BUSY;
    }
    native_commands_.erase(found);
    return SAO_AI_EDITOR_OK;
}

std::optional<ExtensionHost::NativeCommandRegistration>
ExtensionHost::snapshot_native_command(std::string_view command_id) const {
    if (command_id.empty()) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    const auto found = native_commands_.find(std::string(command_id));
    if (found == native_commands_.end()) {
        return std::nullopt;
    }
    return found->second;
}

int32_t ExtensionHost::restore_native_command(
    std::string_view command_id,
    const std::optional<NativeCommandRegistration>& prior,
    uint64_t owner) {
    if (command_id.empty() || owner == 0) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    const std::string id(command_id);
    const auto found = native_commands_.find(id);
    if (found != native_commands_.end() && found->second.owner != owner) {
        return SAO_AI_EDITOR_ERR_BUSY;
    }
    if (prior.has_value()) {
        native_commands_.insert_or_assign(id, *prior);
    } else if (found != native_commands_.end()) {
        native_commands_.erase(found);
    }
    return SAO_AI_EDITOR_OK;
}

int32_t ExtensionHost::post_webview_message(const Json& params, Json& out) {
    std::shared_ptr<NodeRuntime> node;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        node = node_runtime_;
    }
    if (!node || !node->alive()) {
        return SAO_AI_EDITOR_ERR_IPC_CLOSED;
    }
    return node->request("webview.postToView", params, 5000, out);
}

Json ExtensionHost::snapshot() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return Json{{"nodeExecutable", boot_options_.node_executable},
                {"entryScript", boot_options_.entry_script},
                {"nodeAlive", node_runtime_ && node_runtime_->alive()},
                {"total", extensions_.size()}};
}

void ExtensionHost::deactivate_all() {
    std::shared_ptr<NodeRuntime> node;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        node = node_runtime_;
    }
    if (node && node->alive()) {
        Json result;
        (void)node->request("host.shutdown", Json::object(), 2000, result);
    }
    if (node) {
        node->shutdown();
    }
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (node_runtime_ == node) {
            node_runtime_.reset();
        }
        for (auto& [id, record] : extensions_) {
            (void)id;
            record.activated = false;
            record.operation = ExtensionOperation::idle;
        }
    }
}

}  // namespace sao::ai_editor::native
