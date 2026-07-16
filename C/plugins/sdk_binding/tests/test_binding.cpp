#include "sao/plugins/sdk_binding/binding_common.h"
#include "sao/plugins/sdk_binding/binding_emma.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <stdexcept>

using namespace sao::plugins::sdk_binding;

namespace {

struct fixture_state {
    int loads = 0;
    int unloads = 0;
    int invokes = 0;
    int dispatches = 0;
    bool available = true;
};

struct fixture_plugin {
    fixture_state* state = nullptr;
};

bool SAO_PLUGINS_CALL available(void* user_data) {
    return static_cast<fixture_state*>(user_data)->available;
}

int32_t SAO_PLUGINS_CALL load(void* context,
                              void* runtime,
                              void** out_plugin,
                              void* user_data) {
    if (context == nullptr || runtime == nullptr || out_plugin == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    auto* state = static_cast<fixture_state*>(user_data);
    ++state->loads;
    *out_plugin = new fixture_plugin{state};
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL unload(void* plugin, void* /*user_data*/) {
    auto* instance = static_cast<fixture_plugin*>(plugin);
    ++instance->state->unloads;
    delete instance;
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL invoke(void* plugin,
                                const char* method,
                                const uint8_t* /*arguments*/,
                                size_t /*arguments_size*/,
                                uint8_t* output,
                                size_t output_capacity,
                                size_t* required,
                                char* error,
                                size_t error_capacity,
                                void* /*user_data*/) {
    auto* instance = static_cast<fixture_plugin*>(plugin);
    ++instance->state->invokes;
    if (std::strcmp(method, "fail") == 0) {
        std::snprintf(error, error_capacity, "%s", "fixture error");
        return SAO_ERR_OS_CALL_FAILED;
    }
    constexpr char result[] = "{\"ok\":true}";
    *required = sizeof(result);
    if (output == nullptr || output_capacity < sizeof(result)) {
        return SAO_ERR_BUFFER_TOO_SMALL;
    }
    std::memcpy(output, result, sizeof(result));
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL dispatch(language_binding_operation,
                                  language_binding_request*,
                                  void* user_data) {
    ++static_cast<fixture_state*>(user_data)->dispatches;
    return SAO_OK;
}

int32_t throwing_barrier(void*) {
    throw std::runtime_error("barrier fixture");
}

language_host_adapter_vtable fixture_adapter(language_host_kind language,
                                              fixture_state* state,
                                              uint32_t flags = 0) {
    language_host_adapter_vtable adapter{};
    adapter.flags = flags;
    adapter.language = language;
    adapter.available = &available;
    adapter.load_plugin = &load;
    adapter.unload_plugin = &unload;
    adapter.invoke = &invoke;
    adapter.dispatch = &dispatch;
    adapter.user_data = state;
    return adapter;
}

} // namespace

int main() {
    assert(json_node::from_string("{\"value\":7}").to_string() ==
           "{\"value\":7}");

    char* error = nullptr;
    assert(sao_plugins_binding_barrier(&throwing_barrier, nullptr, &error) ==
           SAO_ERR_OS_CALL_FAILED);
    assert(error != nullptr && std::strcmp(error, "barrier fixture") == 0);
    sao_plugins_binding_free_error(error);

    static int context = 0;
    static int runtime = 0;
    plugin_binding_handle_t plugin = nullptr;
    assert(sao_plugins_binding_emma_activate(
               reinterpret_cast<plugin_context_ptr>(&context),
               reinterpret_cast<emma_interpreter_ptr>(&runtime), &plugin) ==
           sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    assert(plugin == nullptr);

    fixture_state python_state{};
    auto python = fixture_adapter(language_host_kind::python, &python_state);
    assert(sao_plugins_binding_register_language_host(&python) ==
           sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    python.flags = SAO_LANGUAGE_HOST_PROVIDER_ISOLATED_PYTHON_ABI;
    assert(sao_plugins_binding_register_language_host(&python) == SAO_OK);
    assert(sao_plugins_binding_unregister_language_host(
               language_host_kind::python) == SAO_OK);

    fixture_state state{};
    const auto emma = fixture_adapter(language_host_kind::emma, &state);
    assert(sao_plugins_binding_register_language_host(&emma) == SAO_OK);
    assert(sao_plugins_binding_language_host_available(
        language_host_kind::emma));
    assert(sao_plugins_binding_emma_activate(
               reinterpret_cast<plugin_context_ptr>(&context),
               reinterpret_cast<emma_interpreter_ptr>(&runtime), &plugin) ==
           SAO_OK);
    assert(plugin != nullptr && state.loads == 1);

    size_t required = 0;
    assert(sao_plugins_binding_plugin_invoke(
               plugin, "run", nullptr, 0, nullptr, 0, &required) ==
           SAO_ERR_BUFFER_TOO_SMALL);
    assert(required == sizeof("{\"ok\":true}"));
    char output[32]{};
    assert(sao_plugins_binding_plugin_invoke(
               plugin, "run", nullptr, 0,
               reinterpret_cast<uint8_t*>(output), sizeof(output),
               &required) == SAO_OK);
    assert(std::strcmp(output, "{\"ok\":true}") == 0);

    assert(sao_plugins_binding_plugin_invoke(
               plugin, "fail", nullptr, 0, nullptr, 0, &required) ==
           SAO_ERR_OS_CALL_FAILED);
    assert(sao_plugins_binding_plugin_last_error(
               plugin, nullptr, 0, &required) == SAO_ERR_BUFFER_TOO_SMALL);
    char last_error[32]{};
    assert(sao_plugins_binding_plugin_last_error(
               plugin, last_error, sizeof(last_error), &required) == SAO_OK);
    assert(std::strcmp(last_error, "fixture error") == 0);

    language_binding_request request{};
    assert(sao_plugins_binding_dispatch_provider(
               language_host_kind::emma,
               language_binding_operation::method_table, &request) == SAO_OK);
    assert(state.dispatches == 1);

    assert(sao_plugins_binding_unregister_language_host(
               language_host_kind::emma) == SAO_OK);
    assert(sao_plugins_binding_emma_deactivate(plugin) == SAO_OK);
    assert(state.unloads == 1 && state.invokes == 3);

    std::printf("sdk_binding provider fixture passed\n");
    return 0;
}
