// as_host.cpp — 真 AngelScript SDK 嵌入实装
//
// CMake gate: SAO_HAS_ANGELSCRIPT (find_package(unofficial-angelscript) 成功)
// 未 gated 时全部 API 返回 SAO_ERR_NOT_IMPLEMENTED。
//
// 只用核心 AngelScript API — 不依赖 scripthelper/scriptstdstring 等 addon
// (那些需要单独编译 addon 源, 未来 wave 引入)。因此 wave3 首切片:
//   - execute source: engine->GetModule("m", asGM_ALWAYS_CREATE) + AddScriptSection
//                     + Build + 找 int main() 或 int foo() 或 double foo() 入口执行
//   - call_function: 拿 module→GetFunctionByName + context Execute (只支持
//                    int/double 返回值 wave3)

#include "sao/plugins/angel_host/as_host.h"

#include "as_generic_bindings_internal.h"

#include "sao/plugins/angel_host/as_error.h"
#include "sao/plugins/angel_host/as_module_bridge.h"
#include "sao/plugins/angel_host/as_stdlib.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(SAO_HAS_ANGELSCRIPT)
#include <angelscript.h>
#endif

namespace sao::plugins::angel_host {

#if defined(SAO_HAS_ANGELSCRIPT)

struct as_host_s {
    struct retained_message {
        std::string section;
        std::string message;
        int row = 0;
        int column = 0;
        int type = 0;
    };

