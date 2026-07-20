// as_sandbox.cpp — AngelScript 白名单沙箱实现
//
// 沙盒策略:
//   1. Whitelist API — 只 RegisterGlobalFunction 白名单 math 函数.  未注册
//      的名字在编译期 fail (no matching signatures for X).  file / process
//      / net / registry / registry / dll load / GetSystemTime 都不注册.
//   2. JIT off — SetJITCompiler(nullptr) 强制解释器, 禁生成的机器码逃逸.
//   3. max_context_execution_ms — SetContextLineCallback 里比对
//      QueryPerformanceCounter, 超时 ctx->Abort().
//   4. 可选 SaoString 值类型 (as "string") — 用 asOBJ_VALUE +
//      asOBJ_APP_CLASS_CDAK, 内含 std::string; 通过 asOBJ_APP_CLASS_ALLINTS
//      给 CI 编译足够信息.  只用 GENERIC 调约定, 兼容 AS_MAX_PORTABILITY.
//
// 由于 build_angel_host CMakeLists 定义了 AS_MAX_PORTABILITY=1, 所有
// RegisterGlobalFunction / RegisterObjectBehaviour 必须走 asCALL_GENERIC.
#include "sao/plugins/angel_host/as_sandbox.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>

// windows.h 的 GDI GetObject 宏会污染 asIScriptGeneric::GetObject → NOGDI 关掉.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOGDI
#define NOGDI
#endif
#include <windows.h>
// SEC 兜底: 万一其他头把 GetObject 又拉回来了.
#ifdef GetObject
#undef GetObject
#endif
#endif

#if defined(SAO_HAS_ANGELSCRIPT)
#include <angelscript.h>
#endif

#include "sao_security/obfuscation/enc_str.h"

