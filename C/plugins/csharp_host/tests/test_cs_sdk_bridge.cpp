#include <catch2/catch_test_macros.hpp>

#include "cs_sdk_bridge_internal.h"

#include "sao/plugins/csharp_host/cs_host.h"
#include "sao/plugins/csharp_host/cs_module_bridge.h"
#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/sdk_binding/binding_common.h"
#include "sao/plugins/sdk_binding/binding_csharp.h"
#include "sao/sdk/sao_sdk.h"
#include "sao/sdk/sao_sdk_provider.h"

#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace sao::plugins::csharp_host;
using namespace sao::plugins::sdk_binding;

namespace {

struct callback_gate {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool released = false;
};

struct busy_sdk_provider {
    bool busy = true;
    uint64_t next_token = 1;

    static void SAO_SDK_CALL retain(void*) {}
    static void SAO_SDK_CALL release(void*) {}

    static sao_sdk_status_t SAO_SDK_CALL register_timer(void* user_data, uint32_t,
                                                        sao_sdk_timer_callback_t, void*,
                                                        uint64_t* out_token) {
        if (user_data == nullptr || out_token == nullptr)
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        auto* provider = static_cast<busy_sdk_provider*>(user_data);
        *out_token = provider->next_token++;
        return SAO_SDK_OK;
    }

    static sao_sdk_status_t SAO_SDK_CALL unregister_timer(void* user_data, uint64_t) {
        if (user_data == nullptr)
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        return static_cast<busy_sdk_provider*>(user_data)->busy ? SAO_SDK_ERR_BUSY : SAO_SDK_OK;
    }

    SaoSdkProviderVTable table() {
        SaoSdkProviderVTable provider{};
        provider.abi_version = SAO_SDK_PROVIDER_ABI_VERSION;
        provider.struct_size = sizeof(provider);
        provider.user_data = this;
        provider.retain = retain;
        provider.release = release;
        provider.register_timer = register_timer;
        provider.unregister_timer = unregister_timer;
        return provider;
    }
};

void SAO_SDK_CALL noop_sdk_timer(sao_sdk_timer_token_t, void*) {}

struct fake_managed_callback {
    std::string name;
    std::vector<std::string>* order = nullptr;
    const cs_managed_sdk_table* table = nullptr;
    cs_managed_sdk_session_t session = nullptr;
    int retain_calls = 0;
    int invoke_calls = 0;
    int release_calls = 0;
    int32_t release_reentry_status = SAO_OK;
    bool throw_on_invoke = false;
    bool throw_on_release = false;
    callback_gate* gate = nullptr;
};

int32_t SAO_CSHOST_MANAGED_CALL fake_retain(void* handle) {
    auto* callback = static_cast<fake_managed_callback*>(handle);
    ++callback->retain_calls;
    if (callback->order != nullptr)
        callback->order->push_back("retain:" + callback->name);
    return SAO_OK;
}

int32_t SAO_CSHOST_MANAGED_CALL fake_invoke(void* handle, cs_managed_callback_kind, const void*) {
    auto* callback = static_cast<fake_managed_callback*>(handle);
    ++callback->invoke_calls;
    if (callback->gate != nullptr) {
        std::unique_lock lock(callback->gate->mutex);
        callback->gate->entered = true;
        callback->gate->condition.notify_all();
        callback->gate->condition.wait(lock, [callback] { return callback->gate->released; });
    }
    if (callback->throw_on_invoke)
        throw std::runtime_error("fake managed invoke");
    return SAO_OK;
}

int32_t SAO_CSHOST_MANAGED_CALL fake_release(void* handle) {
    auto* callback = static_cast<fake_managed_callback*>(handle);
    ++callback->release_calls;
    if (callback->order != nullptr)
        callback->order->push_back("release:" + callback->name);
    if (callback->table != nullptr) {
        callback->release_reentry_status =
            callback->table->log(callback->session, "release reentry");
    }
    if (callback->throw_on_release)
        throw std::runtime_error("fake managed release");
    return SAO_OK;
}

cs_managed_callback_descriptor descriptor(fake_managed_callback& callback,
                                          cs_managed_callback_kind kind) {
    return {
        sizeof(cs_managed_callback_descriptor),
        SAO_CSHOST_SDK_TABLE_ABI_VERSION,
        kind,
        0,
        &callback,
        fake_invoke,
        fake_retain,
        fake_release,
    };
}

class bridge_fixture {
  public:
    explicit bridge_fixture(void* sdk_context = nullptr) {
        REQUIRE(cshost_sdk_bridge_test_start(&runtime_marker_, nullptr, sdk_context, &table_,
                                             &session_) == SAO_OK);
    }

