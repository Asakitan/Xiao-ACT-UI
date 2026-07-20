#include <catch2/catch_test_macros.hpp>

#include "sao/plugins/angel_host/as_error.h"
#include "sao/plugins/angel_host/as_host.h"
#include "sao/plugins/angel_host/as_module_bridge.h"
#include "sao/plugins/angel_host/as_stdlib.h"

#include <array>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#if defined(SAO_TEST_HAS_ANGELSCRIPT)
#include <angelscript.h>
#endif

using namespace sao::plugins::angel_host;

extern "C" {
SAO_PLUGINS_API bool SAO_PLUGINS_CALL sao_plugins_ashost_is_available(void);
SAO_PLUGINS_API void SAO_PLUGINS_CALL sao_plugins_ashost_free_string(char* value);
}

#if defined(SAO_TEST_HAS_ANGELSCRIPT)
namespace {

struct host_owner {
    as_host_handle_t value = nullptr;

    host_owner() = default;
    host_owner(const host_owner&) = delete;
    host_owner& operator=(const host_owner&) = delete;
    host_owner(host_owner&& other) noexcept : value(other.value) {
        other.value = nullptr;
    }
    host_owner& operator=(host_owner&&) = delete;

    ~host_owner() {
        if (value != nullptr) {
            (void)sao_plugins_ashost_destroy(value);
        }
    }
};

struct context_releaser {
    void operator()(asIScriptContext* context) const {
        if (context != nullptr)
            context->Release();
    }
};

asIScriptModule* build_module(asIScriptEngine* engine, const char* name, const char* source) {
    asIScriptModule* module = engine->GetModule(name, asGM_ALWAYS_CREATE);
    REQUIRE(module != nullptr);
    REQUIRE(module->AddScriptSection(name, source) >= 0);
    REQUIRE(module->Build() >= 0);
    return module;
}

int execute_int(asIScriptEngine* engine, asIScriptFunction* function) {
    std::unique_ptr<asIScriptContext, context_releaser> context(engine->CreateContext());
    REQUIRE(context != nullptr);
    REQUIRE(context->Prepare(function) >= 0);
    REQUIRE(context->Execute() == asEXECUTION_FINISHED);
    return static_cast<int>(context->GetReturnDWord());
}

host_owner make_host() {
    host_owner host;
    REQUIRE(sao_plugins_ashost_create(nullptr, &host.value) == SAO_OK);
    REQUIRE(host.value != nullptr);
    return host;
}

} // namespace
#endif

TEST_CASE("Angel build matrix reports its configured capability", "[plugins][angel][matrix]") {
#if defined(SAO_TEST_ANGEL_RUNTIME_REQUIRED)
    CHECK(sao_plugins_ashost_is_available());
#if !defined(SAO_TEST_HAS_ANGELSCRIPT) || !defined(SAO_TEST_HAS_ANGELSCRIPT_ADDONS)
    FAIL("runtime-required build must compile the runtime and all required addons");
#endif
#elif defined(SAO_TEST_ANGEL_FORCE_FALLBACK)
    CHECK_FALSE(sao_plugins_ashost_is_available());
#else
    SUCCEED("non-matrix build follows discovered runtime capability");
#endif
#if defined(AS_MAX_PORTABILITY) && defined(SAO_TEST_HAS_ANGELSCRIPT)
    REQUIRE(asGetLibraryOptions() != nullptr);
    CHECK(std::string(asGetLibraryOptions()).find("AS_MAX_PORTABILITY") != std::string::npos);
#endif
}

TEST_CASE("Angel bridge fallback remains fail closed", "[plugins][angel][bridge][fallback]") {
    if (sao_plugins_ashost_is_available()) {
        SUCCEED("runtime capability is enabled");
        return;
    }

    char* error = reinterpret_cast<char*>(uintptr_t{1});
    CHECK(sao_plugins_ashost_register_sdk(nullptr) == SAO_ERR_NOT_IMPLEMENTED);
    CHECK(sao_plugins_ashost_bind_ctx(nullptr, nullptr, nullptr) == SAO_ERR_NOT_IMPLEMENTED);
    CHECK(sao_plugins_ashost_install_stdlib(nullptr) == SAO_ERR_NOT_IMPLEMENTED);
    CHECK(sao_plugins_ashost_take_exception(nullptr, &error) == SAO_ERR_NOT_IMPLEMENTED);
    CHECK(error == nullptr);
}

