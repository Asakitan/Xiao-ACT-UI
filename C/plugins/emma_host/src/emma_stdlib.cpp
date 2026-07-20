// emma_stdlib.cpp — Emma 内置装载
//
// 对齐 Python emma_runtime.py _EmmaInterpreter._builtins:
//   print / str / int / float / len / type / tostring / tonumber /
//   pairs / ipairs / range / abs / min / max
//
// C++ 额外内置 (补足容器构造和常用数学函数):
//   list()   — 空数组构造 (对齐 python list())
//   dict()   — 空字典构造 (对齐 python dict())
//   floor / ceil / sqrt — math 扩展
//
// 数组/字典通过 wrapper (emma_list / emma_dict) 承载, 打破 std::variant 里
// 不能包含不完整容器的循环 (MSVC pre-C++23 严格). 详见 emma_interpreter.h。
//
// print 的输出走一个可注入的 log 桥 (未装桥时默认 fputs stdout, 供 test 捕获)。
// 每 interpreter 保留自己的 log_fn 状态 — 用一个 static 表挂 host。

#include "sao/plugins/emma_host/emma_stdlib.h"
#include "emma_source_io.h"
#include "sao/plugins/emma_host/emma_error.h"
#include "sao/plugins/emma_host/emma_interpreter.h"
#include "sao/plugins/emma_host/emma_lexer.h"
#include "sao/plugins/emma_host/emma_parser.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