    ~bridge_fixture() {
        if (session_ != nullptr)
            (void)cshost_sdk_bridge_test_finish(session_);
    }

    bridge_fixture(const bridge_fixture&) = delete;
    bridge_fixture& operator=(const bridge_fixture&) = delete;

    const cs_managed_sdk_table* table() const {
        return table_;
    }

    cs_managed_sdk_session_t session() const {
        return session_;
    }

    void* runtime() {
        return &runtime_marker_;
    }

    int32_t finish() {
        const int32_t status = cshost_sdk_bridge_test_finish(session_);
        session_ = nullptr;
        return status;
    }

  private:
    int runtime_marker_ = 0;
    const cs_managed_sdk_table* table_ = nullptr;
    cs_managed_sdk_session_t session_ = nullptr;
};

void wrap_callback(bridge_fixture& fixture, cs_managed_callback_descriptor& managed,
                   void** out_callback, void** out_user_data) {
    REQUIRE(sao_plugins_binding_csharp_wrap_delegate(
                reinterpret_cast<csharp_domain_ptr>(fixture.runtime()), &managed, out_callback,
                out_user_data) == SAO_OK);
    REQUIRE(*out_callback != nullptr);
    REQUIRE(*out_user_data != nullptr);
}

} // namespace

TEST_CASE("C# SDK table preserves layout and fails closed without runtime contexts",
          "[plugins][csharp][bridge][no_runtime]") {
    STATIC_REQUIRE(offsetof(cs_sdk_bridge, log_info) == 0);
    STATIC_REQUIRE(offsetof(cs_sdk_bridge, sdk_table) == sizeof(void*) * 5 + sizeof(uint32_t) * 2);
    STATIC_REQUIRE(sizeof(cs_managed_plugin_context) == sizeof(uint32_t) * 2 + sizeof(void*) * 4);

    bridge_fixture fixture;
    REQUIRE(fixture.table()->struct_size == sizeof(cs_managed_sdk_table));
    REQUIRE(fixture.table()->abi_version == SAO_CSHOST_SDK_TABLE_ABI_VERSION);

    cs_managed_sdk_call call{};
    call.struct_size = sizeof(call);
    call.method_id = static_cast<uint16_t>(sdk_method_id::prop_plugin_id);
    REQUIRE(fixture.table()->dispatch(fixture.session(), &call) == SAO_ERR_NOT_INITIALIZED);
    REQUIRE(fixture.table()->log(fixture.session(), "no context") == SAO_ERR_NOT_INITIALIZED);
    const auto* stale_table = fixture.table();
    const auto stale_session = fixture.session();
    REQUIRE(fixture.finish() == SAO_OK);
    REQUIRE(stale_table->log(stale_session, "closed session") == SAO_ERR_HANDLE_INVALID);

#if defined(_WIN32)
    cs_host_config config{};
    config.hostfxr_path = L"Z:\\sao-csharp-host-tests\\missing-hostfxr.dll";
    cs_host_handle_t host = nullptr;
    REQUIRE(sao_plugins_cshost_init(&config, &host) != SAO_OK);
    REQUIRE(host == nullptr);
#endif
}

