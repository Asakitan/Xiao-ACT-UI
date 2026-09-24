// py_call.cpp — legacy 直接脚本入口 (load_script / unload_script)
//
// 这两个入口是 v1 时代的无-host 句柄形态：进程内 CPython singleton
// (sao_plugins_pyhost_init) 建立后，对 singleton 借用一个可复用 host
// handle 派发到 py_host.cpp 的正规 load_plugin / unload_plugin，
// 生命周期协议（manifest 兼容、sys.path 前插/回滚、sys.modules 摘除、
// hook DECREF、SDK ctx teardown、GIL 串行化）全部由那条正规路径执行。
// 非 embed 构建返回 SAO_ERR_NOT_IMPLEMENTED 作为明确的 capability 信号；
// embed 编译但 singleton 未初始化时返回 SAO_ERR_NOT_INITIALIZED。
#if defined(SAO_HAS_PYTHON_EMBED)
#define PY_SSIZE_T_CLEAN
#include "sao/plugins/python_host/py_release_abi.h"
#endif

#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/python_host/py_call.h"
#include "sao/plugins/python_host/py_error.h"
#include "sao/plugins/python_host/py_host.h"
#include "sao/plugins/sdk_binding/binding_common.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace sao::plugins::python_host {

namespace {

bool valid_utf8(std::string_view value) noexcept {
    size_t offset = 0;
    while (offset < value.size()) {
        const auto first = static_cast<unsigned char>(value[offset]);
        size_t width = 0;
        uint32_t codepoint = 0;
        if (first <= 0x7fU) {
            width = 1;
            codepoint = first;
        } else if (first >= 0xc2U && first <= 0xdfU) {
            width = 2;
            codepoint = first & 0x1fU;
        } else if (first >= 0xe0U && first <= 0xefU) {
            width = 3;
            codepoint = first & 0x0fU;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            width = 4;
            codepoint = first & 0x07U;
        } else {
            return false;
        }
        if (offset + width > value.size())
            return false;
        for (size_t index = 1; index < width; ++index) {
            const auto next = static_cast<unsigned char>(value[offset + index]);
            if ((next & 0xc0U) != 0x80U)
                return false;
            codepoint = (codepoint << 6U) | (next & 0x3fU);
        }
        if ((width == 2 && codepoint < 0x80U) || (width == 3 && codepoint < 0x800U) ||
            (width == 4 && codepoint < 0x10000U) || codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            return false;
        }
        offset += width;
    }
    return true;
}

bool valid_member_name(const char* value) noexcept {
    if (value == nullptr || *value == '\0')
        return false;
    size_t length = 0;
    while (length <= 4096 && value[length] != '\0')
        ++length;
    return length != 0 && length <= 4096 && valid_utf8({value, length});
}

} // namespace

#if defined(SAO_HAS_PYTHON_EMBED)
namespace detail {

// Defined in py_host.cpp: publishes (once per singleton lifetime) a
// reusable borrowed handle on the process-wide host singleton.  The
// returned handle must NOT be passed to sao_plugins_pyhost_shutdown —
// runtime teardown stays owned by the real init/shutdown callers.
int32_t ensure_legacy_script_host(py_host_handle_t* out_host) noexcept;

} // namespace detail

namespace {

using ordered_json = nlohmann::ordered_json;

class helper_json_sax final : public ordered_json::json_sax_t {
  public:
    bool null() override {
        return consume_node();
    }
    bool boolean(bool) override {
        return consume_node();
    }
    bool number_integer(number_integer_t) override {
        return consume_node();
    }
    bool number_unsigned(number_unsigned_t value) override {
        return value <= static_cast<number_unsigned_t>((std::numeric_limits<int64_t>::max)()) &&
               consume_node();
    }
    bool number_float(number_float_t value, const string_t&) override {
        return std::isfinite(value) && consume_node();
    }
    bool string(string_t& value) override {
        return consume_node() && consume_string(value);
    }
    bool binary(binary_t&) override {
        return false;
    }
    bool start_object(std::size_t) override {
        return start_container(true);
    }
    bool key(string_t& value) override {
        return !containers_.empty() && containers_.back().object && consume_string(value) &&
               containers_.back().keys.emplace(value).second;
    }
    bool end_object() override {
        return end_container(true);
    }
    bool start_array(std::size_t) override {
        return start_container(false);
    }
    bool end_array() override {
        return end_container(false);
    }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override {
        return false;
    }