#if defined(SAO_TEST_HAS_ANGELSCRIPT)
TEST_CASE("Angel module bridge is idempotent and clears borrowed context",
          "[plugins][angel][bridge]") {
    if (!sao_plugins_ashost_is_available())
        SKIP("AngelScript SDK unavailable");

    auto host = make_host();
    asIScriptEngine* engine = sao_plugins_ashost_engine(host.value);
    REQUIRE(engine != nullptr);
    REQUIRE(sao_plugins_ashost_register_sdk(engine) == SAO_OK);
    REQUIRE(sao_plugins_ashost_register_sdk(engine) == SAO_OK);

    asIScriptModule* module =
        build_module(engine, "bridge_focused",
                     "PluginContext@ ctx; int value() { return ctx is null ? 0 : 1; }");
    void* borrowed = reinterpret_cast<void*>(uintptr_t{0x1234});
    REQUIRE(sao_plugins_ashost_bind_ctx(engine, borrowed, "bridge_focused") == SAO_OK);
    REQUIRE(sao_plugins_ashost_bind_ctx(engine, borrowed, "bridge_focused") == SAO_OK);

    const int index = module->GetGlobalVarIndexByName("ctx");
    REQUIRE(index >= 0);
    auto** slot = static_cast<void**>(module->GetAddressOfGlobalVar(index));
    REQUIRE(slot != nullptr);
    CHECK(*slot == borrowed);

    REQUIRE(sao_plugins_ashost_bind_ctx(engine, nullptr, "bridge_focused") == SAO_OK);
    CHECK(*slot == nullptr);
    CHECK(sao_plugins_ashost_bind_ctx(engine, borrowed, "missing_module") ==
          SAO_ERR_HANDLE_INVALID);
    REQUIRE(engine->DiscardModule("bridge_focused") >= 0);
}

TEST_CASE("Angel exception includes message declaration line and column",
          "[plugins][angel][error]") {
    if (!sao_plugins_ashost_is_available())
        SKIP("AngelScript SDK unavailable");

    auto host = make_host();
    asIScriptEngine* engine = sao_plugins_ashost_engine(host.value);
    asIScriptModule* module = build_module(
        engine, "exception_focused", "int explode() {\n  int zero = 0; return 7 / zero;\n}\n");
    asIScriptFunction* function = module->GetFunctionByName("explode");
    REQUIRE(function != nullptr);
    std::unique_ptr<asIScriptContext, context_releaser> context(engine->CreateContext());
    REQUIRE(context != nullptr);
    REQUIRE(context->Prepare(function) >= 0);
    REQUIRE(context->Execute() == asEXECUTION_EXCEPTION);

    char* raw_error = nullptr;
    REQUIRE(sao_plugins_ashost_take_exception(context.get(), &raw_error) == SAO_ERR_OS_CALL_FAILED);
    REQUIRE(raw_error != nullptr);
    const std::string error(raw_error);
    sao_plugins_ashost_free_string(raw_error);
    CHECK(error.find("AngelScript exception:") != std::string::npos);
    CHECK(error.find("int explode()") != std::string::npos);
    CHECK(error.find("location: exception_focused:2:") != std::string::npos);
    REQUIRE(engine->DiscardModule("exception_focused") >= 0);
}

TEST_CASE("Angel build messages remain structured under concurrent calls",
          "[plugins][angel][error][concurrency]") {
    auto host = make_host();
    std::array<int32_t, 2> statuses{SAO_OK, SAO_OK};
    std::array<std::string, 2> errors;
    std::array<std::thread, 2> workers;
    for (size_t index = 0; index < workers.size(); ++index) {
        workers[index] = std::thread([&, index] {
            char* result = nullptr;
            char* error = nullptr;
            constexpr char invalid_source[] = "int broken( { return 1; }";
            statuses[index] = sao_plugins_ashost_execute(
                host.value, invalid_source, sizeof(invalid_source) - 1, &result, &error);
            if (error != nullptr)
                errors[index] = error;
            sao_plugins_ashost_free_string(result);
            sao_plugins_ashost_free_string(error);
        });
    }
    for (auto& worker : workers)
        worker.join();
    for (size_t index = 0; index < workers.size(); ++index) {
        CHECK(statuses[index] == SAO_ERR_INVALID_ARGUMENT);
        CHECK(errors[index].find("inline_source") != std::string::npos);
        CHECK(errors[index].find("ERR:") != std::string::npos);
    }
}