TEST_CASE("C# SDK table dispatches through the canonical SaoSdkContext",
          "[plugins][csharp][bridge][dispatch]") {
    SaoSdkContext* sdk_context = nullptr;
    REQUIRE(sao_sdk_context_create("bridge-fixture", "managed.bridge", &sdk_context) == SAO_SDK_OK);
    {
        bridge_fixture fixture(sdk_context);
        char output[64]{};
        size_t required = 0;
        cs_managed_sdk_call call{};
        call.struct_size = sizeof(call);
        call.method_id = static_cast<uint16_t>(sdk_method_id::prop_plugin_id);
        call.args_json_utf8 = "{}";
        call.args_size = 2;
        call.out_result_json_utf8 = output;
        call.out_capacity = sizeof(output);
        call.out_required = &required;
        REQUIRE(fixture.table()->dispatch(fixture.session(), &call) == SAO_OK);
        REQUIRE(std::string(output) == "\"managed.bridge\"");
        REQUIRE(required == std::string(output).size() + 1);
        REQUIRE(fixture.finish() == SAO_OK);
    }
    REQUIRE(sao_sdk_context_try_destroy(sdk_context) == SAO_SDK_OK);
}

TEST_CASE("C# SDK BUSY keeps session ownership intact for teardown retry",
          "[plugins][csharp][bridge][destroy][busy][retry]") {
    SaoSdkContext* sdk_context = nullptr;
    REQUIRE(sao_sdk_context_create("bridge-busy", "managed.bridge.busy", &sdk_context) ==
            SAO_SDK_OK);
    busy_sdk_provider provider_state;
    auto provider = provider_state.table();
    REQUIRE(sao_sdk_context_bind_provider(sdk_context, &provider) == SAO_SDK_OK);
    sao_sdk_timer_token_t timer = 0;
    REQUIRE(sao_sdk_timer_register(sdk_context, 10, noop_sdk_timer, nullptr, &timer) == SAO_SDK_OK);

    bridge_fixture fixture(sdk_context);
    auto* session = cshost_sdk_session_find_handle(fixture.session());
    REQUIRE(session != nullptr);
    REQUIRE(cshost_sdk_session_quiesce(session) == SAO_OK);
    CHECK(sao_sdk_context_try_destroy(sdk_context) == SAO_SDK_ERR_BUSY);
    REQUIRE(cshost_sdk_session_resume(session) == SAO_OK);

    char output[64]{};
    size_t required = 0;
    cs_managed_sdk_call call{};
    call.struct_size = sizeof(call);
    call.method_id = static_cast<uint16_t>(sdk_method_id::prop_plugin_id);
    call.args_json_utf8 = "{}";
    call.args_size = 2;
    call.out_result_json_utf8 = output;
    call.out_capacity = sizeof(output);
    call.out_required = &required;
    CHECK(fixture.table()->dispatch(fixture.session(), &call) == SAO_OK);
    CHECK(std::string(output) == "\"managed.bridge.busy\"");

    provider_state.busy = false;
    REQUIRE(cshost_sdk_session_quiesce(session) == SAO_OK);
    REQUIRE(sao_sdk_context_try_destroy(sdk_context) == SAO_SDK_OK);
    REQUIRE(cshost_sdk_session_clear_sdk_context(session, sdk_context) == SAO_OK);
    REQUIRE(fixture.finish() == SAO_OK);
}

