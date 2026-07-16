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

#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#if defined(SAO_HAS_ANGELSCRIPT)
#include <angelscript.h>
#endif

namespace sao::plugins::angel_host {

#if defined(SAO_HAS_ANGELSCRIPT)

struct as_host_s {
    asIScriptEngine* engine = nullptr;
    void (*message_callback)(const char*, int, int, int, void*) = nullptr;
    void* user_data = nullptr;
    // 收集 message 内部 buffer, 供 execute 报错时读
    std::string last_error;
};

static void as_message_relay(const asSMessageInfo* msg, void* param) {
    auto* host = static_cast<as_host_s*>(param);
    // 收集到 last_error
    std::ostringstream oss;
    oss << (msg->section ? msg->section : "") << " (" << msg->row << ", " << msg->col << ") ";
    switch (msg->type) {
        case asMSGTYPE_ERROR:       oss << "ERR: "; break;
        case asMSGTYPE_WARNING:     oss << "WARN: "; break;
        case asMSGTYPE_INFORMATION: oss << "INFO: "; break;
    }
    oss << (msg->message ? msg->message : "");
    std::string line = oss.str();
    if (!host->last_error.empty()) host->last_error.push_back('\n');
    host->last_error += line;
    if (host->message_callback) {
        host->message_callback(msg->message, msg->row, msg->col,
                               static_cast<int>(msg->type), host->user_data);
    }
}

#endif // SAO_HAS_ANGELSCRIPT

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_create(const as_host_config* cfg, as_host_handle_t* out_host) {
    if (out_host == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *out_host = nullptr;
#if defined(SAO_HAS_ANGELSCRIPT)
    asIScriptEngine* engine = asCreateScriptEngine(ANGELSCRIPT_VERSION);
    if (engine == nullptr) return SAO_ERR_OS_CALL_FAILED;
    auto* host = new as_host_s();
    host->engine = engine;
    if (cfg) {
        host->message_callback = cfg->message_callback;
        host->user_data = cfg->callback_user_data;
    }
    engine->SetMessageCallback(asFUNCTION(as_message_relay), host, asCALL_CDECL);
    *out_host = host;
    return SAO_OK;
#else
    (void)cfg;
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_destroy(as_host_handle_t host) {
    if (host == nullptr) return SAO_ERR_HANDLE_INVALID;
#if defined(SAO_HAS_ANGELSCRIPT)
    if (host->engine) host->engine->ShutDownAndRelease();
    delete host;
    return SAO_OK;
#else
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API asIScriptEngine* SAO_PLUGINS_CALL
sao_plugins_ashost_engine(as_host_handle_t host) {
#if defined(SAO_HAS_ANGELSCRIPT)
    return host ? host->engine : nullptr;
#else
    (void)host;
    return nullptr;
#endif
}

extern "C" SAO_PLUGINS_API const char* SAO_PLUGINS_CALL
sao_plugins_ashost_version(void) {
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
sao_plugins_ashost_execute(as_host_handle_t host,
                           const char* source_utf8,
                           size_t source_len,
                           char** out_result_utf8,
                           char** out_error_utf8) {
    if (out_result_utf8) *out_result_utf8 = nullptr;
    if (out_error_utf8)  *out_error_utf8  = nullptr;
    if (host == nullptr || source_utf8 == nullptr) return SAO_ERR_INVALID_ARGUMENT;
#if defined(SAO_HAS_ANGELSCRIPT)
    asIScriptEngine* engine = host->engine;
    if (engine == nullptr) return SAO_ERR_HANDLE_INVALID;

    host->last_error.clear();

    asIScriptModule* mod = engine->GetModule("wave3", asGM_ALWAYS_CREATE);
    if (mod == nullptr) {
        if (out_error_utf8) {
            const char* m = "GetModule failed";
            char* buf = static_cast<char*>(std::malloc(std::strlen(m) + 1));
            if (buf) { std::strcpy(buf, m); *out_error_utf8 = buf; }
        }
        return SAO_ERR_OS_CALL_FAILED;
    }

    std::string src(source_utf8, source_len);
    int r = mod->AddScriptSection("wave3_source", src.c_str(), src.size());
    if (r < 0) {
        if (out_error_utf8) {
            std::string msg = host->last_error.empty() ? "AddScriptSection failed" : host->last_error;
            char* buf = static_cast<char*>(std::malloc(msg.size() + 1));
            if (buf) { std::memcpy(buf, msg.data(), msg.size()); buf[msg.size()] = '\0'; *out_error_utf8 = buf; }
        }
        return SAO_ERR_INVALID_ARGUMENT;
    }
    r = mod->Build();
    if (r < 0) {
        if (out_error_utf8) {
            std::string msg = host->last_error.empty() ? "Build failed" : host->last_error;
            char* buf = static_cast<char*>(std::malloc(msg.size() + 1));
            if (buf) { std::memcpy(buf, msg.data(), msg.size()); buf[msg.size()] = '\0'; *out_error_utf8 = buf; }
        }
        return SAO_ERR_INVALID_ARGUMENT;
    }

    // 找 entry
    asIScriptFunction* fn = mod->GetFunctionByName("__entry__");
    if (fn == nullptr) fn = mod->GetFunctionByName("main");
    if (fn == nullptr) {
        // 拿第一个 no-arg 函数
        for (asUINT i = 0; i < mod->GetFunctionCount(); ++i) {
            asIScriptFunction* candidate = mod->GetFunctionByIndex(i);
            if (candidate && candidate->GetParamCount() == 0) { fn = candidate; break; }
        }
    }
    if (fn == nullptr) {
        // 无 entry, 视作 OK (顶层 build 已成功 → 相当于只做定义/编译)
        return SAO_OK;
    }

    asIScriptContext* ctx = engine->CreateContext();
    if (ctx == nullptr) return SAO_ERR_OS_CALL_FAILED;
    ctx->Prepare(fn);
    r = ctx->Execute();
    if (r == asEXECUTION_EXCEPTION) {
        if (out_error_utf8) {
            const char* msg = ctx->GetExceptionString();
            if (msg) {
                size_t mlen = std::strlen(msg);
                char* buf = static_cast<char*>(std::malloc(mlen + 1));
                if (buf) { std::memcpy(buf, msg, mlen); buf[mlen] = '\0'; *out_error_utf8 = buf; }
            }
        }
        ctx->Release();
        return SAO_ERR_OS_CALL_FAILED;
    }
    if (r != asEXECUTION_FINISHED) {
        if (out_error_utf8) {
            const char* m = "Execute did not finish";
            char* buf = static_cast<char*>(std::malloc(std::strlen(m) + 1));
            if (buf) { std::strcpy(buf, m); *out_error_utf8 = buf; }
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
            ret_type_id == asTYPEID_INT8  || ret_type_id == asTYPEID_UINT8) {
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
            if (buf) { std::memcpy(buf, s.data(), s.size()); buf[s.size()] = '\0'; *out_result_utf8 = buf; }
        }
    }
    ctx->Release();
    return SAO_OK;
#else
    (void)source_len;
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_function_by_name(as_host_handle_t host,
                                          const char* fn_name,
                                          char** out_result_utf8,
                                          char** out_error_utf8) {
    if (out_result_utf8) *out_result_utf8 = nullptr;
    if (out_error_utf8)  *out_error_utf8  = nullptr;
    if (host == nullptr || fn_name == nullptr) return SAO_ERR_INVALID_ARGUMENT;
#if defined(SAO_HAS_ANGELSCRIPT)
    asIScriptEngine* engine = host->engine;
    if (engine == nullptr) return SAO_ERR_HANDLE_INVALID;
    asIScriptModule* mod = engine->GetModule("wave3", asGM_ONLY_IF_EXISTS);
    if (mod == nullptr) return SAO_ERR_HANDLE_INVALID;
    asIScriptFunction* fn = mod->GetFunctionByName(fn_name);
    if (fn == nullptr) {
        if (out_error_utf8) {
            const char* m = "function not found";
            char* buf = static_cast<char*>(std::malloc(std::strlen(m) + 1));
            if (buf) { std::strcpy(buf, m); *out_error_utf8 = buf; }
        }
        return SAO_ERR_HANDLE_INVALID;
    }
    asIScriptContext* ctx = engine->CreateContext();
    if (ctx == nullptr) return SAO_ERR_OS_CALL_FAILED;
    ctx->Prepare(fn);
    int r = ctx->Execute();
    if (r != asEXECUTION_FINISHED) {
        if (out_error_utf8 && r == asEXECUTION_EXCEPTION) {
            const char* msg = ctx->GetExceptionString();
            if (msg) {
                size_t mlen = std::strlen(msg);
                char* buf = static_cast<char*>(std::malloc(mlen + 1));
                if (buf) { std::memcpy(buf, msg, mlen); buf[mlen] = '\0'; *out_error_utf8 = buf; }
            }
        }
        ctx->Release();
        return SAO_ERR_OS_CALL_FAILED;
    }
    if (out_result_utf8) {
        int t = fn->GetReturnTypeId();
        std::string s;
        if (t == asTYPEID_INT32 || t == asTYPEID_UINT32) s = std::to_string(ctx->GetReturnDWord());
        else if (t == asTYPEID_FLOAT) {
            char buf[32]; std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(ctx->GetReturnFloat())); s = buf;
        }
        else if (t == asTYPEID_DOUBLE) {
            char buf[32]; std::snprintf(buf, sizeof(buf), "%g", ctx->GetReturnDouble()); s = buf;
        }
        if (!s.empty()) {
            char* buf = static_cast<char*>(std::malloc(s.size() + 1));
            if (buf) { std::memcpy(buf, s.data(), s.size()); buf[s.size()] = '\0'; *out_result_utf8 = buf; }
        }
    }
    ctx->Release();
    return SAO_OK;
#else
    (void)fn_name;
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_ashost_free_string(char* s) {
    if (s != nullptr) std::free(s);
}

extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_ashost_is_available(void) {
#if defined(SAO_HAS_ANGELSCRIPT)
    return true;
#else
    return false;
#endif
}

} // namespace sao::plugins::angel_host