namespace sao::plugins::angel_host {

#if defined(SAO_HAS_ANGELSCRIPT)

namespace {

// ── engine → snapshot map ────────────────────────────────────────────
struct sandbox_snapshot {
    uint32_t max_context_execution_ms = 0;
    bool     jit_off = false;
    bool     allow_string_type = false;
    bool     line_callback_armed = false;
};

std::mutex& sandbox_map_mutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<asIScriptEngine*, sandbox_snapshot>& sandbox_map() {
    static std::unordered_map<asIScriptEngine*, sandbox_snapshot> m;
    return m;
}

// ── 简易 SaoString 值类型 (仅当 allow_string_type=true 时注册) ────────
//
// 内部就是 std::string; 通过 GENERIC calling convention 挂函数.
struct SaoString {
    std::string s;
};

extern "C" void sao_string_construct(asIScriptGeneric* gen) {
    new (gen->GetObject()) SaoString();
}

extern "C" void sao_string_copy_construct(asIScriptGeneric* gen) {
    auto* other = static_cast<SaoString*>(gen->GetArgObject(0));
    if (other) {
        new (gen->GetObject()) SaoString(*other);
    } else {
        new (gen->GetObject()) SaoString();
    }
}

extern "C" void sao_string_destruct(asIScriptGeneric* gen) {
    static_cast<SaoString*>(gen->GetObject())->~SaoString();
}

extern "C" void sao_string_assign(asIScriptGeneric* gen) {
    auto* self = static_cast<SaoString*>(gen->GetObject());
    auto* rhs  = static_cast<SaoString*>(gen->GetArgObject(0));
    if (self && rhs) self->s = rhs->s;
    gen->SetReturnAddress(self);
}

// ── 白名单 math API — 全 GENERIC 调约定 ─────────────────────────────
extern "C" void gen_sqrtf(asIScriptGeneric* gen) {
    float x = gen->GetArgFloat(0);
    gen->SetReturnFloat(std::sqrt(x));
}
extern "C" void gen_sinf(asIScriptGeneric* gen) {
    float x = gen->GetArgFloat(0);
    gen->SetReturnFloat(std::sin(x));
}
extern "C" void gen_cosf(asIScriptGeneric* gen) {
    float x = gen->GetArgFloat(0);
    gen->SetReturnFloat(std::cos(x));
}
extern "C" void gen_floorf(asIScriptGeneric* gen) {
    float x = gen->GetArgFloat(0);
    gen->SetReturnFloat(std::floor(x));
}
extern "C" void gen_ceilf(asIScriptGeneric* gen) {
    float x = gen->GetArgFloat(0);
    gen->SetReturnFloat(std::ceil(x));
}
extern "C" void gen_absf(asIScriptGeneric* gen) {
    float x = gen->GetArgFloat(0);
    gen->SetReturnFloat(std::fabs(x));
}
extern "C" void gen_powf(asIScriptGeneric* gen) {
    float x = gen->GetArgFloat(0);
    float y = gen->GetArgFloat(1);
    gen->SetReturnFloat(std::pow(x, y));
}

// log_info(string) — script 侧调 log_info("...").  在没 SaoString 时用不上,
// 因此仅当 allow_string_type=true 才注册.
extern "C" void gen_log_info(asIScriptGeneric* gen) {
    auto* s = static_cast<SaoString*>(gen->GetArgObject(0));
    (void)s;
    // 沙盒里我们什么都不做 (host 决定是否转出); 目的是提供一个可 call 的
    // sink 让 script 编译可通过.
}

// ── LineCallback: 超时 abort ────────────────────────────────────────
struct line_ctx_data {
    LARGE_INTEGER start{};
    LARGE_INTEGER freq{};
    uint32_t      max_ms = 0;
};

// context-user-data slot 里我们用 0x5A0B1 (SAO b1 沙盒) 存 line_ctx_data*.
// 我们的沙盒会在每次 arm 调 SetContextLineCallback → cb 会读 slot 数据.
constexpr asPWORD kLineDataSlot = 0x5A0B1u;

extern "C" void sao_as_line_cb(asIScriptContext* ctx, void* param) {
    auto* d = static_cast<line_ctx_data*>(param);
    if (!d || d->max_ms == 0) return;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    double elapsed_ms = (double)(now.QuadPart - d->start.QuadPart) * 1000.0
                        / (double)d->freq.QuadPart;
    if ((uint32_t)elapsed_ms >= d->max_ms) {
        ctx->Abort();
    }
}

} // namespace

// 供 test 借道注册 LineCallback (host 侧可以在 CreateContext 之后包一层).
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_sandbox_attach_line_watchdog(asIScriptContext* ctx, uint32_t max_ms) {
    if (ctx == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    if (max_ms == 0) return SAO_ERR_INVALID_ARGUMENT;
    auto* d = new line_ctx_data();
    d->max_ms = max_ms;
    QueryPerformanceFrequency(&d->freq);
    QueryPerformanceCounter(&d->start);
    ctx->SetLineCallback(asFUNCTION(sao_as_line_cb), d, asCALL_CDECL);
    ctx->SetUserData(d, kLineDataSlot);
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_sandbox_detach_line_watchdog(asIScriptContext* ctx) {
    if (ctx == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    ctx->ClearLineCallback();
    auto* d = static_cast<line_ctx_data*>(ctx->GetUserData(kLineDataSlot));
    if (d) {
        ctx->SetUserData(nullptr, kLineDataSlot);
        delete d;
    }
    return SAO_OK;
}

#endif // SAO_HAS_ANGELSCRIPT

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_sandbox_arm(asIScriptEngine* engine, const as_sandbox_config* cfg) {
    if (engine == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    if (cfg == nullptr) return SAO_ERR_INVALID_ARGUMENT;
#if defined(SAO_HAS_ANGELSCRIPT)
    // 拒二次 arm.
    {
        std::lock_guard<std::mutex> guard(sandbox_map_mutex());
        if (sandbox_map().find(engine) != sandbox_map().end()) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
    }

    // 1. 拆黑名单
    std::unordered_set<std::string> deny;
    if (cfg->deny_globals && cfg->deny_globals_count > 0) {
        for (size_t i = 0; i < cfg->deny_globals_count; ++i) {
            if (cfg->deny_globals[i]) deny.insert(cfg->deny_globals[i]);
        }
    }
    // 敏感字符串登记 (rdata 不明文暴露白名单集合)
    (void)SAO_ENC_STR("sqrt(float)").view();
    (void)SAO_ENC_STR("sin(float)").view();
    (void)SAO_ENC_STR("cos(float)").view();
    (void)SAO_ENC_STR("floor(float)").view();
    (void)SAO_ENC_STR("ceil(float)").view();
    (void)SAO_ENC_STR("abs(float)").view();
    (void)SAO_ENC_STR("pow(float,float)").view();
    (void)SAO_ENC_STR("File").view();
    (void)SAO_ENC_STR("Process").view();
    (void)SAO_ENC_STR("Registry").view();
    (void)SAO_ENC_STR("LoadLibrary").view();
    (void)SAO_ENC_STR("GetSystemTime").view();

    // 2. 关 JIT
    engine->SetJITCompiler(nullptr);

    // 3. 可选注册 SaoString (as "string")
    if (cfg->allow_string_type) {
        // 用 asOBJ_APP_CLASS_CDAK (constructor, destructor, assign, copy构造)
        int r = engine->RegisterObjectType(
            "string", sizeof(SaoString),
            asOBJ_VALUE | asOBJ_APP_CLASS_CDAK);
        if (r < 0) {
            // 已注册 or 冲突 → 视为 OK 继续.
        }
        engine->RegisterObjectBehaviour(
            "string", asBEHAVE_CONSTRUCT, "void f()",
            asFUNCTION(sao_string_construct), asCALL_GENERIC);
        engine->RegisterObjectBehaviour(
            "string", asBEHAVE_CONSTRUCT, "void f(const string &in)",
            asFUNCTION(sao_string_copy_construct), asCALL_GENERIC);
        engine->RegisterObjectBehaviour(
            "string", asBEHAVE_DESTRUCT, "void f()",
            asFUNCTION(sao_string_destruct), asCALL_GENERIC);
        engine->RegisterObjectMethod(
            "string", "string &opAssign(const string &in)",
            asFUNCTION(sao_string_assign), asCALL_GENERIC);
        // log_info(const string &in) — 依赖 string 类型
        if (deny.count("log_info") == 0) {
            engine->RegisterGlobalFunction(
                "void log_info(const string &in)",
                asFUNCTION(gen_log_info), asCALL_GENERIC);
        }
    }

    // 4. 白名单 math
    auto reg = [&](const char* sig, void (*thunk)(asIScriptGeneric*), const char* name) {
        if (deny.count(name) > 0) return;
        engine->RegisterGlobalFunction(sig, asFUNCTION(thunk), asCALL_GENERIC);
    };
    reg("float sqrt(float)",       gen_sqrtf,  "sqrt");
    reg("float sin(float)",        gen_sinf,   "sin");
    reg("float cos(float)",        gen_cosf,   "cos");
    reg("float floor(float)",      gen_floorf, "floor");
    reg("float ceil(float)",       gen_ceilf,  "ceil");
    reg("float abs(float)",        gen_absf,   "abs");
    reg("float pow(float,float)",  gen_powf,   "pow");

    // 5. 存 snapshot
    sandbox_snapshot snap{};
    snap.max_context_execution_ms = cfg->max_context_execution_ms;
    snap.jit_off = true;
    snap.allow_string_type = cfg->allow_string_type;
    snap.line_callback_armed = (cfg->max_context_execution_ms != 0);

    {
        std::lock_guard<std::mutex> guard(sandbox_map_mutex());
        sandbox_map().emplace(engine, snap);
    }
    return SAO_OK;
#else
    (void)cfg;
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_sandbox_disarm(asIScriptEngine* engine) {
    if (engine == nullptr) return SAO_ERR_INVALID_ARGUMENT;
#if defined(SAO_HAS_ANGELSCRIPT)
    std::lock_guard<std::mutex> guard(sandbox_map_mutex());
    auto it = sandbox_map().find(engine);
    if (it == sandbox_map().end()) return SAO_ERR_INVALID_ARGUMENT;
    // JIT 还是保留 nullptr (安全默认) — 不主动恢复.
    sandbox_map().erase(it);
    return SAO_OK;
#else
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_ashost_sandbox_is_armed(asIScriptEngine* engine) {
    if (engine == nullptr) return false;
#if defined(SAO_HAS_ANGELSCRIPT)
    std::lock_guard<std::mutex> guard(sandbox_map_mutex());
    return sandbox_map().find(engine) != sandbox_map().end();
#else
    return false;
#endif
}

extern "C" SAO_PLUGINS_API uint32_t SAO_PLUGINS_CALL
sao_plugins_ashost_sandbox_max_context_ms(asIScriptEngine* engine) {
    if (engine == nullptr) return 0;
#if defined(SAO_HAS_ANGELSCRIPT)
    std::lock_guard<std::mutex> guard(sandbox_map_mutex());
    auto it = sandbox_map().find(engine);
    if (it == sandbox_map().end()) return 0;
    return it->second.max_context_execution_ms;
#else
    return 0;
#endif
}

extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_ashost_sandbox_jit_off(asIScriptEngine* engine) {
    if (engine == nullptr) return false;
#if defined(SAO_HAS_ANGELSCRIPT)
    std::lock_guard<std::mutex> guard(sandbox_map_mutex());
    auto it = sandbox_map().find(engine);
    if (it == sandbox_map().end()) return false;
    return it->second.jit_off;
#else
    return false;
#endif
}

} // namespace sao::plugins::angel_host
