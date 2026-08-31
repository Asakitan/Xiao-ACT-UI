#include "vt_tools.h"

#include "native_tool_registry.h"
#include "vt_bridge.h"

#include "sao/ai_editor/ai_editor_status.h"

#include <array>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace sao::ai_editor::vt {
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
    bool explicit_confirmation_required = false;
};

Json empty_schema() {
    return Json{{"type", "object"},
                {"properties", Json::object()},
                {"additionalProperties", false}};
}

Json address_schema() {
    return Json{{"type", "string"},
                {"minLength", 3},
                {"maxLength", 18},
                {"pattern", "^0x[0-9A-Fa-f]{1,16}$"},
                {"description", "0x-prefixed hexadecimal address string."}};
}

Json confirmation_schema() {
    return Json{{"type", "boolean"}, {"enum", Json::array({true})}};
}

int32_t execute_tool(const Json& arguments, Json& result, void* user,
                     std::string_view name) {
    if (user == nullptr) return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    return static_cast<Bridge*>(user)->execute(name, arguments, result);
}

int32_t execute_status(const Json& arguments, Json& result, void* user) {
    return execute_tool(arguments, result, user, "vt.status");
}

int32_t execute_capabilities(const Json& arguments, Json& result, void* user) {
    return execute_tool(arguments, result, user, "vt.capabilities");
}

int32_t execute_probe(const Json& arguments, Json& result, void* user) {
    return execute_tool(arguments, result, user, "vt.probe");
}

int32_t execute_hook_page(const Json& arguments, Json& result, void* user) {
    return execute_tool(arguments, result, user, "vt.hookPage");
}

int32_t execute_hide_region(const Json& arguments, Json& result, void* user) {
    return execute_tool(arguments, result, user, "vt.hideRegion");
}

int32_t execute_unhook(const Json& arguments, Json& result, void* user) {
    return execute_tool(arguments, result, user, "vt.unhook");
}

int32_t execute_read_phys(const Json& arguments, Json& result, void* user) {
    return execute_tool(arguments, result, user, "vt.readPhys");
}

int32_t execute_write_phys(const Json& arguments, Json& result, void* user) {
    return execute_tool(arguments, result, user, "vt.writePhys");
}

constexpr std::array<std::string_view, 8> kToolNames{
    "vt.status", "vt.capabilities", "vt.probe", "vt.hookPage",
    "vt.hideRegion", "vt.unhook", "vt.readPhys", "vt.writePhys"};