TEST_CASE("C# managed callback tokens release exactly once in reverse order",
          "[plugins][csharp][bridge][callback][release]") {
    bridge_fixture fixture;
    std::vector<std::string> order;
    fake_managed_callback first{"first", &order, fixture.table(), fixture.session()};
    fake_managed_callback second{"second", &order, fixture.table(), fixture.session()};
    auto first_descriptor = descriptor(first, cs_managed_callback_kind::timer);
    auto second_descriptor = descriptor(second, cs_managed_callback_kind::timer);
    void* first_entry = nullptr;
    void* first_token = nullptr;
    void* second_entry = nullptr;
    void* second_token = nullptr;
    wrap_callback(fixture, first_descriptor, &first_entry, &first_token);
    wrap_callback(fixture, second_descriptor, &second_entry, &second_token);

    REQUIRE(first.retain_calls == 1);
    REQUIRE(second.retain_calls == 1);
    REQUIRE(fixture.finish() == SAO_OK);
    REQUIRE(first.release_calls == 1);
    REQUIRE(second.release_calls == 1);
    REQUIRE(order == std::vector<std::string>{"retain:first", "retain:second", "release:second",
                                              "release:first"});
    REQUIRE(first.release_reentry_status == sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
    REQUIRE(second.release_reentry_status == sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
}

TEST_CASE("C# callback release token is idempotent and managed throws stay behind the ABI",
          "[plugins][csharp][bridge][callback][throw]") {
    bridge_fixture fixture;
    fake_managed_callback callback{"throw", nullptr, fixture.table(), fixture.session()};
    callback.throw_on_invoke = true;
    auto managed = descriptor(callback, cs_managed_callback_kind::timer);
    void* native_entry = nullptr;
    void* token = nullptr;
    wrap_callback(fixture, managed, &native_entry, &token);

    using timer_callback = void(SAO_PLUGINS_CALL*)(uint64_t, void*);
    reinterpret_cast<timer_callback>(native_entry)(7, token);
    REQUIRE(callback.invoke_calls == 1);

    char error[128]{};
    size_t required = 0;
    REQUIRE(fixture.table()->last_error(fixture.session(), error, sizeof(error), &required) ==
            SAO_OK);
    REQUIRE(std::string(error).find(std::to_string(SAO_ERR_OS_CALL_FAILED)) != std::string::npos);

    REQUIRE(fixture.table()->release_callback(fixture.session(), token) == SAO_OK);
    REQUIRE(fixture.table()->release_callback(fixture.session(), token) == SAO_ERR_HANDLE_INVALID);
    REQUIRE(callback.release_calls == 1);

    fake_managed_callback replacement{"replacement", nullptr, fixture.table(), fixture.session()};
    auto replacement_descriptor = descriptor(replacement, cs_managed_callback_kind::timer);
    void* replacement_entry = nullptr;
    void* replacement_token = nullptr;
    wrap_callback(fixture, replacement_descriptor, &replacement_entry, &replacement_token);
    REQUIRE(replacement_token != token);
    REQUIRE(fixture.table()->release_callback(fixture.session(), token) == SAO_ERR_HANDLE_INVALID);
    REQUIRE(fixture.table()->release_callback(fixture.session(), replacement_token) == SAO_OK);
    REQUIRE(replacement.release_calls == 1);

    REQUIRE(fixture.finish() == SAO_OK);
    REQUIRE(callback.release_calls == 1);
}

TEST_CASE("C# session quiesce waits for active callbacks without partial publication teardown",
          "[plugins][csharp][bridge][callback][quiesce][race]") {
    bridge_fixture fixture;
    callback_gate gate;
    fake_managed_callback callback{"blocking", nullptr, fixture.table(), fixture.session()};
    callback.gate = &gate;
    auto managed = descriptor(callback, cs_managed_callback_kind::timer);
    void* native_entry = nullptr;
    void* token = nullptr;
    wrap_callback(fixture, managed, &native_entry, &token);

    using timer_callback = void(SAO_PLUGINS_CALL*)(uint64_t, void*);
    std::thread invoking([&] { reinterpret_cast<timer_callback>(native_entry)(9, token); });
    {
        std::unique_lock lock(gate.mutex);
        gate.condition.wait(lock, [&] { return gate.entered; });
    }

    auto finish = std::async(std::launch::async, [&] { return fixture.finish(); });
    REQUIRE(finish.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
    CHECK(fixture.table()->log(fixture.session(), "retiring") ==
          sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
    {
        std::lock_guard lock(gate.mutex);
        gate.released = true;
    }
    gate.condition.notify_all();
    invoking.join();
    CHECK(finish.get() == SAO_OK);
    CHECK(callback.invoke_calls == 1);
    CHECK(callback.release_calls == 1);
}

TEST_CASE("C# managed release throw is observable after provider deactivation",
          "[plugins][csharp][bridge][callback][release_throw]") {
    bridge_fixture fixture;
    fake_managed_callback callback{"release-throw", nullptr, fixture.table(), fixture.session()};
    callback.throw_on_release = true;
    auto managed = descriptor(callback, cs_managed_callback_kind::timer);
    void* native_entry = nullptr;
    void* token = nullptr;
    wrap_callback(fixture, managed, &native_entry, &token);

    REQUIRE(fixture.finish() == SAO_ERR_OS_CALL_FAILED);
    REQUIRE(callback.release_calls == 1);
    REQUIRE(callback.release_reentry_status == sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
}
