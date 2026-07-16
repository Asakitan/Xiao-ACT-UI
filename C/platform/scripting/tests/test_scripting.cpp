#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "sao/scripting/abi.h"
#include "sao/scripting/script_context.h"
#include "sao/scripting/script_engine.h"
#include "sao/scripting/script_error.h"
#include "sao/scripting/script_registry.h"

namespace {

struct FixtureContext {
    SaoScriptError error{};
};

struct FixtureProvider {
    std::atomic<int> retains{0};
    std::atomic<int> releases{0};
    std::atomic<int> creates{0};
    std::atomic<int> destroys{0};
    std::atomic<int> loads{0};
    std::atomic<int> unloads{0};
    std::atomic<int> runs{0};
    std::atomic<int> invokes{0};
    std::atomic<int> cancels{0};
    bool available = true;
};

class RegistryGuard {
public:
    RegistryGuard() { clear(); }
    ~RegistryGuard() { clear(); }

private:
    static void clear() {
        for (int32_t language = SAO_SCRIPT_LANG_PYTHON;
             language <= SAO_SCRIPT_LANG_CSHARP; ++language) {
            (void)sao_script_registry_unregister(language);
        }
    }
};

bool SAO_SCRIPTING_CALL fixture_available(void* user_data) {
    return static_cast<FixtureProvider*>(user_data)->available;
}

void SAO_SCRIPTING_CALL fixture_retain(void* user_data) {
    ++static_cast<FixtureProvider*>(user_data)->retains;
}

void SAO_SCRIPTING_CALL fixture_release(void* user_data) {
    ++static_cast<FixtureProvider*>(user_data)->releases;
}

sao_status_t SAO_SCRIPTING_CALL fixture_create(void* user_data,
                                               void** out_engine_impl) {
    auto* fixture = static_cast<FixtureProvider*>(user_data);
    ++fixture->creates;
    *out_engine_impl = fixture;
    return SAO_STATUS_OK;
}

void SAO_SCRIPTING_CALL fixture_destroy(void*, void* user_data) {
    ++static_cast<FixtureProvider*>(user_data)->destroys;
}

sao_status_t SAO_SCRIPTING_CALL fixture_load(
    void*, const SaoScriptContextConfig*, void** out_ctx_impl,
    void* user_data) {
    ++static_cast<FixtureProvider*>(user_data)->loads;
    *out_ctx_impl = new FixtureContext();
    return SAO_STATUS_OK;
}

void SAO_SCRIPTING_CALL fixture_unload(void*, void* ctx_impl,
                                      void* user_data) {
    ++static_cast<FixtureProvider*>(user_data)->unloads;
    delete static_cast<FixtureContext*>(ctx_impl);
}

sao_status_t SAO_SCRIPTING_CALL fixture_run(void*, void*, void* user_data) {
    ++static_cast<FixtureProvider*>(user_data)->runs;
    return SAO_STATUS_OK;
}

sao_status_t SAO_SCRIPTING_CALL fixture_invoke(
    void*, void* ctx_impl, const char* function_name_utf8, const uint8_t*,
    size_t, uint8_t* output, size_t output_capacity, size_t* out_required,
    void* user_data) {
    ++static_cast<FixtureProvider*>(user_data)->invokes;
    if (std::strcmp(function_name_utf8, "throw") == 0) {
        throw std::runtime_error("fixture provider exception");
    }
    if (std::strcmp(function_name_utf8, "fail") == 0) {
        auto* context = static_cast<FixtureContext*>(ctx_impl);
        context->error.status = SAO_STATUS_ERR_SCRIPT_RUNTIME;
        std::strcpy(context->error.message_utf8, "fixture runtime error");
        return SAO_STATUS_ERR_SCRIPT_RUNTIME;
    }
    constexpr char result[] = "{\"ok\":true}";
    *out_required = sizeof(result);
    if (output == nullptr || output_capacity < sizeof(result)) {
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    }
    std::memcpy(output, result, sizeof(result));
    return SAO_STATUS_OK;
}

sao_status_t SAO_SCRIPTING_CALL fixture_last_error(
    void*, void* ctx_impl, SaoScriptError* out_error, void*) {
    *out_error = static_cast<FixtureContext*>(ctx_impl)->error;
    return SAO_STATUS_OK;
}

sao_status_t SAO_SCRIPTING_CALL fixture_capability(
    void*, void*, const char* capability_utf8, int32_t* out_supported) {
    *out_supported = std::strcmp(capability_utf8, "fixture.custom") == 0 ? 1 : 0;
    return SAO_STATUS_OK;
}

sao_status_t SAO_SCRIPTING_CALL fixture_cancel(void* engine_impl, void*) {
    ++static_cast<FixtureProvider*>(engine_impl)->cancels;
    return SAO_STATUS_OK;
}

SaoScriptEngineVTable make_provider(int32_t language,
                                    FixtureProvider* fixture,
                                    const void* owner = nullptr) {
    SaoScriptEngineVTable provider{};
    provider.name_utf8 = "fixture";
    provider.language = language;
    provider.engine_version = 1u << 16;
    provider.abi_version = SAO_SCRIPTING_ABI_VERSION;
    provider.struct_size = sizeof(provider);
    provider.owner = owner;
    provider.user_data = fixture;
    provider.available = fixture_available;
    provider.retain = fixture_retain;
    provider.release = fixture_release;
    provider.context_has_capability = fixture_capability;
    provider.context_cancel = fixture_cancel;
    provider.create = fixture_create;
    provider.destroy = fixture_destroy;
    provider.load = fixture_load;
    provider.unload = fixture_unload;
    provider.run = fixture_run;
    provider.invoke = fixture_invoke;
    provider.last_error = fixture_last_error;
    return provider;
}

SaoScriptContextConfig make_config(
    int32_t language,
    const SaoSdkContext* owner = nullptr) {
    SaoScriptContextConfig config{};
    config.language = language;
    config.owner_sdk_ctx = owner;
    config.source_dir_utf8 = "fixture";
    config.entry_file_utf8 = "main.fixture";
    return config;
}

sao_status_t SAO_SCRIPTING_CALL throwing_barrier(void*) {
    throw std::runtime_error("barrier fixture");
}

}  // namespace