  private:
    struct container_state {
        bool object = false;
        std::unordered_set<std::string> keys;
    };

    size_t depth_ = 0;
    size_t nodes_ = 0;
    size_t string_bytes_ = 0;
    std::vector<container_state> containers_;

    bool consume_node() noexcept {
        if (nodes_ >= sdk_binding::kMaximumBindingJsonNodes)
            return false;
        ++nodes_;
        return true;
    }

    bool consume_string(std::string_view value) noexcept {
        if (value.find('\0') != std::string_view::npos ||
            value.size() > sdk_binding::kMaximumBindingJsonStringBytes ||
            value.size() > sdk_binding::kMaximumBindingJsonTotalStringBytes ||
            string_bytes_ > sdk_binding::kMaximumBindingJsonTotalStringBytes - value.size()) {
            return false;
        }
        string_bytes_ += value.size();
        return true;
    }

    bool start_container(bool object) {
        if (depth_ >= sdk_binding::kMaximumBindingJsonDepth || !consume_node())
            return false;
        containers_.push_back(container_state{object, {}});
        ++depth_;
        return true;
    }

    bool end_container(bool object) noexcept {
        if (depth_ == 0 || containers_.empty() || containers_.back().object != object)
            return false;
        containers_.pop_back();
        --depth_;
        return true;
    }
};

bool helper_json_valid(const char* text, size_t size) {
    helper_json_sax sax;
    return text != nullptr && size != 0 && size <= sdk_binding::kMaximumBindingJsonBytes &&
           ordered_json::sax_parse(text, text + size, &sax);
}

class scoped_gil {
  public:
    scoped_gil() : scope_(sao_plugins_pyhost_gil_scope_enter()) {}
    ~scoped_gil() {
        sao_plugins_pyhost_gil_scope_leave(scope_);
    }
    explicit operator bool() const noexcept {
        return scope_ != nullptr;
    }