namespace sao::plugins::emma_host {

// 对外声明 (被 interpreter.cpp 用)
std::string emma_value_to_string(const emma_value& v);

namespace {

// 每 interpreter 一份 log 桥
struct log_bridge_slot {
    void (*fn)(const char* utf8, void* ud) = nullptr;
    void* ud = nullptr;
    std::filesystem::path plugin_dir;
};

std::unordered_map<interpreter*, log_bridge_slot>& log_bridge_table() {
    static std::unordered_map<interpreter*, log_bridge_slot> m;
    return m;
}

std::mutex& log_bridge_mutex() {
    static std::mutex m;
    return m;
}

void log_bridge_call(interpreter* interp, const std::string& msg) {
    log_bridge_slot slot;
    {
        std::lock_guard<std::mutex> lock(log_bridge_mutex());
        auto it = log_bridge_table().find(interp);
        if (it != log_bridge_table().end())
            slot = it->second;
    }
    if (slot.fn) {
        slot.fn(msg.c_str(), slot.ud);
    } else {
        std::fputs(msg.c_str(), stdout);
        std::fputc('\n', stdout);
    }
}

// 便利: 创建 host callable 装到 scope
std::shared_ptr<callable> make_host(const std::string& name,
                                    std::function<emma_value(std::vector<emma_value>)> fn) {
    auto c = std::make_shared<callable>();
    c->name = name;
    c->host_impl = std::move(fn);
    return c;
}

[[noreturn]] void throw_runtime_error(std::string message) {
    emma_error error;
    error.kind = error_kind::runtime_error;
    error.message = std::move(message);
    throw emma_exception(std::move(error));
}

const emma_value& require_argument(const std::vector<emma_value>& arguments, size_t index,
                                   const char* operation) {
    if (index >= arguments.size()) {
        throw_runtime_error(std::string(operation) + ": missing required argument");
    }
    return arguments[index];
}

std::string require_string(const std::vector<emma_value>& arguments, size_t index,
                           const char* operation) {
    const auto& value = require_argument(arguments, index, operation);
    if (!std::holds_alternative<std::string>(value)) {
        throw_runtime_error(std::string(operation) + ": expected string argument");
    }
    return std::get<std::string>(value);
}

double numeric_value(const emma_value& value, const char* operation) {
    if (const auto* integer = std::get_if<int64_t>(&value)) {
        return static_cast<double>(*integer);
    }
    if (const auto* number = std::get_if<double>(&value))
        return *number;
    throw_runtime_error(std::string(operation) + ": expected numeric argument");
}

bool value_to_json(const emma_value& value, nlohmann::json& output, size_t depth = 0) {
    if (depth > 64)
        return false;
    if (std::holds_alternative<std::nullptr_t>(value)) {
        output = nullptr;
    } else if (const auto* boolean = std::get_if<bool>(&value)) {
        output = *boolean;
    } else if (const auto* integer = std::get_if<int64_t>(&value)) {
        output = *integer;
    } else if (const auto* number = std::get_if<double>(&value)) {
        if (!std::isfinite(*number))
            return false;
        output = *number;
    } else if (const auto* string = std::get_if<std::string>(&value)) {
        output = *string;
    } else if (const auto* list = std::get_if<std::shared_ptr<emma_list>>(&value)) {
        output = nlohmann::json::array();
        if (*list != nullptr) {
            for (const auto& item : (*list)->items) {
                nlohmann::json converted;
                if (!value_to_json(item, converted, depth + 1))
                    return false;
                output.push_back(std::move(converted));
            }
        }
    } else if (const auto* dictionary = std::get_if<std::shared_ptr<emma_dict>>(&value)) {
        output = nlohmann::json::object();
        if (*dictionary != nullptr) {
            for (const auto& [key, item] : (*dictionary)->items) {
                nlohmann::json converted;
                if (!value_to_json(item, converted, depth + 1))
                    return false;
                output[key] = std::move(converted);
            }
        }
    } else {
        return false;
    }
    return true;
}

bool json_to_value(const nlohmann::json& input, emma_value& output, size_t depth = 0) {
    if (depth > 64)
        return false;
    if (input.is_null()) {
        output = nullptr;
    } else if (input.is_boolean()) {
        output = input.get<bool>();
    } else if (input.is_number_integer()) {
        output = input.get<int64_t>();
    } else if (input.is_number_unsigned()) {
        const uint64_t value = input.get<uint64_t>();
        if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return false;
        }
        output = static_cast<int64_t>(value);
    } else if (input.is_number_float()) {
        const double value = input.get<double>();
        if (!std::isfinite(value))
            return false;
        output = value;
    } else if (input.is_string()) {
        output = input.get<std::string>();
    } else if (input.is_array()) {
        auto list = std::make_shared<emma_list>();
        list->items.reserve(input.size());
        for (const auto& item : input) {
            emma_value converted = nullptr;
            if (!json_to_value(item, converted, depth + 1))
                return false;
            list->items.push_back(std::move(converted));
        }
        output = std::move(list);
    } else if (input.is_object()) {
        auto dictionary = std::make_shared<emma_dict>();
        for (auto iterator = input.begin(); iterator != input.end(); ++iterator) {
            emma_value converted = nullptr;
            if (!json_to_value(iterator.value(), converted, depth + 1)) {
                return false;
            }
            dictionary->items.emplace(iterator.key(), std::move(converted));
        }
        output = std::move(dictionary);
    } else {
        return false;
    }
    return true;
}

emma_value bi_json_encode(std::vector<emma_value> arguments) {
    const auto& value = require_argument(arguments, 0, "json_encode");
    nlohmann::json converted;
    if (!value_to_json(value, converted)) {
        throw_runtime_error("json_encode: value is not serializable");
    }
    return converted.dump();
}

emma_value bi_json_decode(std::vector<emma_value> arguments) {
    const std::string source = require_string(arguments, 0, "json_decode");
    const auto parsed = nlohmann::json::parse(source, nullptr, false);
    emma_value converted = nullptr;
    if (parsed.is_discarded() || !json_to_value(parsed, converted)) {
        throw_runtime_error("json_decode: invalid JSON value");
    }
    return converted;
}

emma_value bi_time(std::vector<emma_value>) {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration<double>(now).count();
}

emma_value bi_sleep(std::vector<emma_value> arguments) {
    const double seconds = numeric_value(require_argument(arguments, 0, "sleep"), "sleep");
    if (!std::isfinite(seconds) || seconds < 0.0 || seconds > 86400.0) {
        throw_runtime_error("sleep: duration is outside the supported range");
    }
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    return nullptr;
}

std::filesystem::path io_base_dir(interpreter* interp) {
    std::lock_guard<std::mutex> lock(log_bridge_mutex());
    const auto found = log_bridge_table().find(interp);
    if (found == log_bridge_table().end() || found->second.plugin_dir.empty()) {
        throw_runtime_error("load_script: plugin directory is unavailable");
    }
    return found->second.plugin_dir;
}

emma_value bi_load_script(interpreter* interp, std::vector<emma_value> arguments) {
    const std::string relative = require_string(arguments, 0, "load_script");
    if (relative.empty() || relative.find('\0') != std::string::npos) {
        throw_runtime_error("load_script: invalid relative path");
    }
    const auto base = io_base_dir(interp);
    std::string source;
    std::filesystem::path final_path;
    std::string read_error;
    const int32_t read_status =
        read_emma_source_file(base, relative, source, final_path, read_error);
    if (read_status != SAO_OK) {
        emma_error error;
        error.kind = error_kind::runtime_error;
        error.status = read_status;
        error.message = "load_script: " + read_error;
        throw emma_exception(std::move(error));
    }

    ast_pool* pool = interp->mutable_pool();
    if (pool == nullptr) {
        throw_runtime_error("load_script: interpreter AST pool is unavailable");
    }
    const std::vector<token> tokens = tokenize_source(source);
    parser source_parser(tokens, pool);
    std::vector<node_id> statements;
    std::string message;
    int32_t status = source_parser.parse_program(statements, message);
    if (status != SAO_OK) {
        emma_error error;
        error.kind = error_kind::parse_error;
        error.message = std::move(message);
        set_error_location_from_message(error);
        throw emma_exception(std::move(error));
    }
    emma_error error;
    status = interp->execute(statements, message, &error);
    if (status != SAO_OK) {
        if (error.kind == error_kind::none) {
            error.kind = error_kind::runtime_error;
            error.message = std::move(message);
        }
        throw emma_exception(std::move(error));
    }
    return true;
}

// ── builtin 实装 ──
emma_value bi_print(interpreter* interp, std::vector<emma_value> args) {
    std::string s;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i)
            s += ' ';
        s += emma_value_to_string(args[i]);
    }
    log_bridge_call(interp, s);
    return nullptr;
}