TEST_CASE("scripting ABI version is non-zero", "[scripting][abi]") {
    REQUIRE(sao_scripting_abi_version() == SAO_SCRIPTING_ABI_VERSION);
}

TEST_CASE("sao_scripting_error_clear zeroes the struct", "[scripting][error]") {
    SaoScriptError err;
    for (auto& b : reinterpret_cast<uint8_t (&)[sizeof(err)]>(err)) b = 0xEE;
    sao_scripting_error_clear(&err);
    REQUIRE(err.status == 0);
    REQUIRE(err.script_line == 0);
    REQUIRE(err.message_utf8[0] == '\0');
}

TEST_CASE("script registry reports unsupported without a language provider",
      "[scripting][registry]") {
    RegistryGuard guard;
    const SaoScriptEngineVTable* provider = nullptr;
    REQUIRE(sao_script_registry_find(SAO_SCRIPT_LANG_LUA, &provider) ==
        SAO_STATUS_ERR_SCRIPT_UNSUPPORTED);
    REQUIRE(provider == nullptr);

    sao_script_engine_handle_t engine = nullptr;
    REQUIRE(sao_script_engine_create(SAO_SCRIPT_LANG_LUA, &engine) ==
        SAO_STATUS_ERR_SCRIPT_UNSUPPORTED);
    REQUIRE(engine == nullptr);

    size_t count = 42;
    REQUIRE(sao_script_registry_enumerate(nullptr, 0, &count) ==
        SAO_STATUS_OK);
    REQUIRE(count == 0);

    FixtureProvider unavailable_state;
    unavailable_state.available = false;
    auto unavailable =
        make_provider(SAO_SCRIPT_LANG_LUA, &unavailable_state);
    REQUIRE(sao_script_registry_register(&unavailable) == SAO_STATUS_OK);
    REQUIRE(sao_script_engine_create(SAO_SCRIPT_LANG_LUA, &engine) ==
            SAO_STATUS_ERR_SCRIPT_UNSUPPORTED);
    REQUIRE(engine == nullptr);
    REQUIRE(unavailable_state.retains == 0);
}