  private:
    void* scope_ = nullptr;
};

int32_t copy_text(std::string_view value, char** output) noexcept {
    if (output == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *output = nullptr;
    if (value.size() > sdk_binding::kMaximumBindingJsonBytes ||
        value.find('\0') != std::string::npos)
        return SAO_ERR_INVALID_ARGUMENT;
    auto* buffer = static_cast<char*>(std::malloc(value.size() + 1));
    if (buffer == nullptr)
        return SAO_ERR_OS_CALL_FAILED;
    if (!value.empty())
        std::memcpy(buffer, value.data(), value.size());
    buffer[value.size()] = '\0';
    *output = buffer;
    return SAO_OK;
}

std::string take_python_error(const char* fallback) noexcept {
    try {
        char* raw = nullptr;
        if (sao_plugins_pyhost_take_error(&raw) == SAO_OK && raw != nullptr) {
            const auto release = [](char* text) { sao_plugins_pyhost_free_string(text); };
            std::unique_ptr<char, decltype(release)> owned(raw, release);
            return std::string(owned.get());
        }
    } catch (...) {
    }
    try {
        return fallback == nullptr ? std::string{} : std::string(fallback);
    } catch (...) {
        return {};
    }
}

int32_t publish_error(std::string_view error, char** output, int32_t status) noexcept {
    if (output != nullptr && !error.empty() && copy_text(error, output) != SAO_OK)
        return SAO_ERR_OS_CALL_FAILED;
    return status;
}

bool consume_python_json_node(size_t& nodes, std::string& error) {
    if (nodes >= sdk_binding::kMaximumBindingJsonNodes) {
        error = "value exceeds the cross-language JSON node budget";
        return false;
    }
    ++nodes;
    return true;
}

bool consume_python_json_string(std::string_view value, size_t& string_bytes, std::string& error) {
    constexpr size_t maximum_string = sdk_binding::kMaximumBindingJsonStringBytes;
    constexpr size_t maximum_total = sdk_binding::kMaximumBindingJsonTotalStringBytes;
    if (value.find('\0') != std::string_view::npos || value.size() > maximum_string ||
        value.size() > maximum_total || string_bytes > maximum_total - value.size()) {
        error = "value exceeds the cross-language JSON string budget";
        return false;
    }
    string_bytes += value.size();
    return true;
}

class active_python_container {
  public:
    active_python_container(std::vector<PyObject*>& active, PyObject* value) : active_(active) {
        if (std::find(active_.begin(), active_.end(), value) == active_.end()) {
            active_.push_back(value);
            accepted_ = true;
        }
    }
    ~active_python_container() {
        if (accepted_)
            active_.pop_back();
    }
    explicit operator bool() const noexcept {
        return accepted_;
    }

  private:
    std::vector<PyObject*>& active_;
    bool accepted_ = false;
};

bool python_value_to_json(PyObject* value, ordered_json& output, size_t depth, size_t& nodes,
                          size_t& string_bytes, std::vector<PyObject*>& active,
                          std::string& error) {
    if (value == nullptr) {
        error = take_python_error("helper value is unavailable");
        return false;
    }
    if (!consume_python_json_node(nodes, error))
        return false;
    if (value == Py_None) {
        output = nullptr;
        return true;
    }
    if (PyBool_Check(value)) {
        output = value == Py_True;
        return true;
    }
    if (PyLong_Check(value)) {
        const long long integer = PyLong_AsLongLong(value);
        if (integer == -1 && PyErr_Occurred() != nullptr) {
            PyErr_Clear();
            error = "integer exceeds the signed 64-bit helper boundary";
            return false;
        }
        output = static_cast<int64_t>(integer);
        return true;
    }
    if (PyFloat_Check(value)) {
        const double number = PyFloat_AsDouble(value);
        if (!std::isfinite(number)) {
            error = "non-finite numbers cannot cross the helper boundary";
            return false;
        }
        output = number;
        return true;
    }
    if (PyUnicode_Check(value)) {
        Py_ssize_t size = 0;
        const char* text = PyUnicode_AsUTF8AndSize(value, &size);
        if (text == nullptr || size < 0) {
            error = take_python_error("helper string conversion failed");
            return false;
        }
        const std::string_view view(text, static_cast<size_t>(size));
        if (!consume_python_json_string(view, string_bytes, error))
            return false;
        output = std::string(view);
        return true;
    }
    if (PyList_Check(value)) {
        if (depth >= sdk_binding::kMaximumBindingJsonDepth) {
            error = "value exceeds the cross-language JSON depth budget";
            return false;
        }
        active_python_container container(active, value);
        if (!container) {
            error = "cyclic lists cannot cross the helper boundary";
            return false;
        }
        const Py_ssize_t count = PyList_Size(value);
        if (count < 0) {
            error = take_python_error("helper list inspection failed");
            return false;
        }
        output = ordered_json::array();
        for (Py_ssize_t index = 0; index < count; ++index) {
            ordered_json item;
            if (!python_value_to_json(PyList_GetItem(value, index), item, depth + 1, nodes,
                                      string_bytes, active, error))
                return false;
            output.push_back(std::move(item));
        }
        return true;
    }
    if (PyDict_Check(value)) {
        if (depth >= sdk_binding::kMaximumBindingJsonDepth) {
            error = "value exceeds the cross-language JSON depth budget";
            return false;
        }
        active_python_container container(active, value);
        if (!container) {
            error = "cyclic maps cannot cross the helper boundary";
            return false;
        }
        output = ordered_json::object();
        Py_ssize_t position = 0;
        PyObject* key = nullptr;
        PyObject* item_value = nullptr;
        while (PyDict_Next(value, &position, &key, &item_value) != 0) {
            if (!PyUnicode_Check(key)) {
                error = "helper maps require string keys";
                return false;
            }
            Py_ssize_t key_size = 0;
            const char* key_text = PyUnicode_AsUTF8AndSize(key, &key_size);
            if (key_text == nullptr || key_size < 0) {
                error = take_python_error("helper map key conversion failed");
                return false;
            }
            const std::string_view key_view(key_text, static_cast<size_t>(key_size));
            if (!consume_python_json_string(key_view, string_bytes, error))
                return false;
            ordered_json item;
            if (!python_value_to_json(item_value, item, depth + 1, nodes, string_bytes, active,
                                      error))
                return false;
            output[std::string(key_view)] = std::move(item);
        }
        return true;
    }
    error = "value is not one of the supported JSON-compatible helper types";
    return false;
}

int32_t dump_python_json(PyObject* value, std::string& output, std::string& error,
                         size_t* aggregate_nodes = nullptr,
                         size_t* aggregate_string_bytes = nullptr) {
    try {
        ordered_json converted;
        size_t nodes = aggregate_nodes != nullptr ? *aggregate_nodes : 0;
        size_t string_bytes = aggregate_string_bytes != nullptr ? *aggregate_string_bytes : 0;
        std::vector<PyObject*> active;
        active.reserve(sdk_binding::kMaximumBindingJsonDepth);
        const bool converted_ok =
            python_value_to_json(value, converted, 0, nodes, string_bytes, active, error);
        if (aggregate_nodes != nullptr)
            *aggregate_nodes = nodes;
        if (aggregate_string_bytes != nullptr)
            *aggregate_string_bytes = string_bytes;
        if (!converted_ok)
            return sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED;
        output = converted.dump();
        if (!helper_json_valid(output.data(), output.size())) {
            error = "value exceeds the cross-language JSON budget";
            return sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED;
        }
        return SAO_OK;
    } catch (...) {
        error = "JSON-compatible helper value serialization failed";
        return SAO_ERR_OS_CALL_FAILED;
    }
}

PyObject* json_value_to_python(const ordered_json& value, size_t depth, size_t& nodes,
                               size_t& string_bytes, std::string& error) {
    if (!consume_python_json_node(nodes, error))
        return nullptr;
    if (value.is_null())
        return Py_NewRef(Py_None);
    if (value.is_boolean())
        return PyBool_FromLong(value.get<bool>() ? 1 : 0);
    if (value.is_number_integer())
        return PyLong_FromLongLong(value.get<int64_t>());
    if (value.is_number_unsigned()) {
        const uint64_t integer = value.get<uint64_t>();
        if (integer > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)())) {
            error = "helper argument integer exceeds int64";
            return nullptr;
        }
        return PyLong_FromLongLong(static_cast<int64_t>(integer));
    }
    if (value.is_number_float()) {
        const double number = value.get<double>();
        if (!std::isfinite(number)) {
            error = "helper arguments cannot contain non-finite numbers";
            return nullptr;
        }
        return PyFloat_FromDouble(number);
    }
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        if (!consume_python_json_string(text, string_bytes, error))
            return nullptr;
        return PyUnicode_FromStringAndSize(text.data(), static_cast<Py_ssize_t>(text.size()));
    }
    if (value.is_array()) {
        if (depth >= sdk_binding::kMaximumBindingJsonDepth) {
            error = "helper arguments exceed the JSON depth budget";
            return nullptr;
        }
        PyObject* list = PyList_New(0);
        if (list == nullptr) {
            error = take_python_error("cannot allocate helper argument list");
            return nullptr;
        }
        for (const auto& item : value) {
            PyObject* converted = json_value_to_python(item, depth + 1, nodes, string_bytes, error);
            if (converted == nullptr || PyList_Append(list, converted) != 0) {
                Py_XDECREF(converted);
                Py_DECREF(list);
                if (error.empty())
                    error = take_python_error("cannot append helper argument");
                return nullptr;
            }
            Py_DECREF(converted);
        }
        return list;
    }
    if (value.is_object()) {
        if (depth >= sdk_binding::kMaximumBindingJsonDepth) {
            error = "helper arguments exceed the JSON depth budget";
            return nullptr;
        }
        PyObject* dictionary = PyDict_New();
        if (dictionary == nullptr) {
            error = take_python_error("cannot allocate helper argument map");
            return nullptr;
        }
        for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
            if (!consume_python_json_string(iterator.key(), string_bytes, error)) {
                Py_DECREF(dictionary);
                return nullptr;
            }
            PyObject* converted =
                json_value_to_python(iterator.value(), depth + 1, nodes, string_bytes, error);
            if (converted == nullptr ||
                PyDict_SetItemString(dictionary, iterator.key().c_str(), converted) != 0) {
                Py_XDECREF(converted);
                Py_DECREF(dictionary);
                if (error.empty())
                    error = take_python_error("cannot insert helper map argument");
                return nullptr;
            }
            Py_DECREF(converted);
        }
        return dictionary;
    }
    error = "unsupported helper JSON argument";
    return nullptr;
}

