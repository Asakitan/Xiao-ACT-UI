#include "extension_host.h"

#include <windows.h>

#include <filesystem>
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

Json ExtensionRecord::to_json() const {
    return Json{{"id", id},
                {"name", name},
                {"publisher", publisher},
                {"version", version},
                {"extensionPath", extension_path},
                {"main", main_module},
                {"activated", activated},
                {"manifest", manifest},
                {"activationResult", activation_result}};
}

ExtensionHost::~ExtensionHost() { deactivate_all(); }

int32_t ExtensionHost::configure(const Json& params) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!params.is_object() ||
        !params.contains("nodeExecutable") ||
        !params["nodeExecutable"].is_string()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    boot_options_ = NodeRuntime::BootOptions{};
    boot_options_.node_executable =
        params["nodeExecutable"].get<std::string>();
    if (params.contains("entryScript") &&
        params["entryScript"].is_string() &&
        !params["entryScript"].get<std::string>().empty()) {
        boot_options_.entry_script =
            params["entryScript"].get<std::string>();
    } else {
        boot_options_.entry_script = default_shim_path_utf8();
        if (boot_options_.entry_script.empty()) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
    }
    if (params.contains("nodeArgs") && params["nodeArgs"].is_array()) {
        for (const auto& item : params["nodeArgs"]) {
            if (!item.is_string()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            boot_options_.node_args.push_back(item.get<std::string>());
        }
    }
    if (params.contains("extraArgs") && params["extraArgs"].is_array()) {
        for (const auto& item : params["extraArgs"]) {
            if (!item.is_string()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            boot_options_.extra_args.push_back(item.get<std::string>());
        }
    }
    if (params.contains("environment") &&
        params["environment"].is_object()) {
        for (const auto& [key, value] : params["environment"].items()) {
            if (!value.is_string()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            boot_options_.environment.emplace_back(
                key, value.get<std::string>());
        }
    }
    boot_options_.working_directory =
        params.value("workingDirectory", std::string{});
    boot_options_.startup_ms = params.value("startupMs", 15000U);
    return SAO_AI_EDITOR_OK;
}

int32_t ExtensionHost::ensure_runtime(
    std::shared_ptr<NodeRuntime>& runtime) {
    std::shared_ptr<NodeRuntime> stale_runtime;
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
    auto candidate = std::make_shared<NodeRuntime>();
    candidate->set_native_runtime(&runtime_);
    const int32_t status = candidate->boot(boot_options_);
    if (status == SAO_AI_EDITOR_OK) {
        node_runtime_ = candidate;
        runtime = std::move(candidate);
    }
    return status;
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
    extensions_[record.id] = record;
    out = record.to_json();
    return SAO_AI_EDITOR_OK;
}

int32_t ExtensionHost::unregister_extension(std::string_view extension_id,
                                            Json& out) {
    const std::string id(extension_id);
    std::shared_ptr<NodeRuntime> node;
    bool activated = false;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = extensions_.find(id);
        if (found == extensions_.end()) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        activated = found->second.activated;
        node = node_runtime_;
    }

    if (activated) {
        if (!node || !node->alive()) {
            out = Json{{"message", "extension host is not running"},
                       {"extensionId", id}};
            return SAO_AI_EDITOR_ERR_IPC_CLOSED;
        }
        Json deactivate_result;
        const int32_t status = node->request(
            "host.deactivate", Json{{"extensionId", id}}, 5000,
            deactivate_result);
        if (status != SAO_AI_EDITOR_OK) {
            out = std::move(deactivate_result);
            return status;
        }
    }

    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = extensions_.find(id);
        if (found == extensions_.end()) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        if (found->second.activated != activated) {
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
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = extensions_.find(id);
        if (found == extensions_.end()) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        record = found->second;
    }
    std::shared_ptr<NodeRuntime> node;
    const int32_t runtime_status = ensure_runtime(node);
    if (runtime_status != SAO_AI_EDITOR_OK) {
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
        out = std::move(result);
        return status;
    }
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = extensions_.find(id);
        if (found == extensions_.end()) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        found->second.activated = true;
        found->second.activation_result = std::move(result);
        out = found->second.to_json();
    }
    return SAO_AI_EDITOR_OK;
}

int32_t ExtensionHost::deactivate(std::string_view extension_id, Json& out) {
    const std::string id(extension_id);
    std::shared_ptr<NodeRuntime> node;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = extensions_.find(id);
        if (found == extensions_.end()) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        if (!found->second.activated) {
            out = Json{{"ok", true}, {"already", true}};
            return SAO_AI_EDITOR_OK;
        }
        node = node_runtime_;
    }

    if (!node || !node->alive()) {
        out = Json{{"message", "extension host is not running"},
                   {"extensionId", id}};
        return SAO_AI_EDITOR_ERR_IPC_CLOSED;
    }
    Json deactivate_result;
    const int32_t status = node->request(
        "host.deactivate", Json{{"extensionId", id}}, 5000,
        deactivate_result);
    if (status != SAO_AI_EDITOR_OK) {
        out = std::move(deactivate_result);
        return status;
    }

    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = extensions_.find(id);
        if (found == extensions_.end()) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        found->second.activated = false;
        found->second.activation_result = Json::object();
    }
    out = Json{{"ok", true}, {"extensionId", id}};
    return SAO_AI_EDITOR_OK;
}

int32_t ExtensionHost::execute_command(std::string_view command_id,
                                       const Json& args, uint32_t timeout_ms,
                                       Json& out) {
    std::shared_ptr<NodeRuntime> node;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        node = node_runtime_;
    }
    if (!node || !node->alive()) {
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    Json params{{"command", std::string(command_id)}, {"arguments", args}};
    return node->request("commands.execute", params,
                         timeout_ms == 0 ? 15000 : timeout_ms, out);
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
        }
    }
}

}  // namespace sao::ai_editor::native