    asIScriptEngine* engine = nullptr;
    void (*message_callback)(const char*, int, int, int, void*) = nullptr;
    void* user_data = nullptr;
    std::mutex engine_mutex;
    std::mutex message_mutex;
    std::vector<retained_message> messages;
};

std::mutex g_host_registry_mutex;
std::unordered_map<as_host_handle_t, std::shared_ptr<as_host_s>> g_host_registry;

std::shared_ptr<as_host_s> acquire_host(as_host_handle_t host) {
    if (host == nullptr)
        return {};
    std::lock_guard lock(g_host_registry_mutex);
    const auto found = g_host_registry.find(host);
    return found == g_host_registry.end() ? std::shared_ptr<as_host_s>{} : found->second;
}

std::shared_ptr<as_host_s> retire_host(as_host_handle_t host) {
    if (host == nullptr)
        return {};
    std::lock_guard lock(g_host_registry_mutex);
    const auto found = g_host_registry.find(host);
    if (found == g_host_registry.end())
        return {};
    auto state = found->second;
    g_host_registry.erase(found);
    return state;
}

std::string retained_messages_text(as_host_s& host, const char* fallback) {
    std::lock_guard lock(host.message_mutex);
    if (host.messages.empty())
        return fallback;
    std::ostringstream output;
    for (size_t i = 0; i < host.messages.size(); ++i) {
        const auto& message = host.messages[i];
        if (i != 0)
            output << '\n';
        output << message.section << " (" << message.row << ", " << message.column << ") ";
        switch (message.type) {
        case asMSGTYPE_ERROR:
            output << "ERR: ";
            break;
        case asMSGTYPE_WARNING:
            output << "WARN: ";
            break;
        default:
            output << "INFO: ";
            break;
        }
        output << message.message;
    }
    return output.str();
}

void clear_retained_messages(as_host_s& host) {
    std::lock_guard lock(host.message_mutex);
    host.messages.clear();
}

static void as_message_relay(const asSMessageInfo* msg, void* param) {
    auto* host = static_cast<as_host_s*>(param);
    void (*callback)(const char*, int, int, int, void*) = nullptr;
    void* callback_user_data = nullptr;
    {
        std::lock_guard lock(host->message_mutex);
        host->messages.push_back({msg->section == nullptr ? "" : msg->section,
                                  msg->message == nullptr ? "" : msg->message, msg->row, msg->col,
                                  static_cast<int>(msg->type)});
        callback = host->message_callback;
        callback_user_data = host->user_data;
    }
    if (callback != nullptr) {
        callback(msg->message, msg->row, msg->col, static_cast<int>(msg->type), callback_user_data);
    }
}

#endif // SAO_HAS_ANGELSCRIPT

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_create(const as_host_config* cfg, as_host_handle_t* out_host) {
    if (out_host == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_host = nullptr;
#if defined(SAO_HAS_ANGELSCRIPT)
    asIScriptEngine* engine = asCreateScriptEngine(ANGELSCRIPT_VERSION);
    if (engine == nullptr)
        return SAO_ERR_OS_CALL_FAILED;
    auto host = std::make_shared<as_host_s>();
    host->engine = engine;
    if (cfg) {
        host->message_callback = cfg->message_callback;
        host->user_data = cfg->callback_user_data;
    }
    engine->SetMessageCallback(asFUNCTION(as_message_relay), host.get(), asCALL_CDECL);
    {
        std::lock_guard lock(g_host_registry_mutex);
        g_host_registry.emplace(host.get(), host);
    }
    *out_host = host.get();
    return SAO_OK;
#else
    (void)cfg;
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_destroy(as_host_handle_t host) {
    if (host == nullptr)
        return SAO_ERR_HANDLE_INVALID;
#if defined(SAO_HAS_ANGELSCRIPT)
    const auto state = retire_host(host);
    if (!state)
        return SAO_ERR_HANDLE_INVALID;
    std::lock_guard lock(state->engine_mutex);
    std::lock_guard engine_lock(engine_execution_mutex());
    if (state->engine) {
        state->engine->ShutDownAndRelease();
        state->engine = nullptr;
    }
    return SAO_OK;
#else
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API asIScriptEngine* SAO_PLUGINS_CALL
sao_plugins_ashost_engine(as_host_handle_t host) {
#if defined(SAO_HAS_ANGELSCRIPT)
    const auto state = acquire_host(host);
    if (!state)
        return nullptr;
    std::lock_guard lock(state->engine_mutex);
    return state->engine;
#else
    (void)host;
    return nullptr;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_get_last_error(as_host_handle_t host, char** out_error_utf8) {
    if (out_error_utf8 == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_error_utf8 = nullptr;
#if defined(SAO_HAS_ANGELSCRIPT)
    const auto state = acquire_host(host);
    if (!state)
        return SAO_ERR_HANDLE_INVALID;
    try {
        const std::string error = retained_messages_text(*state, "AngelScript build failed");
        auto* copy = static_cast<char*>(std::malloc(error.size() + 1));
        if (copy == nullptr)
            return SAO_ERR_OS_CALL_FAILED;
        std::memcpy(copy, error.c_str(), error.size() + 1);
        *out_error_utf8 = copy;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#else
    (void)host;
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API const char* SAO_PLUGINS_CALL sao_plugins_ashost_version(void) {
#if defined(SAO_HAS_ANGELSCRIPT)
    return asGetLibraryVersion();
#else
    return "angel_host: not available (SAO_HAS_ANGELSCRIPT not defined)";
#endif
}

// ── Wave 3 host-level 便利入口 ──
//
// execute source 语义:
//   1. 建/覆盖 module "main"
//   2. AddScriptSection + Build
//   3. 找 "int __entry__()" / "double __entry__()" 之类; 若没有 __entry__,
//      找第一个 no-arg 函数.
//   4. 用 context 调, 结果 int → 十进制字符串, double → %g
//
// 简化: 要求脚本必须定义 int __entry__() 或 void __entry__(). 详见 test。

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_execute(as_host_handle_t host, const char* source_utf8, size_t source_len,
                           char** out_result_utf8, char** out_error_utf8) {
    if (out_result_utf8)
        *out_result_utf8 = nullptr;
    if (out_error_utf8)
        *out_error_utf8 = nullptr;
    if (host == nullptr || source_utf8 == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
#if defined(SAO_HAS_ANGELSCRIPT)
    const auto state = acquire_host(host);
    if (!state)
        return SAO_ERR_HANDLE_INVALID;
    std::lock_guard lock(state->engine_mutex);
    std::lock_guard engine_lock(engine_execution_mutex());
    asIScriptEngine* engine = state->engine;
    if (engine == nullptr)
        return SAO_ERR_HANDLE_INVALID;

    clear_retained_messages(*state);

    asIScriptModule* mod = engine->GetModule("wave3", asGM_ALWAYS_CREATE);
    if (mod == nullptr) {
        if (out_error_utf8) {
            const char* m = "GetModule failed";
            char* buf = static_cast<char*>(std::malloc(std::strlen(m) + 1));
            if (buf) {
                std::strcpy(buf, m);
                *out_error_utf8 = buf;
            }
        }
        return SAO_ERR_OS_CALL_FAILED;
    }

    std::string src(source_utf8, source_len);
    int r = mod->AddScriptSection("wave3_source", src.c_str(), src.size());
    if (r < 0) {
        if (out_error_utf8) {
            const std::string msg = retained_messages_text(*state, "AddScriptSection failed");
            char* buf = static_cast<char*>(std::malloc(msg.size() + 1));
            if (buf) {
                std::memcpy(buf, msg.data(), msg.size());
                buf[msg.size()] = '\0';
                *out_error_utf8 = buf;
            }
        }
        return SAO_ERR_INVALID_ARGUMENT;
    }
    r = mod->Build();
    if (r < 0) {
        if (out_error_utf8) {
            const std::string msg = retained_messages_text(*state, "Build failed");
            char* buf = static_cast<char*>(std::malloc(msg.size() + 1));
            if (buf) {
                std::memcpy(buf, msg.data(), msg.size());
                buf[msg.size()] = '\0';
                *out_error_utf8 = buf;
            }
        }
        return SAO_ERR_INVALID_ARGUMENT;
    }

    // 找 entry
    asIScriptFunction* fn = mod->GetFunctionByName("__entry__");
    if (fn == nullptr)
        fn = mod->GetFunctionByName("main");
    if (fn == nullptr) {
        // 拿第一个 no-arg 函数
        for (asUINT i = 0; i < mod->GetFunctionCount(); ++i) {
            asIScriptFunction* candidate = mod->GetFunctionByIndex(i);
            if (candidate && candidate->GetParamCount() == 0) {
                fn = candidate;
                break;
            }
        }
    }
    if (fn == nullptr) {
        // 无 entry, 视作 OK (顶层 build 已成功 → 相当于只做定义/编译)
        return SAO_OK;
    }

    asIScriptContext* ctx = engine->CreateContext();
    if (ctx == nullptr)
        return SAO_ERR_OS_CALL_FAILED;
    if (ctx->Prepare(fn) < 0) {
        ctx->Release();
        return SAO_ERR_OS_CALL_FAILED;
    }
    r = ctx->Execute();
    if (r == asEXECUTION_EXCEPTION) {
        const int32_t status = out_error_utf8 == nullptr
                                   ? SAO_ERR_OS_CALL_FAILED
                                   : sao_plugins_ashost_take_exception(ctx, out_error_utf8);
        ctx->Release();
        return status;
    }
    if (r != asEXECUTION_FINISHED) {
        if (out_error_utf8) {
            const char* m = "Execute did not finish";
            char* buf = static_cast<char*>(std::malloc(std::strlen(m) + 1));
            if (buf) {
                std::strcpy(buf, m);
                *out_error_utf8 = buf;
            }
        }
        ctx->Release();
        return SAO_ERR_OS_CALL_FAILED;
    }

    // 返回值
    if (out_result_utf8) {
        int ret_type_id = fn->GetReturnTypeId();
        std::string s;
        if (ret_type_id == asTYPEID_INT32 || ret_type_id == asTYPEID_UINT32 ||
            ret_type_id == asTYPEID_INT16 || ret_type_id == asTYPEID_UINT16 ||
            ret_type_id == asTYPEID_INT8 || ret_type_id == asTYPEID_UINT8) {
            s = std::to_string(ctx->GetReturnDWord());
        } else if (ret_type_id == asTYPEID_INT64 || ret_type_id == asTYPEID_UINT64) {
            s = std::to_string(ctx->GetReturnQWord());
        } else if (ret_type_id == asTYPEID_FLOAT) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(ctx->GetReturnFloat()));
            s = buf;
        } else if (ret_type_id == asTYPEID_DOUBLE) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", ctx->GetReturnDouble());
            s = buf;
        } else if (ret_type_id == asTYPEID_BOOL) {
            s = ctx->GetReturnByte() ? "true" : "false";
        }
        if (!s.empty()) {
            char* buf = static_cast<char*>(std::malloc(s.size() + 1));
            if (buf) {
                std::memcpy(buf, s.data(), s.size());
                buf[s.size()] = '\0';
                *out_result_utf8 = buf;
            }
        }
    }
    ctx->Release();
    return SAO_OK;
#else
    (void)source_len;
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ashost_call_function_by_name(
    as_host_handle_t host, const char* fn_name, char** out_result_utf8, char** out_error_utf8) {
    if (out_result_utf8)
        *out_result_utf8 = nullptr;
    if (out_error_utf8)
        *out_error_utf8 = nullptr;
    if (host == nullptr || fn_name == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
#if defined(SAO_HAS_ANGELSCRIPT)
    const auto state = acquire_host(host);
    if (!state)
        return SAO_ERR_HANDLE_INVALID;
    std::lock_guard lock(state->engine_mutex);
    std::lock_guard engine_lock(engine_execution_mutex());
    asIScriptEngine* engine = state->engine;
    if (engine == nullptr)
        return SAO_ERR_HANDLE_INVALID;
    asIScriptModule* mod = engine->GetModule("wave3", asGM_ONLY_IF_EXISTS);
    if (mod == nullptr)
        return SAO_ERR_HANDLE_INVALID;
    asIScriptFunction* fn = mod->GetFunctionByName(fn_name);
    if (fn == nullptr) {
        if (out_error_utf8) {
            const char* m = "function not found";
            char* buf = static_cast<char*>(std::malloc(std::strlen(m) + 1));
            if (buf) {
                std::strcpy(buf, m);
                *out_error_utf8 = buf;
            }
        }
        return SAO_ERR_HANDLE_INVALID;
    }
    asIScriptContext* ctx = engine->CreateContext();
    if (ctx == nullptr)
        return SAO_ERR_OS_CALL_FAILED;
    if (ctx->Prepare(fn) < 0) {
        ctx->Release();
        return SAO_ERR_OS_CALL_FAILED;
    }
    int r = ctx->Execute();
    if (r != asEXECUTION_FINISHED) {
        const int32_t status = out_error_utf8 != nullptr && r == asEXECUTION_EXCEPTION
                                   ? sao_plugins_ashost_take_exception(ctx, out_error_utf8)
                                   : SAO_ERR_OS_CALL_FAILED;
        ctx->Release();
        return status;
    }
    if (out_result_utf8) {
        int t = fn->GetReturnTypeId();
        std::string s;
        if (t == asTYPEID_INT32 || t == asTYPEID_UINT32)
            s = std::to_string(ctx->GetReturnDWord());
        else if (t == asTYPEID_FLOAT) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(ctx->GetReturnFloat()));
            s = buf;
        } else if (t == asTYPEID_DOUBLE) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", ctx->GetReturnDouble());
            s = buf;
        }
        if (!s.empty()) {
            char* buf = static_cast<char*>(std::malloc(s.size() + 1));
            if (buf) {
                std::memcpy(buf, s.data(), s.size());
                buf[s.size()] = '\0';
                *out_result_utf8 = buf;
            }
        }
    }
    ctx->Release();
    return SAO_OK;
#else
    (void)fn_name;
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL sao_plugins_ashost_free_string(char* s) {
    if (s != nullptr)
        std::free(s);
}

extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL sao_plugins_ashost_is_available(void) {
#if defined(SAO_HAS_ANGELSCRIPT)
    return true;
#else
    return false;
#endif
}

} // namespace sao::plugins::angel_host