emma_value bi_str(std::vector<emma_value> args) {
    if (args.empty() || std::holds_alternative<std::nullptr_t>(args[0]))
        return std::string("nil");
    return emma_value_to_string(args[0]);
}

emma_value bi_int(std::vector<emma_value> args) {
    if (args.empty())
        return static_cast<int64_t>(0);
    const emma_value& v = args[0];
    if (std::holds_alternative<int64_t>(v))
        return v;
    if (std::holds_alternative<double>(v))
        return static_cast<int64_t>(std::get<double>(v));
    if (std::holds_alternative<bool>(v))
        return static_cast<int64_t>(std::get<bool>(v) ? 1 : 0);
    if (std::holds_alternative<std::string>(v)) {
        try {
            return static_cast<int64_t>(std::stoll(std::get<std::string>(v)));
        } catch (...) {
            return static_cast<int64_t>(0);
        }
    }
    return static_cast<int64_t>(0);
}

emma_value bi_float(std::vector<emma_value> args) {
    if (args.empty())
        return 0.0;
    const emma_value& v = args[0];
    if (std::holds_alternative<double>(v))
        return v;
    if (std::holds_alternative<int64_t>(v))
        return static_cast<double>(std::get<int64_t>(v));
    if (std::holds_alternative<bool>(v))
        return std::get<bool>(v) ? 1.0 : 0.0;
    if (std::holds_alternative<std::string>(v)) {
        try {
            return std::stod(std::get<std::string>(v));
        } catch (...) {
            return 0.0;
        }
    }
    return 0.0;
}

emma_value bi_len(std::vector<emma_value> args) {
    if (args.empty())
        return static_cast<int64_t>(0);
    const emma_value& v = args[0];
    if (std::holds_alternative<std::string>(v))
        return static_cast<int64_t>(std::get<std::string>(v).size());
    if (std::holds_alternative<std::shared_ptr<emma_list>>(v)) {
        auto& a = std::get<std::shared_ptr<emma_list>>(v);
        return static_cast<int64_t>(a ? a->items.size() : 0);
    }
    if (std::holds_alternative<std::shared_ptr<emma_dict>>(v)) {
        auto& d = std::get<std::shared_ptr<emma_dict>>(v);
        return static_cast<int64_t>(d ? d->items.size() : 0);
    }
    return static_cast<int64_t>(0);
}

emma_value bi_type(std::vector<emma_value> args) {
    if (args.empty())
        return std::string("nil");
    const emma_value& v = args[0];
    if (std::holds_alternative<std::nullptr_t>(v))
        return std::string("nil");
    if (std::holds_alternative<bool>(v))
        return std::string("bool");
    if (std::holds_alternative<int64_t>(v))
        return std::string("int");
    if (std::holds_alternative<double>(v))
        return std::string("float");
    if (std::holds_alternative<std::string>(v))
        return std::string("string");
    if (std::holds_alternative<std::shared_ptr<emma_list>>(v))
        return std::string("array");
    if (std::holds_alternative<std::shared_ptr<emma_dict>>(v))
        return std::string("dict");
    if (std::holds_alternative<std::shared_ptr<callable>>(v))
        return std::string("fn");
    return std::string("unknown");
}