TEST_CASE("script registry is idempotent, ordered and caller-sized",
      "[scripting][registry]") {
    RegistryGuard guard;
    FixtureProvider first_state;
    FixtureProvider second_state;
    auto first = make_provider(SAO_SCRIPT_LANG_LUA, &first_state);
    auto second = make_provider(SAO_SCRIPT_LANG_EMMA, &second_state);
    REQUIRE(sao_script_registry_register(&first) == SAO_STATUS_OK);
    REQUIRE(sao_script_registry_register(&second) == SAO_STATUS_OK);

    size_t count = 0;
    REQUIRE(sao_script_registry_enumerate(nullptr, 0, &count) ==
        SAO_STATUS_ERR_BUFFER_TOO_SMALL);
    REQUIRE(count == 2);
    std::array<const SaoScriptEngineVTable*, 1> small{};
    REQUIRE(sao_script_registry_enumerate(small.data(), small.size(), &count) ==
        SAO_STATUS_ERR_BUFFER_TOO_SMALL);

    std::array<const SaoScriptEngineVTable*, 2> providers{};
    REQUIRE(sao_script_registry_enumerate(
        providers.data(), providers.size(), &count) == SAO_STATUS_OK);
    REQUIRE(providers[0]->language == SAO_SCRIPT_LANG_LUA);
    REQUIRE(providers[1]->language == SAO_SCRIPT_LANG_EMMA);

    first.user_data = &second_state;
    REQUIRE(sao_script_registry_register(&first) == SAO_STATUS_OK);
    const SaoScriptEngineVTable* found = nullptr;
    REQUIRE(sao_script_registry_find(SAO_SCRIPT_LANG_LUA, &found) ==
        SAO_STATUS_OK);
    REQUIRE(found->user_data == &second_state);
}

TEST_CASE("script engine and context execute through unified provider lifecycle",
      "[scripting][engine][context]") {
    RegistryGuard guard;
    FixtureProvider fixture;
    auto provider = make_provider(SAO_SCRIPT_LANG_LUA, &fixture);
    REQUIRE(sao_script_registry_register(&provider) == SAO_STATUS_OK);

    auto config = make_config(SAO_SCRIPT_LANG_LUA);
    sao_script_context_handle_t context = nullptr;
    REQUIRE(sao_script_context_create(&config, &context) == SAO_STATUS_OK);
    REQUIRE(fixture.retains == 1);
    REQUIRE(fixture.creates == 1);
    REQUIRE(fixture.loads == 1);
    REQUIRE(sao_script_context_run(context) == SAO_STATUS_OK);

    size_t required = 0;
    REQUIRE(sao_script_context_call(context, "invoke", nullptr, 0, nullptr, 0,
                    &required) ==
        SAO_STATUS_ERR_BUFFER_TOO_SMALL);
    REQUIRE(required == sizeof("{\"ok\":true}"));
    std::array<uint8_t, 32> output{};
    REQUIRE(sao_script_context_call(
        context, "invoke", nullptr, 0, output.data(), output.size(),
        &required) == SAO_STATUS_OK);
    REQUIRE(std::strcmp(reinterpret_cast<const char*>(output.data()),
            "{\"ok\":true}") == 0);

    SaoScriptContextStats stats{};
    REQUIRE(sao_script_context_stats(context, &stats) == SAO_STATUS_OK);
    REQUIRE(stats.total_call_count == 2);
    REQUIRE(fixture.runs == 1);
    REQUIRE(fixture.invokes == 2);

    sao_script_context_destroy(context);
    REQUIRE(fixture.unloads == 1);
    REQUIRE(fixture.destroys == 1);
    REQUIRE(fixture.releases == 1);
}

