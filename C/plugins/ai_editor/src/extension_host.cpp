#include "extension_host.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <limits>
#include <new>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include "native_runtime_internal.h"
#include "plugin_contributions.h"

namespace sao::ai_editor::native {
namespace extapi {
void retire_language_owner(std::string_view extension_id, uint64_t generation);
void retire_extension_owner(std::string_view extension_id, uint64_t generation);
}
namespace {

constexpr std::size_t kMaximumTreeChildren = 500;
constexpr std::size_t kMaximumTreeDepth = 64;
constexpr std::size_t kMaximumTreeLabelBytes = 64U * 1024U;
constexpr std::size_t kMaximumTreePayloadBytes = 512U * 1024U;
constexpr std::size_t kMaximumTreeNodes = 16384;
constexpr std::size_t kMaximumTreeActionsPerItem = 64;
constexpr std::size_t kMaximumTreeProviders = 256;
constexpr std::size_t kMaximumRuntimeRegistrations = 4096;
constexpr std::size_t kMaximumExtensionInventoryBytes = 4U * 1024U * 1024U - 64U * 1024U;
constexpr std::size_t kMaximumWebviewMessageBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaximumWebviewMessageStringBytes = 1U * 1024U * 1024U;
constexpr uint64_t kMaximumSafeJsonInteger = 9007199254740991ULL;

class CallbackGuard final {
  public:
    explicit CallbackGuard(std::atomic<bool>& active) noexcept : active_(active) {
        bool expected = false;
        acquired_ = active_.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                    std::memory_order_acquire);
    }

    ~CallbackGuard() {
        if (acquired_) {
            active_.store(false, std::memory_order_release);
        }
    }

    CallbackGuard(const CallbackGuard&) = delete;
    CallbackGuard& operator=(const CallbackGuard&) = delete;

    bool owns_lock() const noexcept {
        return acquired_;
    }

  private:
    std::atomic<bool>& active_;
    bool acquired_ = false;
};

struct TreeBudget {
    std::size_t labels = 0;
    std::size_t nodes = 0;
    std::size_t payload = 0;
    std::unordered_set<std::string> handles;
    std::optional<std::array<uint64_t, 3>> handle_identity;
};

bool safe_nonnegative_integer(const Json& value, uint64_t& result) {
    if (value.is_number_unsigned()) {
        result = value.get<uint64_t>();
    } else if (value.is_number_integer()) {
        const int64_t signed_value = value.get<int64_t>();
        if (signed_value < 0) {
            return false;
        }
        result = static_cast<uint64_t>(signed_value);
    } else {
        return false;
    }
    return result <= kMaximumSafeJsonInteger;
}

bool exact_generation_member(const Json& value, std::string_view key, uint64_t expected) {
    if (!value.is_object()) {
        return false;
    }
    const auto member = value.find(std::string(key));
    uint64_t actual = 0;
    return member != value.end() && safe_nonnegative_integer(*member, actual) && actual == expected;
}

bool true_boolean_member(const Json& value, std::string_view key) {
    if (!value.is_object()) {
        return false;
    }
    const auto member = value.find(std::string(key));
    return member != value.end() && member->is_boolean() && member->get<bool>();
}

bool exact_string_member(const Json& value, std::string_view key, std::string_view expected) {
    if (!value.is_object()) {
        return false;
    }
    const auto member = value.find(std::string(key));
    return member != value.end() && member->is_string() &&
           member->get_ref<const std::string&>() == expected;
}

std::string node_error_message(const Json& value, std::string_view fallback) {
    if (value.is_object()) {
        const auto message = value.find("message");
        if (message != value.end() && message->is_string() &&
            !message->get_ref<const std::string&>().empty() &&
            message->get_ref<const std::string&>().size() <= 4096 &&
            valid_utf8(message->get_ref<const std::string&>())) {
            return message->get<std::string>();
        }
    }
    return std::string(fallback);
}

bool valid_cleanup_report(const Json& value, bool cleanup_complete) {
    if (!value.is_object()) {
        return false;
    }
    const auto errors = value.find("cleanupErrors");
    if (errors == value.end() || !errors->is_array() || errors->size() > 16 ||
        cleanup_complete != errors->empty()) {
        return false;
    }
    return std::all_of(errors->begin(), errors->end(), [](const Json& error) {
        return error.is_string() && error.get_ref<const std::string&>().size() <= 4096 &&
               valid_utf8(error.get_ref<const std::string&>());
    });
}

bool decode_tree_handle(std::string_view value, std::array<uint64_t, 4>& parts) {
    if (value.empty() || value.size() > 128 || value.find('\0') != std::string_view::npos) {
        return false;
    }
    parts = {};
    std::size_t offset = 0;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        const std::size_t separator = value.find(':', offset);
        const bool last = index + 1 == parts.size();
        if ((last && separator != std::string_view::npos) ||
            (!last && separator == std::string_view::npos)) {
            return false;
        }
        const std::size_t end = last ? value.size() : separator;
        if (end == offset) {
            return false;
        }
        const char* first = value.data() + offset;
        const char* final = value.data() + end;
        const auto parsed = std::from_chars(first, final, parts[index]);
        if (parsed.ec != std::errc{} || parsed.ptr != final || parts[index] == 0 ||
            parts[index] > kMaximumSafeJsonInteger) {
            return false;
        }
        offset = end + 1;
    }
    return true;
}

bool consume_tree_text(const Json& value, TreeBudget& budget) {
    if (!value.is_string()) {
        return false;
    }
    const auto& text = value.get_ref<const std::string&>();
    if (!valid_utf8(text) ||
        std::any_of(text.begin(), text.end(),
                    [](unsigned char byte) { return byte < 0x20 || byte == 0x7f; }) ||
        text.size() > kMaximumTreeLabelBytes - budget.labels) {
        return false;
    }
    budget.labels += text.size();
    return true;
}

bool serialized_json_within(const Json& value, std::size_t maximum_bytes) {
    try {
        return value.dump().size() <= maximum_bytes;
    } catch (...) {
        return false;
    }
}

bool webview_json_within_budget(const Json& value, std::size_t depth, std::size_t& nodes) {
    if (depth > kMaximumTreeDepth || nodes >= kMaximumTreeNodes) {
        return false;
    }
    ++nodes;
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        return text.size() <= kMaximumWebviewMessageStringBytes && valid_utf8(text);
    }
    if (value.is_array()) {
        for (const auto& item : value) {
            if (!webview_json_within_budget(item, depth + 1, nodes)) {
                return false;
            }
        }
    } else if (value.is_object()) {
        for (const auto& [key, item] : value.items()) {
            if (key.size() > 1024 || !valid_utf8(key) ||
                !webview_json_within_budget(item, depth + 1, nodes)) {
                return false;
            }
        }
    } else if (value.is_number_float() && !std::isfinite(value.get<double>())) {
        return false;
    }
    return true;
}

bool valid_webview_message_payload(const Json& value) {
    std::size_t nodes = 0;
    return webview_json_within_budget(value, 0, nodes) &&
           serialized_json_within(value, kMaximumWebviewMessageBytes);
}

bool consume_optional_tree_text(const Json& object, std::string_view key, TreeBudget& budget) {
    const auto value = object.find(std::string(key));
    return value == object.end() || consume_tree_text(*value, budget);
}

bool validate_tree_items(const Json& items, std::size_t depth, uint64_t generation,
                         uint64_t version, TreeBudget& budget) {
    if (!items.is_array() || items.size() > kMaximumTreeChildren || depth >= kMaximumTreeDepth) {
        return false;
    }
    for (const auto& item : items) {
        if (!item.is_object() || ++budget.nodes > kMaximumTreeNodes) {
            return false;
        }
        const auto handle = item.find("handle");
        const auto label = item.find("label");
        std::array<uint64_t, 4> handle_parts{};
        if (handle == item.end() || !handle->is_string() ||
            !decode_tree_handle(handle->get_ref<const std::string&>(), handle_parts) ||
            handle_parts[0] != generation || handle_parts[2] != version ||
            !budget.handles.insert(handle->get<std::string>()).second || label == item.end() ||
            !consume_tree_text(*label, budget)) {
            return false;
        }
        const std::array<uint64_t, 3> identity{handle_parts[0], handle_parts[1], handle_parts[2]};
        if (budget.handle_identity.has_value() && *budget.handle_identity != identity) {
            return false;
        }
        budget.handle_identity = identity;
        for (const char* key : {"description", "tooltip", "contextValue", "resourceUri"}) {
            if (!consume_optional_tree_text(item, key, budget)) {
                return false;
            }
        }
        const auto collapsible_state = item.find("collapsibleState");
        const auto children_loaded = item.find("childrenLoaded");
        const auto lazy_children = item.find("lazyChildren");
        uint64_t collapsible = 0;
        if (collapsible_state == item.end() ||
            !safe_nonnegative_integer(*collapsible_state, collapsible) || collapsible > 2 ||
            children_loaded == item.end() || !children_loaded->is_boolean() ||
            lazy_children == item.end() || !lazy_children->is_boolean()) {
            return false;
        }
        const auto command = item.find("command");
        if (command != item.end() && !command->is_null()) {
            if (!command->is_object() || !consume_optional_tree_text(*command, "command", budget) ||
                !consume_optional_tree_text(*command, "title", budget) ||
                !consume_optional_tree_text(*command, "tooltip", budget)) {
                return false;
            }
            const auto command_id = command->find("command");
            const auto command_title = command->find("title");
            if (command_id == command->end() || !command_id->is_string() ||
                command_id->get_ref<const std::string&>().empty() ||
                command_title == command->end() || !command_title->is_string()) {
                return false;
            }
            const auto arguments = command->find("arguments");
            if (arguments != command->end() &&
                (!arguments->is_array() || arguments->size() > kMaximumTreeActionsPerItem)) {
                return false;
            }
        }
        const auto checkbox = item.find("checkbox");
        if (checkbox != item.end() && !checkbox->is_null() &&
            (!checkbox->is_object() || !consume_optional_tree_text(*checkbox, "tooltip", budget) ||
             !checkbox->contains("isChecked") || !(*checkbox)["isChecked"].is_boolean())) {
            return false;
        }
        const auto accessibility = item.find("accessibilityInformation");
        if (accessibility != item.end() && !accessibility->is_null() &&
            (!accessibility->is_object() ||
             !consume_optional_tree_text(*accessibility, "label", budget) ||
             !consume_optional_tree_text(*accessibility, "role", budget))) {
            return false;
        }
        const auto actions = item.find("actions");
        if (actions != item.end()) {
            if (!actions->is_array() || actions->size() > kMaximumTreeActionsPerItem) {
                return false;
            }
            for (const auto& action : *actions) {
                if (!action.is_object()) {
                    return false;
                }
                for (const char* key :
                     {"command", "submenu", "group", "title", "tooltip", "disabledReason"}) {
                    if (!consume_optional_tree_text(action, key, budget)) {
                        return false;
                    }
                }
                const std::string command_id = action.value("command", std::string{});
                const std::string submenu_id = action.value("submenu", std::string{});
                if (command_id.empty() == submenu_id.empty()) {
                    return false;
                }
                for (const char* key : {"runtimeAvailable", "enabled", "disabled", "inline"}) {
                    const auto value = action.find(key);
                    if (value != action.end() && !value->is_boolean()) {
                        return false;
                    }
                }
                const auto arguments = action.find("arguments");
                if (arguments != action.end() &&
                    (!arguments->is_array() || arguments->size() > kMaximumTreeActionsPerItem)) {
                    return false;
                }
            }
        }
        const auto children = item.find("children");
        if (children == item.end() || !children->is_array() ||
            (!children->empty() &&
             !validate_tree_items(*children, depth + 1, generation, version, budget))) {
            return false;
        }
    }
    return true;
}

bool validate_tree_payload(const Json& items, uint64_t generation, uint64_t version,
                           TreeBudget& budget) {
    if (!validate_tree_items(items, 0, generation, version, budget)) {
        return false;
    }
    try {
        const std::size_t bytes = items.dump().size();
        if (bytes > kMaximumTreePayloadBytes - budget.payload) {
            return false;
        }
        budget.payload += bytes;
        return true;
    } catch (...) {
        return false;
    }
}

bool validate_tree_response(const Json& result, std::string_view view_id, TreeBudget& budget,
                            std::string& owner, uint64_t& generation, uint64_t& version) {
    owner.clear();
    generation = 0;
    version = 0;
    if (!serialized_json_within(result, kMaximumTreePayloadBytes) || !result.is_object() ||
        !result.contains("ok") || !result["ok"].is_boolean() || !result.contains("available") ||
        !result["available"].is_boolean() || !result.contains("applied") ||
        !result["applied"].is_boolean() || !result.contains("items") ||
        !result.contains("errorCode") || !result["errorCode"].is_string() ||
        !result.contains("error") || !result["error"].is_string() ||
        !result.contains("viewVersion") ||
        !safe_nonnegative_integer(result["viewVersion"], version)) {
        return false;
    }
    const auto returned_view = result.find("viewId");
    if (returned_view == result.end() || !returned_view->is_string() ||
        returned_view->get_ref<const std::string&>() != view_id) {
        return false;
    }
    const bool ok = result["ok"].get<bool>();
    const bool available = result["available"].get<bool>();
    const bool applied = result["applied"].get<bool>();
    const auto& error_code = result["errorCode"].get_ref<const std::string&>();
    const auto& error = result["error"].get_ref<const std::string&>();
    if (error_code.size() > 128 || error.size() > 4096 || !valid_utf8(error_code) ||
        !valid_utf8(error) ||
        std::any_of(error_code.begin(), error_code.end(),
                    [](unsigned char byte) { return byte < 0x20 || byte == 0x7f; }) ||
        std::any_of(error.begin(), error.end(),
                    [](unsigned char byte) { return byte < 0x20 || byte == 0x7f; }) ||
        (applied && (!ok || !available || !error_code.empty() || !error.empty())) ||
        (!applied && !result["items"].empty()) || (!ok && (error_code.empty() || error.empty())) ||
        (ok && !applied)) {
        return false;
    }
    const auto owner_value = result.find("extensionId");
    const auto generation_value = result.find("generation");
    const bool has_owner = owner_value != result.end();
    const bool has_generation = generation_value != result.end();
    if (has_owner != has_generation || (applied && (!has_owner || !has_generation))) {
        return false;
    }
    if (has_owner) {
        if (!owner_value->is_string() ||
            !valid_simple_id(owner_value->get_ref<const std::string&>()) ||
            !safe_nonnegative_integer(*generation_value, generation) || generation == 0) {
            return false;
        }
        owner = owner_value->get<std::string>();
    }
    if (applied && version == 0) {
        return false;
    }
    for (const char* key : {"title", "description", "message"}) {
        if (!consume_optional_tree_text(result, key, budget)) {
            return false;
        }
    }
    const auto badge = result.find("badge");
    if (badge != result.end() && !badge->is_null()) {
        uint64_t badge_value = 0;
        if (!badge->is_object() || !consume_optional_tree_text(*badge, "tooltip", budget) ||
            !badge->contains("value") ||
            !safe_nonnegative_integer((*badge)["value"], badge_value)) {
            return false;
        }
    }
    const auto visible = result.find("visible");
    if (visible != result.end() && !visible->is_boolean()) {
        return false;
    }
    const auto drag_and_drop = result.find("dragAndDrop");
    if (drag_and_drop != result.end()) {
        if (!drag_and_drop->is_object()) {
            return false;
        }
        for (const char* key : {"canDrag", "canDrop"}) {
            const auto value = drag_and_drop->find(key);
            if (value == drag_and_drop->end() || !value->is_boolean()) {
                return false;
            }
        }
        for (const char* key : {"dragMimeTypes", "dropMimeTypes"}) {
            const auto values = drag_and_drop->find(key);
            if (values == drag_and_drop->end()) {
                continue;
            }
            if (!values->is_array() || values->size() > 64) {
                return false;
            }
            for (const auto& value : *values) {
                if (!value.is_string() || value.get_ref<const std::string&>().size() > 256 ||
                    !valid_utf8(value.get_ref<const std::string&>()) ||
                    std::any_of(value.get_ref<const std::string&>().begin(),
                                value.get_ref<const std::string&>().end(),
                                [](unsigned char byte) { return byte < 0x20 || byte == 0x7f; })) {
                    return false;
                }
            }
        }
    }
    return validate_tree_payload(result["items"], generation, version, budget);
}

bool validate_tree_inventory(const Json& result) {
    uint64_t top_version = 0;
    if (!serialized_json_within(result, kMaximumTreePayloadBytes) || !result.is_object() ||
        !result.contains("ok") || !result["ok"].is_boolean() || !result.contains("available") ||
        !result["available"].is_boolean() || !result.contains("applied") ||
        !result["applied"].is_boolean() || !result.contains("items") ||
        !result["items"].is_array() || result["items"].size() > kMaximumTreeProviders ||
        !result.contains("errorCode") || !result["errorCode"].is_string() ||
        !result.contains("error") || !result["error"].is_string() ||
        !result.contains("viewVersion") ||
        !safe_nonnegative_integer(result["viewVersion"], top_version)) {
        return false;
    }
    const bool available = result["available"].get<bool>();
    const bool applied = result["applied"].get<bool>();
    const bool ok = result["ok"].get<bool>();
    const auto& error_code = result["errorCode"].get_ref<const std::string&>();
    const auto& error = result["error"].get_ref<const std::string&>();
    if (available != !result["items"].empty() || (applied && (!available || !ok)) ||
        error_code.size() > 128 || error.size() > 4096 || !valid_utf8(error_code) ||
        !valid_utf8(error) || (!ok && (error_code.empty() || error.empty()))) {
        return false;
    }
    TreeBudget budget;
    std::unordered_set<std::string> view_ids;
    bool all_ok = true;
    bool all_applied = !result["items"].empty();
    uint64_t maximum_version = 0;
    for (const auto& row : result["items"]) {
        const auto view = row.find("viewId");
        if (view == row.end() || !view->is_string() ||
            view->get_ref<const std::string&>().empty() ||
            !view_ids.insert(view->get<std::string>()).second) {
            return false;
        }
        std::string owner;
        uint64_t generation = 0;
        uint64_t version = 0;
        budget.handles.clear();
        budget.handle_identity.reset();
        if (!validate_tree_response(row, view->get_ref<const std::string&>(), budget, owner,
                                    generation, version)) {
            return false;
        }
        all_ok = all_ok && row["ok"].get<bool>();
        all_applied = all_applied && row["applied"].get<bool>();
        maximum_version = std::max(maximum_version, version);
    }
    return ok == all_ok && applied == all_applied && top_version == maximum_version;
}

bool tree_callback_unresolved(const Json& result, bool inventory = false) {
    if (!result.is_object()) {
        return false;
    }
    const auto code = result.find("errorCode");
    if (code != result.end() && code->is_string() &&
        (code->get_ref<const std::string&>() == "TREE_CALLBACK_TIMEOUT" ||
         code->get_ref<const std::string&>() == "TREE_CALLBACK_QUARANTINED")) {
        return true;
    }
    const auto items = result.find("items");
    if (inventory && items != result.end() && items->is_array()) {
        return std::any_of(items->begin(), items->end(), [](const Json& item) {
            if (!item.is_object()) {
                return false;
            }
            const auto item_code = item.find("errorCode");
            return item_code != item.end() && item_code->is_string() &&
                   (item_code->get_ref<const std::string&>() == "TREE_CALLBACK_TIMEOUT" ||
                    item_code->get_ref<const std::string&>() == "TREE_CALLBACK_QUARANTINED");
        });
    }
    return false;
}