// range(stop) / range(start, stop) / range(start, stop, step) → array
emma_value bi_range(std::vector<emma_value> args) {
    int64_t start = 0, stop = 0, step = 1;
    if (args.size() == 1) {
        stop = std::holds_alternative<int64_t>(args[0]) ? std::get<int64_t>(args[0])
               : std::holds_alternative<double>(args[0])
                   ? static_cast<int64_t>(std::get<double>(args[0]))
                   : 0;
    } else if (args.size() == 2) {
        start = std::holds_alternative<int64_t>(args[0]) ? std::get<int64_t>(args[0]) : 0;
        stop = std::holds_alternative<int64_t>(args[1]) ? std::get<int64_t>(args[1]) : 0;
    } else if (args.size() >= 3) {
        start = std::holds_alternative<int64_t>(args[0]) ? std::get<int64_t>(args[0]) : 0;
        stop = std::holds_alternative<int64_t>(args[1]) ? std::get<int64_t>(args[1]) : 0;
        step = std::holds_alternative<int64_t>(args[2]) ? std::get<int64_t>(args[2]) : 1;
        if (step == 0)
            step = 1;
    }
    auto arr = std::make_shared<emma_list>();
    if (step > 0) {
        for (int64_t i = start; i < stop; i += step)
            arr->items.push_back(i);
    } else {
        for (int64_t i = start; i > stop; i += step)
            arr->items.push_back(i);
    }
    return arr;
}

emma_value bi_list(std::vector<emma_value> args) {
    // list()      → 空数组
    // list(iter)  → 从可迭代物拷贝 (array/dict/string)
    auto arr = std::make_shared<emma_list>();
    if (args.empty())
        return arr;
    const emma_value& v = args[0];
    if (std::holds_alternative<std::shared_ptr<emma_list>>(v)) {
        auto& src = std::get<std::shared_ptr<emma_list>>(v);
        if (src)
            arr->items = src->items;
    } else if (std::holds_alternative<std::string>(v)) {
        for (char c : std::get<std::string>(v))
            arr->items.push_back(std::string(1, c));
    } else if (std::holds_alternative<std::shared_ptr<emma_dict>>(v)) {
        auto& d = std::get<std::shared_ptr<emma_dict>>(v);
        if (d)
            for (auto& kv : d->items)
                arr->items.push_back(kv.first);
    }
    return arr;
}

emma_value bi_dict(std::vector<emma_value> /*args*/) {
    return std::make_shared<emma_dict>();
}

emma_value bi_pairs(std::vector<emma_value> args) {
    auto out = std::make_shared<emma_list>();
    if (args.empty())
        return out;
    const emma_value& v = args[0];
    if (std::holds_alternative<std::shared_ptr<emma_dict>>(v)) {
        auto& d = std::get<std::shared_ptr<emma_dict>>(v);
        if (d)
            for (auto& kv : d->items) {
                auto pair = std::make_shared<emma_list>();
                pair->items.push_back(kv.first);
                pair->items.push_back(kv.second);
                out->items.push_back(pair);
            }
    }
    return out;
}

emma_value bi_ipairs(std::vector<emma_value> args) {
    auto out = std::make_shared<emma_list>();
    if (args.empty())
        return out;
    const emma_value& v = args[0];
    if (std::holds_alternative<std::shared_ptr<emma_list>>(v)) {
        auto& src = std::get<std::shared_ptr<emma_list>>(v);
        if (src)
            for (size_t i = 0; i < src->items.size(); ++i) {
                auto pair = std::make_shared<emma_list>();
                pair->items.push_back(static_cast<int64_t>(i));
                pair->items.push_back(src->items[i]);
                out->items.push_back(pair);
            }
    }
    return out;
}

emma_value bi_abs(std::vector<emma_value> args) {
    if (args.empty())
        return static_cast<int64_t>(0);
    const emma_value& v = args[0];
    if (std::holds_alternative<int64_t>(v))
        return static_cast<int64_t>(std::abs(std::get<int64_t>(v)));
    if (std::holds_alternative<double>(v))
        return std::fabs(std::get<double>(v));
    if (std::holds_alternative<bool>(v))
        return static_cast<int64_t>(std::get<bool>(v) ? 1 : 0);
    return static_cast<int64_t>(0);
}

