// binding_python.cpp — Python SDK binding provider facade
//
// 兼容入口转发到 Python provider；activate/deactivate 复用共享插件生命周期。

#include "sao/plugins/sdk_binding/binding_python.h"

#include <cstring>

namespace sao::plugins::sdk_binding {

// forward from binding_common.cpp
struct plugin_binding_s;
namespace detail {
plugin_binding_handle_t make_binding(void* ctx,
                                     void* lang_state,
                                     int lang);
void free_binding(plugin_binding_handle_t plugin);
} // namespace detail

// 保留旧签名并转发到 provider。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_python_register_module(void) {
    language_binding_request request{};
    return sao_plugins_binding_dispatch_provider(
        language_host_kind::python,
        language_binding_operation::module_init,
        &request);
}

extern "C" SAO_PLUGINS_API PyObject* SAO_PLUGINS_CALL
sao_plugins_binding_python_wrap_ctx(plugin_context_ptr ctx) {
    if (ctx == nullptr) return nullptr;
    language_binding_request request{};
    request.context = reinterpret_cast<void*>(ctx);
    request.out_object = reinterpret_cast<void**>(&request.value);
    return sao_plugins_binding_dispatch_provider(
               language_host_kind::python,
               language_binding_operation::context_bind,
               &request) == SAO_OK
               ? reinterpret_cast<PyObject*>(request.value)
               : nullptr;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_python_arg_to_utf8(PyObject* arg, char** out_utf8) {
    if (out_utf8 != nullptr) *out_utf8 = nullptr;
    if (arg == nullptr || out_utf8 == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    language_binding_request request{};
    request.value = arg;
    request.out_object = reinterpret_cast<void**>(out_utf8);
    return sao_plugins_binding_dispatch_provider(
        language_host_kind::python,
        language_binding_operation::value_to_json,
        &request);
}

extern "C" SAO_PLUGINS_API PyObject* SAO_PLUGINS_CALL
sao_plugins_binding_python_json_to_pyobject(const char* utf8_json) {
    if (utf8_json == nullptr) return nullptr;
    language_binding_request request{};
    request.input = reinterpret_cast<const uint8_t*>(utf8_json);
    request.input_size = std::strlen(utf8_json);
    request.out_object = reinterpret_cast<void**>(&request.value);
    return sao_plugins_binding_dispatch_provider(
               language_host_kind::python,
               language_binding_operation::json_to_value,
               &request) == SAO_OK
               ? reinterpret_cast<PyObject*>(request.value)
               : nullptr;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_python_pyobject_to_json(PyObject* obj, char** out_json_utf8) {
    if (out_json_utf8 != nullptr) *out_json_utf8 = nullptr;
    if (obj == nullptr || out_json_utf8 == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    language_binding_request request{};
    request.value = obj;
    request.out_object = reinterpret_cast<void**>(out_json_utf8);
    return sao_plugins_binding_dispatch_provider(
        language_host_kind::python,
        language_binding_operation::value_to_json,
        &request);
}

// Typed menu/action callbacks are implemented by python_host against the
// loader context. These legacy declarations remain linkable but are not a
// second Python callback surface.
extern "C" PyObject* py_ctx_register_menu_category(PyObject*, PyObject*) {
    return nullptr;
}

extern "C" PyObject* py_ctx_register_menu_surface(PyObject*, PyObject*) {
    return nullptr;
}

extern "C" PyObject* py_ctx_register_action_handler(PyObject*, PyObject*) {
    return nullptr;
}

// 共享插件 binding 的 activate / deactivate

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_python_activate(plugin_context_ptr plugin_ctx,
                                    void* py_host_state,
                                    plugin_binding_handle_t* out_plugin) {
    if (out_plugin == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *out_plugin = nullptr;
    return sao_plugins_binding_plugin_load(
        language_host_kind::python, reinterpret_cast<void*>(plugin_ctx),
        py_host_state, out_plugin);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_python_deactivate(plugin_binding_handle_t plugin) {
    if (plugin == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    return sao_plugins_binding_plugin_unload(plugin);
}

} // namespace sao::plugins::sdk_binding