std::vector<ToolSpec> make_specs() {
    std::vector<ToolSpec> specs;
    specs.push_back({"vt.status",
                     "Report the aggregate VT status snapshot and availability.",
                     empty_schema(), true, &execute_status, false});
    specs.push_back({"vt.capabilities",
                     "Report derived supported and active VT capability names.",
                     empty_schema(), true, &execute_capabilities, false});

    Json probe_properties = Json::object();
    probe_properties["items"] = Json{
        {"type", "string"},
        {"enum", Json::array({"0x0", "0x1", "0x2", "0x3", "0x4", "0x5",
                               "0x6", "0x7", "0x8", "0x9", "0xa", "0xb",
                               "0xc", "0xd", "0xe", "0xf"})},
        {"description", "Optional 0x0..0xf probe item mask; absent or 0x0 means all."}};
    specs.push_back({"vt.probe",
                     "Probe VT status, capabilities, performance, and hooks.",
                     Json{{"type", "object"},
                          {"properties", std::move(probe_properties)},
                          {"additionalProperties", false}},
                     true, &execute_probe, false});

    Json hook_properties = Json::object();
    hook_properties["gva"] = address_schema();
    hook_properties["patchBytes"] = Json{
        {"type", "string"},
        {"minLength", 2},
        {"maxLength", 66},
        {"pattern", "^(?:0x)?(?:[0-9A-Fa-f]{2}){1,32}$"},
        {"description", "1..32 patch bytes as contiguous hexadecimal digits, optionally 0x-prefixed."}};
    hook_properties["confirmed"] = confirmation_schema();
    specs.push_back({"vt.hookPage",
                     "Install a VT page hook at a 0x-prefixed hexadecimal GVA; explicit confirmed:true is required.",
                     Json{{"type", "object"},
                          {"properties", std::move(hook_properties)},
                          {"required", Json::array({"gva", "patchBytes", "confirmed"})},
                          {"additionalProperties", false}},
                     false, &execute_hook_page, true});

    Json hide_properties = Json::object();
    hide_properties["gva"] = address_schema();
    hide_properties["pageCount"] = Json{{"type", "integer"}, {"minimum", 1}, {"maximum", 64}};
    hide_properties["decoyMode"] = Json{
        {"type", "string"},
        {"enum", Json::array({"zero", "template", "ucs"})}};
    hide_properties["templateBytes"] = Json{
        {"type", "string"},
        {"minLength", 2},
        {"maxLength", 8194},
        {"pattern", "^(?:0x)?(?:[0-9A-Fa-f]{2}){1,4096}$"},
        {"description", "Template bytes as contiguous hexadecimal digits, optionally 0x-prefixed."}};
    hide_properties["confirmed"] = confirmation_schema();
    specs.push_back({"vt.hideRegion",
                     "Hide a 0x-prefixed hexadecimal GVA region using zero, template, or ucs decoy mode; explicit confirmed:true is required.",
                     Json{{"type", "object"},
                          {"properties", std::move(hide_properties)},
                          {"required", Json::array({"gva", "pageCount", "decoyMode", "confirmed"})},
                          {"additionalProperties", false}},
                     false, &execute_hide_region, true});

    Json unhook_properties = Json::object();
    unhook_properties["hookId"] = address_schema();
    unhook_properties["confirmed"] = confirmation_schema();
    specs.push_back({"vt.unhook",
                     "Remove a VT hook identified by a 0x-prefixed hexadecimal hookId; explicit confirmed:true is required.",
                     Json{{"type", "object"},
                          {"properties", std::move(unhook_properties)},
                          {"required", Json::array({"hookId", "confirmed"})},
                          {"additionalProperties", false}},
                     false, &execute_unhook, true});

    Json read_properties = Json::object();
    read_properties["gpa"] = address_schema();
    read_properties["length"] = Json{{"type", "integer"}, {"minimum", 1}, {"maximum", 1048576}};
    specs.push_back({"vt.readPhys",
                     "Read physical memory from a 0x-prefixed hexadecimal GPA for 1..1048576 bytes.",
                     Json{{"type", "object"},
                          {"properties", std::move(read_properties)},
                          {"required", Json::array({"gpa", "length"})},
                          {"additionalProperties", false}},
                     true, &execute_read_phys, false});

    Json write_properties = Json::object();
    write_properties["gpa"] = address_schema();
    write_properties["length"] = Json{{"type", "integer"}, {"minimum", 1}, {"maximum", 1048576}};
    write_properties["dataHex"] = Json{
        {"type", "string"},
        {"minLength", 2},
        {"maxLength", 2097154},
        {"pattern", "^(?:0x)?(?:[0-9A-Fa-f]{2}){1,1048576}$"},
        {"description", "Exactly length bytes as contiguous hexadecimal digits, optionally 0x-prefixed."}};
    write_properties["confirmed"] = confirmation_schema();
    specs.push_back({"vt.writePhys",
                     "Write physical memory at a 0x-prefixed hexadecimal GPA; explicit confirmed:true is required.",
                     Json{{"type", "object"},
                          {"properties", std::move(write_properties)},
                          {"required", Json::array({"gpa", "length", "dataHex", "confirmed"})},
                          {"additionalProperties", false}},
                     false, &execute_write_phys, true});
    return specs;
}

int32_t install_specs(Registry& registry, const std::vector<ToolSpec>& specs,
                      const std::shared_ptr<Bridge>& bridge) {
    if (bridge == nullptr) return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    std::vector<Registry::CustomToolDescriptor> descriptors;
    descriptors.reserve(specs.size());
    for (const auto& spec : specs) {
        descriptors.push_back(Registry::CustomToolDescriptor{
            spec.name, spec.description, spec.schema, spec.read_only,
            spec.execute, bridge.get(), std::static_pointer_cast<void>(bridge),
            spec.explicit_confirmation_required});
    }
    return registry.upsert_custom_batch(descriptors, bridge.get());
}

int32_t unregister_specs(Registry& registry,
                         const std::shared_ptr<Bridge>& bridge) {
    if (bridge == nullptr) return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    std::vector<std::string> names;
    names.reserve(kToolNames.size());
    for (const auto name : kToolNames) {
        names.emplace_back(name);
    }
    return registry.remove_custom_batch(names, bridge.get());
}

}  // namespace

int32_t register_vt_tools(Registry& registry,
                          const std::shared_ptr<Bridge>& bridge) noexcept {
    if (bridge == nullptr) return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    try {
        return install_specs(registry, make_specs(), bridge);
    } catch (const std::bad_alloc&) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    } catch (...) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

int32_t unregister_vt_tools(Registry& registry,
                            const std::shared_ptr<Bridge>& bridge) noexcept {
    if (bridge == nullptr) return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    try {
        return unregister_specs(registry, bridge);
    } catch (const std::bad_alloc&) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    } catch (...) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

void abandon_vt_tools(Registry& registry) noexcept {
    (void)registry;
}

}  // namespace sao::ai_editor::vt