emma_value bi_min(std::vector<emma_value> args) {
    if (args.empty())
        return nullptr;
    // 若第一个参数是 array, 就在 array 内比较
    std::vector<emma_value> pool;
    if (args.size() == 1 && std::holds_alternative<std::shared_ptr<emma_list>>(args[0])) {
        auto& src = std::get<std::shared_ptr<emma_list>>(args[0]);
        if (src)
            pool = src->items;
    } else {
        pool = std::move(args);
    }
    if (pool.empty())
        return nullptr;
    size_t best = 0;
    for (size_t i = 1; i < pool.size(); ++i) {
        double a = std::holds_alternative<int64_t>(pool[i])
                       ? static_cast<double>(std::get<int64_t>(pool[i]))
                   : std::holds_alternative<double>(pool[i]) ? std::get<double>(pool[i])
                   : std::holds_alternative<bool>(pool[i])   ? (std::get<bool>(pool[i]) ? 1.0 : 0.0)
                                                             : 0.0;
        double b = std::holds_alternative<int64_t>(pool[best])
                       ? static_cast<double>(std::get<int64_t>(pool[best]))
                   : std::holds_alternative<double>(pool[best]) ? std::get<double>(pool[best])
                   : std::holds_alternative<bool>(pool[best])
                       ? (std::get<bool>(pool[best]) ? 1.0 : 0.0)
                       : 0.0;
        if (a < b)
            best = i;
    }
    return pool[best];
}

emma_value bi_max(std::vector<emma_value> args) {
    if (args.empty())
        return nullptr;
    std::vector<emma_value> pool;
    if (args.size() == 1 && std::holds_alternative<std::shared_ptr<emma_list>>(args[0])) {
        auto& src = std::get<std::shared_ptr<emma_list>>(args[0]);
        if (src)
            pool = src->items;
    } else {
        pool = std::move(args);
    }
    if (pool.empty())
        return nullptr;
    size_t best = 0;
    for (size_t i = 1; i < pool.size(); ++i) {
        double a = std::holds_alternative<int64_t>(pool[i])
                       ? static_cast<double>(std::get<int64_t>(pool[i]))
                   : std::holds_alternative<double>(pool[i]) ? std::get<double>(pool[i])
                   : std::holds_alternative<bool>(pool[i])   ? (std::get<bool>(pool[i]) ? 1.0 : 0.0)
                                                             : 0.0;
        double b = std::holds_alternative<int64_t>(pool[best])
                       ? static_cast<double>(std::get<int64_t>(pool[best]))
                   : std::holds_alternative<double>(pool[best]) ? std::get<double>(pool[best])
                   : std::holds_alternative<bool>(pool[best])
                       ? (std::get<bool>(pool[best]) ? 1.0 : 0.0)
                       : 0.0;
        if (a > b)
            best = i;
    }
    return pool[best];
}

emma_value bi_math_floor(std::vector<emma_value> args) {
    if (args.empty())
        return 0.0;
    return std::floor(std::holds_alternative<int64_t>(args[0])
                          ? static_cast<double>(std::get<int64_t>(args[0]))
                      : std::holds_alternative<double>(args[0]) ? std::get<double>(args[0])
                                                                : 0.0);
}

emma_value bi_math_ceil(std::vector<emma_value> args) {
    if (args.empty())
        return 0.0;
    return std::ceil(std::holds_alternative<int64_t>(args[0])
                         ? static_cast<double>(std::get<int64_t>(args[0]))
                     : std::holds_alternative<double>(args[0]) ? std::get<double>(args[0])
                                                               : 0.0);
}