PyObject* load_python_args(const char* args_json_utf8, std::string& error) {
    size_t input_size = 0;
    if (!sdk_binding::sao_plugins_binding_bounded_json_c_string(args_json_utf8, input_size) ||
        !sdk_binding::sao_plugins_binding_validate_json_text(
            reinterpret_cast<const uint8_t*>(args_json_utf8), input_size) ||
        !helper_json_valid(args_json_utf8, input_size)) {
        error = "helper arguments must be bounded JSON";
        return nullptr;
    }
    ordered_json parsed =
        ordered_json::parse(args_json_utf8, args_json_utf8 + input_size, nullptr, false);
    if (!parsed.is_array()) {
        error = "helper arguments must be a JSON array";
        return nullptr;
    }
    size_t nodes = 0;
    size_t string_bytes = 0;
    PyObject* list = json_value_to_python(parsed, 0, nodes, string_bytes, error);
    if (list == nullptr) {
        if (PyErr_Occurred() != nullptr) {
            const std::string python_error =
                take_python_error("cannot materialize helper arguments");
            if (error.empty())
                error = python_error;
        }
        return nullptr;
    }
    PyObject* tuple = PyList_AsTuple(list);
    Py_DECREF(list);
    if (tuple == nullptr)
        error = take_python_error("cannot materialize helper arguments");
    return tuple;
}

