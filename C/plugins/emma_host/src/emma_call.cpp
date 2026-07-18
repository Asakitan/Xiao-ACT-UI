// emma_call.cpp — Emma 脚本加载 + 生命周期 hook 分派 实装
//
// 对齐 Python emma_runtime.py EmmaRuntime.load_script:
//   1. read utf-8 file → tokenize → parse → interp.execute
//   2. abstract on_load / on_enable / on_disable / on_unload
//
// 加入 host-level in-memory 便利函数 (test / repl 用):
//   sao_plugins_emma_execute_source(interp, source, source_len, out_error)
//   sao_plugins_emma_call_by_name(interp, name, args, argc, out_result, out_error)

#include "sao/plugins/emma_host/emma_call.h"
#include "sao/plugins/emma_host/emma_lexer.h"
#include "sao/plugins/emma_host/emma_parser.h"
#include "sao/plugins/emma_host/emma_interpreter.h"
#include "sao/plugins/emma_host/emma_stdlib.h"
#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_install.h"
#include "sao/plugins/sdk_binding/binding_emma.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace sao::plugins::emma_host {

// 外部符号 (定义在 emma_interpreter.cpp)
std::string emma_value_to_string(const emma_value& v);

struct emma_plugin_runtime {
    std::string plugin_id;
    std::filesystem::path entry_path;
    ast_pool pool;
    std::unique_ptr<interpreter> interp;
    emma_value context_value = nullptr;
    std::mutex invocation_mutex;
};

// 句柄记录作为 tombstone 保留到进程退出，重复/过期调用因此不会解引用
// 已释放内存；真正占资源的 runtime 在 unload 成功时立即销毁。
struct emma_plugin_s {
    enum class state {
        open,
        closing,
        destroyed,
    };

    std::unique_ptr<emma_plugin_runtime> runtime;
    state lifecycle = state::open;
    size_t active_operations = 0;
};

namespace {

using json = nlohmann::json;
using loader_context_t = sao::plugins::loader::plugin_context_t;

struct direct_plugin_registry {
    std::mutex mutex;
    std::unordered_map<emma_plugin_handle_t,
                       std::unique_ptr<emma_plugin_s>> plugins;
};

direct_plugin_registry& direct_plugins() {
    static direct_plugin_registry registry;
    return registry;
}

class direct_operation_lease {
public:
    direct_operation_lease() = default;
    ~direct_operation_lease() { reset(); }

    direct_operation_lease(const direct_operation_lease&) = delete;
    direct_operation_lease& operator=(const direct_operation_lease&) = delete;

    emma_plugin_runtime& runtime() const noexcept { return *runtime_; }

    void assign(emma_plugin_s* plugin,
                emma_plugin_runtime* runtime) noexcept {
        plugin_ = plugin;
        runtime_ = runtime;
    }