emma_value bi_math_sqrt(std::vector<emma_value> args) {
    if (args.empty())
        return 0.0;
    double x = std::holds_alternative<int64_t>(args[0])
                   ? static_cast<double>(std::get<int64_t>(args[0]))
               : std::holds_alternative<double>(args[0]) ? std::get<double>(args[0])
                                                         : 0.0;
    return x < 0.0 ? 0.0 : std::sqrt(x);
}

} // namespace

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_install_stdlib(interpreter* interp) {
    if (interp == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;

    try {
        interpreter* interp_ptr = interp;
        interp->register_global("print",
                                make_host("print", [interp_ptr](std::vector<emma_value> args) {
                                    return bi_print(interp_ptr, std::move(args));
                                }));
        interp->register_global("str", make_host("str", &bi_str));
        interp->register_global("int", make_host("int", &bi_int));
        interp->register_global("float", make_host("float", &bi_float));
        interp->register_global("tostring", make_host("tostring", &bi_str));
        interp->register_global("tonumber", make_host("tonumber", &bi_float));
        interp->register_global("len", make_host("len", &bi_len));
        interp->register_global("type", make_host("type", &bi_type));
        interp->register_global("range", make_host("range", &bi_range));
        interp->register_global("list", make_host("list", &bi_list));
        interp->register_global("dict", make_host("dict", &bi_dict));
        interp->register_global("pairs", make_host("pairs", &bi_pairs));
        interp->register_global("ipairs", make_host("ipairs", &bi_ipairs));
        interp->register_global("abs", make_host("abs", &bi_abs));
        interp->register_global("min", make_host("min", &bi_min));
        interp->register_global("max", make_host("max", &bi_max));
        interp->register_global("floor", make_host("floor", &bi_math_floor));
        interp->register_global("ceil", make_host("ceil", &bi_math_ceil));
        interp->register_global("sqrt", make_host("sqrt", &bi_math_sqrt));
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_install_io_stdlib(interpreter* interp, const wchar_t* plugin_base_dir) {
    if (interp == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    if (plugin_base_dir == nullptr || plugin_base_dir[0] == L'\0') {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        std::error_code error;
        const auto base = std::filesystem::weakly_canonical(plugin_base_dir, error);
        if (error || !std::filesystem::is_directory(base, error)) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        {
            std::lock_guard<std::mutex> lock(log_bridge_mutex());
            log_bridge_table()[interp].plugin_dir = base;
        }
        interp->register_global("json_encode", make_host("json_encode", &bi_json_encode));
        interp->register_global("json_decode", make_host("json_decode", &bi_json_decode));
        interp->register_global("time", make_host("time", &bi_time));
        interp->register_global("sleep", make_host("sleep", &bi_sleep));
        interp->register_global("load_script",
                                make_host("load_script", [interp](std::vector<emma_value> args) {
                                    return bi_load_script(interp, std::move(args));
                                }));
        interp->register_global("import",
                                make_host("import", [interp](std::vector<emma_value> args) {
                                    return bi_load_script(interp, std::move(args));
                                }));
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_install_math_stdlib(interpreter* interp) {
    return sao_plugins_emma_install_stdlib(interp);
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_emma_install_log_bridge(
    interpreter* interp, void (*log_fn)(const char* utf8, void* ud), void* user_data) {
    if (interp == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard<std::mutex> lock(log_bridge_mutex());
        auto& slot = log_bridge_table()[interp];
        slot.fn = log_fn;
        slot.ud = user_data;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_emma_list_builtins(
    interpreter* /*interp*/, const char*** out_names, size_t* out_count) {
    if (out_names != nullptr)
        *out_names = nullptr;
    if (out_count != nullptr)
        *out_count = 0;
    if (out_names == nullptr || out_count == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    static const char* const NAMES[] = {
        "print", "str",   "int",         "float", "tostring", "tonumber",    "len",
        "type",  "range", "list",        "dict",  "pairs",    "ipairs",      "abs",
        "min",   "max",   "floor",       "ceil",  "sqrt",     "json_encode", "json_decode",
        "time",  "sleep", "load_script", "import"};
    static constexpr size_t COUNT = sizeof(NAMES) / sizeof(NAMES[0]);
    const char** buf = static_cast<const char**>(std::malloc(sizeof(const char*) * COUNT));
    if (buf == nullptr)
        return SAO_ERR_OS_CALL_FAILED;
    for (size_t i = 0; i < COUNT; ++i)
        buf[i] = NAMES[i];
    *out_names = buf;
    *out_count = COUNT;
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL sao_plugins_emma_free_names(const char** names,
                                                                             size_t /*count*/) {
    if (names != nullptr)
        std::free(names);
}

void release_stdlib_state(interpreter* interp) noexcept {
    if (interp == nullptr)
        return;
    try {
        std::lock_guard<std::mutex> lock(log_bridge_mutex());
        log_bridge_table().erase(interp);
    } catch (...) {
    }
}

} // namespace sao::plugins::emma_host