TEST_CASE("script context owns values capabilities cancellation and errors",
      "[scripting][context]") {
    RegistryGuard guard;
    FixtureProvider fixture;
    auto provider = make_provider(SAO_SCRIPT_LANG_EMMA, &fixture);
    REQUIRE(sao_script_registry_register(&provider) == SAO_STATUS_OK);
    auto config = make_config(SAO_SCRIPT_LANG_EMMA);
    sao_script_context_handle_t context = nullptr;
    REQUIRE(sao_script_context_create(&config, &context) == SAO_STATUS_OK);

    constexpr std::array<uint8_t, 3> value{1, 2, 3};
    REQUIRE(sao_script_context_set_value(
        context, "answer", value.data(), value.size()) ==
        SAO_STATUS_OK);
    size_t required = 0;
    REQUIRE(sao_script_context_get_value(context, "answer", nullptr, 0,
                     &required) ==
        SAO_STATUS_ERR_BUFFER_TOO_SMALL);
    REQUIRE(required == value.size());
    std::array<uint8_t, 3> copy{};
    REQUIRE(sao_script_context_get_value(
        context, "answer", copy.data(), copy.size(), &required) ==
        SAO_STATUS_OK);
    REQUIRE(copy == value);

    int32_t supported = 0;
    REQUIRE(sao_script_context_has_capability(
        context, SAO_SCRIPT_CAPABILITY_KEY_VALUE, &supported) ==
        SAO_STATUS_OK);
    REQUIRE(supported == 1);
    REQUIRE(sao_script_context_has_capability(
        context, "fixture.custom", &supported) == SAO_STATUS_OK);
    REQUIRE(supported == 1);

    size_t ignored = 0;
    REQUIRE(sao_script_context_call(context, "fail", nullptr, 0, nullptr, 0,
                    &ignored) ==
        SAO_STATUS_ERR_SCRIPT_RUNTIME);
    SaoScriptError error{};
    REQUIRE(sao_script_context_last_error(context, &error) == SAO_STATUS_OK);
    REQUIRE(error.status == SAO_STATUS_ERR_SCRIPT_RUNTIME);
    REQUIRE(std::strcmp(error.message_utf8, "fixture runtime error") == 0);

    REQUIRE(sao_script_context_request_cancel(context) == SAO_STATUS_OK);
    REQUIRE(fixture.cancels == 1);
    int32_t cancelled = 0;
    REQUIRE(sao_script_context_is_cancelled(context, &cancelled) ==
        SAO_STATUS_OK);
    REQUIRE(cancelled == 1);
    REQUIRE(sao_script_context_run(context) == SAO_STATUS_ERR_CANCELLED);
    REQUIRE(sao_script_context_remove_value(context, "answer") ==
        SAO_STATUS_OK);
    sao_script_context_destroy(context);
}

TEST_CASE("provider exceptions stop at scripting ABI barriers",
      "[scripting][barrier]") {
    SaoScriptError barrier_error{};
    REQUIRE(sao_scripting_provider_barrier(
        throwing_barrier, nullptr, &barrier_error) ==
        SAO_STATUS_ERR_SCRIPT_PROVIDER_EXCEPTION);
    REQUIRE(std::strcmp(barrier_error.message_utf8, "barrier fixture") == 0);

    RegistryGuard guard;
    FixtureProvider fixture;
    auto provider = make_provider(SAO_SCRIPT_LANG_ANGELSCRIPT, &fixture);
    REQUIRE(sao_script_registry_register(&provider) == SAO_STATUS_OK);
    auto config = make_config(SAO_SCRIPT_LANG_ANGELSCRIPT);
    sao_script_context_handle_t context = nullptr;
    REQUIRE(sao_script_context_create(&config, &context) == SAO_STATUS_OK);
    size_t required = 0;
    REQUIRE(sao_script_context_call(context, "throw", nullptr, 0, nullptr, 0,
                    &required) ==
        SAO_STATUS_ERR_SCRIPT_PROVIDER_EXCEPTION);
    SaoScriptError error{};
    REQUIRE(sao_script_context_last_error(context, &error) == SAO_STATUS_OK);
    REQUIRE(error.status == SAO_STATUS_ERR_SCRIPT_PROVIDER_EXCEPTION);
    REQUIRE(std::strcmp(error.message_utf8, "fixture provider exception") == 0);
    sao_script_context_destroy(context);
}

