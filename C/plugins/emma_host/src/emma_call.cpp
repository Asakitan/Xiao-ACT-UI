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

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace sao::plugins::emma_host {

// 外部符号 (定义在 emma_interpreter.cpp)
std::string emma_value_to_string(const emma_value& v);

// ── 内部句柄类型 ──
struct emma_plugin_s {
    std::string plugin_id;
    std::string entry_path;
    ast_pool pool;
    std::unique_ptr<interpreter> interp;
    // hook 引用 (弱查, 每次调再 get_function 也可)
};

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_load_script(const wchar_t* plugin_dir,
                             const char* entry_relative,
                             const char* plugin_id_utf8,
                             void* /*ctx_ptr*/,
                             emma_plugin_handle_t* out_plugin) {
    if (out_plugin == nullptr || entry_relative == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *out_plugin = nullptr;

    // 拼路径 (wchar_t + narrow relative)
    std::string entry_path;
    if (plugin_dir != nullptr) {
        std::wstring wide(plugin_dir);
        std::string narrow;
        narrow.reserve(wide.size());
        for (wchar_t wc : wide) {
            if (wc < 0x80) narrow.push_back(static_cast<char>(wc));
            else narrow.push_back('?');   // 简化: ASCII fallback
        }
        entry_path = std::move(narrow);
        if (!entry_path.empty() && entry_path.back() != '/' && entry_path.back() != '\\') {
            entry_path.push_back('/');
        }
    }
    entry_path += entry_relative;

    std::ifstream fp(entry_path, std::ios::binary);
    if (!fp) return SAO_ERR_OS_CALL_FAILED;
    std::stringstream ss;
    ss << fp.rdbuf();
    std::string source = ss.str();

    auto plugin = std::make_unique<emma_plugin_s>();
    if (plugin_id_utf8) plugin->plugin_id = plugin_id_utf8;
    plugin->entry_path = entry_path;

    plugin->interp = std::make_unique<interpreter>();
    plugin->interp->set_pool(&plugin->pool);
    sao_plugins_emma_install_stdlib(plugin->interp.get());
    if (plugin_dir != nullptr) {
        sao_plugins_emma_install_io_stdlib(plugin->interp.get(), plugin_dir);
    }

    std::vector<token> tokens = tokenize_source(source);
    parser p(tokens, &plugin->pool);
    std::vector<node_id> stmts;
    std::string err;
    int32_t rc = p.parse_program(stmts, err);
    if (rc != SAO_OK) return SAO_ERR_INVALID_ARGUMENT;

    rc = plugin->interp->execute(stmts, err);
    if (rc != SAO_OK) return SAO_ERR_OS_CALL_FAILED;

    *out_plugin = plugin.release();
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_call_on_load(emma_plugin_handle_t plugin) {
    if (plugin == nullptr) return SAO_ERR_HANDLE_INVALID;
    auto fn = plugin->interp->get_function("on_load");
    if (!fn) return SAO_OK;   // 无 hook 视作成功 skip
    std::string err;
    (void)plugin->interp->call_function(fn, {}, err);
    return err.empty() ? SAO_OK : SAO_ERR_OS_CALL_FAILED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_call_on_enable(emma_plugin_handle_t plugin) {
    if (plugin == nullptr) return SAO_ERR_HANDLE_INVALID;
    auto fn = plugin->interp->get_function("on_enable");
    if (!fn) return SAO_OK;
    std::string err;
    (void)plugin->interp->call_function(fn, {}, err);
    return err.empty() ? SAO_OK : SAO_ERR_OS_CALL_FAILED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_call_on_disable(emma_plugin_handle_t plugin) {
    if (plugin == nullptr) return SAO_ERR_HANDLE_INVALID;
    auto fn = plugin->interp->get_function("on_disable");
    if (!fn) return SAO_OK;
    std::string err;
    (void)plugin->interp->call_function(fn, {}, err);
    return err.empty() ? SAO_OK : SAO_ERR_OS_CALL_FAILED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_call_on_unload(emma_plugin_handle_t plugin, bool* out_allow_unload) {
    if (plugin == nullptr) return SAO_ERR_HANDLE_INVALID;
    if (out_allow_unload) *out_allow_unload = true;
    auto fn = plugin->interp->get_function("on_unload");
    if (!fn) return SAO_OK;
    std::string err;
    (void)plugin->interp->call_function(fn, {}, err);
    return err.empty() ? SAO_OK : SAO_ERR_OS_CALL_FAILED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_call_hook(emma_plugin_handle_t plugin,
                           const char* hook_name,
                           const char* /*args_json_utf8*/,
                           char** out_result_json_utf8) {
    if (plugin == nullptr || hook_name == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    if (out_result_json_utf8) *out_result_json_utf8 = nullptr;
    auto fn = plugin->interp->get_function(hook_name);
    if (!fn) return SAO_ERR_HANDLE_INVALID;
    std::string err;
    emma_value r = plugin->interp->call_function(fn, {}, err);
    if (!err.empty()) return SAO_ERR_OS_CALL_FAILED;
    if (out_result_json_utf8) {
        std::string s = emma_value_to_string(r);
        char* buf = static_cast<char*>(std::malloc(s.size() + 1));
        if (buf) {
            std::memcpy(buf, s.data(), s.size());
            buf[s.size()] = '\0';
            *out_result_json_utf8 = buf;
        }
    }
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_emma_has_hook(emma_plugin_handle_t plugin, const char* hook_name) {
    if (plugin == nullptr || hook_name == nullptr) return false;
    auto fn = plugin->interp->get_function(hook_name);
    return static_cast<bool>(fn);
}

extern "C" SAO_PLUGINS_API interpreter* SAO_PLUGINS_CALL
sao_plugins_emma_get_interpreter(emma_plugin_handle_t plugin) {
    if (plugin == nullptr) return nullptr;
    return plugin->interp.get();
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_unload_script(emma_plugin_handle_t plugin) {
    if (plugin == nullptr) return SAO_ERR_HANDLE_INVALID;
    delete plugin;
    return SAO_OK;
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
    if (interp == nullptr || source_utf8 == nullptr || out_pool == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    if (out_error_utf8) *out_error_utf8 = nullptr;

    interp->set_pool(out_pool);
    std::vector<token> tokens = tokenize_source(std::string_view(source_utf8, source_len));
    parser p(tokens, out_pool);
    std::vector<node_id> stmts;
    std::string err;
    int32_t rc = p.parse_program(stmts, err);
    if (rc != SAO_OK) {
        if (out_error_utf8) {
            char* buf = static_cast<char*>(std::malloc(err.size() + 1));
            if (buf) { std::memcpy(buf, err.data(), err.size()); buf[err.size()] = '\0'; *out_error_utf8 = buf; }
        }
        return rc;
    }
    rc = interp->execute(stmts, err);
    if (rc != SAO_OK && out_error_utf8) {
        char* buf = static_cast<char*>(std::malloc(err.size() + 1));
        if (buf) { std::memcpy(buf, err.data(), err.size()); buf[err.size()] = '\0'; *out_error_utf8 = buf; }
    }
    return rc;
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_emma_free_string(char* s) {
    if (s != nullptr) std::free(s);
}

} // namespace sao::plugins::emma_host