TEST_CASE("Angel stdlib registers JSON and official basic addons", "[plugins][angel][stdlib]") {
    if (!sao_plugins_ashost_is_available())
        SKIP("AngelScript SDK unavailable");

    auto host = make_host();
    asIScriptEngine* engine = sao_plugins_ashost_engine(host.value);
    REQUIRE(engine != nullptr);
    const int32_t stdlib_status = sao_plugins_ashost_install_stdlib(engine);
    CHECK(sao_plugins_ashost_install_stdlib(engine) == stdlib_status);
    INFO("stdlib status=" << stdlib_status
                          << " string=" << (engine->GetTypeInfoByName("string") != nullptr)
                          << " context=" << (engine->GetTypeInfoByName("PluginContext") != nullptr)
                          << " json=" << (engine->GetTypeInfoByName("json") != nullptr)
                          << " array=" << (engine->GetTypeInfoByDecl("array<int>") != nullptr)
                          << " dictionary=" << (engine->GetTypeInfoByName("dictionary") != nullptr)
                          << " math="
                          << (engine->GetGlobalFunctionByDecl("float cos(float)") != nullptr)
                          << " datetime=" << (engine->GetTypeInfoByName("datetime") != nullptr));
#if defined(SAO_TEST_HAS_ANGELSCRIPT_ADDONS)
    REQUIRE(stdlib_status == SAO_OK);
#else
    REQUIRE(stdlib_status == SAO_ERR_NOT_IMPLEMENTED);
    CHECK(engine->GetTypeInfoByName("string") == nullptr);
    CHECK(engine->GetTypeInfoByName("json") == nullptr);
    return;
#endif
    CHECK(engine->GetTypeInfoByName("string") != nullptr);
    CHECK(engine->GetTypeInfoByName("json") != nullptr);

    asIScriptModule* json_module = build_module(engine, "json_focused", R"AS(
int json_check() {
    json@ value = json_parse("{\"name\":\"sao\",\"count\":7,\"ok\":true}");
    if (value is null || !value.contains("name")) return 0;
    if (value.get_string("name", "") != "sao") return 0;
    if (value.get_int("count", 0) != 7) return 0;
    if (!value.get_bool("ok", false)) return 0;
    value.set("extra", int64(9));
    return value.contains("extra") && value.size() == 4 ? 1 : 0;
}
)AS");
    CHECK(execute_int(engine, json_module->GetFunctionByName("json_check")) == 1);
    REQUIRE(engine->DiscardModule("json_focused") >= 0);

    asIScriptModule* addon_module = build_module(engine, "addon_focused", R"AS(
int addon_check() {
    array<int> values = {1, 2, 3};
    values.insertLast(4);
    dictionary data;
    data.set("answer", int64(42));
    int64 answer = 0;
    datetime now;
    if (!data.get("answer", answer)) return 0;
    if (answer != 42 || values.length() != 4) return 0;
    return now.year >= 2020 && cos(0.0) == 1.0 ? 1 : 0;
}
)AS");
    CHECK(execute_int(engine, addon_module->GetFunctionByName("addon_check")) == 1);
    REQUIRE(engine->DiscardModule("addon_focused") >= 0);
}

#if defined(SAO_TEST_HAS_ANGELSCRIPT_ADDONS)
TEST_CASE("Angel stdlib retains the first failed installation result", "[plugins][angel][stdlib]") {
    auto host = make_host();
    asIScriptEngine* engine = sao_plugins_ashost_engine(host.value);
    REQUIRE(engine != nullptr);
    REQUIRE(engine->RegisterObjectType("json", 0, asOBJ_REF) >= 0);
    CHECK(sao_plugins_ashost_install_stdlib(engine) == SAO_ERR_OS_CALL_FAILED);
    CHECK(sao_plugins_ashost_install_stdlib(engine) == SAO_ERR_OS_CALL_FAILED);
    CHECK(engine->GetTypeInfoByName("string") == nullptr);
}

TEST_CASE("Angel JSON generic failure becomes a script exception",
          "[plugins][angel][stdlib][error]") {
    auto host = make_host();
    asIScriptEngine* engine = sao_plugins_ashost_engine(host.value);
    REQUIRE(sao_plugins_ashost_install_stdlib(engine) == SAO_OK);
    asIScriptModule* module = build_module(engine, "json_exception_focused", R"AS(
void invalid_json() {
    json@ value = json_parse("{");
}
)AS");
    asIScriptFunction* function = module->GetFunctionByName("invalid_json");
    REQUIRE(function != nullptr);
    std::unique_ptr<asIScriptContext, context_releaser> context(engine->CreateContext());
    REQUIRE(context != nullptr);
    REQUIRE(context->Prepare(function) >= 0);
    REQUIRE(context->Execute() == asEXECUTION_EXCEPTION);
    REQUIRE(context->GetExceptionString() != nullptr);
    CHECK(std::string(context->GetExceptionString()).find("invalid JSON") != std::string::npos);
    REQUIRE(engine->DiscardModule("json_exception_focused") >= 0);
}
#endif
#endif
