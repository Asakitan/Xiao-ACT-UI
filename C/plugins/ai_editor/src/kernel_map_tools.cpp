// SAO AI Editor - kernel-map tool-registry wiring implementation.

#include "kernel_map_tools.h"
#include "kernel_map_commands.h"

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "sao/ai_editor/ai_editor_status.h"
#include "sao/ai_editor/kernel_map_bridge.h"
#include "native_tool_registry.h"

namespace sao::ai_editor::kernel_map {
namespace {
using Json = nlohmann::json;
using Registry = sao::ai_editor::native::NativeToolRegistry;
using ExecuteFn = Registry::CustomExecuteFn;

struct ToolSpec final {
    std::string name;
    std::string description;
    Json schema;
    bool read_only = true;
    ExecuteFn execute = nullptr;
};

struct ToolRegistrationState final {
    std::shared_ptr<Bridge> bridge;
    std::vector<ToolSpec> specs;
    std::vector<ToolSpec> rollback_specs;
    bool rollback_pending = false;
};

#if defined(SAO_AI_EDITOR_TESTING)
struct FailureInjectionState final {
    int32_t register_fail_call = 0;
    int32_t register_fail_count = 0;
    int32_t unregister_fail_call = 0;
    int32_t unregister_fail_count = 0;
    int32_t failure_status = SAO_AI_EDITOR_ERR_BUSY;
    int32_t register_calls = 0;
    int32_t unregister_calls = 0;
};

FailureInjectionState& failure_injection() {
    static FailureInjectionState state;
    return state;
}

bool failure_active(int32_t call, int32_t first, int32_t count) {
    return first > 0 && count > 0 && call >= first && call < first + count;
}
#endif

std::mutex& registration_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<Registry*, ToolRegistrationState>& registration_states() {
    static std::unordered_map<Registry*, ToolRegistrationState> states;
    return states;
}

Json empty_object_schema() {
    return Json{{"type", "object"}, {"properties", Json::object()}};
}

int32_t execute_status(const Json& args, Json& result, void* user) {
    if (user == nullptr) return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    return dispatch_kernel_map_command(*static_cast<Bridge*>(user),
                                       "sao.kernelMap.status", args, result);
}

int32_t execute_enumerate(const Json& args, Json& result, void* user) {
    if (user == nullptr) return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    return dispatch_kernel_map_command(*static_cast<Bridge*>(user),
                                       "sao.kernelMap.enumerate", args, result);
}

int32_t execute_map(const Json& args, Json& result, void* user) {
    if (user == nullptr) return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    return dispatch_kernel_map_command(*static_cast<Bridge*>(user),
                                       "sao.kernelMap.map", args, result);
}

int32_t execute_unmap(const Json& args, Json& result, void* user) {
    if (user == nullptr) return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    return dispatch_kernel_map_command(*static_cast<Bridge*>(user),
                                       "sao.kernelMap.unmap", args, result);
}

const std::array<std::string_view, 4> kToolNames{
    "kernelMap.status", "kernelMap.enumerate", "kernelMap.map",
    "kernelMap.unmap"};

std::vector<ToolSpec> make_specs() {
    std::vector<ToolSpec> specs;
    specs.push_back({"kernelMap.status",
                     "Report whether the kernel map adapter is active and how many drivers are currently mapped.",
                     empty_object_schema(), true, &execute_status});
    specs.push_back({"kernelMap.enumerate",
                     "List kernel virtual addresses of currently mapped driver images.",
                     empty_object_schema(), true, &execute_enumerate});

    Json map_properties = Json::object();
    map_properties["driver_path"] = Json{
        {"type", "string"},
        {"description", "Absolute path to a PE32+ AMD64 driver file (.sys)."}};
    specs.push_back({
        "kernelMap.map",
        "Load a driver PE into non-paged pool without invoking DriverEntry.",
        Json{{"type", "object"},
             {"properties", std::move(map_properties)},
             {"required", Json::array({"driver_path"})}},
        false, &execute_map});

    Json unmap_properties = Json::object();
    unmap_properties["target_base"] = Json{
        {"type", "string"},
        {"description", "Kernel VA as decimal or 0x-prefixed hexadecimal."}};
    specs.push_back({
        "kernelMap.unmap",
        "Free a driver image previously loaded by kernelMap.map.",
        Json{{"type", "object"},
             {"properties", std::move(unmap_properties)},
             {"required", Json::array({"target_base"})}},
        false, &execute_unmap});
    return specs;
}

bool descriptor_exists(const Registry& registry, std::string_view name) {
    const Json descriptors = registry.describe("chat");
    if (!descriptors.is_array()) return false;
    for (const auto& descriptor : descriptors) {
        if (descriptor.is_object() && descriptor.value("name", std::string{}) == name) {
            return true;
        }
    }
    return false;
}

int32_t install_specs(Registry& registry, const std::vector<ToolSpec>& specs,
                      const std::shared_ptr<Bridge>& bridge) {
    for (const auto& spec : specs) {
#if defined(SAO_AI_EDITOR_TESTING)
        auto& injection = failure_injection();
        ++injection.register_calls;
        if (failure_active(injection.register_calls,
                           injection.register_fail_call,
                           injection.register_fail_count)) {
            return injection.failure_status;
        }
#endif
        const int32_t status = registry.register_custom(
            spec.name, spec.description, spec.schema, spec.read_only,
            spec.execute, bridge.get());
        if (status != SAO_AI_EDITOR_OK) return status;
    }
    return SAO_AI_EDITOR_OK;
}

int32_t restore_specs(Registry& registry,
                      const std::vector<ToolSpec>& specs,
                      const std::shared_ptr<Bridge>& bridge) {
    int32_t first_error = SAO_AI_EDITOR_OK;
    if (specs.empty()) {
        for (const auto name : kToolNames) {
#if defined(SAO_AI_EDITOR_TESTING)
            auto& injection = failure_injection();
            ++injection.unregister_calls;
            if (failure_active(injection.unregister_calls,
                               injection.unregister_fail_call,
                               injection.unregister_fail_count)) {
                if (first_error == SAO_AI_EDITOR_OK) {
                    first_error = injection.failure_status;
                }
                continue;
            }
#endif
            const int32_t status = registry.unregister_custom(name);
            if (status != SAO_AI_EDITOR_OK &&
                status != SAO_AI_EDITOR_ERR_NOT_FOUND &&
                first_error == SAO_AI_EDITOR_OK) {
                first_error = status;
            }
        }
        return first_error;
    }

    for (const auto& spec : specs) {
#if defined(SAO_AI_EDITOR_TESTING)
        auto& injection = failure_injection();
        ++injection.register_calls;
        if (failure_active(injection.register_calls,
                           injection.register_fail_call,
                           injection.register_fail_count)) {
            if (first_error == SAO_AI_EDITOR_OK) {
                first_error = injection.failure_status;
            }
            continue;
        }
#endif
        const int32_t status = registry.register_custom(
            spec.name, spec.description, spec.schema, spec.read_only,
            spec.execute, bridge.get());
        if (status != SAO_AI_EDITOR_OK && first_error == SAO_AI_EDITOR_OK) {
            first_error = status;
        }
    }
    return first_error;
}

int32_t unregister_specs(Registry& registry) {
    int32_t first_error = SAO_AI_EDITOR_OK;
    for (const auto name : kToolNames) {
#if defined(SAO_AI_EDITOR_TESTING)
        auto& injection = failure_injection();
        ++injection.unregister_calls;
        if (failure_active(injection.unregister_calls,
                           injection.unregister_fail_call,
                           injection.unregister_fail_count)) {
            if (first_error == SAO_AI_EDITOR_OK) {
                first_error = injection.failure_status;
            }
            continue;
        }
#endif
        const int32_t status = registry.unregister_custom(name);
        if (status != SAO_AI_EDITOR_OK &&
            status != SAO_AI_EDITOR_ERR_NOT_FOUND &&
            first_error == SAO_AI_EDITOR_OK) {
            first_error = status;
        }
    }
    return first_error;
}

}  // namespace

int32_t register_kernel_map_tools(
    sao::ai_editor::native::NativeToolRegistry& registry,
    const std::shared_ptr<Bridge>& bridge) {
    if (bridge == nullptr) return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> guard(registration_mutex());
    auto& states = registration_states();
    auto found = states.find(&registry);
    if (found != states.end() && found->second.bridge.get() != bridge.get()) {
        return SAO_AI_EDITOR_ERR_BUSY;
    }
    if (found != states.end() && found->second.rollback_pending) {
        const int32_t recovery_status = restore_specs(
            registry, found->second.rollback_specs, found->second.bridge);
        if (recovery_status != SAO_AI_EDITOR_OK) {
            return recovery_status;
        }
        if (found->second.rollback_specs.empty()) {
            states.erase(found);
            found = states.end();
        } else {
            found->second.specs = found->second.rollback_specs;
            found->second.rollback_specs.clear();
            found->second.rollback_pending = false;
        }
    }
    if (found == states.end()) {
        for (const auto name : kToolNames) {
            if (descriptor_exists(registry, name)) {
                return SAO_AI_EDITOR_ERR_BUSY;
            }
        }
    }

    const std::vector<ToolSpec> specs = make_specs();
    const std::vector<ToolSpec> prior =
        found == states.end() ? std::vector<ToolSpec>{} : found->second.specs;
    const int32_t status = install_specs(registry, specs, bridge);
    if (status != SAO_AI_EDITOR_OK) {
        const std::shared_ptr<Bridge> prior_bridge =
            found == states.end() ? bridge : found->second.bridge;
        const int32_t rollback_status = restore_specs(
            registry, prior, prior_bridge);
        if (rollback_status != SAO_AI_EDITOR_OK) {
            states[&registry] = ToolRegistrationState{
                prior_bridge, prior, prior, true};
            return rollback_status;
        }
        if (found == states.end()) {
            states.erase(&registry);
        }
        return status;
    }
    states[&registry] = ToolRegistrationState{bridge, specs, {}, false};
    return SAO_AI_EDITOR_OK;
}

int32_t unregister_kernel_map_tools(
    sao::ai_editor::native::NativeToolRegistry& registry,
    const std::shared_ptr<Bridge>& bridge) {
    if (bridge == nullptr) return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> guard(registration_mutex());
    auto& states = registration_states();
    const auto found = states.find(&registry);
    if (found == states.end()) return SAO_AI_EDITOR_ERR_NOT_FOUND;
    if (found->second.bridge.get() != bridge.get()) {
        return SAO_AI_EDITOR_ERR_BUSY;
    }
    if (found->second.rollback_pending) {
        const int32_t recovery_status = restore_specs(
            registry, found->second.rollback_specs, found->second.bridge);
        if (recovery_status != SAO_AI_EDITOR_OK) {
            return recovery_status;
        }
        if (found->second.rollback_specs.empty()) {
            states.erase(found);
            return SAO_AI_EDITOR_OK;
        }
        found->second.specs = found->second.rollback_specs;
        found->second.rollback_specs.clear();
        found->second.rollback_pending = false;
    }
    const int32_t first_error = unregister_specs(registry);
    if (first_error == SAO_AI_EDITOR_OK) {
        states.erase(found);
    }
    return first_error;
}

void abandon_kernel_map_tools(
    sao::ai_editor::native::NativeToolRegistry& registry) {
    std::lock_guard<std::mutex> guard(registration_mutex());
    registration_states().erase(&registry);
}


#if defined(SAO_AI_EDITOR_TESTING)
void set_kernel_map_tool_failure_injection(
    int32_t register_fail_call, int32_t register_fail_count,
    int32_t unregister_fail_call, int32_t unregister_fail_count,
    int32_t failure_status) {
    std::lock_guard<std::mutex> guard(registration_mutex());
    auto& injection = failure_injection();
    injection = FailureInjectionState{register_fail_call, register_fail_count,
                                      unregister_fail_call,
                                      unregister_fail_count, failure_status,
                                      0, 0};
}

void clear_kernel_map_tool_failure_injection() {
    std::lock_guard<std::mutex> guard(registration_mutex());
    failure_injection() = FailureInjectionState{};
}
#endif

}  // namespace sao::ai_editor::kernel_map
