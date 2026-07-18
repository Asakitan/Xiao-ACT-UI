#include "extension_host.h"

#include <utility>

#include "native_runtime_internal.h"

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

int32_t ExtensionHost::configure(const Json& params) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!params.is_object() ||
        !params.contains("nodeExecutable") ||
        !params["nodeExecutable"].is_string() ||
        !params.contains("entryScript") ||
        !params["entryScript"].is_string()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    boot_options_ = NodeRuntime::BootOptions{};
    boot_options_.node_executable =
        params["nodeExecutable"].get<std::string>();
    boot_options_.entry_script = params["entryScript"].get<std::string>();
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

int32_t ExtensionHost::ensure_runtime() {
    if (boot_options_.node_executable.empty() ||
        boot_options_.entry_script.empty()) {
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    if (node_runtime_ && node_runtime_->alive()) {
        return SAO_AI_EDITOR_OK;
    }
    node_runtime_ = std::make_unique<NodeRuntime>();
    node_runtime_->set_native_runtime(&runtime_);
    const int32_t status = node_runtime_->boot(boot_options_);
    if (status != SAO_AI_EDITOR_OK) {
        node_runtime_.reset();
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
    std::lock_guard<std::mutex> guard(mutex_);
    const auto found = extensions_.find(std::string(extension_id));
    if (found == extensions_.end()) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    if (found->second.activated && node_runtime_ && node_runtime_->alive()) {
        Json deactivate_result;
        (void)node_runtime_->request(
            "host.deactivate",
            Json{{"extensionId", found->first}}, 5000, deactivate_result);
    }
    extensions_.erase(found);
    out = Json{{"ok", true}, {"extensionId", std::string(extension_id)}};
    return SAO_AI_EDITOR_OK;
}

int32_t ExtensionHost::activate(std::string_view extension_id,
                                uint32_t timeout_ms, Json& out) {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto found = extensions_.find(std::string(extension_id));
    if (found == extensions_.end()) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    const int32_t runtime_status = ensure_runtime();
    if (runtime_status != SAO_AI_EDITOR_OK) {
        return runtime_status;
    }
    Json params{{"extensionId", found->first},
                {"extensionPath", found->second.extension_path},
                {"main", found->second.main_module},
                {"manifest", found->second.manifest}};
    Json result;
    const int32_t status = node_runtime_->request(
        "host.activate", params, timeout_ms == 0 ? 15000 : timeout_ms, result);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    found->second.activated = true;
    found->second.activation_result = result;
    out = found->second.to_json();
    return SAO_AI_EDITOR_OK;
}

int32_t ExtensionHost::deactivate(std::string_view extension_id, Json& out) {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto found = extensions_.find(std::string(extension_id));
    if (found == extensions_.end()) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    if (!found->second.activated) {
        out = Json{{"ok", true}, {"already", true}};
        return SAO_AI_EDITOR_OK;
    }
    if (node_runtime_ && node_runtime_->alive()) {
        Json result;
        const int32_t status = node_runtime_->request(
            "host.deactivate",
            Json{{"extensionId", found->first}}, 5000, result);
        if (status != SAO_AI_EDITOR_OK &&
            status != SAO_AI_EDITOR_ERR_IPC_CLOSED) {
            return status;
        }
    }
    found->second.activated = false;
    found->second.activation_result = Json::object();
    out = Json{{"ok", true}, {"extensionId", std::string(extension_id)}};
    return SAO_AI_EDITOR_OK;
}

int32_t ExtensionHost::execute_command(std::string_view command_id,
                                       const Json& args, uint32_t timeout_ms,
                                       Json& out) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!node_runtime_ || !node_runtime_->alive()) {
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    Json params{{"command", std::string(command_id)}, {"arguments", args}};
    return node_runtime_->request("commands.execute", params,
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
    std::lock_guard<std::mutex> guard(mutex_);
    if (node_runtime_ && node_runtime_->alive()) {
        Json result;
        (void)node_runtime_->request("host.shutdown", Json::object(), 2000,
                                      result);
    }
    if (node_runtime_) {
        node_runtime_->shutdown();
        node_runtime_.reset();
    }
    for (auto& [id, record] : extensions_) {
        (void)id;
        record.activated = false;
    }
}

}  // namespace sao::ai_editor::native