    void reset() noexcept {
        if (plugin_ == nullptr) return;
        try {
            auto& registry = direct_plugins();
            std::lock_guard lock(registry.mutex);
            if (plugin_->active_operations > 0) {
                --plugin_->active_operations;
            }
        } catch (...) {
        }
        plugin_ = nullptr;
        runtime_ = nullptr;
    }

private:
    emma_plugin_s* plugin_ = nullptr;
    emma_plugin_runtime* runtime_ = nullptr;
};

int32_t acquire_direct_plugin(emma_plugin_handle_t plugin,
                              direct_operation_lease& lease) {
    auto& registry = direct_plugins();
    std::lock_guard lock(registry.mutex);
    const auto found = registry.plugins.find(plugin);
    if (found == registry.plugins.end() || found->second == nullptr ||
        found->second->runtime == nullptr ||
        found->second->lifecycle == emma_plugin_s::state::destroyed) {
        return SAO_ERR_HANDLE_INVALID;
    }
    if (found->second->lifecycle != emma_plugin_s::state::open) {
        return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
    }
    ++found->second->active_operations;
    lease.assign(found->second.get(), found->second->runtime.get());
    return SAO_OK;
}

template <typename Callback>
int32_t with_direct_plugin(emma_plugin_handle_t plugin,
                           Callback&& callback) {
    direct_operation_lease lease;
    const int32_t status = acquire_direct_plugin(plugin, lease);
    if (status != SAO_OK) return status;
    auto& runtime = lease.runtime();
    std::lock_guard invocation_lock(runtime.invocation_mutex);
    return callback(runtime);
}

std::shared_ptr<callable> make_host_callable(
    const char* name,
    std::function<emma_value(std::vector<emma_value>)> implementation) {
    auto result = std::make_shared<callable>();
    result->name = name;
    result->host_impl = std::move(implementation);
    return result;
}

std::string require_string(const std::vector<emma_value>& arguments,
                           size_t index,
                           const char* method) {
    if (index >= arguments.size() ||
        !std::holds_alternative<std::string>(arguments[index])) {
        throw std::runtime_error(std::string(method) +
                                 ": expected string argument");
    }
    return std::get<std::string>(arguments[index]);
}

bool value_to_json(const emma_value& value, json& output, size_t depth = 0) {
    if (depth > 64) return false;
    if (std::holds_alternative<std::nullptr_t>(value)) {
        output = nullptr;
        return true;
    }
    if (const auto* boolean = std::get_if<bool>(&value)) {
        output = *boolean;
        return true;
    }
    if (const auto* integer = std::get_if<int64_t>(&value)) {
        output = *integer;
        return true;
    }
    if (const auto* number = std::get_if<double>(&value)) {
        if (!std::isfinite(*number)) return false;
        output = *number;
        return true;
    }
    if (const auto* string = std::get_if<std::string>(&value)) {
        output = *string;
        return true;
    }
    if (const auto* list =
            std::get_if<std::shared_ptr<emma_list>>(&value)) {
        output = json::array();
        if (!*list) return true;
        for (const auto& item : (*list)->items) {
            json converted;
            if (!value_to_json(item, converted, depth + 1)) return false;
            output.push_back(std::move(converted));
        }
        return true;
    }
    if (const auto* dictionary =
            std::get_if<std::shared_ptr<emma_dict>>(&value)) {
        output = json::object();
        if (!*dictionary) return true;
        for (const auto& [key, item] : (*dictionary)->items) {
            json converted;
            if (!value_to_json(item, converted, depth + 1)) return false;
            output[key] = std::move(converted);
        }
        return true;
    }
    return false;
}

bool json_to_value(const json& input, emma_value& output, size_t depth = 0) {
    if (depth > 64) return false;
    if (input.is_null()) {
        output = nullptr;
        return true;
    }
    if (input.is_boolean()) {
        output = input.get<bool>();
        return true;
    }
    if (input.is_number_integer()) {
        output = input.get<int64_t>();
        return true;
    }
    if (input.is_number_unsigned()) {
        const auto value = input.get<uint64_t>();
        if (value > static_cast<uint64_t>(
                        std::numeric_limits<int64_t>::max())) {
            return false;
        }
        output = static_cast<int64_t>(value);
        return true;
    }
    if (input.is_number_float()) {
        const double value = input.get<double>();
        if (!std::isfinite(value)) return false;
        output = value;
        return true;
    }
    if (input.is_string()) {
        output = input.get<std::string>();
        return true;
    }
    if (input.is_array()) {
        auto list = std::make_shared<emma_list>();
        list->items.reserve(input.size());
        for (const auto& item : input) {
            emma_value converted = nullptr;
            if (!json_to_value(item, converted, depth + 1)) return false;
            list->items.push_back(std::move(converted));
        }
        output = std::move(list);
        return true;
    }
    if (input.is_object()) {
        auto dictionary = std::make_shared<emma_dict>();
        for (auto iterator = input.begin(); iterator != input.end();
             ++iterator) {
            emma_value converted = nullptr;
            if (!json_to_value(iterator.value(), converted, depth + 1)) {
                return false;
            }
            dictionary->items.emplace(iterator.key(), std::move(converted));
        }
        output = std::move(dictionary);
        return true;
    }
    return false;
}

std::string serialize_value_or_throw(const emma_value& value) {
    json converted;
    if (!value_to_json(value, converted)) {
        throw std::runtime_error("Emma value is not JSON serializable");
    }
    return converted.dump();
}

emma_value make_native_context(loader_context_t* context) {
    auto wrapper = std::make_shared<emma_dict>();
    wrapper->items.emplace(
        "log", make_host_callable(
                   "ctx.log", [context](std::vector<emma_value> arguments) {
                       if (arguments.empty()) {
                           throw std::runtime_error(
                               "ctx.log: expected at least one argument");
                       }
                       std::string message;
                       for (size_t index = 0; index < arguments.size();
                            ++index) {
                           if (index != 0) message.push_back(' ');
                           message += emma_value_to_string(arguments[index]);
                       }
                       sao::plugins::loader::sao_plugins_ctx_log(
                           context, message.c_str());
                       return emma_value(nullptr);
                   }));
    wrapper->items.emplace(
        "register_ui_panel",
        make_host_callable(
            "ctx.register_ui_panel",
            [context](std::vector<emma_value> arguments) {
                const std::string panel_id = require_string(
                    arguments, 0, "ctx.register_ui_panel");
                if (arguments.size() < 2) {
                    throw std::runtime_error(
                        "ctx.register_ui_panel: expected metadata");
                }
                if ((arguments.size() > 2 &&
                     !std::holds_alternative<std::nullptr_t>(arguments[2])) ||
                    (arguments.size() > 3 &&
                     !std::holds_alternative<std::nullptr_t>(arguments[3]))) {
                    throw std::runtime_error(
                        "ctx.register_ui_panel callbacks are unsupported by "
                        "the canonical loader context");
                }
                const std::string metadata =
                    serialize_value_or_throw(arguments[1]);
                const int32_t status =
                    sao::plugins::loader::sao_plugins_ctx_register_ui_panel(
                        context, panel_id.c_str(), metadata.c_str(), nullptr,
                        nullptr, nullptr);
                if (status != SAO_OK) {
                    throw std::runtime_error(
                        "ctx.register_ui_panel failed with status " +
                        std::to_string(status));
                }
                return emma_value(true);
            }));
    return wrapper;
}

bool has_callable_member(const emma_value& value, const char* name) {
    const auto* dictionary =
        std::get_if<std::shared_ptr<emma_dict>>(&value);
    if (dictionary == nullptr || !*dictionary) return false;
    const auto found = (*dictionary)->items.find(name);
    return found != (*dictionary)->items.end() &&
           std::holds_alternative<std::shared_ptr<callable>>(found->second) &&
           std::get<std::shared_ptr<callable>>(found->second) != nullptr;
}

int32_t install_context(emma_plugin_runtime* plugin,
                        loader_context_t* context) {
    const int32_t binding_status =
        sao::plugins::sdk_binding::sao_plugins_binding_emma_register_ctx(
            reinterpret_cast<
                sao::plugins::sdk_binding::emma_interpreter_ptr>(
                plugin->interp.get()),
            reinterpret_cast<sao::plugins::sdk_binding::plugin_context_ptr>(
                context));
    if (binding_status == SAO_OK) {
        const emma_value candidate = plugin->interp->get_global("ctx");
        if (has_callable_member(candidate, "log") &&
            has_callable_member(candidate, "register_ui_panel")) {
            plugin->context_value = candidate;
            return SAO_OK;
        }
    }
    plugin->context_value = make_native_context(context);
    plugin->interp->register_global("ctx", plugin->context_value);
    return SAO_OK;
}

int32_t call_named(emma_plugin_runtime* plugin,
                   const char* name,
                   std::vector<emma_value> arguments,
                   emma_value* out_result,
                   bool missing_is_success) {
    if (plugin == nullptr || name == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    auto function = plugin->interp->get_function(name);
    if (!function) {
        if (out_result != nullptr) *out_result = nullptr;
        return missing_is_success ? SAO_OK : SAO_ERR_HANDLE_INVALID;
    }
    std::string error;
    emma_value result = plugin->interp->call_function(
        function, std::move(arguments), error);
    if (!error.empty()) return SAO_ERR_OS_CALL_FAILED;
    if (out_result != nullptr) *out_result = std::move(result);
    return SAO_OK;
}

} // namespace

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_load_script(const wchar_t* plugin_dir,
                             const char* entry_relative,
                             const char* plugin_id_utf8,
                             void* ctx_ptr,
                             emma_plugin_handle_t* out_plugin) {
    if (out_plugin != nullptr) *out_plugin = nullptr;
    if (out_plugin == nullptr || plugin_dir == nullptr ||
        plugin_dir[0] == L'\0' || entry_relative == nullptr ||
        entry_relative[0] == '\0' || plugin_id_utf8 == nullptr ||
        plugin_id_utf8[0] == '\0' || ctx_ptr == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        std::error_code error;
        const auto base =
            std::filesystem::weakly_canonical(plugin_dir, error);
        if (error || !std::filesystem::is_directory(base, error)) {
            return SAO_ERR_HANDLE_INVALID;
        }
        const auto entry_path = std::filesystem::weakly_canonical(
            base / std::filesystem::u8path(entry_relative), error);
        if (error ||
            !sao::plugins::loader::path_is_within_base(
                base.wstring(), entry_path.wstring()) ||
            !std::filesystem::is_regular_file(entry_path, error)) {
            return SAO_ERR_HANDLE_INVALID;
        }

        std::ifstream input(entry_path, std::ios::binary);
        if (!input) return SAO_ERR_OS_CALL_FAILED;
        std::stringstream stream;
        stream << input.rdbuf();
        if (!input.good() && !input.eof()) return SAO_ERR_OS_CALL_FAILED;

        auto plugin = std::make_unique<emma_plugin_s>();
        plugin->runtime = std::make_unique<emma_plugin_runtime>();
        auto& runtime = *plugin->runtime;
        runtime.plugin_id = plugin_id_utf8;
        runtime.entry_path = entry_path;
        runtime.interp = std::make_unique<interpreter>();
        runtime.interp->set_pool(&runtime.pool);
        int32_t status = sao_plugins_emma_install_stdlib(
            runtime.interp.get());
        if (status != SAO_OK) return status;
        status = sao_plugins_emma_install_io_stdlib(runtime.interp.get(),
                                                     base.c_str());
        if (status != SAO_OK) return status;
        status = install_context(
            &runtime, static_cast<loader_context_t*>(ctx_ptr));
        if (status != SAO_OK) return status;

        std::vector<token> tokens = tokenize_source(stream.str());
        parser source_parser(tokens, &runtime.pool);
        std::vector<node_id> statements;
        std::string parse_error;
        status = source_parser.parse_program(statements, parse_error);
        if (status != SAO_OK) return status;
        status = runtime.interp->execute(statements, parse_error);
        if (status != SAO_OK) return status;

        const emma_plugin_handle_t handle = plugin.get();
        auto& registry = direct_plugins();
        {
            std::lock_guard lock(registry.mutex);
            registry.plugins.emplace(handle, std::move(plugin));
        }
        *out_plugin = handle;
        return SAO_OK;
    } catch (...) {
        *out_plugin = nullptr;
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_call_on_load(emma_plugin_handle_t plugin) {
    if (plugin == nullptr) return SAO_ERR_HANDLE_INVALID;
    try {
        return with_direct_plugin(
            plugin, [](emma_plugin_runtime& runtime) {
                return call_named(&runtime, "on_load",
                                  {runtime.context_value}, nullptr, true);
            });
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_call_on_enable(emma_plugin_handle_t plugin) {
    if (plugin == nullptr) return SAO_ERR_HANDLE_INVALID;
    try {
        return with_direct_plugin(
            plugin, [](emma_plugin_runtime& runtime) {
                return call_named(&runtime, "on_enable", {}, nullptr, true);
            });
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_call_on_disable(emma_plugin_handle_t plugin) {
    if (plugin == nullptr) return SAO_ERR_HANDLE_INVALID;
    try {
        return with_direct_plugin(
            plugin, [](emma_plugin_runtime& runtime) {
                return call_named(&runtime, "on_disable", {}, nullptr, true);
            });
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_call_on_unload(emma_plugin_handle_t plugin, bool* out_allow_unload) {
    if (plugin == nullptr) return SAO_ERR_HANDLE_INVALID;
    if (out_allow_unload) *out_allow_unload = true;
    try {
        return with_direct_plugin(
            plugin, [out_allow_unload](emma_plugin_runtime& runtime) {
                emma_value result = nullptr;
                const int32_t status =
                    call_named(&runtime, "on_unload", {}, &result, true);
                if (status == SAO_OK && out_allow_unload != nullptr &&
                    std::holds_alternative<bool>(result) &&
                    !std::get<bool>(result)) {
                    *out_allow_unload = false;
                }
                return status;
            });
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_call_hook(emma_plugin_handle_t plugin,
                           const char* hook_name,
                           const char* args_json_utf8,
                           char** out_result_json_utf8) {
    if (plugin == nullptr || hook_name == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    if (out_result_json_utf8) *out_result_json_utf8 = nullptr;
    try {
        return with_direct_plugin(
            plugin, [&](emma_plugin_runtime& runtime) -> int32_t {
                std::vector<emma_value> arguments;
                if (args_json_utf8 != nullptr && args_json_utf8[0] != '\0') {
                    const json parsed =
                        json::parse(args_json_utf8, nullptr, false);
                    if (parsed.is_discarded() || !parsed.is_array()) {
                        return SAO_ERR_INVALID_ARGUMENT;
                    }
                    arguments.reserve(parsed.size());
                    for (const auto& item : parsed) {
                        emma_value converted = nullptr;
                        if (!json_to_value(item, converted)) {
                            return sao::plugins::loader::
                                SAO_PLUGINS_ERR_UNSUPPORTED;
                        }
                        arguments.push_back(std::move(converted));
                    }
                }
                emma_value result = nullptr;
                const int32_t status = call_named(
                    &runtime, hook_name, std::move(arguments), &result, false);
                if (status != SAO_OK || out_result_json_utf8 == nullptr) {
                    return status;
                }
                json serialized;
                if (!value_to_json(result, serialized)) {
                    return sao::plugins::loader::
                        SAO_PLUGINS_ERR_UNSUPPORTED;
                }
                const std::string text = serialized.dump();
                auto* buffer =
                    static_cast<char*>(std::malloc(text.size() + 1));
                if (buffer == nullptr) return SAO_ERR_OS_CALL_FAILED;
                std::memcpy(buffer, text.data(), text.size());
                buffer[text.size()] = '\0';
                *out_result_json_utf8 = buffer;
                return SAO_OK;
            });
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_emma_has_hook(emma_plugin_handle_t plugin, const char* hook_name) {
    if (plugin == nullptr || hook_name == nullptr) return false;
    try {
        bool result = false;
        const int32_t status = with_direct_plugin(
            plugin, [&](emma_plugin_runtime& runtime) {
                result = static_cast<bool>(
                    runtime.interp->get_function(hook_name));
                return SAO_OK;
            });
        return status == SAO_OK && result;
    } catch (...) {
        return false;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_with_interpreter(emma_plugin_handle_t plugin,
                                  emma_interpreter_callback_t callback,
                                  void* user_data) {
    if (callback == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    try {
        return with_direct_plugin(
            plugin, [&](emma_plugin_runtime& runtime) {
                return callback(runtime.interp.get(), user_data);
            });
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_unload_script(emma_plugin_handle_t plugin) {
    if (plugin == nullptr) return SAO_ERR_HANDLE_INVALID;
    try {
        std::unique_ptr<emma_plugin_runtime> runtime;
        auto& registry = direct_plugins();
        {
            std::lock_guard lock(registry.mutex);
            const auto found = registry.plugins.find(plugin);
            if (found == registry.plugins.end() ||
                found->second == nullptr ||
                found->second->runtime == nullptr ||
                found->second->lifecycle ==
                    emma_plugin_s::state::destroyed) {
                return SAO_ERR_HANDLE_INVALID;
            }
            found->second->lifecycle = emma_plugin_s::state::closing;
            if (found->second->active_operations != 0) {
                return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
            }
            found->second->lifecycle = emma_plugin_s::state::destroyed;
            runtime = std::move(found->second->runtime);
        }
        if (runtime == nullptr) {
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        }
        runtime.reset();
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

// ── Wave 3 host-level in-memory 便利函数 ──
//
// task 描述里说的 sao_emma_host_execute/call_function 走这个门。签名式对齐:
//   sao_plugins_emma_execute_source(interp, source, source_len, out_result_utf8)
//   sao_plugins_emma_call_by_name(interp, fn_name, out_result_utf8)
//
// 供 test / repl 用。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_execute_source(interpreter* interp,
                                const char* source_utf8,
                                size_t source_len,
                                ast_pool* out_pool,
                                char** out_error_utf8) {
    if (out_error_utf8) *out_error_utf8 = nullptr;
    if (interp == nullptr || source_utf8 == nullptr || out_pool == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        interp->set_pool(out_pool);
        std::vector<token> tokens =
            tokenize_source(std::string_view(source_utf8, source_len));
        parser source_parser(tokens, out_pool);
        std::vector<node_id> statements;
        std::string error;
        int32_t status = source_parser.parse_program(statements, error);
        if (status == SAO_OK) {
            status = interp->execute(statements, error);
        }
        if (status != SAO_OK && out_error_utf8 != nullptr) {
            auto* buffer =
                static_cast<char*>(std::malloc(error.size() + 1));
            if (buffer == nullptr) return SAO_ERR_OS_CALL_FAILED;
            std::memcpy(buffer, error.data(), error.size());
            buffer[error.size()] = '\0';
            *out_error_utf8 = buffer;
        }
        return status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_emma_free_string(char* s) {
    if (s != nullptr) std::free(s);
}

} // namespace sao::plugins::emma_host