bool tree_json_within_request_budget(const Json& value, std::size_t depth, std::size_t& nodes) {
    if (depth > kMaximumTreeDepth || nodes >= kMaximumTreeNodes) {
        return false;
    }
    ++nodes;
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        return text.size() <= kMaximumTreeLabelBytes && valid_utf8(text) &&
               std::none_of(text.begin(), text.end(),
                            [](unsigned char byte) { return byte < 0x20 || byte == 0x7f; });
    }
    if (value.is_array()) {
        for (const auto& item : value) {
            if (!tree_json_within_request_budget(item, depth + 1, nodes)) {
                return false;
            }
        }
    } else if (value.is_object()) {
        for (const auto& [key, item] : value.items()) {
            if (key.size() > 256 || !valid_utf8(key) ||
                !tree_json_within_request_budget(item, depth + 1, nodes)) {
                return false;
            }
        }
    } else if (value.is_number_float() && !std::isfinite(value.get<double>())) {
        return false;
    }
    return true;
}

bool valid_tree_request_payload(const Json& value) {
    std::size_t nodes = 0;
    try {
        return tree_json_within_request_budget(value, 0, nodes) &&
               value.dump().size() <= kMaximumTreePayloadBytes;
    } catch (...) {
        return false;
    }
}

bool valid_runtime_command_inventory(const Json& result) {
    const auto ok = result.is_object() ? result.find("ok") : result.end();
    const auto available = result.is_object() ? result.find("available") : result.end();
    const auto applied = result.is_object() ? result.find("applied") : result.end();
    if (!result.is_object() || ok == result.end() || !ok->is_boolean() || !ok->get<bool>() ||
        available == result.end() || !available->is_boolean() || !available->get<bool>() ||
        applied == result.end() || !applied->is_boolean() || applied->get<bool>() ||
        !result.contains("items") || !result["items"].is_array() ||
        result["items"].size() > kMaximumRuntimeRegistrations) {
        return false;
    }
    std::unordered_set<std::string> commands;
    for (const auto& item : result["items"]) {
        if (!item.is_object()) {
            return false;
        }
        const auto command = item.find("command");
        const auto owner = item.find("extensionId");
        const auto generation_value = item.find("generation");
        const auto editor_required = item.find("editorRequired");
        uint64_t generation = 0;
        if (command == item.end() || !command->is_string() ||
            command->get_ref<const std::string&>().empty() ||
            command->get_ref<const std::string&>().size() > 256 ||
            command->get_ref<const std::string&>().find('\0') != std::string::npos ||
            !valid_utf8(command->get_ref<const std::string&>()) || owner == item.end() ||
            !owner->is_string() || !valid_simple_id(owner->get_ref<const std::string&>()) ||
            generation_value == item.end() ||
            !safe_nonnegative_integer(*generation_value, generation) || generation == 0 ||
            editor_required == item.end() || !editor_required->is_boolean() ||
            !commands.insert(command->get<std::string>()).second) {
            return false;
        }
    }
    return true;
}

bool valid_runtime_webview_inventory(const Json& result) {
    if (!result.is_object() || !result.contains("items") || !result["items"].is_array() ||
        result["items"].size() > kMaximumRuntimeRegistrations) {
        return false;
    }
    std::unordered_set<std::string> view_ids;
    for (const auto& item : result["items"]) {
        if (!item.is_object()) {
            return false;
        }
        const auto view = item.find("viewId");
        const auto owner = item.find("extensionId");
        const auto generation_value = item.find("generation");
        const auto available = item.find("available");
        const auto html = item.find("html");
        const auto html_available = item.find("htmlAvailable");
        const auto html_length_value = item.find("htmlLength");
        const auto options = item.find("options");
        const auto error_code = item.find("errorCode");
        const auto error = item.find("error");
        uint64_t generation = 0;
        uint64_t html_length = 0;
        if (view == item.end() || !view->is_string() ||
            view->get_ref<const std::string&>().empty() ||
            view->get_ref<const std::string&>().size() > 256 ||
            view->get_ref<const std::string&>().find('\0') != std::string::npos ||
            !valid_utf8(view->get_ref<const std::string&>()) ||
            !view_ids.insert(view->get<std::string>()).second || owner == item.end() ||
            !owner->is_string() || !valid_simple_id(owner->get_ref<const std::string&>()) ||
            generation_value == item.end() ||
            !safe_nonnegative_integer(*generation_value, generation) || generation == 0 ||
            available == item.end() || !available->is_boolean() || !available->get<bool>() ||
            html == item.end() || !html->is_string() ||
            html->get_ref<const std::string&>().size() > kMaximumExtensionMainBytes ||
            !valid_utf8(html->get_ref<const std::string&>()) || html_available == item.end() ||
            !html_available->is_boolean() || html_length_value == item.end() ||
            !safe_nonnegative_integer(*html_length_value, html_length) ||
            html_length != html->get_ref<const std::string&>().size() ||
            html_available->get<bool>() != (html_length != 0) || options == item.end() ||
            !options->is_object() || !valid_webview_message_payload(*options) ||
            !serialized_json_within(*options, kMaximumExtensionMainBytes) ||
            error_code == item.end() || !error_code->is_string() ||
            error_code->get_ref<const std::string&>().size() > 128 ||
            !valid_utf8(error_code->get_ref<const std::string&>()) || error == item.end() ||
            !error->is_string() || error->get_ref<const std::string&>().size() > 4096 ||
            !valid_utf8(error->get_ref<const std::string&>()) ||
            error_code->get_ref<const std::string&>().empty() !=
                error->get_ref<const std::string&>().empty() ||
            (!error_code->get_ref<const std::string&>().empty() && html_available->get<bool>())) {
            return false;
        }
    }
    return true;
}

std::filesystem::path module_directory() {
    HMODULE module_handle = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&module_directory), &module_handle) ||
        module_handle == nullptr) {
        return {};
    }
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD written =
            GetModuleFileNameW(module_handle, buffer.data(), static_cast<DWORD>(buffer.size()));
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

} // namespace
} // namespace sao::ai_editor::native