TEST_CASE("registry and contexts release resources by owner",
      "[scripting][owner]") {
    RegistryGuard guard;
    FixtureProvider fixture;
    int provider_owner = 0;
    int context_owner = 0;
    auto provider = make_provider(SAO_SCRIPT_LANG_PYTHON, &fixture,
                  &provider_owner);
    REQUIRE(sao_script_registry_register(&provider) == SAO_STATUS_OK);
    auto config = make_config(
    SAO_SCRIPT_LANG_PYTHON,
    reinterpret_cast<const SaoSdkContext*>(&context_owner));
    sao_script_context_handle_t first = nullptr;
    sao_script_context_handle_t second = nullptr;
    REQUIRE(sao_script_context_create(&config, &first) == SAO_STATUS_OK);
    REQUIRE(sao_script_context_create(&config, &second) == SAO_STATUS_OK);

    size_t released = 0;
    REQUIRE(sao_script_context_release_owner(config.owner_sdk_ctx, &released) ==
        SAO_STATUS_OK);
    REQUIRE(released == 2);
    REQUIRE(fixture.unloads == 2);
    REQUIRE(fixture.destroys == 2);
    REQUIRE(fixture.releases == 2);
    sao_script_context_destroy(first);
    sao_script_context_destroy(second);
    REQUIRE(fixture.unloads == 2);

    size_t removed = 0;
    REQUIRE(sao_script_registry_release_owner(&provider_owner, &removed) ==
        SAO_STATUS_OK);
    REQUIRE(removed == 1);
    const SaoScriptEngineVTable* found = nullptr;
    REQUIRE(sao_script_registry_find(SAO_SCRIPT_LANG_PYTHON, &found) ==
        SAO_STATUS_ERR_SCRIPT_UNSUPPORTED);
}

TEST_CASE("script registry supports concurrent provider replacement",
      "[scripting][registry][threading]") {
    RegistryGuard guard;
    FixtureProvider fixture;
    std::atomic<bool> failed{false};
    std::vector<std::thread> workers;
    for (int thread_index = 0; thread_index < 8; ++thread_index) {
    workers.emplace_back([&, thread_index] {
        for (int iteration = 0; iteration < 64; ++iteration) {
        const int32_t language =
            (thread_index + iteration) %
            (SAO_SCRIPT_LANG_CSHARP + 1);
        auto provider = make_provider(language, &fixture);
        if (sao_script_registry_register(&provider) != SAO_STATUS_OK) {
            failed.store(true, std::memory_order_relaxed);
        }
        size_t count = 0;
        const sao_status_t status =
            sao_script_registry_enumerate(nullptr, 0, &count);
        if (status != SAO_STATUS_ERR_BUFFER_TOO_SMALL &&
            status != SAO_STATUS_OK) {
            failed.store(true, std::memory_order_relaxed);
        }
        }
    });
    }
    for (auto& worker : workers) worker.join();
    REQUIRE_FALSE(failed.load(std::memory_order_relaxed));

    size_t count = 0;
    REQUIRE(sao_script_registry_enumerate(nullptr, 0, &count) ==
        SAO_STATUS_ERR_BUFFER_TOO_SMALL);
    REQUIRE(count == 5);
}