PyObject* module_object(py_plugin_handle_t plugin) noexcept {
    return static_cast<PyObject*>(sao_plugins_pyhost_get_module_pyobject(plugin));
}

} // namespace
#endif

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_load_script(const wchar_t* plugin_dir, const char* entry_relative,
                               const char* plugin_id_utf8, py_plugin_handle_t* out_plugin) {
    if (out_plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_plugin = nullptr;
    if (plugin_dir == nullptr || plugin_dir[0] == L'\0')
        return SAO_ERR_INVALID_ARGUMENT;
#if !defined(SAO_HAS_PYTHON_EMBED)
    (void)entry_relative;
    (void)plugin_id_utf8;
    return SAO_ERR_NOT_IMPLEMENTED;
#else
    try {
        py_host_handle_t host = nullptr;
        const int32_t host_status = detail::ensure_legacy_script_host(&host);
        if (host_status != SAO_OK)
            return host_status;
        // sao_plugins_pyhost_load_plugin requires the GIL held by the
        // caller; gil_scope_enter returns nullptr when the runtime is not
        // up (imports unresolved or interpreter finalized).
        void* gil = sao_plugins_pyhost_gil_scope_enter();
        if (gil == nullptr)
            return SAO_ERR_NOT_INITIALIZED;
        const int32_t status = sao_plugins_pyhost_load_plugin(host, plugin_dir, entry_relative,
                                                              plugin_id_utf8, nullptr, out_plugin);
        sao_plugins_pyhost_gil_scope_leave(gil);
        if (status != SAO_OK)
            *out_plugin = nullptr;
        return status;
    } catch (...) {
        *out_plugin = nullptr;
        return SAO_ERR_OS_CALL_FAILED;
    }
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_unload_script(py_plugin_handle_t plugin) {
    if (plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
#if !defined(SAO_HAS_PYTHON_EMBED)
    (void)plugin;
    return SAO_ERR_NOT_IMPLEMENTED;
#else
    try {
        // Conditional scope: when the interpreter is already finalized the
        // returned scope is nullptr and unload_plugin still runs its
        // registry-level teardown without touching PyObject pointers.
        void* gil = sao_plugins_pyhost_gil_scope_enter();
        const int32_t status = sao_plugins_pyhost_unload_plugin(plugin);
        sao_plugins_pyhost_gil_scope_leave(gil);
        return status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_pyhost_module_members_json(
    py_plugin_handle_t plugin, char** out_json_utf8, char** out_error_utf8) {
    if (out_json_utf8 != nullptr)
        *out_json_utf8 = nullptr;
    if (out_error_utf8 != nullptr)
        *out_error_utf8 = nullptr;
    if (plugin == nullptr || out_json_utf8 == nullptr || out_error_utf8 == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
#if !defined(SAO_HAS_PYTHON_EMBED)
    return SAO_ERR_NOT_IMPLEMENTED;
#else
    try {
        scoped_gil gil;
        if (!gil)
            return publish_error("CPython runtime is not initialized", out_error_utf8,
                                 SAO_ERR_NOT_INITIALIZED);
        PyObject* module = module_object(plugin);
        PyObject* dictionary = module == nullptr ? nullptr : PyModule_GetDict(module);
        if (dictionary == nullptr)
            return publish_error("CPython helper module is unavailable", out_error_utf8,
                                 SAO_ERR_HANDLE_INVALID);
        ordered_json names = ordered_json::array();
        size_t inspection_nodes = 1;
        size_t string_bytes = 0;
        Py_ssize_t position = 0;
        PyObject* key = nullptr;
        PyObject* value = nullptr;
        while (PyDict_Next(dictionary, &position, &key, &value) != 0) {
            if (!PyUnicode_Check(key))
                continue;
            Py_ssize_t size = 0;
            const char* name = PyUnicode_AsUTF8AndSize(key, &size);
            if (name == nullptr) {
                PyErr_Clear();
                continue;
            }
            if (size <= 0 || name[0] == '_' || size > 4096 ||
                std::memchr(name, '\0', static_cast<size_t>(size)) != nullptr)
                continue;
            if (PyCallable_Check(value) == 0) {
                std::string ignored_json;
                std::string ignored_error;
                const int32_t value_status = dump_python_json(value, ignored_json, ignored_error,
                                                              &inspection_nodes, &string_bytes);
                if (value_status != SAO_OK) {
                    if (value_status != sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED) {
                        return publish_error(ignored_error.empty()
                                                 ? "CPython helper member enumeration failed"
                                                 : ignored_error,
                                             out_error_utf8, value_status);
                    }
                    if (inspection_nodes >= sdk_binding::kMaximumBindingJsonNodes ||
                        string_bytes >= sdk_binding::kMaximumBindingJsonTotalStringBytes) {
                        return publish_error("CPython helper member enumeration exceeds its budget",
                                             out_error_utf8,
                                             sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
                    }
                    continue;
                }
            }
            std::string budget_error;
            if (inspection_nodes >= sdk_binding::kMaximumBindingJsonNodes ||
                !consume_python_json_string(std::string_view(name, static_cast<size_t>(size)),
                                            string_bytes, budget_error)) {
                return publish_error("CPython helper member enumeration exceeds its budget",
                                     out_error_utf8,
                                     sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
            }
            ++inspection_nodes;
            names.emplace_back(std::string(name, static_cast<size_t>(size)));
        }
        const std::string serialized = names.dump();
        if (!helper_json_valid(serialized.data(), serialized.size())) {
            return publish_error("CPython helper member enumeration exceeds its budget",
                                 out_error_utf8, sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
        }
        return copy_text(serialized, out_json_utf8);
    } catch (...) {
        return publish_error("CPython helper member enumeration failed", out_error_utf8,
                             SAO_ERR_OS_CALL_FAILED);
    }
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_pyhost_module_get_json(
    py_plugin_handle_t plugin, const char* member_utf8, bool* out_callable, char** out_json_utf8,
    char** out_error_utf8) {
    if (out_callable != nullptr)
        *out_callable = false;
    if (out_json_utf8 != nullptr)
        *out_json_utf8 = nullptr;
    if (out_error_utf8 != nullptr)
        *out_error_utf8 = nullptr;
    if (plugin == nullptr || !valid_member_name(member_utf8) || out_callable == nullptr ||
        out_json_utf8 == nullptr || out_error_utf8 == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
#if !defined(SAO_HAS_PYTHON_EMBED)
    return SAO_ERR_NOT_IMPLEMENTED;
#else
    try {
        scoped_gil gil;
        if (!gil)
            return publish_error("CPython runtime is not initialized", out_error_utf8,
                                 SAO_ERR_NOT_INITIALIZED);
        PyObject* module = module_object(plugin);
        if (module == nullptr)
            return publish_error("CPython helper module is unavailable", out_error_utf8,
                                 SAO_ERR_HANDLE_INVALID);
        PyObject* value = PyObject_GetAttrString(module, member_utf8);
        if (value == nullptr)
            return publish_error(take_python_error("helper member was not found"), out_error_utf8,
                                 SAO_ERR_HANDLE_INVALID);
        if (PyCallable_Check(value) != 0) {
            *out_callable = true;
            Py_DECREF(value);
            return SAO_OK;
        }
        std::string serialized;
        std::string error;
        const int32_t status = dump_python_json(value, serialized, error);
        Py_DECREF(value);
        if (status != SAO_OK)
            return publish_error(error, out_error_utf8, status);
        return copy_text(serialized, out_json_utf8);
    } catch (...) {
        return publish_error("CPython helper member access failed", out_error_utf8,
                             SAO_ERR_OS_CALL_FAILED);
    }
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_pyhost_module_call_json(
    py_plugin_handle_t plugin, const char* member_utf8, const char* args_json_utf8,
    char** out_json_utf8, char** out_error_utf8) {
    if (out_json_utf8 != nullptr)
        *out_json_utf8 = nullptr;
    if (out_error_utf8 != nullptr)
        *out_error_utf8 = nullptr;
    if (plugin == nullptr || !valid_member_name(member_utf8) || args_json_utf8 == nullptr ||
        out_json_utf8 == nullptr || out_error_utf8 == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
#if !defined(SAO_HAS_PYTHON_EMBED)
    return SAO_ERR_NOT_IMPLEMENTED;
#else
    try {
        scoped_gil gil;
        if (!gil)
            return publish_error("CPython runtime is not initialized", out_error_utf8,
                                 SAO_ERR_NOT_INITIALIZED);
        PyObject* module = module_object(plugin);
        if (module == nullptr)
            return publish_error("CPython helper module is unavailable", out_error_utf8,
                                 SAO_ERR_HANDLE_INVALID);
        PyObject* callable = PyObject_GetAttrString(module, member_utf8);
        if (callable == nullptr)
            return publish_error(take_python_error("helper member was not found"), out_error_utf8,
                                 SAO_ERR_HANDLE_INVALID);
        if (PyCallable_Check(callable) == 0) {
            Py_DECREF(callable);
            return publish_error("helper member is not callable", out_error_utf8,
                                 SAO_ERR_INVALID_ARGUMENT);
        }
        std::string error;
        PyObject* args = load_python_args(args_json_utf8, error);
        if (args == nullptr) {
            Py_DECREF(callable);
            return publish_error(error, out_error_utf8, SAO_ERR_INVALID_ARGUMENT);
        }
        PyObject* result = PyObject_CallObject(callable, args);
        Py_DECREF(args);
        Py_DECREF(callable);
        if (result == nullptr)
            return publish_error(take_python_error("CPython helper call failed"), out_error_utf8,
                                 SAO_ERR_OS_CALL_FAILED);
        std::string serialized;
        const int32_t status = dump_python_json(result, serialized, error);
        Py_DECREF(result);
        if (status != SAO_OK)
            return publish_error(error, out_error_utf8, status);
        return copy_text(serialized, out_json_utf8);
    } catch (...) {
        return publish_error("CPython helper call crossed a C++ exception boundary", out_error_utf8,
                             SAO_ERR_OS_CALL_FAILED);
    }
#endif
}

} // namespace sao::plugins::python_host