namespace sao::ai_editor::native {

namespace {

const char* operation_name(ExtensionOperation operation) noexcept {
    switch (operation) {
    case ExtensionOperation::activating:
        return "activating";
    case ExtensionOperation::quarantined:
        return "quarantined";
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
    return std::any_of(extensions.begin(), extensions.end(), [](const auto& entry) {
        return entry.second.operation != ExtensionOperation::idle &&
               entry.second.operation != ExtensionOperation::quarantined;
    });
}

bool activation_outcome_unknown(int32_t status) noexcept {
    return status == SAO_AI_EDITOR_ERR_TIMEOUT || status == SAO_AI_EDITOR_ERR_NOT_INITIALIZED ||
           status == SAO_AI_EDITOR_ERR_IPC_CLOSED || status == SAO_AI_EDITOR_ERR_PROTOCOL;
}

bool allocate_extension_generation(uint64_t current_generation, uint64_t& next_generation,
                                   uint64_t& generation) noexcept {
    if (current_generation >= kMaximumSafeJsonInteger ||
        next_generation > kMaximumSafeJsonInteger) {
        return false;
    }
    generation = std::max(next_generation, current_generation + 1);
    if (generation > kMaximumSafeJsonInteger) {
        return false;
    }
    next_generation = generation + 1;
    return true;
}

bool advance_extension_generation(ExtensionRecord& record, uint64_t& next_generation,
                                  uint64_t& generation) noexcept {
    if (!allocate_extension_generation(record.generation, next_generation, generation)) {
        return false;
    }
    record.generation = generation;
    record.operation_generation = generation;
    return true;
}

bool valid_extension_segment(std::string_view value) {
    return value.find('.') == std::string_view::npos && valid_simple_id(value);
}

bool path_component_equal(const std::filesystem::path& left, const std::filesystem::path& right) {
    const std::wstring& left_value = left.native();
    const std::wstring& right_value = right.native();
    if (left_value.size() > static_cast<std::size_t>(INT_MAX) ||
        right_value.size() > static_cast<std::size_t>(INT_MAX)) {
        return false;
    }
    return CompareStringOrdinal(left_value.data(), static_cast<int>(left_value.size()),
                                right_value.data(), static_cast<int>(right_value.size()),
                                TRUE) == CSTR_EQUAL;
}

std::filesystem::path normalize_handle_path(std::wstring value) {
    constexpr std::wstring_view kUncPrefix = L"\\\\?\\UNC\\";
    constexpr std::wstring_view kDosPrefix = L"\\\\?\\";
    if (value.starts_with(kUncPrefix)) {
        value = L"\\\\" + value.substr(kUncPrefix.size());
    } else if (value.starts_with(kDosPrefix)) {
        value.erase(0, kDosPrefix.size());
    }
    return std::filesystem::path(std::move(value)).lexically_normal();
}

int32_t verify_handle_path(HANDLE handle, const std::filesystem::path& expected) {
    constexpr DWORD kFlags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD required = GetFinalPathNameByHandleW(handle, nullptr, 0, kFlags);
    if (required == 0) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    std::vector<wchar_t> buffer(required);
    const DWORD written = GetFinalPathNameByHandleW(handle, buffer.data(), required, kFlags);
    if (written == 0 || written >= required) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    const std::filesystem::path actual =
        normalize_handle_path(std::wstring(buffer.data(), written));
    const std::filesystem::path normalized_expected = normalize_handle_path(expected.native());
    auto actual_component = actual.begin();
    auto expected_component = normalized_expected.begin();
    for (; actual_component != actual.end() && expected_component != normalized_expected.end();
         ++actual_component, ++expected_component) {
        if (!path_component_equal(*actual_component, *expected_component)) {
            return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
        }
    }
    return actual_component == actual.end() && expected_component == normalized_expected.end()
               ? SAO_AI_EDITOR_OK
               : SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
}

class PinnedPathHandle final {
  public:
    PinnedPathHandle() = default;
    explicit PinnedPathHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~PinnedPathHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    PinnedPathHandle(const PinnedPathHandle&) = delete;
    PinnedPathHandle& operator=(const PinnedPathHandle&) = delete;
    PinnedPathHandle(PinnedPathHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}
    PinnedPathHandle& operator=(PinnedPathHandle&& other) noexcept {
        if (this != &other) {
            if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
                CloseHandle(handle_);
            }
            handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }

    HANDLE get() const noexcept {
        return handle_;
    }

  private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

int32_t pin_directory(const std::filesystem::path& path, std::vector<PinnedPathHandle>& pins) {
    HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                   ? SAO_AI_EDITOR_ERR_NOT_FOUND
                   : SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    FILE_BASIC_INFO info{};
    if (!GetFileInformationByHandleEx(handle, FileBasicInfo, &info, sizeof(info))) {
        CloseHandle(handle);
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    if ((info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        CloseHandle(handle);
        return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
    }
    const int32_t path_status = verify_handle_path(handle, path);
    if (path_status != SAO_AI_EDITOR_OK) {
        CloseHandle(handle);
        return path_status;
    }
    pins.emplace_back(handle);
    return SAO_AI_EDITOR_OK;
}

int32_t pin_root_chain(const std::filesystem::path& canonical_root,
                       std::vector<PinnedPathHandle>& pins) {
    std::filesystem::path current = canonical_root.root_path();
    if (current.empty()) {
        return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
    }
    int32_t status = pin_directory(current, pins);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    for (const auto& component : canonical_root.relative_path()) {
        if (component == L"." || component.empty()) {
            continue;
        }
        if (component == L"..") {
            return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
        }
        current /= component;
        status = pin_directory(current, pins);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
    }
    return SAO_AI_EDITOR_OK;
}

int32_t pin_regular_file(const std::filesystem::path& canonical_file, std::size_t maximum_bytes,
                         std::vector<PinnedPathHandle>& pins) {
    HANDLE file_handle = CreateFileW(
        canonical_file.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file_handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                   ? SAO_AI_EDITOR_ERR_NOT_FOUND
                   : SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    FILE_BASIC_INFO file_info{};
    LARGE_INTEGER file_size{};
    if (!GetFileInformationByHandleEx(file_handle, FileBasicInfo, &file_info, sizeof(file_info)) ||
        !GetFileSizeEx(file_handle, &file_size)) {
        CloseHandle(file_handle);
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    if ((file_info.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
        0) {
        CloseHandle(file_handle);
        return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
    }
    const int32_t path_status = verify_handle_path(file_handle, canonical_file);
    if (path_status != SAO_AI_EDITOR_OK) {
        CloseHandle(file_handle);
        return path_status;
    }
    if (file_size.QuadPart < 0 || static_cast<uint64_t>(file_size.QuadPart) > maximum_bytes) {
        CloseHandle(file_handle);
        return SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL;
    }
    pins.emplace_back(file_handle);
    return SAO_AI_EDITOR_OK;
}

int32_t pin_extension_main_path(const std::filesystem::path& canonical_root,
                                const std::filesystem::path& canonical_main,
                                std::vector<PinnedPathHandle>& pins) {
    std::filesystem::path current = canonical_root;

    const std::filesystem::path relative_parent =
        canonical_main.parent_path().lexically_relative(canonical_root);
    if (relative_parent.empty() || relative_parent.has_root_path()) {
        return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
    }
    int32_t status = SAO_AI_EDITOR_OK;
    for (const auto& component : relative_parent) {
        if (component == L"." || component.empty()) {
            continue;
        }
        if (component == L"..") {
            return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
        }
        current /= component;
        status = pin_directory(current, pins);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
    }
    return pin_regular_file(canonical_main, kMaximumExtensionMainBytes, pins);
}

int32_t read_pinned_text(HANDLE handle, std::size_t maximum_bytes, std::string& text) {
    text.clear();
    LARGE_INTEGER size{};
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE || !GetFileSizeEx(handle, &size) ||
        size.QuadPart < 0 || static_cast<uint64_t>(size.QuadPart) > maximum_bytes) {
        return size.QuadPart > static_cast<LONGLONG>(maximum_bytes)
                   ? SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL
                   : SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    text.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    while (offset < text.size()) {
        DWORD bytes_read = 0;
        const DWORD chunk =
            static_cast<DWORD>(std::min<std::size_t>(text.size() - offset, 64U * 1024U));
        if (!ReadFile(handle, text.data() + offset, chunk, &bytes_read, nullptr) ||
            bytes_read == 0) {
            text.clear();
            return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
        }
        offset += bytes_read;
    }
    return SAO_AI_EDITOR_OK;
}

int32_t load_pinned_extension_manifest(const std::filesystem::path& canonical_root,
                                       std::filesystem::path& canonical_package,
                                       std::vector<PinnedPathHandle>& pins, Json& manifest,
                                       ManifestTextParseResult& parse_result) {
    pins.clear();
    manifest = Json();
    parse_result = ManifestTextParseResult::invalid;
    if (!resolve_bounded_path(canonical_root, "package.json", false, canonical_package) ||
        !path_component_equal(canonical_package.parent_path(), canonical_root)) {
        return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
    }
    int32_t status = pin_root_chain(canonical_root, pins);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    status = pin_regular_file(canonical_package, kMaximumExtensionManifestBytes, pins);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    std::string text;
    status = read_pinned_text(pins.back().get(), kMaximumExtensionManifestBytes, text);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    parse_result = parse_manifest_text_strict(text, manifest);
    if (parse_result != ManifestTextParseResult::ok || !manifest.is_object()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::filesystem::path rebound_package;
    if (!resolve_bounded_path(canonical_root, "package.json", false, rebound_package) ||
        !path_component_equal(rebound_package, canonical_package)) {
        return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
    }
    return SAO_AI_EDITOR_OK;
}

int32_t validate_extension_paths(std::string_view extension_path, std::string_view main_module,
                                 std::filesystem::path& canonical_root,
                                 std::filesystem::path& canonical_main) {
    if (extension_path.find('\0') != std::string_view::npos ||
        main_module.find('\0') != std::string_view::npos ||
        !normalize_root(extension_path, canonical_root, false) || main_module.empty() ||
        !valid_utf8(main_module)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::wstring main_wide = utf8_to_wide(main_module);
    if (main_wide.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::filesystem::path requested(main_wide);
    if (requested.is_absolute() || requested.has_root_name() || requested.has_root_directory() ||
        !resolve_bounded_path(canonical_root, main_module, false, canonical_main)) {
        return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(canonical_main, error) || error ||
        canonical_main == canonical_root) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    const std::uintmax_t main_size = std::filesystem::file_size(canonical_main, error);
    if (error) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    if (main_size > kMaximumExtensionMainBytes) {
        return SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL;
    }
    return SAO_AI_EDITOR_OK;
}

const char* extension_path_error_code(int32_t status) noexcept {
    switch (status) {
    case SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION:
        return "EXTENSION_MAIN_OUTSIDE_ROOT";
    case SAO_AI_EDITOR_ERR_NOT_FOUND:
        return "EXTENSION_MAIN_NOT_FOUND";
    case SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL:
        return "EXTENSION_MAIN_TOO_LARGE";
    default:
        return "EXTENSION_PATH_INVALID";
    }
}

const char* extension_path_error_message(int32_t status) noexcept {
    return status == SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL
               ? "extension main exceeds 1 MiB"
               : "extensionPath must be a canonical directory and main must be a contained file";
}

const char* extension_manifest_error_code(int32_t status,
                                          ManifestTextParseResult parse_result) noexcept {
    if (status == SAO_AI_EDITOR_ERR_NOT_FOUND) {
        return "EXTENSION_MANIFEST_NOT_FOUND";
    }
    if (status == SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL) {
        return "EXTENSION_MANIFEST_TOO_LARGE";
    }
    if (status == SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION) {
        return "EXTENSION_MANIFEST_OUTSIDE_ROOT";
    }
    if (parse_result == ManifestTextParseResult::duplicate_key) {
        return "EXTENSION_MANIFEST_DUPLICATE_KEY";
    }
    if (parse_result == ManifestTextParseResult::invalid_value) {
        return "EXTENSION_MANIFEST_INVALID_VALUE";
    }
    return status == SAO_AI_EDITOR_ERR_INVALID_ARGUMENT ? "EXTENSION_MANIFEST_INVALID_JSON"
                                                        : "EXTENSION_MANIFEST_UNREADABLE";
}

const char* extension_manifest_error_message(int32_t status,
                                             ManifestTextParseResult parse_result) noexcept {
    if (status == SAO_AI_EDITOR_ERR_NOT_FOUND) {
        return "extension root does not contain package.json";
    }
    if (status == SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL) {
        return "extension package.json exceeds 1 MiB";
    }
    if (status == SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION) {
        return "extension package.json is not a canonical root file";
    }
    if (parse_result == ManifestTextParseResult::duplicate_key) {
        return "extension package.json contains duplicate object keys";
    }
    if (parse_result == ManifestTextParseResult::invalid_value) {
        return "extension package.json contains invalid UTF-8, control, or non-finite data";
    }
    return status == SAO_AI_EDITOR_ERR_INVALID_ARGUMENT
               ? "extension package.json is invalid JSON"
               : "extension package.json could not be read";
}

const char* runtime_state_name(const ExtensionRecord& record, bool node_alive) noexcept {
    if (!record.runtime_state_known || record.operation == ExtensionOperation::quarantined) {
        return "quarantined";
    }
    switch (record.operation) {
    case ExtensionOperation::activating:
        return "activating";
    case ExtensionOperation::deactivating:
        return "deactivating";
    case ExtensionOperation::unregistering:
        return "unregistering";
    case ExtensionOperation::quarantined:
        return "quarantined";
    case ExtensionOperation::idle:
    default:
        return record.activated && node_alive ? "active" : "inactive";
    }
}

std::string tree_status_error_code(int32_t status) {
    switch (status) {
    case SAO_AI_EDITOR_ERR_NOT_FOUND:
        return "TREE_NOT_FOUND";
    case SAO_AI_EDITOR_ERR_INVALID_ARGUMENT:
        return "TREE_INVALID_ARGUMENT";
    case SAO_AI_EDITOR_ERR_TIMEOUT:
        return "TREE_CALLBACK_TIMEOUT";
    case SAO_AI_EDITOR_ERR_IPC_CLOSED:
        return "TREE_HOST_CLOSED";
    case SAO_AI_EDITOR_ERR_PROTOCOL:
        return "TREE_HOST_PROTOCOL";
    case SAO_AI_EDITOR_ERR_NOT_INITIALIZED:
        return "TREE_HOST_UNAVAILABLE";
    case SAO_AI_EDITOR_ERR_BUSY:
        return "TREE_CALLBACK_BUSY";
    case SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL:
        return "TREE_RESPONSE_TOO_LARGE";
    default:
        return "TREE_HOST_ERROR";
    }
}

std::string runtime_status_error_code(int32_t status) {
    switch (status) {
    case SAO_AI_EDITOR_ERR_NOT_FOUND:
        return "EXTENSION_NOT_FOUND";
    case SAO_AI_EDITOR_ERR_INVALID_ARGUMENT:
        return "EXTENSION_CALLBACK_INVALID";
    case SAO_AI_EDITOR_ERR_TIMEOUT:
        return "EXTENSION_CALLBACK_TIMEOUT";
    case SAO_AI_EDITOR_ERR_NOT_INITIALIZED:
        return "EXTENSION_HOST_UNAVAILABLE";
    case SAO_AI_EDITOR_ERR_IPC_CLOSED:
        return "HOST_EXITED";
    case SAO_AI_EDITOR_ERR_PROTOCOL:
        return "EXTENSION_HOST_PROTOCOL";
    case SAO_AI_EDITOR_ERR_BUSY:
        return "EXTENSION_CALLBACK_BUSY";
    default:
        return "EXTENSION_CALLBACK_FAILED";
    }
}

Json tree_error_result(int32_t status, std::string_view message, uint64_t view_version = 0) {
    return Json{{"ok", false},
                {"available", false},
                {"applied", false},
                {"items", Json::array()},
                {"errorCode", tree_status_error_code(status)},
                {"error", std::string(message)},
                {"viewVersion", view_version}};
}

std::optional<std::string_view> tree_method_for_command(std::string_view command_id) {
    if (command_id == "load_extension_tree_children") {
        return "tree.children";
    }
    if (command_id == "set_extension_tree_item_expanded") {
        return "tree.expand";
    }
    if (command_id == "set_extension_tree_item_checkbox_state") {
        return "tree.checkbox";
    }
    if (command_id == "select_extension_tree_item") {
        return "tree.select";
    }
    if (command_id == "drop_extension_tree_items") {
        return "tree.drop";
    }
    if (command_id == "execute_extension_tree_item_action") {
        return "tree.action";
    }
    if (command_id == "set_extension_activity_view_visibility") {
        return "tree.visibility";
    }
    return std::nullopt;
}

bool reserved_host_command(std::string_view command_id) {
    return tree_method_for_command(command_id).has_value() ||
           command_id == "resolve_extension_webview_view";
}

std::optional<std::string> reserved_contribution_conflict(const Json& contribution) {
    for (const char* family : {"commands", "menus"}) {
        const Json items = contribution.value(family, Json::array());
        if (!items.is_array()) {
            continue;
        }
        for (const auto& item : items) {
            if (!item.is_object()) {
                continue;
            }
            const auto command = item.find("command");
            if (command != item.end() && command->is_string() &&
                reserved_host_command(command->get_ref<const std::string&>())) {
                return std::string(family) + ":" + command->get_ref<const std::string&>();
            }
        }
    }
    return std::nullopt;
}

void append_array(Json& target, const Json& source) {
    if (!target.is_array()) {
        target = Json::array();
    }
    if (!source.is_array()) {
        return;
    }
    for (const auto& item : source) {
        target.push_back(item);
    }
}

std::string item_id(const Json& item) {
    if (!item.is_object()) {
        return {};
    }
    for (const char* key : {"id", "command", "viewType", "type", "name"}) {
        const auto found = item.find(key);
        if (found != item.end() && found->is_string()) {
            return found->get<std::string>();
        }
    }
    return {};
}

std::optional<std::string> contribution_identity_conflict(const Json& candidate,
                                                          const Json& existing) {
    for (const auto& [array_name, id_name] :
         {std::pair<const char*, const char*>{"commands", "command"},
          {"viewContainers", "id"},
          {"views", "id"},
          {"notebooks", "type"},
          {"debuggers", "type"},
          {"taskDefinitions", "type"},
          {"customEditors", "viewType"}}) {
        const Json candidate_items = candidate.value(array_name, Json::array());
        const Json existing_items = existing.value(array_name, Json::array());
        if (!candidate_items.is_array() || !existing_items.is_array()) {
            continue;
        }
        std::unordered_set<std::string> existing_ids;
        for (const auto& item : existing_items) {
            if (!item.is_object()) {
                continue;
            }
            const auto value = item.find(id_name);
            if (value != item.end() && value->is_string() &&
                !value->get_ref<const std::string&>().empty()) {
                existing_ids.insert(value->get<std::string>());
            }
        }
        for (const auto& item : candidate_items) {
            if (!item.is_object()) {
                continue;
            }
            const auto value = item.find(id_name);
            if (value != item.end() && value->is_string() &&
                existing_ids.contains(value->get_ref<const std::string&>())) {
                return std::string(array_name) + ":" + value->get_ref<const std::string&>();
            }
        }
    }
    return std::nullopt;
}

void sort_inventory_array(Json& values) {
    if (!values.is_array()) {
        return;
    }
    std::sort(values.begin(), values.end(), [](const Json& left, const Json& right) {
        const std::string left_owner = left.value("extensionId", std::string{});
        const std::string right_owner = right.value("extensionId", std::string{});
        if (left_owner != right_owner) {
            return left_owner < right_owner;
        }
        return item_id(left) < item_id(right);
    });
}

bool menu_matches_view(const Json& menu, std::string_view view_id,
                       std::string_view context_value = {}) {
    const auto when_value = menu.find("when");
    const std::string when = when_value != menu.end() && when_value->is_string()
                                 ? when_value->get<std::string>()
                                 : std::string{};
    if (when.empty()) {
        return true;
    }
    const bool has_view_clause = when.find("view ==") != std::string::npos ||
                                 when.find("view ===") != std::string::npos ||
                                 when.find("view =~") != std::string::npos;
    const bool has_container_clause = when.find("viewContainer ==") != std::string::npos ||
                                      when.find("viewContainer ===") != std::string::npos ||
                                      when.find("viewContainer =~") != std::string::npos;
    if ((has_view_clause || has_container_clause) && when.find(view_id) == std::string::npos) {
        return false;
    }
    if (!context_value.empty() && when.find("viewItem") != std::string::npos &&
        when.find(context_value) == std::string::npos) {
        return false;
    }
    return true;
}

Json menu_action(const Json& menu, const Json& commands) {
    Json action = menu;
    const auto command_value = menu.find("command");
    const std::string command = command_value != menu.end() && command_value->is_string()
                                    ? command_value->get<std::string>()
                                    : std::string{};
    if (!command.empty() && !action.contains("title")) {
        for (const auto& declaration : commands) {
            if (declaration.value("command", std::string{}) == command) {
                const auto title = declaration.find("title");
                action["title"] =
                    title != declaration.end() && title->is_string() ? *title : Json(command);
                if (!action.contains("icon") && declaration.contains("icon")) {
                    action["icon"] = declaration["icon"];
                }
                break;
            }
        }
    }
    const bool runtime_available = menu.value("runtimeAvailable", false);
    action["runtimeAvailable"] = runtime_available;
    action["enabled"] = runtime_available;
    action["disabled"] = !runtime_available;
    if (!runtime_available) {
        action["disabledReason"] = "Extension command handler is unavailable.";
    }
    return action;
}

bool attach_tree_actions(Json& items, const Json& menus, const Json& commands,
                         std::string_view view_id, std::size_t& action_count,
                         std::size_t& action_bytes) {
    constexpr std::size_t kMaximumTreeActions = 4096;
    constexpr std::size_t kMaximumTreeActionBytes = 512U * 1024U;
    if (!items.is_array()) {
        return false;
    }
    for (auto& item : items) {
        if (!item.is_object()) {
            continue;
        }
        Json actions = item.value("actions", Json::array());
        if (!actions.is_array()) {
            actions = Json::array();
        }
        const std::size_t original_action_count = actions.size();
        Json bounded_actions = Json::array();
        std::unordered_set<std::string> action_ids;
        for (const auto& action : actions) {
            if (!action.is_object() || bounded_actions.size() >= kMaximumTreeActionsPerItem ||
                action_count >= kMaximumTreeActions) {
                return false;
            }
            const std::size_t serialized_size = action.dump().size();
            if (serialized_size > kMaximumTreeActionBytes - action_bytes) {
                return false;
            }
            action_ids.insert(action.value("command", std::string{}) + "\n" +
                              action.value("submenu", std::string{}) + "\n" +
                              action.value("group", std::string{}));
            bounded_actions.push_back(action);
            ++action_count;
            action_bytes += serialized_size;
        }
        if (bounded_actions.size() != original_action_count) {
            return false;
        }
        actions = std::move(bounded_actions);
        const std::string context = item.value("contextValue", std::string{});
        for (const auto& menu : menus) {
            if (actions.size() >= kMaximumTreeActionsPerItem ||
                action_count >= kMaximumTreeActions) {
                return false;
            }
            if (menu.value("menu", std::string{}) != "view/item/context" ||
                !menu_matches_view(menu, view_id, context)) {
                continue;
            }
            const std::string action_id = menu.value("command", std::string{}) + "\n" +
                                          menu.value("submenu", std::string{}) + "\n" +
                                          menu.value("group", std::string{});
            if (!action_ids.insert(action_id).second) {
                continue;
            }
            Json action = menu_action(menu, commands);
            const std::size_t serialized_size = action.dump().size();
            if (serialized_size > kMaximumTreeActionBytes - action_bytes) {
                return false;
            }
            actions.push_back(std::move(action));
            ++action_count;
            action_bytes += serialized_size;
        }
        item["actions"] = std::move(actions);
    }
    return true;
}

} // namespace

Json ExtensionRecord::to_json(bool node_alive) const {
    Json kinds = Json::array();
    if (contribution_summary.is_object()) {
        for (const auto& [kind, count] : contribution_summary.items()) {
            if (count.is_number_unsigned() && count.get<std::size_t>() != 0U) {
                kinds.push_back(kind);
            }
        }
    }
    const bool known = runtime_state_known && (!activated || node_alive);
    const std::size_t contribution_count =
        contribution_summary.is_object()
            ? contribution_summary.value("dynamicSurfaces", std::size_t{})
            : 0;
    Json active = known ? Json(activated && node_alive) : Json(nullptr);
    Json manifest_metadata = Json::object();
    for (const char* key : {"name", "displayName", "description", "publisher", "version", "main",
                            "engines", "activationEvents", "categories"}) {
        const auto value = manifest.find(key);
        if (value != manifest.end()) {
            manifest_metadata[key] = *value;
        }
    }
    return Json{{"id", id},
                {"name", name},
                {"displayName", manifest.value("displayName", name)},
                {"description", manifest.value("description", std::string{})},
                {"publisher", publisher},
                {"version", version},
                {"extensionPath", extension_path},
                {"_extensionPath", extension_path},
                {"packagePath", package_path},
                {"extensionIndex", inventory_index},
                {"ownerId", id},
                {"ownerPath", extension_path},
                {"ownerIndex", inventory_index},
                {"ext_dir", extension_path},
                {"main", main_module},
                {"activated", known && activated && node_alive},
                {"errorCode", runtime_error_code},
                {"error", runtime_error},
                {"installed", true},
                {"canUninstall", operation == ExtensionOperation::idle ||
                                     operation == ExtensionOperation::quarantined},
                {"source", "runtime"},
                {"operation", operation_name(operation)},
                {"runtimeState", runtime_state_name(*this, node_alive)},
                {"runtime", Json{{"available", node_alive},
                                 {"hostAlive", node_alive},
                                 {"known", known},
                                 {"active", std::move(active)},
                                 {"lastKnownActive", activated},
                                 {"state", runtime_state_name(*this, node_alive)},
                                 {"errorCode", runtime_error_code},
                                 {"error", runtime_error}}},
                {"generation", generation},
                {"operationGeneration", operation_generation},
                {"inventory", Json{{"available", true},
                                   {"state", contribution_count == 0 ? "empty" : "ready"},
                                   {"totalEntries", contribution_count},
                                   {"summary", contribution_summary}}},
                {"contributionSummary", contribution_summary},
                {"contributes", std::move(kinds)},
                {"manifest", std::move(manifest_metadata)},
                {"activationResult", activation_result}};
}

ExtensionHost::~ExtensionHost() {
    deactivate_all();
}

void ExtensionHost::mark_runtime_dead_locked(std::string_view error_code, std::string_view error,
                                             bool preserve_activation) {
    for (auto& [id, record] : extensions_) {
        (void)id;
        if ((!record.activated && record.operation == ExtensionOperation::idle) ||
            (preserve_activation && record.operation == ExtensionOperation::activating)) {
            continue;
        }
        record.runtime_state_known = false;
        extapi::retire_language_owner(id, 0);
        record.operation = ExtensionOperation::quarantined;
        record.runtime_error_code = error_code;
        record.runtime_error = error;
    }
}

void ExtensionHost::retire_runtime(const std::shared_ptr<NodeRuntime>& runtime,
                                   std::string_view error_code, std::string_view error) {
    std::string detailed_error(error);
    if (runtime) {
        const std::string tail = runtime->stderr_tail();
        if (!tail.empty()) {
            detailed_error += ": ";
            detailed_error += tail.substr(0, 4096);
        }
    }
    bool current_runtime = false;
    std::vector<std::pair<std::string, uint64_t>> retiring_owners;
    {
        std::unique_lock<std::shared_mutex> runtime_guard(runtime_mutex_);
        std::lock_guard<std::mutex> guard(mutex_);
        if (node_runtime_ == runtime) {
            retiring_owners.reserve(extensions_.size());
            for (const auto& [id, record] : extensions_) {
                const uint64_t generation =
                    (record.operation == ExtensionOperation::deactivating ||
                     record.operation == ExtensionOperation::unregistering)
                        ? record.retiring_generation
                        : record.operation_generation;
                retiring_owners.emplace_back(id, generation);
            }
            mark_runtime_dead_locked(error_code, detailed_error);
            node_runtime_.reset();
            runtime_retiring_ = true;
            current_runtime = true;
        }
    }
    if (runtime) {
        try {
            runtime->shutdown();
        } catch (...) {
        }
    }
    for (const auto& [id, generation] : retiring_owners)
        extapi::retire_extension_owner(id, generation);
    if (current_runtime) {
        std::unique_lock<std::shared_mutex> runtime_guard(runtime_mutex_);
        runtime_retiring_ = false;
    }
}

int32_t ExtensionHost::configure(const Json& params) {
    try {
        std::unique_lock<std::shared_mutex> runtime_guard(runtime_mutex_);
        if (runtime_starting_ || runtime_retiring_) {
            return SAO_AI_EDITOR_ERR_BUSY;
        }
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
        candidate.node_executable = params["nodeExecutable"].get<std::string>();
        if (candidate.node_executable.empty() || !valid_utf8(candidate.node_executable)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }

        if (params.contains("entryScript")) {
            if (!params["entryScript"].is_string()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            candidate.entry_script = params["entryScript"].get<std::string>();
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

        const auto parse_string_array = [](const Json& value, std::vector<std::string>& target) {
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
            parse_string_array(params["nodeArgs"], candidate.node_args) != SAO_AI_EDITOR_OK) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        if (params.contains("extraArgs") &&
            parse_string_array(params["extraArgs"], candidate.extra_args) != SAO_AI_EDITOR_OK) {
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
            candidate.working_directory = params["workingDirectory"].get<std::string>();
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
                const int64_t signed_startup_ms = params["startupMs"].get<int64_t>();
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

int32_t ExtensionHost::ensure_runtime(std::shared_ptr<NodeRuntime>& runtime) {
    std::unique_lock<std::shared_mutex> runtime_guard(runtime_mutex_);
    if (runtime_starting_ || runtime_retiring_) {
        return SAO_AI_EDITOR_ERR_BUSY;
    }
    std::shared_ptr<NodeRuntime> stale_runtime;
    NodeRuntime::BootOptions options;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (boot_options_.node_executable.empty() || boot_options_.entry_script.empty()) {
            return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
        }
        if (node_runtime_ && node_runtime_->alive()) {
            runtime = node_runtime_;
            return SAO_AI_EDITOR_OK;
        }
        if (node_runtime_) {
            mark_runtime_dead_locked("HOST_EXITED", "extension host process exited", true);
        }
        stale_runtime = std::move(node_runtime_);
        options = boot_options_;
    }
    // Merge extension-declared environmentVariableCollection mutations into
    // the spawned Node process environment.  The extras vector wins over the
    // ambient parent block in build_environment_block, so extension ops are
    // applied as real overrides at process creation time.
    // Failures are surfaced as a no-merge boot (ambient env only) — the host
    // process must still start even if the envvars registry is unreadable.
    (void)sao::ai_editor::native::extapi::apply_environment_overrides(
        options.environment);
    if (stale_runtime) {
        runtime_retiring_ = true;
        runtime_guard.unlock();
        try {
            stale_runtime->shutdown();
        } catch (...) {
        }
        runtime_guard.lock();
        runtime_retiring_ = false;
    }
    auto candidate = std::make_shared<NodeRuntime>();
    candidate->set_native_runtime(&runtime_);
    runtime_starting_ = true;
    runtime_guard.unlock();
    int32_t status = SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    try {
        status = candidate->boot(options);
    } catch (...) {
        status = SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    runtime_guard.lock();
    runtime_starting_ = false;
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
            for (auto& [id, record] : extensions_) {
                (void)id;
                if (record.operation == ExtensionOperation::activating) {
                    continue;
                }
                if (record.activated || record.operation == ExtensionOperation::quarantined ||
                    !record.runtime_state_known) {
                    record.activated = false;
                    record.runtime_state_known = true;
                    record.operation = ExtensionOperation::idle;
                    record.activation_result = Json::object();
                    record.runtime_error_code = "HOST_RESTARTED";
                    record.runtime_error = "extension is inactive in the restarted host";
                }
            }
        }
    }
    if (candidate) {
        candidate->shutdown();
    }
    return SAO_AI_EDITOR_OK;
}

int32_t ExtensionHost::list_extensions(Json& out) {
    try {
        std::shared_ptr<NodeRuntime> node;
        std::vector<ExtensionRecord> records;
        std::string node_executable;
        bool node_alive = false;
        bool node_starting = false;
        bool node_retiring = false;
        bool observed_dead_runtime = false;
        {
            std::shared_lock<std::shared_mutex> runtime_guard(runtime_mutex_);
            std::lock_guard<std::mutex> guard(mutex_);
            node_starting = runtime_starting_;
            node_retiring = runtime_retiring_;
            if (node_runtime_ && !node_runtime_->alive()) {
                mark_runtime_dead_locked("HOST_EXITED", "extension host process exited");
                observed_dead_runtime = true;
            }
            node = node_runtime_;
            node_alive = node && node->alive();
            node_executable = boot_options_.node_executable;
            records.reserve(extensions_.size());
            for (const auto& [id, record] : extensions_) {
                (void)id;
                records.push_back(record);
            }
        }
        if (observed_dead_runtime) {
            retire_runtime(node, "HOST_EXITED", "extension host process exited");
        }
        std::sort(records.begin(), records.end(),
                  [](const ExtensionRecord& left, const ExtensionRecord& right) {
                      return left.id < right.id;
                  });

        Json command_runtime = Json{{"items", Json::array()}};
        Json webview_runtime = Json{{"items", Json::array()}};
        Json tree_runtime = tree_error_result(SAO_AI_EDITOR_ERR_NOT_INITIALIZED,
                                              "tree provider host is not running");
        int32_t command_status = SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
        int32_t webview_status = SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
        int32_t tree_status = SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
        std::string command_error = "command registry host is not running";
        std::string webview_error = "WebviewView registry host is not running";
        if (node_alive) {
            std::string fatal_error;
            CallbackGuard callback_guard(callback_active_);
            if (!callback_guard.owns_lock()) {
                command_status = SAO_AI_EDITOR_ERR_BUSY;
                webview_status = SAO_AI_EDITOR_ERR_BUSY;
                tree_status = SAO_AI_EDITOR_ERR_BUSY;
                command_error = "another extension callback is active";
                webview_error = "another extension callback is active";
                tree_runtime =
                    tree_error_result(tree_status, "another extension callback is active");
            } else {
                Json snapshot;
                command_status = node->request("commands.list", Json::object(), 250, snapshot);
                if (command_status == SAO_AI_EDITOR_OK &&
                    valid_runtime_command_inventory(snapshot)) {
                    command_runtime = std::move(snapshot);
                    command_error.clear();
                } else if (command_status == SAO_AI_EDITOR_OK) {
                    command_status = SAO_AI_EDITOR_ERR_PROTOCOL;
                    command_error = "command registry returned an invalid response";
                } else {
                    command_error = "command registry request failed";
                }
                snapshot = Json::object();
                webview_status =
                    node->request("webview.listProviders", Json::object(), 250, snapshot);
                if (webview_status == SAO_AI_EDITOR_OK &&
                    valid_runtime_webview_inventory(snapshot)) {
                    webview_runtime = std::move(snapshot);
                    webview_error.clear();
                } else if (webview_status == SAO_AI_EDITOR_OK) {
                    webview_status = SAO_AI_EDITOR_ERR_PROTOCOL;
                    webview_error = "WebviewView registry returned an invalid response";
                } else {
                    webview_error = "WebviewView registry request failed";
                }
                snapshot = Json::object();
                tree_status = node->request("tree.inventory", Json::object(), 4500, snapshot);
                if (tree_status == SAO_AI_EDITOR_OK && validate_tree_inventory(snapshot)) {
                    if (tree_callback_unresolved(snapshot, true)) {
                        fatal_error = "tree provider callback did not settle";
                    }
                    tree_runtime = std::move(snapshot);
                } else {
                    if (tree_status == SAO_AI_EDITOR_OK) {
                        tree_status = SAO_AI_EDITOR_ERR_PROTOCOL;
                    }
                    tree_runtime = tree_error_result(
                        tree_status, tree_status == SAO_AI_EDITOR_ERR_PROTOCOL
                                         ? "tree provider inventory response is invalid"
                                         : "tree provider inventory request failed");
                    if (activation_outcome_unknown(tree_status)) {
                        fatal_error =
                            tree_runtime.value("error", "tree provider inventory request failed");
                    }
                }
            }
            if (!fatal_error.empty()) {
                retire_runtime(node, "HOST_EXITED", fatal_error);
                node_alive = false;
                if (tree_status == SAO_AI_EDITOR_OK) {
                    tree_status = SAO_AI_EDITOR_ERR_TIMEOUT;
                }
                command_runtime = Json{{"items", Json::array()}};
                webview_runtime = Json{{"items", Json::array()}};
                command_status = SAO_AI_EDITOR_ERR_IPC_CLOSED;
                webview_status = SAO_AI_EDITOR_ERR_IPC_CLOSED;
                command_error = "extension host stopped during inventory request";
                webview_error = command_error;
                for (auto& record : records) {
                    if (record.activated || record.operation != ExtensionOperation::idle) {
                        record.runtime_state_known = false;
                        record.operation = ExtensionOperation::quarantined;
                        record.runtime_error_code = "HOST_EXITED";
                        record.runtime_error = fatal_error;
                    }
                }
            } else if (activation_outcome_unknown(command_status) ||
                       activation_outcome_unknown(webview_status)) {
                const std::string fatal_registry_error =
                    "extension host state became indeterminate during registry inspection";
                retire_runtime(node, "HOST_EXITED", fatal_registry_error);
                node_alive = false;
                command_status = SAO_AI_EDITOR_ERR_IPC_CLOSED;
                webview_status = SAO_AI_EDITOR_ERR_IPC_CLOSED;
                tree_status = SAO_AI_EDITOR_ERR_IPC_CLOSED;
                command_error = fatal_registry_error;
                webview_error = fatal_registry_error;
                command_runtime = Json{{"items", Json::array()}};
                webview_runtime = Json{{"items", Json::array()}};
                tree_runtime =
                    tree_error_result(SAO_AI_EDITOR_ERR_IPC_CLOSED, fatal_registry_error);
                for (auto& record : records) {
                    if (record.activated || record.operation != ExtensionOperation::idle) {
                        record.runtime_state_known = false;
                        record.operation = ExtensionOperation::quarantined;
                        record.runtime_error_code = "HOST_EXITED";
                        record.runtime_error = fatal_registry_error;
                    }
                }
            } else {
                node_alive = node->alive();
            }
        }

        std::unordered_map<std::string, Json> registered_commands;
        const Json command_items = command_runtime.value("items", Json::array());
        if (command_items.is_array()) {
            for (const auto& item : command_items) {
                if (!item.is_object()) {
                    continue;
                }
                const std::string command = item.value("command", std::string{});
                const std::string owner = item.value("extensionId", std::string{});
                if (!command.empty() && !owner.empty()) {
                    registered_commands.insert_or_assign(owner + "\n" + command, item);
                }
            }
        }

        std::unordered_map<std::string, Json> registered_tree_views;
        const Json tree_items = tree_runtime.value("items", Json::array());
        if (tree_items.is_array()) {
            for (const auto& item : tree_items) {
                if (!item.is_object()) {
                    continue;
                }
                const std::string view_id = item.value("viewId", std::string{});
                const std::string owner = item.value("extensionId", std::string{});
                if (!view_id.empty() && !owner.empty()) {
                    registered_tree_views.insert_or_assign(owner + "\n" + view_id, item);
                }
            }
        }

        std::unordered_map<std::string, Json> registered_webview_views;
        const Json webview_items = webview_runtime.value("items", Json::array());
        if (webview_items.is_array()) {
            for (const auto& item : webview_items) {
                if (!item.is_object()) {
                    continue;
                }
                const std::string view_id = item.value("viewId", std::string{});
                const std::string owner = item.value("extensionId", std::string{});
                if (!view_id.empty() && !owner.empty()) {
                    registered_webview_views.insert_or_assign(owner + "\n" + view_id, item);
                }
            }
        }

        Json extension_items = Json::array();
        Json commands = Json::array();
        Json declared_containers = Json::array();
        Json views = Json::array();
        Json menus = Json::array();
        Json configurations = Json::array();
        Json notebooks = Json::array();
        Json debuggers = Json::array();
        Json task_definitions = Json::array();
        Json custom_editors = Json::array();
        Json mcp_servers = Json::array();
        Json chat_providers = Json::array();
        std::size_t tree_action_count = 0;
        std::size_t tree_action_bytes = 0;
        TreeBudget tree_presentation_budget;

        for (const auto& record : records) {
            Json contribution = record.contributions;
            if (!contribution.is_object()) {
                contribution = Json::object();
            }
            const bool record_runtime_active =
                node_alive && record.activated && record.runtime_state_known &&
                record.operation == ExtensionOperation::idle && record.operation_generation != 0;
            Json owned_commands = contribution.value("commands", Json::array());
            if (!owned_commands.is_array()) {
                owned_commands = Json::array();
            }
            for (auto& command : owned_commands) {
                const std::string command_id = command.value("command", std::string{});
                const auto runtime = registered_commands.find(record.id + "\n" + command_id);
                const bool registered =
                    record_runtime_active && runtime != registered_commands.end() &&
                    runtime->second.value("generation", uint64_t{}) == record.operation_generation;
                const bool editor_required =
                    registered && runtime->second.value("editorRequired", false);
                command["runtimeAvailable"] = registered;
                command["runtimeKind"] = editor_required ? "textEditorCommand" : "command";
                command["editorRequired"] = editor_required;
                command["dynamicSource"] = registered ? "manifest+runtime" : "manifest";
                command["surfaceEvidence"] =
                    Json{{"kind", editor_required ? "textEditorCommand" : "command"},
                         {"command", command_id},
                         {"runtimeAvailable", registered},
                         {"editorRequired", editor_required},
                         {"readiness", registered ? "ready" : "manifest-only"},
                         {"readinessScore", registered ? 100 : 0},
                         {"readinessIssues",
                          registered ? Json::array() : Json::array({"handler-unavailable"})}};
            }
            contribution["commands"] = owned_commands;

            Json owned_menus = contribution.value("menus", Json::array());
            if (!owned_menus.is_array()) {
                owned_menus = Json::array();
            }
            for (auto& menu : owned_menus) {
                const std::string command_id = menu.value("command", std::string{});
                const auto runtime = registered_commands.find(record.id + "\n" + command_id);
                const bool registered =
                    record_runtime_active && !command_id.empty() &&
                    runtime != registered_commands.end() &&
                    runtime->second.value("generation", uint64_t{}) == record.operation_generation;
                menu["runtimeAvailable"] = registered;
                menu["dynamicSource"] = registered ? "manifest+runtime" : "manifest";
            }
            contribution["menus"] = owned_menus;

            Json owned_views = contribution.value("views", Json::array());
            if (!owned_views.is_array()) {
                owned_views = Json::array();
            }
            for (auto& view : owned_views) {
                const std::string view_id = view.value("id", std::string{});
                const std::string runtime_kind = view.value("runtimeKind", std::string{"treeView"});
                if (runtime_kind == "webviewView") {
                    const auto provider = registered_webview_views.find(record.id + "\n" + view_id);
                    const bool available = record_runtime_active &&
                                           provider != registered_webview_views.end() &&
                                           provider->second.value("generation", uint64_t{}) ==
                                               record.operation_generation &&
                                           provider->second.value("available", false);
                    view["runtimeAvailable"] = available;
                    view["dynamicSource"] = available ? "manifest+runtime" : "manifest";
                    Json runtime_state{{"kind", "webviewView"},
                                       {"available", available},
                                       {"runtimeAvailable", available},
                                       {"message", available
                                                       ? "WebviewView provider registered."
                                                       : "WebviewView provider is unavailable."}};
                    if (available) {
                        for (const char* key : {"html", "options", "htmlAvailable", "htmlLength",
                                                "errorCode", "error"}) {
                            const auto value = provider->second.find(key);
                            if (value != provider->second.end()) {
                                runtime_state[key] = *value;
                            }
                        }
                        runtime_state["webviewEvidence"] =
                            Json{{"htmlAvailable", provider->second.value("htmlAvailable", false)},
                                 {"htmlLength", provider->second.value("htmlLength", uint64_t{})},
                                 {"options", provider->second.value("options", Json::object())},
                                 {"runtimeAvailable", true}};
                    }
                    view["runtimeState"] = std::move(runtime_state);
                } else {
                    const auto provider = registered_tree_views.find(record.id + "\n" + view_id);
                    const bool current = record_runtime_active &&
                                         provider != registered_tree_views.end() &&
                                         provider->second.value("generation", uint64_t{}) ==
                                             record.operation_generation;
                    Json presented_provider = current ? provider->second : Json::object();
                    bool presentation_valid = current;
                    if (current) {
                        const std::size_t prior_action_count = tree_action_count;
                        const std::size_t prior_action_bytes = tree_action_bytes;
                        bool actions_complete = true;
                        if (presented_provider.value("applied", false)) {
                            actions_complete = attach_tree_actions(
                                presented_provider["items"], owned_menus, owned_commands, view_id,
                                tree_action_count, tree_action_bytes);
                        }
                        TreeBudget candidate_budget = tree_presentation_budget;
                        candidate_budget.handles.clear();
                        candidate_budget.handle_identity.reset();
                        std::string presented_owner;
                        uint64_t presented_generation = 0;
                        uint64_t presented_version = 0;
                        presentation_valid =
                            actions_complete &&
                            validate_tree_response(presented_provider, view_id, candidate_budget,
                                                   presented_owner, presented_generation,
                                                   presented_version) &&
                            presented_owner == record.id &&
                            presented_generation == record.operation_generation;
                        if (presentation_valid) {
                            tree_presentation_budget = std::move(candidate_budget);
                        } else {
                            tree_action_count = prior_action_count;
                            tree_action_bytes = prior_action_bytes;
                        }
                    }
                    const bool available =
                        presentation_valid && presented_provider.value("available", false);
                    Json runtime_state{
                        {"kind", "treeView"},
                        {"registered", current},
                        {"available", available},
                        {"runtimeAvailable", available},
                        {"nodes", Json::array()},
                        {"refreshVersion", uint64_t{}},
                        {"errorCode", presentation_valid
                                          ? presented_provider.value("errorCode", std::string{})
                                      : current ? "TREE_PRESENTATION_LIMIT_EXCEEDED"
                                                : "TREE_PROVIDER_UNAVAILABLE"},
                        {"error",
                         presentation_valid ? presented_provider.value("error", std::string{})
                         : current ? "TreeDataProvider output exceeds native presentation limits."
                                   : "TreeDataProvider is not registered."}};
                    if (presentation_valid) {
                        const bool applied = presented_provider.value("applied", false);
                        if (applied) {
                            runtime_state["nodes"] =
                                presented_provider.value("items", Json::array());
                        }
                        runtime_state["refreshVersion"] =
                            presented_provider.value("viewVersion", uint64_t{});
                        runtime_state["applied"] = applied;
                        runtime_state["state"] = applied ? "ready"
                                                 : presented_provider.value("ok", false)
                                                     ? "registered"
                                                     : "error";
                        for (const char* key : {"title", "description", "message"}) {
                            const auto value = presented_provider.find(key);
                            if (value != presented_provider.end() && value->is_string()) {
                                runtime_state[key] = *value;
                            }
                        }
                        for (const char* key : {"badge", "dragAndDrop", "visible"}) {
                            const auto value = presented_provider.find(key);
                            if (value != presented_provider.end()) {
                                runtime_state[key] = *value;
                            }
                        }
                    } else {
                        runtime_state["applied"] = false;
                        runtime_state["state"] = current ? "error" : "unavailable";
                    }
                    view["runtimeAvailable"] = available;
                    view["dynamicSource"] = available ? "manifest+runtime" : "manifest";
                    view["runtimeState"] = std::move(runtime_state);
                }
                Json title_actions = Json::array();
                for (const auto& menu : owned_menus) {
                    if (menu.value("menu", std::string{}) == "view/title" &&
                        menu_matches_view(menu, view_id)) {
                        title_actions.push_back(menu_action(menu, owned_commands));
                    }
                }
                view["titleActions"] = std::move(title_actions);
            }
            contribution["views"] = owned_views;

            Json owned_containers = contribution.value("viewContainers", Json::array());
            if (!owned_containers.is_array()) {
                owned_containers = Json::array();
            }
            for (auto& container : owned_containers) {
                const std::string container_id = container.value("id", std::string{});
                Json title_actions = Json::array();
                for (const auto& menu : owned_menus) {
                    if (menu.value("menu", std::string{}) == "viewContainer/title" &&
                        menu_matches_view(menu, container_id)) {
                        title_actions.push_back(menu_action(menu, owned_commands));
                    }
                }
                container["titleActions"] = std::move(title_actions);
                container["views"] = Json::array();
            }
            contribution["viewContainers"] = owned_containers;

            Json item = record.to_json(node_alive);
            extension_items.push_back(std::move(item));
            append_array(commands, owned_commands);
            append_array(declared_containers, owned_containers);
            append_array(views, owned_views);
            append_array(menus, owned_menus);
            append_array(configurations, contribution.value("configurations", Json::array()));
            append_array(notebooks, contribution.value("notebooks", Json::array()));
            append_array(debuggers, contribution.value("debuggers", Json::array()));
            append_array(task_definitions, contribution.value("taskDefinitions", Json::array()));
            append_array(custom_editors, contribution.value("customEditors", Json::array()));
            append_array(mcp_servers, contribution.value("mcpServers", Json::array()));
            append_array(chat_providers, contribution.value("chatProviders", Json::array()));
        }

        Json view_containers = Json::array();
        std::unordered_map<std::string, std::size_t> container_indexes;
        for (const auto& declaration : declared_containers) {
            const std::string id = declaration.value("id", std::string{});
            const std::string owner = declaration.value("extensionId", std::string{});
            const std::string key = owner + "\n" + id;
            if (id.empty() || owner.empty() || container_indexes.contains(key)) {
                continue;
            }
            Json container = declaration;
            container["views"] = Json::array();
            container["view_count"] = 0;
            container_indexes[key] = view_containers.size();
            view_containers.push_back(std::move(container));
        }
        const std::unordered_set<std::string> built_in_containers{"explorer", "scm", "debug",
                                                                  "test"};
        for (const auto& view : views) {
            const std::string id = view.value("container", std::string{});
            const std::string owner = view.value("extensionId", std::string{});
            std::string key = owner + "\n" + id;
            auto found = container_indexes.find(key);
            if (found == container_indexes.end() && built_in_containers.contains(id)) {
                key = "builtin\n" + id;
                found = container_indexes.find(key);
                if (found == container_indexes.end()) {
                    Json container{{"id", id},
                                   {"title", id},
                                   {"location", "builtin"},
                                   {"builtin", true},
                                   {"extensionId", "builtin"},
                                   {"extension_id", "builtin"},
                                   {"owner", "builtin"},
                                   {"source", "manifest"},
                                   {"titleActions", Json::array()},
                                   {"views", Json::array()},
                                   {"view_count", 0}};
                    container_indexes[key] = view_containers.size();
                    view_containers.push_back(std::move(container));
                    found = container_indexes.find(key);
                }
            }
            if (found == container_indexes.end()) {
                Json container{{"id", id},
                               {"title", id},
                               {"location", "unresolved"},
                               {"available", false},
                               {"errorCode", "VIEW_CONTAINER_UNDECLARED"},
                               {"error", "View container is not declared."},
                               {"extensionId", owner},
                               {"extension_id", owner},
                               {"owner", owner},
                               {"source", "manifest"},
                               {"titleActions", Json::array()},
                               {"views", Json::array()},
                               {"view_count", 0}};
                container_indexes[key] = view_containers.size();
                view_containers.push_back(std::move(container));
                found = container_indexes.find(key);
            }
            Json& container = view_containers[found->second];
            container["views"].push_back(view);
            container["view_count"] = container["views"].size();
            container["runtimeAvailable"] =
                container.value("runtimeAvailable", false) || view.value("runtimeAvailable", false);
            container["dynamicSource"] =
                container.value("runtimeAvailable", false) ? "manifest+runtime" : "manifest";
        }

        Json activity_bar_items = Json::array();
        Json tree_views = Json::array();
        Json webview_views = Json::array();
        for (const auto& container : view_containers) {
            if (container.value("location", std::string{}) == "activitybar" &&
                container.value("available", true)) {
                activity_bar_items.push_back(container);
            }
        }
        for (const auto& view : views) {
            if (view.value("runtimeKind", std::string{}) == "webviewView") {
                webview_views.push_back(view);
            } else {
                tree_views.push_back(view);
            }
        }

        for (Json* values :
             {&commands, &view_containers, &views, &menus, &configurations, &notebooks, &debuggers,
              &task_definitions, &custom_editors, &mcp_servers, &chat_providers,
              &activity_bar_items, &tree_views, &webview_views}) {
            sort_inventory_array(*values);
        }

        const std::size_t dynamic_surfaces =
            commands.size() + view_containers.size() + views.size() + menus.size() +
            configurations.size() + notebooks.size() + debuggers.size() + task_definitions.size() +
            custom_editors.size() + mcp_servers.size() + chat_providers.size();
        const std::size_t runtime_command_count = static_cast<std::size_t>(
            std::count_if(commands.begin(), commands.end(), [](const Json& command) {
                return command.value("runtimeAvailable", false);
            }));
        const std::size_t text_editor_command_count = static_cast<std::size_t>(
            std::count_if(commands.begin(), commands.end(), [](const Json& command) {
                return command.value("editorRequired", false);
            }));
        std::size_t activated_count = 0;
        std::size_t inactive_count = 0;
        std::size_t quarantined_count = 0;
        std::size_t activating_count = 0;
        std::size_t deactivating_count = 0;
        std::size_t unregistering_count = 0;
        for (const auto& extension : extension_items) {
            const std::string state = extension.value("runtimeState", std::string{});
            if (state == "active") {
                ++activated_count;
            } else if (state == "quarantined") {
                ++quarantined_count;
            } else if (state == "inactive") {
                ++inactive_count;
            } else if (state == "activating") {
                ++activating_count;
            } else if (state == "deactivating") {
                ++deactivating_count;
            } else if (state == "unregistering") {
                ++unregistering_count;
            }
        }
        Json summary{{"extensions", extension_items.size()},
                     {"activated", activated_count},
                     {"inactive", inactive_count},
                     {"quarantined", quarantined_count},
                     {"activating", activating_count},
                     {"deactivating", deactivating_count},
                     {"unregistering", unregistering_count},
                     {"operating", activating_count + deactivating_count + unregistering_count},
                     {"commands", commands.size()},
                     {"runtimeCommands", runtime_command_count},
                     {"textEditorCommands", text_editor_command_count},
                     {"viewContainers", view_containers.size()},
                     {"viewContainerViews", views.size()},
                     {"views", views.size()},
                     {"treeViews", tree_views.size()},
                     {"webviewViews", webview_views.size()},
                     {"menus", menus.size()},
                     {"configurations", configurations.size()},
                     {"notebooks", notebooks.size()},
                     {"debuggers", debuggers.size()},
                     {"taskDefinitions", task_definitions.size()},
                     {"customEditors", custom_editors.size()},
                     {"mcpServers", mcp_servers.size()},
                     {"chatProviders", chat_providers.size()},
                     {"dynamicSurfaces", dynamic_surfaces}};
        const bool inventory_available = !extension_items.empty();
        out = Json{
            {"ok", true},
            {"available", inventory_available},
            {"items", extension_items},
            {"extensions", extension_items},
            {"total", extension_items.size()},
            {"activated", activated_count},
            {"inactive", inactive_count},
            {"quarantined", quarantined_count},
            {"activating", activating_count},
            {"deactivating", deactivating_count},
            {"unregistering", unregistering_count},
            {"operating", activating_count + deactivating_count + unregistering_count},
            {"failed", quarantined_count},
            {"nodeExecutable", node_executable},
            {"nodeAlive", node_alive},
            {"nodeStarting", node_starting},
            {"nodeRetiring", node_retiring},
            {"inventory",
             Json{{"available", inventory_available},
                  {"state", inventory_available ? "ready" : "empty"},
                  {"extensionCount", extension_items.size()},
                  {"summary", summary},
                  {"reason", inventory_available ? "" : "No extension inventory is registered."}}},
            {"runtime",
             Json{{"available", node_alive},
                  {"state", node_alive                ? "running"
                            : node_starting           ? "starting"
                            : node_retiring           ? "retiring"
                            : node_executable.empty() ? "unconfigured"
                                                      : "stopped"},
                  {"commandsAvailable", command_status == SAO_AI_EDITOR_OK},
                  {"commandsErrorCode", command_status == SAO_AI_EDITOR_OK
                                            ? ""
                                            : runtime_status_error_code(command_status)},
                  {"commandsError", command_status == SAO_AI_EDITOR_OK ? "" : command_error},
                  {"webviewViewsAvailable", webview_status == SAO_AI_EDITOR_OK},
                  {"webviewViewsErrorCode", webview_status == SAO_AI_EDITOR_OK
                                                ? ""
                                                : runtime_status_error_code(webview_status)},
                  {"webviewViewsError", webview_status == SAO_AI_EDITOR_OK ? "" : webview_error},
                  {"treeAvailable", node_alive && tree_runtime.value("available", false)},
                  {"treeStatus", tree_status},
                  {"treeErrorCode", tree_runtime.value("errorCode", std::string{})},
                  {"treeError", tree_runtime.value("error", std::string{})}}},
            {"summary", std::move(summary)},
            {"commands", std::move(commands)},
            {"viewContainers", std::move(view_containers)},
            {"activityBarItems", std::move(activity_bar_items)},
            {"views", std::move(views)},
            {"treeViews", std::move(tree_views)},
            {"webviewViews", std::move(webview_views)},
            {"menus", std::move(menus)},
            {"configurations", std::move(configurations)},
            {"notebooks", std::move(notebooks)},
            {"debuggers", std::move(debuggers)},
            {"taskDefinitions", std::move(task_definitions)},
            {"customEditors", std::move(custom_editors)},
            {"mcpServers", std::move(mcp_servers)},
            {"chatProviders", std::move(chat_providers)}};
        if (!inventory_available) {
            out["reason"] = "No extension inventory is registered.";
            out["errorCode"] = "EXTENSION_INVENTORY_EMPTY";
        }
        if (!serialized_json_within(out, kMaximumExtensionInventoryBytes)) {
            out = Json{{"ok", false},
                       {"available", false},
                       {"applied", false},
                       {"items", Json::array()},
                       {"extensions", Json::array()},
                       {"errorCode", "EXTENSION_INVENTORY_LIMIT_EXCEEDED"},
                       {"error", "extension inventory exceeds the 4 MiB delivery limit"}};
            return SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL;
        }
        return SAO_AI_EDITOR_OK;
    } catch (const std::bad_alloc&) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    } catch (const Json::exception&) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    } catch (...) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

int32_t ExtensionHost::register_extension(const Json& params, Json& out) {
    const auto fail = [&](int32_t status, std::string_view code, std::string_view message) {
        const std::string error_code(code);
        const std::string error_message(message);
        out = Json{{"ok", false},
                   {"available", false},
                   {"applied", false},
                   {"errorCode", error_code},
                   {"error", error_message},
                   {"message", error_message}};
        return status;
    };
    try {
        if (!params.is_object() || !params.contains("manifest") ||
            !params["manifest"].is_object() || !params.contains("extensionPath") ||
            !params["extensionPath"].is_string()) {
            return fail(SAO_AI_EDITOR_ERR_INVALID_ARGUMENT, "EXTENSION_REGISTRATION_INVALID",
                        "manifest and extensionPath are required");
        }
        const std::string requested_extension_path = params["extensionPath"].get<std::string>();
        std::filesystem::path canonical_root;
        if (requested_extension_path.find('\0') != std::string::npos ||
            !valid_utf8(requested_extension_path) ||
            !normalize_root(requested_extension_path, canonical_root, false)) {
            return fail(SAO_AI_EDITOR_ERR_INVALID_ARGUMENT, "EXTENSION_PATH_INVALID",
                        "extensionPath must identify a canonical directory");
        }
        std::filesystem::path canonical_package;
        std::vector<PinnedPathHandle> path_pins;
        Json manifest;
        ManifestTextParseResult manifest_parse = ManifestTextParseResult::invalid;
        const int32_t manifest_status = load_pinned_extension_manifest(
            canonical_root, canonical_package, path_pins, manifest, manifest_parse);
        if (manifest_status != SAO_AI_EDITOR_OK) {
            return fail(manifest_status,
                        extension_manifest_error_code(manifest_status, manifest_parse),
                        extension_manifest_error_message(manifest_status, manifest_parse));
        }
        if (manifest != params["manifest"]) {
            return fail(SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION, "EXTENSION_MANIFEST_MISMATCH",
                        "registration manifest does not exactly match package.json");
        }
        for (const char* key : {"name", "publisher", "main"}) {
            const auto value = manifest.find(key);
            if (value == manifest.end() || !value->is_string()) {
                return fail(SAO_AI_EDITOR_ERR_INVALID_ARGUMENT, "EXTENSION_MANIFEST_INVALID",
                            "extension manifest requires string name, publisher, and main");
            }
        }
        for (const char* key : {"version", "displayName", "description", "id"}) {
            const auto value = manifest.find(key);
            if (value != manifest.end() && !value->is_string()) {
                return fail(SAO_AI_EDITOR_ERR_INVALID_ARGUMENT, "EXTENSION_MANIFEST_INVALID",
                            "extension manifest string field has an invalid type");
            }
        }

        ExtensionRecord record;
        record.manifest = manifest;
        record.extension_path = requested_extension_path;
        record.name = manifest["name"].get<std::string>();
        record.publisher = manifest["publisher"].get<std::string>();
        record.version = manifest.value("version", std::string{});
        record.main_module = manifest["main"].get<std::string>();
        if (!valid_extension_segment(record.publisher) || !valid_extension_segment(record.name)) {
            return fail(SAO_AI_EDITOR_ERR_INVALID_ARGUMENT, "EXTENSION_ID_INVALID",
                        "publisher and name must be exact id segments");
        }
        record.id = record.publisher + "." + record.name;
        if (!valid_simple_id(record.id)) {
            return fail(SAO_AI_EDITOR_ERR_INVALID_ARGUMENT, "EXTENSION_ID_INVALID",
                        "publisher.name exceeds the extension id limit");
        }
        for (const auto& [key, expected] :
             {std::pair<std::string_view, std::string_view>{"publisher", record.publisher},
              {"name", record.name}}) {
            const auto value = params.find(std::string(key));
            if (value != params.end() &&
                (!value->is_string() || value->get_ref<const std::string&>() != expected)) {
                return fail(SAO_AI_EDITOR_ERR_INVALID_ARGUMENT, "EXTENSION_ID_MISMATCH",
                            "extension publisher and name must match the manifest exactly");
            }
        }
        const auto require_exact_id = [&](const Json& source, std::string_view key) {
            const auto value = source.find(std::string(key));
            return value == source.end() ||
                   (value->is_string() && value->get_ref<const std::string&>() == record.id);
        };
        if (!require_exact_id(manifest, "id") || !require_exact_id(params, "id") ||
            !require_exact_id(params, "extensionId")) {
            return fail(SAO_AI_EDITOR_ERR_INVALID_ARGUMENT, "EXTENSION_ID_MISMATCH",
                        "extension id must exactly equal publisher.name");
        }

        std::filesystem::path canonical_main;
        const int32_t path_status = validate_extension_paths(
            record.extension_path, record.main_module, canonical_root, canonical_main);
        if (path_status != SAO_AI_EDITOR_OK) {
            return fail(path_status, extension_path_error_code(path_status),
                        extension_path_error_message(path_status));
        }
        record.extension_path = wide_to_utf8(canonical_root.native());
        record.package_path = wide_to_utf8(canonical_package.native());
        record.main_path = wide_to_utf8(canonical_main.native());
        if (record.extension_path.empty() || record.package_path.empty() ||
            record.main_path.empty()) {
            return fail(SAO_AI_EDITOR_ERR_INVALID_ARGUMENT, "EXTENSION_PATH_INVALID",
                        "extension path encoding is invalid");
        }
        const int32_t pin_status =
            pin_extension_main_path(canonical_root, canonical_main, path_pins);
        if (pin_status != SAO_AI_EDITOR_OK) {
            return fail(pin_status, extension_path_error_code(pin_status),
                        extension_path_error_message(pin_status));
        }

        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (extensions_.contains(record.id)) {
                return fail(SAO_AI_EDITOR_ERR_BUSY, "EXTENSION_ALREADY_REGISTERED",
                            "extension id is already registered");
            }
            if (extensions_.size() >= kMaximumRuntimeRegistrations) {
                return fail(SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL,
                            "EXTENSION_INVENTORY_LIMIT_EXCEEDED",
                            "installed extension inventory is full");
            }
            if (next_extension_inventory_index_ > kMaximumSafeJsonInteger ||
                next_extension_inventory_index_ > std::numeric_limits<std::size_t>::max()) {
                return fail(SAO_AI_EDITOR_ERR_OS_CALL_FAILED, "EXTENSION_INDEX_EXHAUSTED",
                            "extension owner index is exhausted");
            }
            record.inventory_index = next_extension_inventory_index_++;
        }

        ManifestContributionInventory inventory;
        std::vector<std::string> diagnostics;
        if (!parse_manifest_contribution_inventory(
                manifest, record.id, inventory, &diagnostics, record.extension_path,
                static_cast<std::size_t>(record.inventory_index))) {
            return inventory.truncated
                       ? fail(SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL,
                              "EXTENSION_MANIFEST_LIMIT_EXCEEDED",
                              "extension contribution inventory exceeds its bounded capacity")
                       : fail(SAO_AI_EDITOR_ERR_INVALID_ARGUMENT, "EXTENSION_CONTRIBUTION_INVALID",
                              "extension contributions contain duplicate or invalid data");
        }
        if (inventory.truncated) {
            return fail(SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL, "EXTENSION_MANIFEST_LIMIT_EXCEEDED",
                        "extension contribution inventory exceeds its bounded capacity");
        }
        record.contributions = inventory.to_json();
        record.contributions.erase("available");
        record.contributions.erase("summary");
        record.contributions.erase("totalEntries");
        record.contributions.erase("serializedBytes");
        record.contributions.erase("truncated");
        if (!diagnostics.empty()) {
            record.contributions["diagnostics"] = diagnostics;
        }
        record.contribution_summary = inventory.summary();
        const auto reserved_conflict = reserved_contribution_conflict(record.contributions);
        if (reserved_conflict.has_value()) {
            return fail(SAO_AI_EDITOR_ERR_INVALID_ARGUMENT, "EXTENSION_CONTRIBUTION_RESERVED",
                        "extension contribution uses a host-reserved command id: " +
                            *reserved_conflict);
        }

        std::lock_guard<std::mutex> guard(mutex_);
        const Json declared_commands = record.contributions.value("commands", Json::array());
        for (const auto& command : declared_commands) {
            const std::string command_id = command.value("command", std::string{});
            if (!command_id.empty() && native_commands_.contains(command_id)) {
                return fail(SAO_AI_EDITOR_ERR_BUSY, "EXTENSION_CONTRIBUTION_CONFLICT",
                            "extension command id conflicts with a native command: " + command_id);
            }
        }
        const auto existing = extensions_.find(record.id);
        if (existing != extensions_.end()) {
            return fail(SAO_AI_EDITOR_ERR_BUSY, "EXTENSION_ALREADY_REGISTERED",
                        "extension id is already registered");
        }
        if (extensions_.size() >= kMaximumRuntimeRegistrations) {
            return fail(SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL, "EXTENSION_INVENTORY_LIMIT_EXCEEDED",
                        "installed extension inventory is full");
        }
        std::size_t aggregate_entries = inventory.total_entries;
        std::size_t aggregate_bytes = record.contributions.dump().size();
        if (aggregate_bytes > kMaximumExtensionInventoryBytes) {
            return fail(SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL, "EXTENSION_INVENTORY_LIMIT_EXCEEDED",
                        "installed extension contributions exceed the host byte limit");
        }
        for (const auto& [owner, installed] : extensions_) {
            (void)owner;
            const std::size_t installed_entries =
                installed.contribution_summary.value("dynamicSurfaces", std::size_t{});
            if (installed_entries > kMaximumRuntimeRegistrations - aggregate_entries) {
                return fail(SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL,
                            "EXTENSION_INVENTORY_LIMIT_EXCEEDED",
                            "installed extension contributions exceed the host inventory limit");
            }
            aggregate_entries += installed_entries;
            const std::size_t installed_bytes = installed.contributions.dump().size();
            if (installed_bytes > kMaximumExtensionInventoryBytes - aggregate_bytes) {
                return fail(SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL,
                            "EXTENSION_INVENTORY_LIMIT_EXCEEDED",
                            "installed extension contributions exceed the host byte limit");
            }
            aggregate_bytes += installed_bytes;
            const auto conflict =
                contribution_identity_conflict(record.contributions, installed.contributions);
            if (conflict.has_value()) {
                return fail(SAO_AI_EDITOR_ERR_BUSY, "EXTENSION_CONTRIBUTION_CONFLICT",
                            "extension contribution id conflicts with an installed extension: " +
                                *conflict);
            }
        }
        Json response = record.to_json(node_runtime_ && node_runtime_->alive());
        response["ok"] = true;
        response["available"] = true;
        response["applied"] = true;
        extensions_.emplace(record.id, std::move(record));
        out = std::move(response);
        return SAO_AI_EDITOR_OK;
    } catch (const std::bad_alloc&) {
        return fail(SAO_AI_EDITOR_ERR_OS_CALL_FAILED, "EXTENSION_REGISTRATION_OOM",
                    "extension registration allocation failed");
    } catch (const Json::exception&) {
        return fail(SAO_AI_EDITOR_ERR_INVALID_ARGUMENT, "EXTENSION_MANIFEST_INVALID",
                    "extension manifest is invalid");
    } catch (...) {
        return fail(SAO_AI_EDITOR_ERR_OS_CALL_FAILED, "EXTENSION_REGISTRATION_FAILED",
                    "extension registration failed");
    }
}

int32_t ExtensionHost::unregister_extension(std::string_view extension_id, Json& out) {
    const std::string id(extension_id);
    std::shared_ptr<NodeRuntime> node;
    uint64_t operation_generation = 0;
    uint64_t runtime_generation = 0;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = extensions_.find(id);
        if (found == extensions_.end()) {
            out = Json{{"ok", false},
                       {"available", false},
                       {"applied", false},
                       {"errorCode", "EXTENSION_NOT_FOUND"},
                       {"error", "extension is not registered"},
                       {"extensionId", id}};
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        if (found->second.operation == ExtensionOperation::quarantined &&
            (!node_runtime_ || !node_runtime_->alive())) {
            const bool last_known_active = found->second.activated;
            extensions_.erase(found);
            out = Json{{"ok", true},
                       {"available", true},
                       {"applied", true},
                       {"extensionId", id},
                       {"runtimeStateWasKnown", false},
                       {"lastKnownActive", last_known_active}};
            return SAO_AI_EDITOR_OK;
        }
        if (found->second.operation != ExtensionOperation::idle) {
            out =
                Json{{"ok", false},
                     {"available", true},
                     {"applied", false},
                     {"errorCode", "EXTENSION_BUSY"},
                     {"error", "extension lifecycle operation is active"},
                     {"extensionId", id},
                     {"runtimeState",
                      runtime_state_name(found->second, node_runtime_ && node_runtime_->alive())}};
            return SAO_AI_EDITOR_ERR_BUSY;
        }
        if (!found->second.activated) {
            extensions_.erase(found);
            out = Json{{"ok", true}, {"available", true}, {"applied", true}, {"extensionId", id}};
            return SAO_AI_EDITOR_OK;
        }
        runtime_generation = found->second.operation_generation;
        found->second.retiring_generation = runtime_generation;
        found->second.operation = ExtensionOperation::unregistering;
        if (!advance_extension_generation(found->second, next_extension_generation_,
                                          operation_generation)) {
            found->second.operation = ExtensionOperation::idle;
            out = Json{{"ok", false},
                       {"available", true},
                       {"applied", false},
                       {"errorCode", "EXTENSION_GENERATION_EXHAUSTED"},
                       {"error", "extension generation is exhausted"},
                       {"extensionId", id}};
            return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
        }
        node = node_runtime_;
    }

    if (!node || !node->alive()) {
        if (node) {
            retire_runtime(node, "HOST_EXITED", "extension host is not running");
        } else {
            std::lock_guard<std::mutex> guard(mutex_);
            mark_runtime_dead_locked("HOST_EXITED", "extension host is not running");
        }
        out = Json{{"ok", false},
                   {"available", false},
                   {"applied", false},
                   {"errorCode", "HOST_EXITED"},
                   {"error", "extension host is not running"},
                   {"message", "extension host is not running"},
                   {"extensionId", id},
                   {"runtimeState", "quarantined"}};
        return SAO_AI_EDITOR_ERR_IPC_CLOSED;
    }
    CallbackGuard callback_guard(callback_active_);
    if (!callback_guard.owns_lock()) {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = extensions_.find(id);
        if (found != extensions_.end() &&
            found->second.operation == ExtensionOperation::unregistering &&
            found->second.operation_generation == operation_generation) {
            found->second.operation = ExtensionOperation::idle;
            found->second.operation_generation = runtime_generation;
        }
        out = Json{{"ok", false},
                   {"available", true},
                   {"applied", false},
                   {"errorCode", "EXTENSION_CALLBACK_BUSY"},
                   {"error", "another extension callback is active"},
                   {"extensionId", id}};
        return SAO_AI_EDITOR_ERR_BUSY;
    }
    Json deactivate_result;
    const int32_t status = node->request(
        "host.deactivate", Json{{"extensionId", id}, {"generation", runtime_generation}}, 5000,
        deactivate_result);
    const bool completion_matches =
        status == SAO_AI_EDITOR_OK && deactivate_result.is_object() &&
        true_boolean_member(deactivate_result, "deactivated") &&
        exact_string_member(deactivate_result, "extensionId", id) &&
        exact_generation_member(deactivate_result, "generation", runtime_generation);
    const auto cleanup_complete_value = deactivate_result.is_object()
                                            ? deactivate_result.find("cleanupComplete")
                                            : deactivate_result.end();
    const bool cleanup_field_valid =
        cleanup_complete_value != deactivate_result.end() && cleanup_complete_value->is_boolean() &&
        valid_cleanup_report(deactivate_result, cleanup_complete_value->get<bool>());
    const bool cleanup_complete = cleanup_field_valid && cleanup_complete_value->get<bool>();
    const bool valid_completion = completion_matches && cleanup_complete;
    if (completion_matches && cleanup_field_valid && !cleanup_complete) {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            const auto found = extensions_.find(id);
            if (found != extensions_.end() &&
                found->second.operation == ExtensionOperation::unregistering &&
                found->second.operation_generation == operation_generation) {
                found->second.runtime_state_known = false;
                found->second.operation = ExtensionOperation::quarantined;
                found->second.runtime_error_code = "EXTENSION_CLEANUP_FAILED";
                found->second.runtime_error = "extension teardown completed with cleanup failures";
            }
        }
        retire_runtime(node, "EXTENSION_CLEANUP_FAILED",
                       "extension teardown completed with cleanup failures");
        out = std::move(deactivate_result);
        out["ok"] = false;
        out["available"] = false;
        out["applied"] = false;
        out["errorCode"] = "EXTENSION_CLEANUP_FAILED";
        out["error"] = "extension teardown completed with cleanup failures";
        out["runtimeState"] = "quarantined";
        out["extensionId"] = id;
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    if (!valid_completion) {
        const int32_t completion_status =
            status == SAO_AI_EDITOR_OK ? SAO_AI_EDITOR_ERR_PROTOCOL : status;
        const bool unknown = activation_outcome_unknown(completion_status);
        const std::string failure_message =
            unknown ? "extension deactivate callback outcome is unknown"
                    : node_error_message(deactivate_result, "extension deactivate callback failed");
        {
            std::lock_guard<std::mutex> guard(mutex_);
            const auto found = extensions_.find(id);
            if (found != extensions_.end() &&
                found->second.operation == ExtensionOperation::unregistering &&
                found->second.operation_generation == operation_generation) {
                if (unknown) {
                    found->second.operation = ExtensionOperation::quarantined;
                    found->second.runtime_state_known = false;
                    found->second.runtime_error_code = runtime_status_error_code(completion_status);
                    found->second.runtime_error = failure_message;
                } else {
                    found->second.operation = ExtensionOperation::idle;
                    found->second.operation_generation = runtime_generation;
                    found->second.runtime_state_known = true;
                    found->second.runtime_error_code = runtime_status_error_code(completion_status);
                    found->second.runtime_error = failure_message;
                }
            }
        }
        if (unknown) {
            retire_runtime(node, "HOST_EXITED",
                           "extension host stopped after uncertain unregister");
        }
        out = std::move(deactivate_result);
        if (!out.is_object()) {
            out = Json::object();
        }
        out["ok"] = false;
        out["available"] = !unknown;
        out["applied"] = false;
        out["errorCode"] = runtime_status_error_code(completion_status);
        out["error"] = failure_message;
        out["runtimeState"] = unknown ? "quarantined" : "active";
        out["extensionId"] = id;
        return completion_status;
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
    out = Json{{"ok", true}, {"available", true}, {"applied", true}, {"extensionId", id}};
    return SAO_AI_EDITOR_OK;
}

int32_t ExtensionHost::activate(std::string_view extension_id, uint32_t timeout_ms, Json& out) {
    const std::string id(extension_id);
    ExtensionRecord record;
    uint64_t operation_generation = 0;
    bool recovering_from_dead_host = false;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = extensions_.find(id);
        if (found == extensions_.end()) {
            out = Json{{"ok", false},
                       {"available", false},
                       {"applied", false},
                       {"errorCode", "EXTENSION_NOT_FOUND"},
                       {"error", "extension is not registered"},
                       {"extensionId", id}};
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        const bool node_alive = node_runtime_ && node_runtime_->alive();
        recovering_from_dead_host = found->second.operation == ExtensionOperation::quarantined ||
                                    !found->second.runtime_state_known ||
                                    (found->second.activated && !node_alive);
        if (found->second.operation != ExtensionOperation::idle && !recovering_from_dead_host) {
            out = Json{{"ok", false},
                       {"available", true},
                       {"applied", false},
                       {"errorCode", "EXTENSION_BUSY"},
                       {"error", "extension lifecycle operation is active"},
                       {"extensionId", id},
                       {"runtimeState", runtime_state_name(found->second, node_alive)}};
            return SAO_AI_EDITOR_ERR_BUSY;
        }
        if (found->second.activated && node_alive && !recovering_from_dead_host) {
            out = Json{{"ok", false},
                       {"available", true},
                       {"applied", false},
                       {"errorCode", "EXTENSION_ALREADY_ACTIVE"},
                       {"error", "extension is already active"},
                       {"extensionId", id},
                       {"runtimeState", "active"}};
            return SAO_AI_EDITOR_ERR_BUSY;
        }
        found->second.operation = ExtensionOperation::activating;
        if (!advance_extension_generation(found->second, next_extension_generation_,
                                          operation_generation)) {
            found->second.operation = recovering_from_dead_host ? ExtensionOperation::quarantined
                                                                : ExtensionOperation::idle;
            out = Json{{"ok", false},
                       {"available", false},
                       {"applied", false},
                       {"errorCode", "EXTENSION_GENERATION_EXHAUSTED"},
                       {"error", "extension generation is exhausted"},
                       {"extensionId", id},
                       {"runtimeState", recovering_from_dead_host ? "quarantined" : "inactive"}};
            return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
        }
        record = found->second;
    }
    std::filesystem::path canonical_root;
    std::filesystem::path canonical_package;
    std::vector<PinnedPathHandle> path_pins;
    Json current_manifest;
    ManifestTextParseResult manifest_parse = ManifestTextParseResult::invalid;
    int32_t manifest_status = SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    if (normalize_root(record.extension_path, canonical_root, false)) {
        manifest_status = load_pinned_extension_manifest(
            canonical_root, canonical_package, path_pins, current_manifest, manifest_parse);
    }
    const bool manifest_binding_changed =
        manifest_status == SAO_AI_EDITOR_OK &&
        (wide_to_utf8(canonical_package.native()) != record.package_path ||
         current_manifest != record.manifest);
    if (manifest_status != SAO_AI_EDITOR_OK || manifest_binding_changed) {
        const std::string error_code =
            manifest_binding_changed
                ? "EXTENSION_MANIFEST_CHANGED"
                : extension_manifest_error_code(manifest_status, manifest_parse);
        const std::string error_message =
            manifest_binding_changed
                ? "extension package.json binding or content changed"
                : extension_manifest_error_message(manifest_status, manifest_parse);
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = extensions_.find(id);
        if (found != extensions_.end() &&
            found->second.operation_generation == operation_generation) {
            found->second.operation = recovering_from_dead_host ? ExtensionOperation::quarantined
                                                                : ExtensionOperation::idle;
            found->second.runtime_state_known = !recovering_from_dead_host;
            found->second.runtime_error_code = error_code;
            found->second.runtime_error = error_message;
        }
        out = Json{{"ok", false},
                   {"available", false},
                   {"applied", false},
                   {"errorCode", error_code},
                   {"error", error_message},
                   {"extensionId", id},
                   {"runtimeState", recovering_from_dead_host ? "quarantined" : "inactive"}};
        return manifest_binding_changed ? SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION : manifest_status;
    }
    std::filesystem::path canonical_main;
    const int32_t path_status = validate_extension_paths(record.extension_path, record.main_module,
                                                         canonical_root, canonical_main);
    const bool binding_changed = path_status == SAO_AI_EDITOR_OK &&
                                 (wide_to_utf8(canonical_root.native()) != record.extension_path ||
                                  wide_to_utf8(canonical_main.native()) != record.main_path);
    if (path_status != SAO_AI_EDITOR_OK || binding_changed) {
        const std::string error_code =
            binding_changed ? "EXTENSION_BINDING_CHANGED" : extension_path_error_code(path_status);
        const std::string error_message = binding_changed
                                              ? "extension root or main file binding changed"
                                              : extension_path_error_message(path_status);
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = extensions_.find(id);
        if (found != extensions_.end() &&
            found->second.operation_generation == operation_generation) {
            found->second.operation = recovering_from_dead_host ? ExtensionOperation::quarantined
                                                                : ExtensionOperation::idle;
            found->second.runtime_state_known = !recovering_from_dead_host;
            found->second.runtime_error_code = error_code;
            found->second.runtime_error = error_message;
        }
        out = Json{{"ok", false},
                   {"available", false},
                   {"applied", false},
                   {"errorCode", error_code},
                   {"error", error_message},
                   {"extensionId", id},
                   {"runtimeState", recovering_from_dead_host ? "quarantined" : "inactive"}};
        return binding_changed ? SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION : path_status;
    }
    const int32_t pin_status = pin_extension_main_path(canonical_root, canonical_main, path_pins);
    if (pin_status != SAO_AI_EDITOR_OK) {
        const std::string error_code = extension_path_error_code(pin_status);
        const std::string error_message = extension_path_error_message(pin_status);
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = extensions_.find(id);
        if (found != extensions_.end() &&
            found->second.operation_generation == operation_generation) {
            found->second.operation = recovering_from_dead_host ? ExtensionOperation::quarantined
                                                                : ExtensionOperation::idle;
            found->second.runtime_state_known = !recovering_from_dead_host;
            found->second.runtime_error_code = error_code;
            found->second.runtime_error = error_message;
        }
        out = Json{{"ok", false},
                   {"available", false},
                   {"applied", false},
                   {"errorCode", error_code},
                   {"error", error_message},
                   {"extensionId", id},
                   {"runtimeState", recovering_from_dead_host ? "quarantined" : "inactive"}};
        return pin_status;
    }
    std::shared_ptr<NodeRuntime> node;
    const int32_t runtime_status = ensure_runtime(node);
    if (runtime_status != SAO_AI_EDITOR_OK) {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = extensions_.find(id);
        if (found != extensions_.end() &&
            found->second.operation_generation == operation_generation) {
            found->second.operation = recovering_from_dead_host ? ExtensionOperation::quarantined
                                                                : ExtensionOperation::idle;
            found->second.runtime_state_known = !recovering_from_dead_host;
            found->second.runtime_error_code = runtime_status_error_code(runtime_status);
            found->second.runtime_error = "extension host could not be started";
        }
        out = Json{{"ok", false},
                   {"available", false},
                   {"applied", false},
                   {"errorCode", runtime_status_error_code(runtime_status)},
                   {"error", "extension host could not be started"},
                   {"extensionId", id},
                   {"runtimeState", recovering_from_dead_host ? "quarantined" : "inactive"}};
        return runtime_status;
    }
    CallbackGuard callback_guard(callback_active_);
    if (!callback_guard.owns_lock()) {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = extensions_.find(id);
        if (found != extensions_.end() &&
            found->second.operation == ExtensionOperation::activating &&
            found->second.operation_generation == operation_generation) {
            found->second.activated = false;
            found->second.operation = ExtensionOperation::idle;
            found->second.runtime_state_known = true;
            found->second.runtime_error_code = "EXTENSION_CALLBACK_BUSY";
            found->second.runtime_error = "another extension callback is active";
        }
        out = Json{{"ok", false},
                   {"available", true},
                   {"applied", false},
                   {"errorCode", "EXTENSION_CALLBACK_BUSY"},
                   {"error", "another extension callback is active"},
                   {"extensionId", id},
                   {"runtimeState", "inactive"}};
        return SAO_AI_EDITOR_ERR_BUSY;
    }
    Json params{{"extensionId", id},
                {"extensionPath", record.extension_path},
                {"main", record.main_module},
                {"mainPath", record.main_path},
                {"generation", operation_generation},
                {"manifest", record.manifest}};
    Json result;
    const int32_t status =
        node->request("host.activate", params, timeout_ms == 0 ? 15000 : timeout_ms, result);
    const bool valid_completion =
        status == SAO_AI_EDITOR_OK && result.is_object() &&
        true_boolean_member(result, "activated") &&
        exact_string_member(result, "extensionId", id) &&
        exact_generation_member(result, "generation", operation_generation);
    if (!valid_completion) {
        const int32_t completion_status =
            status == SAO_AI_EDITOR_OK ? SAO_AI_EDITOR_ERR_PROTOCOL : status;
        const bool unknown = activation_outcome_unknown(completion_status);
        const std::string failure_message =
            unknown ? "extension activation callback outcome is unknown"
                    : node_error_message(result, "extension activation callback failed");
        if (unknown) {
            {
                std::lock_guard<std::mutex> guard(mutex_);
                const auto found = extensions_.find(id);
                if (found != extensions_.end() &&
                    found->second.operation == ExtensionOperation::activating &&
                    found->second.operation_generation == operation_generation) {
                    found->second.operation = ExtensionOperation::quarantined;
                    found->second.runtime_state_known = false;
                    found->second.runtime_error_code = runtime_status_error_code(completion_status);
                    found->second.runtime_error = failure_message;
                }
            }
            retire_runtime(node, "HOST_EXITED",
                           "extension host stopped after uncertain activation");
        } else {
            std::lock_guard<std::mutex> guard(mutex_);
            const auto found = extensions_.find(id);
            if (found != extensions_.end() &&
                found->second.operation == ExtensionOperation::activating &&
                found->second.operation_generation == operation_generation) {
                if (recovering_from_dead_host) {
                    found->second.activated = false;
                }
                found->second.operation = ExtensionOperation::idle;
                found->second.runtime_state_known = true;
                found->second.runtime_error_code = runtime_status_error_code(completion_status);
                found->second.runtime_error = failure_message;
            }
        }
        extapi::retire_language_owner(id, operation_generation);
        out = result.is_object() ? std::move(result) : Json::object();
        out["ok"] = false;
        out["available"] = !unknown;
        out["applied"] = false;
        out["errorCode"] = runtime_status_error_code(completion_status);
        out["error"] = failure_message;
        out["extensionId"] = id;
        out["runtimeState"] = unknown ? "quarantined" : "inactive";
        return completion_status;
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
        found->second.runtime_state_known = true;
        found->second.runtime_error_code.clear();
        found->second.runtime_error.clear();
        found->second.activation_result = std::move(result);
        found->second.operation = ExtensionOperation::idle;
        out = found->second.to_json(true);
        out["ok"] = true;
        out["available"] = true;
        out["applied"] = true;
    }
    return SAO_AI_EDITOR_OK;
}

int32_t ExtensionHost::deactivate(std::string_view extension_id, Json& out) {
    const std::string id(extension_id);
    std::shared_ptr<NodeRuntime> node;
    uint64_t operation_generation = 0;
    uint64_t runtime_generation = 0;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = extensions_.find(id);
        if (found == extensions_.end()) {
            out = Json{{"ok", false},
                       {"available", false},
                       {"applied", false},
                       {"errorCode", "EXTENSION_NOT_FOUND"},
                       {"error", "extension is not registered"},
                       {"extensionId", id}};
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        if (found->second.operation == ExtensionOperation::quarantined &&
            (!node_runtime_ || !node_runtime_->alive())) {
            out = Json{{"ok", false},
                       {"available", false},
                       {"applied", false},
                       {"errorCode", "HOST_EXITED"},
                       {"error", "extension runtime state is unknown"},
                       {"extensionId", id},
                       {"runtimeState", "quarantined"}};
            return SAO_AI_EDITOR_ERR_IPC_CLOSED;
        }
        if (found->second.operation != ExtensionOperation::idle) {
            out =
                Json{{"ok", false},
                     {"available", true},
                     {"applied", false},
                     {"errorCode", "EXTENSION_BUSY"},
                     {"error", "extension lifecycle operation is active"},
                     {"extensionId", id},
                     {"runtimeState",
                      runtime_state_name(found->second, node_runtime_ && node_runtime_->alive())}};
            return SAO_AI_EDITOR_ERR_BUSY;
        }
        if (!found->second.activated) {
            out = Json{{"ok", true},      {"available", true}, {"applied", false},
                       {"already", true}, {"extensionId", id}, {"runtimeState", "inactive"}};
            return SAO_AI_EDITOR_OK;
        }
        runtime_generation = found->second.operation_generation;
        found->second.retiring_generation = runtime_generation;
        found->second.operation = ExtensionOperation::deactivating;
        if (!advance_extension_generation(found->second, next_extension_generation_,
                                          operation_generation)) {
            found->second.operation = ExtensionOperation::idle;
            out = Json{{"ok", false},
                       {"available", true},
                       {"applied", false},
                       {"errorCode", "EXTENSION_GENERATION_EXHAUSTED"},
                       {"error", "extension generation is exhausted"},
                       {"extensionId", id}};
            return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
        }
        node = node_runtime_;
    }

    if (!node || !node->alive()) {
        if (node) {
            retire_runtime(node, "HOST_EXITED", "extension host is not running");
        } else {
            std::lock_guard<std::mutex> guard(mutex_);
            mark_runtime_dead_locked("HOST_EXITED", "extension host is not running");
        }
        out = Json{{"ok", false},
                   {"available", false},
                   {"applied", false},
                   {"errorCode", "HOST_EXITED"},
                   {"error", "extension host is not running"},
                   {"message", "extension host is not running"},
                   {"extensionId", id},
                   {"runtimeState", "quarantined"}};
        return SAO_AI_EDITOR_ERR_IPC_CLOSED;
    }
    CallbackGuard callback_guard(callback_active_);
    if (!callback_guard.owns_lock()) {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = extensions_.find(id);
        if (found != extensions_.end() &&
            found->second.operation == ExtensionOperation::deactivating &&
            found->second.operation_generation == operation_generation) {
            found->second.operation = ExtensionOperation::idle;
            found->second.operation_generation = runtime_generation;
        }
        out = Json{{"ok", false},
                   {"available", true},
                   {"applied", false},
                   {"errorCode", "EXTENSION_CALLBACK_BUSY"},
                   {"error", "another extension callback is active"},
                   {"extensionId", id},
                   {"runtimeState", "active"}};
        return SAO_AI_EDITOR_ERR_BUSY;
    }
    Json deactivate_result;
    const int32_t status = node->request(
        "host.deactivate", Json{{"extensionId", id}, {"generation", runtime_generation}}, 5000,
        deactivate_result);
    const bool completion_matches =
        status == SAO_AI_EDITOR_OK && deactivate_result.is_object() &&
        true_boolean_member(deactivate_result, "deactivated") &&
        exact_string_member(deactivate_result, "extensionId", id) &&
        exact_generation_member(deactivate_result, "generation", runtime_generation);
    const auto cleanup_complete_value = deactivate_result.is_object()
                                            ? deactivate_result.find("cleanupComplete")
                                            : deactivate_result.end();
    const bool cleanup_field_valid =
        cleanup_complete_value != deactivate_result.end() && cleanup_complete_value->is_boolean() &&
        valid_cleanup_report(deactivate_result, cleanup_complete_value->get<bool>());
    const bool cleanup_complete = cleanup_field_valid && cleanup_complete_value->get<bool>();
    const bool valid_completion = completion_matches && cleanup_complete;
    if (completion_matches && cleanup_field_valid && !cleanup_complete) {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            const auto found = extensions_.find(id);
            if (found != extensions_.end() &&
                found->second.operation == ExtensionOperation::deactivating &&
                found->second.operation_generation == operation_generation) {
                found->second.runtime_state_known = false;
                found->second.operation = ExtensionOperation::quarantined;
                found->second.runtime_error_code = "EXTENSION_CLEANUP_FAILED";
                found->second.runtime_error = "extension teardown completed with cleanup failures";
            }
        }
        retire_runtime(node, "EXTENSION_CLEANUP_FAILED",
                       "extension teardown completed with cleanup failures");
        out = std::move(deactivate_result);
        out["ok"] = false;
        out["available"] = false;
        out["applied"] = false;
        out["errorCode"] = "EXTENSION_CLEANUP_FAILED";
        out["error"] = "extension teardown completed with cleanup failures";
        out["runtimeState"] = "quarantined";
        out["extensionId"] = id;
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    if (!valid_completion) {
        const int32_t completion_status =
            status == SAO_AI_EDITOR_OK ? SAO_AI_EDITOR_ERR_PROTOCOL : status;
        const bool unknown = activation_outcome_unknown(completion_status);
        const std::string failure_message =
            unknown
                ? "extension deactivation callback outcome is unknown"
                : node_error_message(deactivate_result, "extension deactivation callback failed");
        {
            std::lock_guard<std::mutex> guard(mutex_);
            const auto found = extensions_.find(id);
            if (found != extensions_.end() &&
                found->second.operation == ExtensionOperation::deactivating &&
                found->second.operation_generation == operation_generation) {
                if (unknown) {
                    found->second.operation = ExtensionOperation::quarantined;
                    found->second.runtime_state_known = false;
                    found->second.runtime_error_code = runtime_status_error_code(completion_status);
                    found->second.runtime_error = failure_message;
                } else {
                    found->second.operation = ExtensionOperation::idle;
                    found->second.operation_generation = runtime_generation;
                    found->second.runtime_state_known = true;
                    found->second.runtime_error_code = runtime_status_error_code(completion_status);
                    found->second.runtime_error = failure_message;
                }
            }
        }
        if (unknown) {
            retire_runtime(node, "HOST_EXITED",
                           "extension host stopped after uncertain deactivation");
        }
        out = deactivate_result.is_object() ? std::move(deactivate_result) : Json::object();
        out["ok"] = false;
        out["available"] = !unknown;
        out["applied"] = false;
        out["errorCode"] = runtime_status_error_code(completion_status);
        out["error"] = failure_message;
        out["extensionId"] = id;
        out["runtimeState"] = unknown ? "quarantined" : "active";
        return completion_status;
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
        found->second.runtime_state_known = true;
        found->second.runtime_error_code.clear();
        found->second.runtime_error.clear();
        found->second.activation_result = Json::object();
        found->second.operation = ExtensionOperation::idle;
    }
    extapi::retire_language_owner(id, runtime_generation);
    out = Json{{"ok", true},
               {"available", true},
               {"applied", true},
               {"extensionId", id},
               {"runtimeState", "inactive"}};
    return SAO_AI_EDITOR_OK;
}

bool ExtensionHost::generation_current(std::string_view extension_id, uint64_t generation) const {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto found = extensions_.find(std::string(extension_id));
    if (found == extensions_.end() || generation == 0) return false;
    if ((found->second.operation == ExtensionOperation::deactivating ||
         found->second.operation == ExtensionOperation::unregistering) &&
        found->second.retiring_generation == generation)
        return true;
    return found->second.operation_generation == generation &&
           (found->second.operation == ExtensionOperation::activating ||
            (found->second.operation == ExtensionOperation::idle && found->second.activated &&
             found->second.runtime_state_known));
}

int32_t ExtensionHost::execute_command(std::string_view command_id, const Json& args,
                                       uint32_t timeout_ms, Json& out) {
    const auto tree_method = tree_method_for_command(command_id);
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
    if (command_id == "sao.internal.extapi.event") {
        if (!node || !node->alive()) return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
        if (!args.is_array() || args.size() != 1 || !args[0].is_object())
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        const int32_t status = node->notify("sao.extapi.event", args[0]);
        out = Json{{"accepted", status == SAO_AI_EDITOR_OK}};
        return status;
    }
    if (native_handler && !tree_method.has_value()) {
        CallbackGuard callback_guard(callback_active_);
        if (!callback_guard.owns_lock()) {
            out = Json{{"ok", false},
                       {"available", true},
                       {"applied", false},
                       {"errorCode", "EXTENSION_CALLBACK_BUSY"},
                       {"error", "another extension callback is active"}};
            return SAO_AI_EDITOR_ERR_BUSY;
        }
        try {
            return native_handler(args, out);
        } catch (...) {
            out = Json{{"ok", false},
                       {"available", true},
                       {"applied", false},
                       {"errorCode", "EXTENSION_CALLBACK_FAILED"},
                       {"error", "native extension command callback failed"}};
            return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
        }
    }
    if (!node || !node->alive()) {
        if (node) {
            retire_runtime(node, "HOST_EXITED", "extension host is not running");
        } else {
            std::lock_guard<std::mutex> guard(mutex_);
            mark_runtime_dead_locked("HOST_EXITED", "extension host is not running");
        }
        if (tree_method.has_value()) {
            out = tree_error_result(SAO_AI_EDITOR_ERR_NOT_INITIALIZED,
                                    "extension tree host is not running");
            const auto view = args.is_object() ? args.find("viewId") : args.end();
            out["viewId"] =
                view != args.end() && view->is_string() ? view->get<std::string>() : std::string{};
        } else {
            out = Json{{"ok", false},
                       {"available", false},
                       {"applied", false},
                       {"errorCode", "HOST_EXITED"},
                       {"error", "extension host is not running"}};
        }
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    CallbackGuard callback_guard(callback_active_);
    if (!callback_guard.owns_lock()) {
        if (tree_method.has_value()) {
            out = tree_error_result(SAO_AI_EDITOR_ERR_BUSY, "another extension callback is active");
            std::string view_id;
            if (args.is_object()) {
                const auto view = args.find("viewId");
                if (view != args.end() && view->is_string()) {
                    view_id = view->get<std::string>();
                }
            }
            out["viewId"] = std::move(view_id);
        } else {
            out = Json{{"ok", false},
                       {"available", true},
                       {"applied", false},
                       {"errorCode", "EXTENSION_CALLBACK_BUSY"},
                       {"error", "another extension callback is active"}};
        }
        return SAO_AI_EDITOR_ERR_BUSY;
    }
    if (tree_method.has_value()) {
        if (!args.is_object()) {
            out = tree_error_result(SAO_AI_EDITOR_ERR_INVALID_ARGUMENT,
                                    "tree request parameters must be an object");
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const auto requested_view = args.find("viewId");
        if (requested_view == args.end() || !requested_view->is_string() ||
            requested_view->get_ref<const std::string&>().empty() ||
            requested_view->get_ref<const std::string&>().size() > 256 ||
            requested_view->get_ref<const std::string&>().find('\0') != std::string::npos ||
            !valid_utf8(requested_view->get_ref<const std::string&>())) {
            out = tree_error_result(SAO_AI_EDITOR_ERR_INVALID_ARGUMENT,
                                    "tree request viewId is invalid");
            out["viewId"] = std::string{};
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string view_id = requested_view->get<std::string>();
        uint64_t requested_version = 0;
        const auto version = args.find("viewVersion");
        if (version != args.end() && !safe_nonnegative_integer(*version, requested_version)) {
            out = tree_error_result(SAO_AI_EDITOR_ERR_INVALID_ARGUMENT,
                                    "tree request viewVersion is invalid");
            out["viewId"] = view_id;
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const auto invalid_tree_request = [&](std::string_view message) {
            out = tree_error_result(SAO_AI_EDITOR_ERR_INVALID_ARGUMENT, message, requested_version);
            out["viewId"] = view_id;
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        };
        std::optional<std::array<uint64_t, 3>> request_handle_identity;
        const auto valid_handle_value = [&](const Json& value, bool allow_empty) {
            if (!value.is_string()) {
                return false;
            }
            const auto& handle = value.get_ref<const std::string&>();
            if (allow_empty && handle.empty()) {
                return true;
            }
            std::array<uint64_t, 4> parts{};
            if (!decode_tree_handle(handle, parts) ||
                (requested_version != 0 && parts[2] != requested_version)) {
                return false;
            }
            const std::array<uint64_t, 3> identity{parts[0], parts[1], parts[2]};
            if (request_handle_identity.has_value() && *request_handle_identity != identity) {
                return false;
            }
            request_handle_identity = identity;
            return true;
        };
        const auto valid_handle_member = [&](std::string_view key, bool allow_empty) {
            const auto value = args.find(std::string(key));
            return value != args.end() && valid_handle_value(*value, allow_empty);
        };
        if (*tree_method == "tree.children") {
            if (!valid_handle_member("handle", false)) {
                return invalid_tree_request("tree item handle is invalid");
            }
        } else if (*tree_method == "tree.expand") {
            if (!valid_handle_member("handle", false) || !args.contains("expanded") ||
                !args["expanded"].is_boolean()) {
                return invalid_tree_request("tree expand request is invalid");
            }
        } else if (*tree_method == "tree.checkbox") {
            if (!valid_handle_member("handle", false) || !args.contains("checked") ||
                !args["checked"].is_boolean()) {
                return invalid_tree_request("tree checkbox request is invalid");
            }
        } else if (*tree_method == "tree.select") {
            if (!valid_handle_member("handle", false)) {
                return invalid_tree_request("tree selection handle is invalid");
            }
        } else if (*tree_method == "tree.drop") {
            const auto sources = args.find("sourceHandles");
            if (sources == args.end() || !sources->is_array() || sources->empty() ||
                sources->size() > kMaximumTreeChildren ||
                !valid_handle_member("targetHandle", true)) {
                return invalid_tree_request("tree drop request is invalid");
            }
            std::unordered_set<std::string> unique_handles;
            for (const auto& source : *sources) {
                if (!valid_handle_value(source, false) ||
                    !unique_handles.insert(source.get<std::string>()).second) {
                    return invalid_tree_request("tree drop source handle is invalid");
                }
            }
            const auto data = args.find("data");
            if (data != args.end() && !data->is_object()) {
                return invalid_tree_request("tree drop data is invalid");
            }
        } else if (*tree_method == "tree.action") {
            const auto command = args.find("command");
            const auto arguments = args.find("arguments");
            if (!valid_handle_member("handle", false) || command == args.end() ||
                !command->is_string() || command->get_ref<const std::string&>().empty() ||
                command->get_ref<const std::string&>().size() > 256 ||
                command->get_ref<const std::string&>().find('\0') != std::string::npos ||
                !valid_utf8(command->get_ref<const std::string&>()) || arguments == args.end() ||
                !arguments->is_array() || arguments->size() > kMaximumTreeActionsPerItem) {
                return invalid_tree_request("tree action request is invalid");
            }
        } else if (*tree_method == "tree.visibility") {
            if (!args.contains("visible") || !args["visible"].is_boolean()) {
                return invalid_tree_request("tree visibility request is invalid");
            }
        }
        if (!valid_tree_request_payload(args)) {
            return invalid_tree_request("tree request exceeds structural limits");
        }
        Json result;
        const uint32_t tree_timeout = timeout_ms == 0 ? 4500 : std::min<uint32_t>(timeout_ms, 4500);
        const int32_t status = node->request(*tree_method, args, tree_timeout, result);
        if (status != SAO_AI_EDITOR_OK) {
            const std::string message = node_error_message(result, "tree callback failed");
            out = tree_error_result(status, message, requested_version);
            out["viewId"] = view_id;
            if (activation_outcome_unknown(status)) {
                retire_runtime(node, "HOST_EXITED", message);
            }
            return status;
        }
        TreeBudget response_budget;
        std::string owner;
        uint64_t response_generation = 0;
        uint64_t response_version = 0;
        if (!validate_tree_response(result, view_id, response_budget, owner, response_generation,
                                    response_version)) {
            out = tree_error_result(SAO_AI_EDITOR_ERR_PROTOCOL,
                                    "tree callback returned an invalid response");
            out["viewId"] = view_id;
            retire_runtime(node, "HOST_EXITED", "tree callback returned an invalid response");
            return SAO_AI_EDITOR_ERR_PROTOCOL;
        }
        if (tree_callback_unresolved(result)) {
            out = std::move(result);
            retire_runtime(node, "HOST_EXITED", "tree provider callback did not settle");
            return SAO_AI_EDITOR_OK;
        }
        const bool applied = result.value("applied", false);
        bool request_identity_current = true;
        if (applied && request_handle_identity.has_value()) {
            request_identity_current =
                (*request_handle_identity)[0] == response_generation &&
                (*request_handle_identity)[2] == response_version &&
                (!response_budget.handle_identity.has_value() ||
                 *response_budget.handle_identity == *request_handle_identity);
        }
        bool identity_current = owner.empty();
        if (!owner.empty()) {
            std::lock_guard<std::mutex> guard(mutex_);
            const auto found = extensions_.find(owner);
            if (found != extensions_.end() && found->second.activated &&
                found->second.runtime_state_known &&
                found->second.operation == ExtensionOperation::idle &&
                found->second.operation_generation == response_generation) {
                const Json declared_views =
                    found->second.contributions.value("views", Json::array());
                identity_current =
                    declared_views.is_array() &&
                    std::any_of(
                        declared_views.begin(), declared_views.end(), [&](const Json& view) {
                            return view.is_object() && view.value("id", std::string{}) == view_id &&
                                   view.value("runtimeKind", std::string{"treeView"}) == "treeView";
                        });
            } else {
                identity_current = false;
            }
        }
        if ((result.value("available", false) && owner.empty()) || !identity_current ||
            !request_identity_current ||
            (applied && requested_version != 0 && response_version != requested_version)) {
            out = tree_error_result(SAO_AI_EDITOR_ERR_BUSY, "tree provider generation is stale",
                                    response_version);
            out["viewId"] = view_id;
            out["errorCode"] = "TREE_PROVIDER_STALE";
            return SAO_AI_EDITOR_OK;
        }
        if (*tree_method == "tree.children" || *tree_method == "tree.expand") {
            Json menus = Json::array();
            Json commands = Json::array();
            uint64_t owner_generation = 0;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                const auto found = extensions_.find(owner);
                if (found != extensions_.end() && found->second.contributions.is_object()) {
                    owner_generation = found->second.operation_generation;
                    menus = found->second.contributions.value("menus", Json::array());
                    commands = found->second.contributions.value("commands", Json::array());
                }
            }
            Json command_snapshot;
            int32_t command_status =
                node->request("commands.list", Json::object(), 250, command_snapshot);
            if (command_status == SAO_AI_EDITOR_OK &&
                !valid_runtime_command_inventory(command_snapshot)) {
                command_status = SAO_AI_EDITOR_ERR_PROTOCOL;
            }
            if (command_status != SAO_AI_EDITOR_OK) {
                out = tree_error_result(command_status,
                                        command_status == SAO_AI_EDITOR_ERR_PROTOCOL
                                            ? "command registry returned an invalid response"
                                            : "command registry request failed",
                                        response_version);
                out["viewId"] = view_id;
                if (activation_outcome_unknown(command_status)) {
                    retire_runtime(node, "HOST_EXITED",
                                   "extension host stopped during command inventory");
                }
                return command_status;
            }
            std::unordered_set<std::string> live_commands;
            if (command_snapshot.is_object()) {
                const Json registrations = command_snapshot.value("items", Json::array());
                if (registrations.is_array()) {
                    for (const auto& registration : registrations) {
                        if (!registration.is_object()) {
                            continue;
                        }
                        const auto registered_owner = registration.find("extensionId");
                        const auto registered_generation = registration.find("generation");
                        const auto registered_command_value = registration.find("command");
                        uint64_t generation = 0;
                        if (registered_owner == registration.end() ||
                            !registered_owner->is_string() ||
                            registered_owner->get_ref<const std::string&>() != owner ||
                            registered_generation == registration.end() ||
                            !safe_nonnegative_integer(*registered_generation, generation) ||
                            generation != owner_generation ||
                            registered_command_value == registration.end() ||
                            !registered_command_value->is_string()) {
                            continue;
                        }
                        const std::string registered_command =
                            registered_command_value->get<std::string>();
                        if (!registered_command.empty()) {
                            live_commands.insert(registered_command);
                        }
                    }
                }
            }
            for (auto& command : commands) {
                if (!command.is_object()) {
                    continue;
                }
                const bool available =
                    live_commands.contains(command.value("command", std::string{}));
                command["runtimeAvailable"] = available;
                command["enabled"] = available;
                command["disabled"] = !available;
            }
            for (auto& menu : menus) {
                if (!menu.is_object()) {
                    continue;
                }
                menu["runtimeAvailable"] =
                    live_commands.contains(menu.value("command", std::string{}));
            }
            std::size_t tree_action_count = 0;
            std::size_t tree_action_bytes = response_budget.payload;
            if (!attach_tree_actions(result["items"], menus, commands, view_id, tree_action_count,
                                     tree_action_bytes)) {
                out = tree_error_result(SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL,
                                        "tree actions exceed native presentation limits",
                                        response_version);
                out["viewId"] = view_id;
                return SAO_AI_EDITOR_OK;
            }
            TreeBudget decorated_budget;
            std::string decorated_owner;
            uint64_t decorated_generation = 0;
            uint64_t decorated_version = 0;
            if (!validate_tree_response(result, view_id, decorated_budget, decorated_owner,
                                        decorated_generation, decorated_version) ||
                decorated_owner != owner || decorated_generation != response_generation ||
                decorated_version != response_version) {
                out = tree_error_result(SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL,
                                        "tree response exceeds native presentation limits",
                                        response_version);
                out["viewId"] = view_id;
                return SAO_AI_EDITOR_OK;
            }
        }
        out = std::move(result);
        return SAO_AI_EDITOR_OK;
    }
    if (command_id == "resolve_extension_webview_view") {
        if (!args.is_object() || !args.contains("viewId") || !args["viewId"].is_string() ||
            args["viewId"].get_ref<const std::string&>().empty() ||
            args["viewId"].get_ref<const std::string&>().size() > 256 ||
            args["viewId"].get_ref<const std::string&>().find('\0') != std::string::npos ||
            !valid_utf8(args["viewId"].get_ref<const std::string&>())) {
            out = Json{{"ok", false},
                       {"available", false},
                       {"applied", false},
                       {"viewId", ""},
                       {"errorCode", "WEBVIEW_PROVIDER_INVALID_ARGUMENT"},
                       {"error", "WebviewView id is invalid"}};
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string view_id = args["viewId"].get<std::string>();
        const auto current_provider = [&](const Json& snapshot, const std::string* expected_owner,
                                          uint64_t expected_generation, Json& provider) {
            if (!snapshot.is_object()) {
                return false;
            }
            const Json items = snapshot.value("items", Json::array());
            if (!items.is_array() || items.size() > kMaximumRuntimeRegistrations) {
                return false;
            }
            std::size_t matches = 0;
            for (const auto& item : items) {
                if (!item.is_object()) {
                    return false;
                }
                const auto item_view = item.find("viewId");
                if (item_view == item.end() || !item_view->is_string()) {
                    return false;
                }
                if (item_view->get_ref<const std::string&>() != view_id) {
                    continue;
                }
                uint64_t generation = 0;
                const auto generation_value = item.find("generation");
                const auto owner_value = item.find("extensionId");
                const auto available_value = item.find("available");
                const auto html_value = item.find("html");
                const auto html_available_value = item.find("htmlAvailable");
                const auto html_length_value = item.find("htmlLength");
                const auto options_value = item.find("options");
                const auto error_code_value = item.find("errorCode");
                const auto error_value = item.find("error");
                uint64_t html_length = 0;
                if (owner_value == item.end() || !owner_value->is_string() ||
                    generation_value == item.end() ||
                    !safe_nonnegative_integer(*generation_value, generation) || generation == 0 ||
                    available_value == item.end() || !available_value->is_boolean() ||
                    !available_value->get<bool>() || html_value == item.end() ||
                    !html_value->is_string() ||
                    html_value->get_ref<const std::string&>().size() > kMaximumExtensionMainBytes ||
                    html_available_value == item.end() || !html_available_value->is_boolean() ||
                    html_length_value == item.end() ||
                    !safe_nonnegative_integer(*html_length_value, html_length) ||
                    html_length != html_value->get_ref<const std::string&>().size() ||
                    html_available_value->get<bool>() != (html_length != 0) ||
                    options_value == item.end() || !options_value->is_object() ||
                    error_code_value == item.end() || !error_code_value->is_string() ||
                    error_value == item.end() || !error_value->is_string()) {
                    return false;
                }
                const std::string owner = owner_value->get<std::string>();
                if (!valid_simple_id(owner) ||
                    (expected_owner != nullptr && owner != *expected_owner) ||
                    (expected_generation != 0 && generation != expected_generation)) {
                    return false;
                }
                bool owner_current = false;
                {
                    std::lock_guard<std::mutex> guard(mutex_);
                    const auto found = extensions_.find(owner);
                    if (found != extensions_.end() && found->second.activated &&
                        found->second.runtime_state_known &&
                        found->second.operation == ExtensionOperation::idle &&
                        found->second.operation_generation == generation) {
                        const Json declared =
                            found->second.contributions.value("views", Json::array());
                        owner_current =
                            declared.is_array() &&
                            std::any_of(declared.begin(), declared.end(), [&](const Json& view) {
                                return view.is_object() &&
                                       view.value("id", std::string{}) == view_id &&
                                       view.value("runtimeKind", std::string{}) == "webviewView";
                            });
                    }
                }
                if (!owner_current || ++matches != 1) {
                    return false;
                }
                provider = item;
            }
            return matches == 1;
        };

        Json provider_snapshot;
        int32_t provider_status =
            node->request("webview.listProviders", Json::object(), 250, provider_snapshot);
        if (provider_status != SAO_AI_EDITOR_OK) {
            out = Json{{"ok", false},
                       {"available", false},
                       {"applied", false},
                       {"viewId", view_id},
                       {"errorCode", runtime_status_error_code(provider_status)},
                       {"error", "WebviewView provider inventory failed"}};
            if (activation_outcome_unknown(provider_status)) {
                retire_runtime(node, "HOST_EXITED", "WebviewView provider inventory failed");
            }
            return provider_status;
        }
        if (!valid_runtime_webview_inventory(provider_snapshot)) {
            out = Json{{"ok", false},
                       {"available", false},
                       {"applied", false},
                       {"viewId", view_id},
                       {"errorCode", "WEBVIEW_PROVIDER_PROTOCOL_ERROR"},
                       {"error", "WebviewView provider inventory is invalid"}};
            retire_runtime(node, "HOST_EXITED", "WebviewView provider inventory is invalid");
            return SAO_AI_EDITOR_ERR_PROTOCOL;
        }
        Json provider;
        if (!current_provider(provider_snapshot, nullptr, 0, provider)) {
            out = Json{{"ok", false},
                       {"available", false},
                       {"applied", false},
                       {"viewId", view_id},
                       {"errorCode", "WEBVIEW_PROVIDER_UNAVAILABLE"},
                       {"error", "WebviewView provider is unavailable"}};
            return SAO_AI_EDITOR_OK;
        }
        const std::string owner = provider.value("extensionId", std::string{});
        uint64_t generation = 0;
        (void)safe_nonnegative_integer(provider["generation"], generation);
        Json callback_result;
        const int32_t status = node->request(
            "webview.resolveView", args,
            timeout_ms == 0 ? 4000 : std::min<uint32_t>(timeout_ms, 4000), callback_result);
        if (status != SAO_AI_EDITOR_OK) {
            out = Json{{"ok", false},
                       {"available", !activation_outcome_unknown(status)},
                       {"applied", false},
                       {"viewId", view_id},
                       {"extensionId", owner},
                       {"generation", generation},
                       {"errorCode", runtime_status_error_code(status)},
                       {"error", "WebviewView provider callback failed"}};
            if (activation_outcome_unknown(status)) {
                retire_runtime(node, "HOST_EXITED",
                               "WebviewView provider callback outcome is unknown");
            }
            return status;
        }
        const auto callback_html =
            callback_result.is_object() ? callback_result.find("html") : callback_result.end();
        const auto callback_options =
            callback_result.is_object() ? callback_result.find("options") : callback_result.end();
        if (!true_boolean_member(callback_result, "ok") ||
            !exact_string_member(callback_result, "viewId", view_id) ||
            !exact_string_member(callback_result, "extensionId", owner) ||
            !exact_generation_member(callback_result, "generation", generation) ||
            callback_html == callback_result.end() || !callback_html->is_string() ||
            callback_options == callback_result.end() || !callback_options->is_object()) {
            out = Json{{"ok", false},
                       {"available", false},
                       {"applied", false},
                       {"viewId", view_id},
                       {"errorCode", "WEBVIEW_PROVIDER_PROTOCOL_ERROR"},
                       {"error", "WebviewView provider callback returned an invalid response"}};
            retire_runtime(node, "HOST_EXITED",
                           "WebviewView provider callback returned an invalid response");
            return SAO_AI_EDITOR_ERR_PROTOCOL;
        }
        Json rebound_snapshot;
        provider_status =
            node->request("webview.listProviders", Json::object(), 250, rebound_snapshot);
        if (provider_status != SAO_AI_EDITOR_OK) {
            out = Json{{"ok", false},
                       {"available", false},
                       {"applied", false},
                       {"viewId", view_id},
                       {"extensionId", owner},
                       {"generation", generation},
                       {"errorCode", runtime_status_error_code(provider_status)},
                       {"error", "WebviewView provider state became unavailable"}};
            if (activation_outcome_unknown(provider_status)) {
                retire_runtime(node, "HOST_EXITED",
                               "WebviewView provider state became unavailable");
            }
            return provider_status;
        }
        if (!valid_runtime_webview_inventory(rebound_snapshot)) {
            out = Json{{"ok", false},
                       {"available", false},
                       {"applied", false},
                       {"viewId", view_id},
                       {"extensionId", owner},
                       {"generation", generation},
                       {"errorCode", "WEBVIEW_PROVIDER_PROTOCOL_ERROR"},
                       {"error", "WebviewView provider inventory is invalid"}};
            retire_runtime(node, "HOST_EXITED", "WebviewView provider inventory is invalid");
            return SAO_AI_EDITOR_ERR_PROTOCOL;
        }
        Json rebound;
        if (!current_provider(rebound_snapshot, &owner, generation, rebound)) {
            out = Json{{"ok", false},
                       {"available", false},
                       {"applied", false},
                       {"viewId", view_id},
                       {"extensionId", owner},
                       {"generation", generation},
                       {"errorCode", "WEBVIEW_PROVIDER_STALE"},
                       {"error", "WebviewView provider generation is stale"}};
            return SAO_AI_EDITOR_OK;
        }
        const auto html = rebound.find("html");
        const auto options = rebound.find("options");
        if (html == rebound.end() || !html->is_string() ||
            html->get_ref<const std::string&>().size() > kMaximumExtensionMainBytes ||
            options == rebound.end() || !options->is_object() ||
            !rebound.value("errorCode", std::string{}).empty()) {
            out = Json{{"ok", false},
                       {"available", true},
                       {"applied", false},
                       {"viewId", view_id},
                       {"extensionId", owner},
                       {"generation", generation},
                       {"errorCode", rebound.value("errorCode", "WEBVIEW_PROVIDER_OUTPUT_INVALID")},
                       {"error", rebound.value("error", "WebviewView provider output is invalid")}};
            return SAO_AI_EDITOR_OK;
        }
        out = std::move(rebound);
        out["ok"] = true;
        out["available"] = true;
        out["applied"] = true;
        out["viewId"] = view_id;
        out["extensionId"] = owner;
        out["generation"] = generation;
        out["errorCode"] = "";
        out["error"] = "";
        return SAO_AI_EDITOR_OK;
    }
    if (!args.is_array() || args.size() > kMaximumTreeActionsPerItem) {
        out = Json{{"ok", false},
                   {"available", true},
                   {"applied", false},
                   {"errorCode", "EXTENSION_CALLBACK_INVALID"},
                   {"error", "extension command arguments must be an array of at most 64 values"}};
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    Json params{{"command", std::string(command_id)}, {"arguments", args}};
    const bool language_callback = command_id == "sao.internal.extapi.invoke" &&
        args.size() == 1 && args[0].is_object() && args[0].value("invoke", std::string{}) == "languages";
    const std::string owner = language_callback ? args[0].value("extensionId", std::string{}) : std::string{};
    const uint64_t generation = language_callback ? args[0].value("generation", uint64_t{}) : 0;
    if (language_callback && !generation_current(owner, generation)) return SAO_AI_EDITOR_ERR_NOT_FOUND;
    const int32_t status =
        node->request("commands.execute", params, timeout_ms == 0 ? 15000 : timeout_ms, out);
    if (language_callback && !generation_current(owner, generation)) {
        out = Json{{"message", "language provider generation retired"}};
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    if (activation_outcome_unknown(status)) {
        retire_runtime(node, "HOST_EXITED",
                       "extension host stopped after an indeterminate command callback");
    }
    return status;
}

int32_t ExtensionHost::register_native_command(std::string_view command_id,
                                               NativeCommandHandler handler, uint64_t owner) {
    if (command_id.empty() || command_id.size() > 256 ||
        command_id.find('\0') != std::string_view::npos || !valid_utf8(command_id) || !handler ||
        reserved_host_command(command_id)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    const std::string id(command_id);
    for (const auto& [extension_id, extension] : extensions_) {
        (void)extension_id;
        const Json commands = extension.contributions.value("commands", Json::array());
        if (commands.is_array() &&
            std::any_of(commands.begin(), commands.end(), [&](const Json& command) {
                return command.is_object() && command.value("command", std::string{}) == id;
            })) {
            return SAO_AI_EDITOR_ERR_BUSY;
        }
    }
    const auto found = native_commands_.find(id);
    if (found != native_commands_.end() && found->second.owner != owner &&
        (found->second.owner != 0 || owner != 0)) {
        return SAO_AI_EDITOR_ERR_BUSY;
    }
    native_commands_.insert_or_assign(id, NativeCommandRegistration{std::move(handler), owner});
    return SAO_AI_EDITOR_OK;
}

int32_t ExtensionHost::unregister_native_command(std::string_view command_id, uint64_t owner) {
    if (command_id.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    const auto found = native_commands_.find(std::string(command_id));
    if (found == native_commands_.end()) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    if (found->second.owner != owner && (found->second.owner != 0 || owner != 0)) {
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

int32_t ExtensionHost::restore_native_command(std::string_view command_id,
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
    const auto panel = params.is_object() ? params.find("panelId") : params.end();
    const auto view = params.is_object() ? params.find("viewId") : params.end();
    const bool has_panel = panel != params.end() && panel->is_string() &&
                           !panel->get_ref<const std::string&>().empty();
    const bool has_view =
        view != params.end() && view->is_string() && !view->get_ref<const std::string&>().empty();
    const auto valid_route = [](const Json::const_iterator& value, const Json& owner) {
        if (value == owner.end() || !value->is_string()) {
            return false;
        }
        const auto& id = value->get_ref<const std::string&>();
        return !id.empty() && id.size() <= 256 && id.find('\0') == std::string::npos &&
               valid_utf8(id);
    };
    if (!params.is_object() || (panel != params.end() && !panel->is_string()) ||
        (view != params.end() && !view->is_string()) ||
        (panel != params.end()) == (view != params.end()) || has_panel == has_view ||
        (has_panel && !valid_route(panel, params)) || (has_view && !valid_route(view, params)) ||
        !params.contains("message") || !valid_webview_message_payload(params)) {
        out = Json{{"ok", false},
                   {"available", false},
                   {"applied", false},
                   {"acknowledged", false},
                   {"errorCode", "WEBVIEW_MESSAGE_INVALID_ARGUMENT"},
                   {"error", "WebView message requires one valid panelId or viewId"}};
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::string route_key = has_panel ? "panelId" : "viewId";
    const std::string route_id = (has_panel ? panel : view)->get<std::string>();
    std::shared_ptr<NodeRuntime> node;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        node = node_runtime_;
    }
    if (!node || !node->alive()) {
        if (node) {
            retire_runtime(node, "HOST_EXITED", "extension host is not running");
        } else {
            std::lock_guard<std::mutex> guard(mutex_);
            mark_runtime_dead_locked("HOST_EXITED", "extension host is not running");
        }
        out = Json{{"ok", false},
                   {"available", false},
                   {"applied", false},
                   {"errorCode", "HOST_EXITED"},
                   {"error", "extension host is not running"}};
        return SAO_AI_EDITOR_ERR_IPC_CLOSED;
    }
    CallbackGuard callback_guard(callback_active_);
    if (!callback_guard.owns_lock()) {
        out = Json{{"ok", false},
                   {"available", true},
                   {"applied", false},
                   {"errorCode", "EXTENSION_CALLBACK_BUSY"},
                   {"error", "another extension callback is active"}};
        return SAO_AI_EDITOR_ERR_BUSY;
    }
    const int32_t status = node->request("webview.postToView", params, 5000, out);
    if (activation_outcome_unknown(status)) {
        retire_runtime(node, "HOST_EXITED", "extension host stopped during Webview delivery");
    } else if (status == SAO_AI_EDITOR_OK) {
        const auto ok_value = out.is_object() ? out.find("ok") : out.end();
        const auto available = out.is_object() ? out.find("available") : out.end();
        const auto applied = out.is_object() ? out.find("applied") : out.end();
        const auto acknowledged = out.is_object() ? out.find("acknowledged") : out.end();
        const auto error_code = out.is_object() ? out.find("errorCode") : out.end();
        const auto error = out.is_object() ? out.find("error") : out.end();
        const auto owner = out.is_object() ? out.find("extensionId") : out.end();
        const auto generation_value = out.is_object() ? out.find("generation") : out.end();
        uint64_t generation = 0;
        const bool boolean_shape = ok_value != out.end() && ok_value->is_boolean() &&
                                   available != out.end() && available->is_boolean() &&
                                   applied != out.end() && applied->is_boolean() &&
                                   acknowledged != out.end() && acknowledged->is_boolean();
        const bool response_ok = boolean_shape && ok_value->get<bool>();
        const bool response_available =
            available != out.end() && available->is_boolean() && available->get<bool>();
        const bool response_applied =
            applied != out.end() && applied->is_boolean() && applied->get<bool>();
        const bool response_acknowledged =
            acknowledged != out.end() && acknowledged->is_boolean() && acknowledged->get<bool>();
        const bool error_shape = error_code != out.end() && error_code->is_string() &&
                                 error != out.end() && error->is_string() &&
                                 error_code->get_ref<const std::string&>().size() <= 128 &&
                                 error->get_ref<const std::string&>().size() <= 4096 &&
                                 valid_utf8(error_code->get_ref<const std::string&>()) &&
                                 valid_utf8(error->get_ref<const std::string&>());
        const bool owner_shape = owner != out.end() && owner->is_string() &&
                                 valid_simple_id(owner->get_ref<const std::string&>()) &&
                                 generation_value != out.end() &&
                                 safe_nonnegative_integer(*generation_value, generation) &&
                                 generation != 0;
        bool current_owner = false;
        if (owner_shape) {
            std::lock_guard<std::mutex> guard(mutex_);
            const auto found = extensions_.find(owner->get_ref<const std::string&>());
            current_owner = found != extensions_.end() && found->second.activated &&
                            found->second.runtime_state_known &&
                            found->second.operation == ExtensionOperation::idle &&
                            found->second.operation_generation == generation;
        }
        const bool state_shape =
            boolean_shape && response_applied == response_acknowledged &&
            (response_ok ? response_available && response_applied && error_shape &&
                               error_code->get_ref<const std::string&>().empty() &&
                               error->get_ref<const std::string&>().empty()
                         : !response_applied && error_shape &&
                               !error_code->get_ref<const std::string&>().empty() &&
                               !error->get_ref<const std::string&>().empty());
        if (!valid_webview_message_payload(out) || !state_shape || !owner_shape || !current_owner ||
            !exact_string_member(out, route_key, route_id)) {
            out = Json{{"ok", false},
                       {"available", false},
                       {"applied", false},
                       {"acknowledged", false},
                       {"errorCode", "WEBVIEW_MESSAGE_PROTOCOL_ERROR"},
                       {"error", "WebView message host returned an invalid response"}};
            retire_runtime(node, "HOST_EXITED",
                           "WebView message host returned an invalid response");
            return SAO_AI_EDITOR_ERR_PROTOCOL;
        }
    }
    return status;
}

Json ExtensionHost::snapshot() {
    Json inventory;
    const int32_t status = list_extensions(inventory);
    if (status != SAO_AI_EDITOR_OK || !inventory.is_object()) {
        return Json{{"ok", false},
                    {"available", false},
                    {"errorCode", "EXTENSION_SNAPSHOT_FAILED"},
                    {"error", "extension inventory snapshot failed"}};
    }
    std::string entry_script;
    std::shared_ptr<NodeRuntime> node;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        entry_script = boot_options_.entry_script;
        node = node_runtime_;
    }
    inventory["entryScript"] = std::move(entry_script);
    inventory["nodeStderrTail"] = node ? node->stderr_tail() : std::string{};
    return inventory;
}

void ExtensionHost::deactivate_all() {
    std::shared_ptr<NodeRuntime> node;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        node = node_runtime_;
    }
    CallbackGuard callback_guard(callback_active_);
    if (callback_guard.owns_lock() && node && node->alive()) {
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

} // namespace sao::ai_editor::native
