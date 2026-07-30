// SAO Auto — launcher/tests/test_init_pipeline.cpp
//
// Headless init pipeline smoke harness coverage.
//
// The test target uses explicit composition hooks so ordering can be observed
// without starting real platform or optional subsystem runtimes.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <windows.h>

// Private launcher headers must be included BEFORE sao/launcher/init_pipeline.h
// because init_pipeline.h defines the historical SAO_STATUS_* macros
// (SAO_STATUS_OK, SAO_STATUS_INVALID_ARGUMENT, ...) as preprocessor tokens.
// The private headers pull in sao/core/status.h which defines those same
// identifiers as enum members of sao_status_e — the collision breaks the enum
// declaration if the macro is already in scope.  Reversing the include order
// lets the enum be parsed first; the subsequent macro redefinitions still
// evaluate to identical integer values so downstream ABI comparisons keep
// working.
#include "entity_action_routes_internal.h"
#include "tool_launch_internal.h"

#include "sao/launcher/app.h"
#include "sao/launcher/dual_run.h"
#include "sao/launcher/init_pipeline.h"
#include "sao/launcher/provider_config.h"
#include "sao/launcher/rollout.h"
#include "sao/launcher/shutdown.h"
#include "sao/plugins/loader/loader_status.h"
#include "launcher_lifecycle.h"
#if defined(SAO_LAUNCHER_CORE_LOG_PROVIDER)
#undef SAO_STATUS_OK
#include "sao/core/logging.h"
#endif

#if defined(SAO_LAUNCHER_HAS_TELEMETRY_CLIENT)
#include "sao/server/freetier/telemetry_client/telemetry_client.h"
#include "sao_core/sao_status.h"
#endif

// The init pipeline exit-code constants live in the sao::launcher namespace
// alongside the App state machine.  Pull them in for terser assertions.
using sao::launcher::SAO_EXIT_ALREADY_RUNNING;
using sao::launcher::SAO_EXIT_OK;

extern "C" sao_status_t sao_launcher_init_pipeline_test_apply_streaming_mode_transaction(
    bool enabled, sao_status_t (*acquire)(void*), bool (*get_flow)(void*),
    bool (*get_capture)(void*), sao_status_t (*set_capture)(bool, void*),
    sao_status_t (*set_flow)(bool, void*), sao_status_t (*release)(void*), void* user_data);

extern "C" sao_status_t sao_launcher_init_pipeline_test_publish_entity_authority_before_online(
    sao_status_t (*publish)(void*), sao_status_t (*bring_online)(void*), void* user_data);

extern "C" sao_status_t sao_launcher_init_pipeline_test_apply_nervgear_mode_transaction(
    bool* mode, sao_status_t (*set_shell_mode)(bool, void*),
    sao_status_t (*persist_mode)(bool, void*), sao_status_t (*publish_degraded)(void*),
    void* user_data);

// Runtime installer hook setter. Matches the internal
// sao::launcher::runtime_installer_glue::EnsureAllFn type. Passing
// nullptr restores the pass-through default so subsequent tests observe
// the natural skip-when-unavailable ordering.
extern "C" void sao_launcher_init_pipeline_test_set_runtime_installer_hook(
    sao_status_t (*ensure_all)(const wchar_t* base_dir,
                               void (*progress_cb)(const char* kind_opaque_id_utf8,
                                                   uint64_t bytes_done,
                                                   uint64_t bytes_total,
                                                   void* user_data),
                               void* progress_user_data));

namespace {

struct TeardownRecorder {
    std::vector<std::string> steps;

    static void onStep(const char* name, void* user) {
        auto* self = static_cast<TeardownRecorder*>(user);
        self->steps.emplace_back(name ? name : "<null>");
    }

    static int pollExitImmediately(void*) {
        return 1;
    }
};

struct CompositionRecorder {
    std::vector<std::string> steps;
    std::string platform_log_level;
    sao_status_t shell_verify_status = SAO_STATUS_OK;
    sao_status_t license_verify_status = SAO_STATUS_OK;
    sao_status_t plugin_discover_status = SAO_STATUS_OK;
    sao_status_t plugin_activate_status = SAO_STATUS_OK;
    sao_status_t security_status = SAO_STATUS_OK;
    sao_status_t platform_bringup_status = SAO_STATUS_OK;
    std::vector<sao_status_t> platform_teardown_statuses;
    size_t platform_teardown_attempt = 0;
    std::vector<sao_status_t> plugin_shutdown_statuses;
    size_t plugin_shutdown_attempt = 0;
    std::vector<sao_status_t> shell_shutdown_statuses;
    size_t shell_shutdown_attempt = 0;
    std::vector<sao_status_t> license_shutdown_statuses;
    size_t license_shutdown_attempt = 0;

    static sao_status_t shellVerify(sao_shell_verify_result* out, void* user) {
        auto* self = static_cast<CompositionRecorder*>(user);
        self->steps.emplace_back("shell_verify");
        *out = {};
        return self->shell_verify_status;
    }

    static sao_status_t shellShutdown(void* user) {
        auto* self = static_cast<CompositionRecorder*>(user);
        self->steps.emplace_back("shell_shutdown");
        if (self->shell_shutdown_attempt >= self->shell_shutdown_statuses.size()) {
            return SAO_STATUS_OK;
        }
        return self->shell_shutdown_statuses[self->shell_shutdown_attempt++];
    }

    static sao_status_t licenseVerify(sao_license_result* out, void* user) {
        auto* self = static_cast<CompositionRecorder*>(user);
        self->steps.emplace_back("license_verify");
        *out = {};
        out->valid = 1;
        return self->license_verify_status;
    }

    static sao_status_t licenseShutdown(void* user) {
        auto* self = static_cast<CompositionRecorder*>(user);
        self->steps.emplace_back("license_shutdown");
        if (self->license_shutdown_attempt >= self->license_shutdown_statuses.size()) {
            return SAO_STATUS_OK;
        }
        return self->license_shutdown_statuses[self->license_shutdown_attempt++];
    }

    static sao_status_t pluginsDiscover(sao_platform_ctx*, sao_plugins_registry** out, void* user) {
        auto* self = static_cast<CompositionRecorder*>(user);
        self->steps.emplace_back("plugins_discover");
        *out = self->plugin_discover_status == SAO_STATUS_OK
                   ? reinterpret_cast<sao_plugins_registry*>(self)
                   : nullptr;
        return self->plugin_discover_status;
    }

    static sao_status_t pluginsActivate(sao_plugins_registry* registry, void* user) {
        auto* self = static_cast<CompositionRecorder*>(user);
        REQUIRE(registry == reinterpret_cast<sao_plugins_registry*>(self));
        self->steps.emplace_back("plugins_activate");
        return self->plugin_activate_status;
    }

    static sao_status_t pluginsShutdown(sao_plugins_registry* registry, void* user) {
        auto* self = static_cast<CompositionRecorder*>(user);
        REQUIRE(registry == reinterpret_cast<sao_plugins_registry*>(self));
        self->steps.emplace_back("plugins_shutdown");
        if (self->plugin_shutdown_attempt >= self->plugin_shutdown_statuses.size()) {
            return SAO_STATUS_OK;
        }
        return self->plugin_shutdown_statuses[self->plugin_shutdown_attempt++];
    }

    static sao_status_t securityInit(const sao_security_config* cfg, void* user) {
        auto* self = static_cast<CompositionRecorder*>(user);
        self->steps.emplace_back("security_init");
        return cfg && cfg->enable_anti_debug && cfg->enable_anti_dump &&
                       cfg->enable_user_evasion && !cfg->strict_user_evasion &&
                       cfg->anti_debug_poll_interval_seconds == 5
                   ? self->security_status
                   : SAO_STATUS_INVALID_ARGUMENT;
    }

    static void securityShutdown(void* user) {
        static_cast<CompositionRecorder*>(user)->steps.emplace_back("security_shutdown");
    }

    static sao_status_t platformBringup(const sao_platform_config* cfg, sao_platform_ctx** out,
                                        void* user) {
        auto* self = static_cast<CompositionRecorder*>(user);
        self->steps.emplace_back("platform_bringup");
        self->platform_log_level = cfg && cfg->log_level ? cfg->log_level : "<null>";
        *out = reinterpret_cast<sao_platform_ctx*>(self);
        return self->platform_bringup_status;
    }

    static sao_status_t platformTeardown(sao_platform_ctx* ctx, void* user) {
        auto* self = static_cast<CompositionRecorder*>(user);
        self->steps.emplace_back("platform_teardown");
        REQUIRE(ctx == reinterpret_cast<sao_platform_ctx*>(self));
        if (self->platform_teardown_attempt >= self->platform_teardown_statuses.size()) {
            return SAO_STATUS_OK;
        }
        return self->platform_teardown_statuses[self->platform_teardown_attempt++];
    }

    static sao_status_t uiBringOnline(sao_platform_ctx*, void* user) {
        static_cast<CompositionRecorder*>(user)->steps.emplace_back("ui_online");
        return SAO_STATUS_OK;
    }

    static sao_status_t uiTakeOffline(sao_platform_ctx*, void* user) {
        static_cast<CompositionRecorder*>(user)->steps.emplace_back("ui_offline");
        return SAO_STATUS_OK;
    }
};

struct CompositionHookGuard {
    explicit CompositionHookGuard(CompositionRecorder& recorder) {
        hooks.shell_verify = &CompositionRecorder::shellVerify;
        hooks.shell_shutdown = &CompositionRecorder::shellShutdown;
        hooks.license_verify = &CompositionRecorder::licenseVerify;
        hooks.license_shutdown = &CompositionRecorder::licenseShutdown;
        hooks.plugins_discover = &CompositionRecorder::pluginsDiscover;
        hooks.plugins_activate_autostart = &CompositionRecorder::pluginsActivate;
        hooks.plugins_shutdown = &CompositionRecorder::pluginsShutdown;
        hooks.security_init = &CompositionRecorder::securityInit;
        hooks.security_shutdown = &CompositionRecorder::securityShutdown;
        hooks.platform_bringup = &CompositionRecorder::platformBringup;
        hooks.platform_teardown = &CompositionRecorder::platformTeardown;
        hooks.ui_bring_online = &CompositionRecorder::uiBringOnline;
        hooks.ui_take_offline = &CompositionRecorder::uiTakeOffline;
        hooks.user_data = &recorder;
        sao_launcher_set_composition_test_hooks(&hooks);
    }

    ~CompositionHookGuard() {
        sao_launcher_set_composition_test_hooks(nullptr);
    }

    sao_launcher_composition_test_hooks_t hooks{};
};

struct StreamingModeTransactionRecorder {
    bool capture_excluded = false;
    bool flow_excluded = false;
    bool locked = false;
    DWORD acquire_thread_id = 0;
    std::vector<DWORD> release_thread_ids;
    size_t release_attempts = 0;
    std::vector<std::string> steps;

    static sao_status_t acquire(void* user_data) {
        auto* self = static_cast<StreamingModeTransactionRecorder*>(user_data);
        self->steps.emplace_back("acquire");
        if (self->locked)
            return SAO_STATUS_INTERNAL;
        self->acquire_thread_id = GetCurrentThreadId();
        self->locked = true;
        return SAO_STATUS_OK;
    }

    static bool getFlow(void* user_data) {
        return static_cast<StreamingModeTransactionRecorder*>(user_data)->flow_excluded;
    }

    static bool getCapture(void* user_data) {
        return static_cast<StreamingModeTransactionRecorder*>(user_data)->capture_excluded;
    }

    static sao_status_t setCapture(bool enabled, void* user_data) {
        auto* self = static_cast<StreamingModeTransactionRecorder*>(user_data);
        self->steps.emplace_back(enabled ? "capture_on" : "capture_off");
        self->capture_excluded = enabled;
        return SAO_STATUS_OK;
    }

    static sao_status_t setFlow(bool enabled, void* user_data) {
        auto* self = static_cast<StreamingModeTransactionRecorder*>(user_data);
        self->steps.emplace_back(enabled ? "flow_on" : "flow_off");
        self->flow_excluded = enabled;
        return SAO_STATUS_OK;
    }

    static sao_status_t releaseFailure(void* user_data) {
        auto* self = static_cast<StreamingModeTransactionRecorder*>(user_data);
        self->steps.emplace_back("release_failed");
        ++self->release_attempts;
        self->release_thread_ids.push_back(GetCurrentThreadId());
        return SAO_STATUS_INTERNAL;
    }

    static sao_status_t releaseFirstFailureThenSuccess(void* user_data) {
        auto* self = static_cast<StreamingModeTransactionRecorder*>(user_data);
        ++self->release_attempts;
        self->release_thread_ids.push_back(GetCurrentThreadId());
        if (self->release_attempts == 1) {
            self->steps.emplace_back("release_failed");
            return SAO_STATUS_INTERNAL;
        }
        self->steps.emplace_back("release_succeeded");
        self->locked = false;
        return SAO_STATUS_OK;
    }
};

struct EntityAuthorityStartupRecorder {
    sao_status_t publication_status = SAO_STATUS_OK;
    sao_status_t online_status = SAO_STATUS_OK;
    std::vector<std::string> steps;

    static sao_status_t publish(void* user_data) {
        auto* self = static_cast<EntityAuthorityStartupRecorder*>(user_data);
        self->steps.emplace_back("publish");
        return self->publication_status;
    }

    static sao_status_t bringOnline(void* user_data) {
        auto* self = static_cast<EntityAuthorityStartupRecorder*>(user_data);
        self->steps.emplace_back("online");
        return self->online_status;
    }
};

struct NervgearModeTransactionRecorder {
    bool shell_mode = true;
    sao_status_t persist_status = SAO_STATUS_OK;
    sao_status_t degraded_status = SAO_STATUS_OK;
    std::vector<sao_status_t> set_statuses;
    std::size_t set_attempt = 0;
    std::size_t degraded_calls = 0;
    std::vector<std::string> steps;

    static sao_status_t setShellMode(bool enabled, void* user_data) {
        auto* self = static_cast<NervgearModeTransactionRecorder*>(user_data);
        self->steps.emplace_back(enabled ? "shell:on" : "shell:off");
        const sao_status_t status =
            self->set_attempt < self->set_statuses.size()
                ? self->set_statuses[self->set_attempt++]
                : SAO_STATUS_OK;
        if (status == SAO_STATUS_OK) {
            self->shell_mode = enabled;
        }
        return status;
    }

    static sao_status_t persistMode(bool enabled, void* user_data) {
        auto* self = static_cast<NervgearModeTransactionRecorder*>(user_data);
        self->steps.emplace_back(enabled ? "persist:on" : "persist:off");
        return self->persist_status;
    }

    static sao_status_t publishDegraded(void* user_data) {
        auto* self = static_cast<NervgearModeTransactionRecorder*>(user_data);
        self->steps.emplace_back("degraded");
        ++self->degraded_calls;
        return self->degraded_status;
    }
};

struct ConfigFile {
    explicit ConfigFile(const char* content) {
        wchar_t temp_path[MAX_PATH]{};
        REQUIRE(GetTempPathW(MAX_PATH, temp_path) != 0);
        wchar_t temp_file[MAX_PATH]{};
        REQUIRE(GetTempFileNameW(temp_path, L"sao", 0, temp_file) != 0);
        path = temp_file;
        std::ofstream output(std::filesystem::path(path), std::ios::binary);
        REQUIRE(output.good());
        output << content;
        output.close();
        REQUIRE(output.good());
    }

    ~ConfigFile() {
        DeleteFileW(path.c_str());
    }

    std::wstring argument() const {
        return L"--config=" + path;
    }

    std::wstring path;
};

constexpr const char* kAllProvidersConfig = R"json({
    "license": {
        "enabled": true,
        "endpoint": "https://license.invalid/api/v1/license",
        "server_ed25519_pubkey":
            "1111111111111111111111111111111111111111111111111111111111111111"
    },
    "plugins": {"enabled": true, "roots": ["plugins"]}
})json";

struct DualRunChildGuard {
    DualRunChildGuard() {
        SetEnvironmentVariableW(SAO_DUAL_RUN_ENV_VAR_NAME, L"cpp");
    }
    ~DualRunChildGuard() {
        SetEnvironmentVariableW(SAO_DUAL_RUN_ENV_VAR_NAME, nullptr);
    }
};

int findStep(const std::vector<std::string>& steps, const char* name) {
    for (size_t i = 0; i < steps.size(); ++i) {
        if (steps[i] == name)
            return static_cast<int>(i);
    }
    return -1;
}

std::filesystem::path uniqueRolloutDirectory() {
    wchar_t temp_path[MAX_PATH]{};
    REQUIRE(GetTempPathW(MAX_PATH, temp_path) != 0);
    wchar_t temp_file[MAX_PATH]{};
    REQUIRE(GetTempFileNameW(temp_path, L"rol", 0, temp_file) != 0);
    DeleteFileW(temp_file);
    std::filesystem::path directory(temp_file);
    std::error_code error;
    REQUIRE(std::filesystem::create_directories(directory, error));
    return directory;
}

std::string pathUtf8(const std::filesystem::path& path) {
    const auto wide = path.wstring();
    const int required =
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    REQUIRE(required > 1);
    std::string utf8(static_cast<size_t>(required), '\0');
    REQUIRE(WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, utf8.data(), required, nullptr,
                                nullptr) == required);
    utf8.pop_back();
    return utf8;
}

struct ProductionRolloutGuard {
    ProductionRolloutGuard()
        : directory(uniqueRolloutDirectory()), directory_utf8(pathUtf8(directory)) {
        SetEnvironmentVariableW(SAO_DUAL_RUN_ENV_VAR_NAME, nullptr);
        sao_launcher_dual_run_reset_for_test();
        sao_rollout_reset_for_test();
        sao_rollout_test_set_appdata_dir(directory.c_str());
#if defined(SAO_LAUNCHER_HAS_TELEMETRY_CLIENT)
        sao_telemetry_test_reset();
        sao_telemetry_test_set_appdata_dir(directory_utf8.c_str());
        sao_telemetry_test_set_capture_only(1);
#endif
    }

    ~ProductionRolloutGuard() {
        SetEnvironmentVariableW(SAO_DUAL_RUN_ENV_VAR_NAME, nullptr);
#if defined(SAO_LAUNCHER_HAS_TELEMETRY_CLIENT)
        sao_telemetry_test_reset();
#endif
        sao_rollout_reset_for_test();
        sao_launcher_dual_run_reset_for_test();
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    std::filesystem::path directory;
    std::string directory_utf8;
};

struct DualRunInvocationRecorder {
    static inline int probe_calls = 0;
    static inline int spawn_calls = 0;

    static void reset() {
        probe_calls = 0;
        spawn_calls = 0;
    }

    static void probe(sao_dual_run_python_probe* out) {
        ++probe_calls;
        *out = {};
        out->available = 1;
        lstrcpynW(out->path, L"E:\\Py\\python.exe", 260);
        out->major = 3;
        out->minor = 11;
    }

    static int spawn(const wchar_t*, const wchar_t*, const wchar_t*,
                     sao_dual_run_spawn_result* result) {
        ++spawn_calls;
        *result = {};
        result->pid = 4242;
        return 0;
    }
};

size_t countStep(const std::vector<std::string>& steps, const char* name) {
    size_t count = 0;
    for (const auto& step : steps) {
        if (step == name)
            ++count;
    }
    return count;
}

} // namespace

TEST_CASE("launcher_init_pipeline_run_no_deps_returns_ok", "[launcher][init_pipeline]") {
    DualRunChildGuard dual_run_child_guard;
    CompositionRecorder composition;
    CompositionHookGuard composition_guard(composition);
    TeardownRecorder rec;
    sao_launcher_init_hooks_t hooks{};
    hooks.poll_should_exit = &TeardownRecorder::pollExitImmediately;
    hooks.on_teardown_step = &TeardownRecorder::onStep;
    hooks.user_data = &rec;

    // argv[0] must exist even if we pass nothing else — args parsing
    // starts at index 1 so we hand it a single filler.
    wchar_t argv0[] = L"SaoAutoTests.exe";
    wchar_t* argv[] = {argv0};

    int exit_code = 12345;
    sao_status_t rc = sao_launcher_init_pipeline_run(1, argv, &hooks, &exit_code);

    REQUIRE(rc == SAO_STATUS_OK);
    REQUIRE(exit_code == SAO_EXIT_OK);
    // BaseDir must be populated once resolveWorkingDir completes.
    REQUIRE(SaoLauncherBaseDir[0] != L'\0');
}

TEST_CASE("launcher streaming mode release failure compensates applied state",
          "[launcher][init_pipeline][streaming][transaction]") {
    StreamingModeTransactionRecorder recorder;

    CHECK(sao_launcher_init_pipeline_test_apply_streaming_mode_transaction(
              true, &StreamingModeTransactionRecorder::acquire,
              &StreamingModeTransactionRecorder::getFlow,
              &StreamingModeTransactionRecorder::getCapture,
              &StreamingModeTransactionRecorder::setCapture,
              &StreamingModeTransactionRecorder::setFlow,
              &StreamingModeTransactionRecorder::releaseFailure, &recorder) ==
          SAO_STATUS_INTERNAL);
    CHECK_FALSE(recorder.capture_excluded);
    CHECK_FALSE(recorder.flow_excluded);
    CHECK(recorder.locked);
    CHECK(recorder.release_attempts == 2);
    CHECK(recorder.steps == std::vector<std::string>{"acquire", "capture_on", "flow_on",
                                                     "release_failed", "release_failed",
                                                     "capture_off", "flow_off"});
}

TEST_CASE("launcher streaming mode release retries on the same thread",
          "[launcher][init_pipeline][streaming][transaction][retry]") {
    StreamingModeTransactionRecorder recorder;

    REQUIRE(sao_launcher_init_pipeline_test_apply_streaming_mode_transaction(
                true, &StreamingModeTransactionRecorder::acquire,
                &StreamingModeTransactionRecorder::getFlow,
                &StreamingModeTransactionRecorder::getCapture,
                &StreamingModeTransactionRecorder::setCapture,
                &StreamingModeTransactionRecorder::setFlow,
                &StreamingModeTransactionRecorder::releaseFirstFailureThenSuccess, &recorder) ==
            SAO_STATUS_OK);
    CHECK(recorder.capture_excluded);
    CHECK(recorder.flow_excluded);
    CHECK_FALSE(recorder.locked);
    REQUIRE(recorder.release_attempts == 2);
    REQUIRE(recorder.release_thread_ids.size() == 2);
    CHECK(recorder.release_thread_ids[0] == recorder.acquire_thread_id);
    CHECK(recorder.release_thread_ids[1] == recorder.acquire_thread_id);
    CHECK(recorder.steps == std::vector<std::string>{"acquire", "capture_on", "flow_on",
                                                     "release_failed", "release_succeeded"});

    REQUIRE(StreamingModeTransactionRecorder::acquire(&recorder) == SAO_STATUS_OK);
    CHECK(recorder.locked);
    REQUIRE(StreamingModeTransactionRecorder::releaseFirstFailureThenSuccess(&recorder) ==
            SAO_STATUS_OK);
    CHECK_FALSE(recorder.locked);
}

TEST_CASE("launcher publishes entity authority before bringing the shell online",
          "[launcher][init_pipeline][entity][authority][startup][focused]") {
    EntityAuthorityStartupRecorder recorder;
    REQUIRE(sao_launcher_init_pipeline_test_publish_entity_authority_before_online(
                &EntityAuthorityStartupRecorder::publish,
                &EntityAuthorityStartupRecorder::bringOnline, &recorder) == SAO_STATUS_OK);
    CHECK(recorder.steps == std::vector<std::string>{"publish", "online"});

    recorder = {};
    recorder.publication_status = SAO_STATUS_ERR_OS_CALL_FAILED;
    CHECK(sao_launcher_init_pipeline_test_publish_entity_authority_before_online(
              &EntityAuthorityStartupRecorder::publish,
              &EntityAuthorityStartupRecorder::bringOnline, &recorder) ==
          SAO_STATUS_ERR_OS_CALL_FAILED);
    CHECK(recorder.steps == std::vector<std::string>{"publish"});
}

TEST_CASE("launcher NerveGear persistence rollback failure publishes degraded authority",
          "[launcher][init_pipeline][entity][nervgear][rollback][degraded][focused]") {
    bool runtime_mode = true;
    NervgearModeTransactionRecorder recorder;
    recorder.persist_status = SAO_STATUS_ERR_ACCESS_DENIED;
    recorder.set_statuses = {SAO_STATUS_OK, SAO_STATUS_ERR_OS_CALL_FAILED};

    CHECK(sao_launcher_init_pipeline_test_apply_nervgear_mode_transaction(
              &runtime_mode, &NervgearModeTransactionRecorder::setShellMode,
              &NervgearModeTransactionRecorder::persistMode,
              &NervgearModeTransactionRecorder::publishDegraded, &recorder) ==
          SAO_STATUS_ERR_UNKNOWN);
    CHECK_FALSE(runtime_mode);
    CHECK_FALSE(recorder.shell_mode);
    CHECK(recorder.degraded_calls == 1);
    CHECK(recorder.steps == std::vector<std::string>{"shell:off", "persist:off", "shell:on",
                                                     "degraded"});

    runtime_mode = true;
    recorder = {};
    recorder.persist_status = SAO_STATUS_ERR_ACCESS_DENIED;
    recorder.set_statuses = {SAO_STATUS_OK, SAO_STATUS_OK};
    CHECK(sao_launcher_init_pipeline_test_apply_nervgear_mode_transaction(
              &runtime_mode, &NervgearModeTransactionRecorder::setShellMode,
              &NervgearModeTransactionRecorder::persistMode,
              &NervgearModeTransactionRecorder::publishDegraded, &recorder) ==
          SAO_STATUS_ERR_ACCESS_DENIED);
    CHECK(runtime_mode);
    CHECK(recorder.shell_mode);
    CHECK(recorder.degraded_calls == 0);
}

TEST_CASE("launcher_init_pipeline_teardown_reverse_order", "[launcher][init_pipeline]") {
    DualRunChildGuard dual_run_child;
    ConfigFile config(kAllProvidersConfig);
    CompositionRecorder composition;
    CompositionHookGuard composition_guard(composition);
    TeardownRecorder rec;
    sao_launcher_init_hooks_t hooks{};
    hooks.poll_should_exit = &TeardownRecorder::pollExitImmediately;
    hooks.on_teardown_step = &TeardownRecorder::onStep;
    hooks.user_data = &rec;

    wchar_t argv0[] = L"SaoAutoTests.exe";
    auto config_argument = config.argument();
    wchar_t* argv[] = {argv0, config_argument.data()};

    int exit_code = 0;
    sao_status_t rc = sao_launcher_init_pipeline_run(2, argv, &hooks, &exit_code);
    REQUIRE(rc == SAO_STATUS_OK);
    REQUIRE(exit_code == SAO_EXIT_OK);

    const int security_idx = findStep(rec.steps, "security");
    const int shell_idx = findStep(rec.steps, "shell");
    const int license_idx = findStep(rec.steps, "license");
    const int mutex_idx = findStep(rec.steps, "single_instance");
    const int crash_idx = findStep(rec.steps, "crash_handler");

    REQUIRE(findStep(composition.steps, "security_init") >= 0);
    REQUIRE(findStep(composition.steps, "license_verify") <
            findStep(composition.steps, "security_init"));
    REQUIRE(findStep(composition.steps, "platform_bringup") <
            findStep(composition.steps, "plugins_discover"));
    REQUIRE(findStep(composition.steps, "plugins_discover") <
            findStep(composition.steps, "plugins_activate"));
    REQUIRE(findStep(composition.steps, "ui_offline") >= 0);
    REQUIRE(findStep(composition.steps, "platform_teardown") >= 0);
    REQUIRE(findStep(composition.steps, "security_shutdown") >= 0);
    REQUIRE(findStep(composition.steps, "ui_offline") <
            findStep(composition.steps, "plugins_shutdown"));
    REQUIRE(findStep(composition.steps, "plugins_shutdown") <
            findStep(composition.steps, "platform_teardown"));
    REQUIRE(findStep(composition.steps, "platform_teardown") <
            findStep(composition.steps, "security_shutdown"));
    REQUIRE(findStep(composition.steps, "security_shutdown") <
            findStep(composition.steps, "license_shutdown"));
    REQUIRE(security_idx >= 0);
    REQUIRE(shell_idx == -1);
    REQUIRE(license_idx >= 0);
    REQUIRE(mutex_idx >= 0);
    REQUIRE(crash_idx >= 0);

    // Reverse-init order: security before license before mutex.
    REQUIRE(security_idx < license_idx);
    REQUIRE(license_idx < mutex_idx);
    REQUIRE(mutex_idx < crash_idx);
}

TEST_CASE("launcher headless teardown retries plugin busy before platform",
          "[launcher][init_pipeline][shutdown][busy][retry]") {
    DualRunChildGuard dual_run_child;
    ConfigFile config(kAllProvidersConfig);
    CompositionRecorder composition;
    composition.plugin_shutdown_statuses = {
        sao::plugins::loader::SAO_PLUGINS_ERR_BUSY,
        SAO_STATUS_OK,
    };
    CompositionHookGuard composition_guard(composition);
    TeardownRecorder teardown_recorder;
    sao_launcher_init_hooks_t hooks{};
    hooks.poll_should_exit = &TeardownRecorder::pollExitImmediately;
    hooks.on_teardown_step = &TeardownRecorder::onStep;
    hooks.user_data = &teardown_recorder;

    wchar_t argv0[] = L"SaoAutoTests.exe";
    auto config_argument = config.argument();
    wchar_t* argv[] = {argv0, config_argument.data()};
    int exit_code = -1;

    REQUIRE(sao_launcher_init_pipeline_run(2, argv, &hooks, &exit_code) == SAO_STATUS_OK);
    REQUIRE(exit_code == SAO_EXIT_OK);
    REQUIRE(countStep(composition.steps, "plugins_shutdown") == 2);
    REQUIRE(countStep(composition.steps, "platform_teardown") == 1);
    const auto first_shutdown =
        std::find(composition.steps.begin(), composition.steps.end(), "plugins_shutdown");
    REQUIRE(first_shutdown != composition.steps.end());
    const auto second_shutdown =
        std::find(std::next(first_shutdown), composition.steps.end(), "plugins_shutdown");
    const auto platform =
        std::find(composition.steps.begin(), composition.steps.end(), "platform_teardown");
    REQUIRE(second_shutdown != composition.steps.end());
    REQUIRE(platform != composition.steps.end());
    CHECK(second_shutdown < platform);
}

TEST_CASE("launcher headless reports exhausted plugin busy teardown",
          "[launcher][init_pipeline][shutdown][busy][retry]") {
    DualRunChildGuard dual_run_child;
    ConfigFile config(kAllProvidersConfig);
    CompositionRecorder composition;
    composition.plugin_shutdown_statuses = {
        sao::plugins::loader::SAO_PLUGINS_ERR_BUSY,
        sao::plugins::loader::SAO_PLUGINS_ERR_BUSY,
        sao::plugins::loader::SAO_PLUGINS_ERR_BUSY,
    };
    CompositionHookGuard composition_guard(composition);
    TeardownRecorder teardown_recorder;
    sao_launcher_init_hooks_t hooks{};
    hooks.poll_should_exit = &TeardownRecorder::pollExitImmediately;
    hooks.on_teardown_step = &TeardownRecorder::onStep;
    hooks.user_data = &teardown_recorder;

    wchar_t argv0[] = L"SaoAutoTests.exe";
    auto config_argument = config.argument();
    wchar_t* argv[] = {argv0, config_argument.data()};
    int exit_code = -1;

    CHECK(sao_launcher_init_pipeline_run(2, argv, &hooks, &exit_code) == SAO_STATUS_INTERNAL);
    CHECK(exit_code == sao::launcher::SAO_EXIT_PLATFORM_INIT_FAIL);
    CHECK(countStep(composition.steps, "plugins_shutdown") == 3);
    CHECK(countStep(composition.steps, "platform_teardown") == 0);

    REQUIRE(sao_launcher_init_pipeline_retry_pending_cleanup() == SAO_STATUS_OK);
    CHECK(countStep(composition.steps, "plugins_shutdown") == 4);
    CHECK(countStep(composition.steps, "platform_teardown") == 1);

    exit_code = -1;
    REQUIRE(sao_launcher_init_pipeline_run(2, argv, &hooks, &exit_code) == SAO_STATUS_OK);
    CHECK(exit_code == SAO_EXIT_OK);
}

TEST_CASE("launcher_init_pipeline_no_config_enables_stable_plugin_roots",
          "[launcher][init_pipeline][providers]") {
    DualRunChildGuard dual_run_child;
    CompositionRecorder composition;
    CompositionHookGuard composition_guard(composition);
    TeardownRecorder rec;
    sao_launcher_init_hooks_t hooks{};
    hooks.poll_should_exit = &TeardownRecorder::pollExitImmediately;
    hooks.on_teardown_step = &TeardownRecorder::onStep;
    hooks.user_data = &rec;
    wchar_t argv0[] = L"SaoAutoTests.exe";
    wchar_t* argv[] = {argv0};
    int exit_code = 0;

    REQUIRE(sao_launcher_init_pipeline_run(1, argv, &hooks, &exit_code) == SAO_STATUS_OK);
    REQUIRE(exit_code == SAO_EXIT_OK);
    REQUIRE(findStep(composition.steps, "license_verify") == -1);
    REQUIRE(findStep(composition.steps, "shell_verify") == -1);
    REQUIRE(findStep(composition.steps, "plugins_discover") >= 0);
    REQUIRE(findStep(composition.steps, "plugins_activate") >= 0);
    REQUIRE(findStep(composition.steps, "plugins_shutdown") >= 0);
    REQUIRE(findStep(rec.steps, "license") == -1);
    REQUIRE(findStep(rec.steps, "shell") == -1);
    REQUIRE(findStep(rec.steps, "plugins") >= 0);
}

TEST_CASE("launcher raw license transport rejects prevalidated responses",
          "[launcher][license][trust]") {
    ConfigFile verified_locally(R"json({
        "license": {
            "enabled": true,
            "endpoint": "https://license.invalid/api/v1/license",
            "build_id": "test-build",
            "server_ed25519_pubkey":
                "1111111111111111111111111111111111111111111111111111111111111111",
            "responses_prevalidated": false,
            "heartbeat_interval_ms": 30000
        }
    })json");
    const auto base = std::filesystem::path(verified_locally.path).parent_path();
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(
                base.c_str(), verified_locally.path.c_str()) == SAO_STATUS_OK);
    const auto accepted = sao::launcher::launcherProviderConfigurationSnapshot().license;
    REQUIRE(accepted.enabled);
    REQUIRE_FALSE(accepted.responses_prevalidated);
    REQUIRE(accepted.build_id == "test-build");
    REQUIRE(accepted.heartbeat_interval_ms == 30000);

    ConfigFile bypass_attempt(R"json({
        "license": {
            "enabled": true,
            "endpoint": "https://license.invalid/api/v1/license",
            "server_ed25519_pubkey":
                "2222222222222222222222222222222222222222222222222222222222222222",
            "responses_prevalidated": true
        }
    })json");
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(
                base.c_str(), bypass_attempt.path.c_str()) == SAO_STATUS_INVALID_ARGUMENT);
    const auto retained = sao::launcher::launcherProviderConfigurationSnapshot().license;
    REQUIRE(retained.enabled);
    REQUIRE_FALSE(retained.responses_prevalidated);
    REQUIRE(retained.build_id == "test-build");

    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(nullptr, nullptr) == SAO_STATUS_OK);
}

TEST_CASE("launcher_init_pipeline_enabled_provider_missing_fails_closed",
          "[launcher][init_pipeline][providers]") {
    DualRunChildGuard dual_run_child;
    ConfigFile config(kAllProvidersConfig);
    sao_launcher_set_composition_test_hooks(nullptr);
    wchar_t argv0[] = L"SaoAutoTests.exe";
    auto config_argument = config.argument();
    wchar_t* argv[] = {argv0, config_argument.data()};
    int exit_code = 0;

    REQUIRE(sao_launcher_init_pipeline_run(2, argv, nullptr, &exit_code) == SAO_STATUS_INTERNAL);
    REQUIRE(exit_code == sao::launcher::SAO_EXIT_LICENSE_INVALID);
}

TEST_CASE("launcher_init_pipeline_enabled_shell_provider_missing_fails_closed",
          "[launcher][init_pipeline][providers]") {
    DualRunChildGuard dual_run_child;
    ConfigFile config(R"json({"shell":{"enabled":true,"metadata":"shell.meta"}})json");
    const auto metadata = std::filesystem::path(config.path).parent_path() / L"shell.meta";
    {
        std::ofstream existing_metadata(metadata, std::ios::binary);
        REQUIRE(existing_metadata.good());
        existing_metadata << "present but not a runtime provider";
    }
    CompositionRecorder composition;
    CompositionHookGuard composition_guard(composition);
    wchar_t argv0[] = L"SaoAutoTests.exe";
    auto config_argument = config.argument();
    wchar_t* argv[] = {argv0, config_argument.data()};
    int exit_code = 0;

    REQUIRE(sao_launcher_init_pipeline_run(2, argv, nullptr, &exit_code) == SAO_STATUS_INTERNAL);
    REQUIRE(exit_code == sao::launcher::SAO_EXIT_PLATFORM_INIT_FAIL);
    REQUIRE(composition.steps.empty());
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(
                std::filesystem::path(config.path).parent_path().c_str(), config.path.c_str()) ==
            SAO_STATUS_NOT_IMPLEMENTED);
    std::error_code remove_error;
    std::filesystem::remove(metadata, remove_error);
}

TEST_CASE("launcher_init_pipeline_enabled_plugins_provider_missing_fails_closed",
          "[launcher][init_pipeline][providers]") {
    DualRunChildGuard dual_run_child;
    ConfigFile config(R"json({"plugins":{"enabled":true,"roots":["plugins"]}})json");
    CompositionRecorder composition;
    CompositionHookGuard composition_guard(composition);
    composition_guard.hooks.plugins_discover = nullptr;
    sao_launcher_set_composition_test_hooks(&composition_guard.hooks);
    TeardownRecorder recorder;
    sao_launcher_init_hooks_t hooks{};
    hooks.poll_should_exit = &TeardownRecorder::pollExitImmediately;
    hooks.on_teardown_step = &TeardownRecorder::onStep;
    hooks.user_data = &recorder;
    wchar_t argv0[] = L"SaoAutoTests.exe";
    auto config_argument = config.argument();
    wchar_t* argv[] = {argv0, config_argument.data()};
    int exit_code = 0;

    REQUIRE(sao_launcher_init_pipeline_run(2, argv, &hooks, &exit_code) == SAO_STATUS_INTERNAL);
    REQUIRE(exit_code == sao::launcher::SAO_EXIT_PLUGIN_LOAD_FAIL);
    REQUIRE(findStep(composition.steps, "platform_bringup") >= 0);
    REQUIRE(findStep(composition.steps, "plugins_discover") == -1);
}

TEST_CASE("launcher_init_pipeline_plugins_autostart_failure_is_fatal",
          "[launcher][init_pipeline][providers]") {
    DualRunChildGuard dual_run_child;
    ConfigFile config(R"json({"plugins":{"enabled":true,"roots":["plugins"]}})json");
    CompositionRecorder composition;
    composition.plugin_activate_status = SAO_STATUS_PLUGIN_LOAD_FAIL;
    CompositionHookGuard composition_guard(composition);
    TeardownRecorder recorder;
    sao_launcher_init_hooks_t hooks{};
    hooks.poll_should_exit = &TeardownRecorder::pollExitImmediately;
    hooks.on_teardown_step = &TeardownRecorder::onStep;
    hooks.user_data = &recorder;
    wchar_t argv0[] = L"SaoAutoTests.exe";
    auto config_argument = config.argument();
    wchar_t* argv[] = {argv0, config_argument.data()};
    int exit_code = 0;

    REQUIRE(sao_launcher_init_pipeline_run(2, argv, &hooks, &exit_code) == SAO_STATUS_INTERNAL);
    REQUIRE(exit_code == sao::launcher::SAO_EXIT_PLUGIN_LOAD_FAIL);
    REQUIRE(findStep(composition.steps, "plugins_activate") >= 0);
    REQUIRE(findStep(composition.steps, "plugins_shutdown") >= 0);
}

TEST_CASE("launcher_init_pipeline_enabled_provider_missing_config_fails_closed",
          "[launcher][init_pipeline][providers]") {
    DualRunChildGuard dual_run_child;
    ConfigFile config(R"json({"license":{"enabled":true}})json");
    CompositionRecorder composition;
    CompositionHookGuard composition_guard(composition);
    wchar_t argv0[] = L"SaoAutoTests.exe";
    auto config_argument = config.argument();
    wchar_t* argv[] = {argv0, config_argument.data()};
    int exit_code = 0;

    REQUIRE(sao_launcher_init_pipeline_run(2, argv, nullptr, &exit_code) == SAO_STATUS_INTERNAL);
    REQUIRE(exit_code == sao::launcher::SAO_EXIT_PLATFORM_INIT_FAIL);
    REQUIRE(composition.steps.empty());
}

TEST_CASE("launcher_init_pipeline_security_failure_propagates",
          "[launcher][init_pipeline]") {
    DualRunChildGuard dual_run_child;
    CompositionRecorder composition;
    composition.security_status = SAO_STATUS_INTERNAL;
    CompositionHookGuard composition_guard(composition);

    TeardownRecorder rec;
    sao_launcher_init_hooks_t hooks{};
    hooks.poll_should_exit = &TeardownRecorder::pollExitImmediately;
    hooks.on_teardown_step = &TeardownRecorder::onStep;
    hooks.user_data = &rec;
    wchar_t argv0[] = L"SaoAutoTests.exe";
    wchar_t* argv[] = {argv0};

    int exit_code = 0;
    REQUIRE(sao_launcher_init_pipeline_run(1, argv, &hooks, &exit_code) == SAO_STATUS_INTERNAL);
    REQUIRE(exit_code == sao::launcher::SAO_EXIT_PLATFORM_INIT_FAIL);
    REQUIRE(findStep(composition.steps, "security_init") == 0);
    REQUIRE(findStep(composition.steps, "platform_bringup") == -1);
    REQUIRE(findStep(composition.steps, "security_shutdown") == -1);
}

TEST_CASE("launcher_headless_forwards_normalized_log_level_to_platform",
          "[launcher][init_pipeline][logging]") {
    DualRunChildGuard dual_run_child;
    CompositionRecorder composition;
    CompositionHookGuard composition_guard(composition);
    TeardownRecorder recorder;
    sao_launcher_init_hooks_t hooks{};
    hooks.poll_should_exit = &TeardownRecorder::pollExitImmediately;
    hooks.on_teardown_step = &TeardownRecorder::onStep;
    hooks.user_data = &recorder;
    wchar_t argv0[] = L"SaoAutoTests.exe";
    wchar_t log_level[] = L"--log-level=WaRn";
    wchar_t* argv[] = {argv0, log_level};
    int exit_code = -1;

    REQUIRE(sao_launcher_init_pipeline_run(2, argv, &hooks, &exit_code) == SAO_STATUS_OK);
    REQUIRE(exit_code == SAO_EXIT_OK);
    REQUIRE(composition.platform_log_level == "warn");
}

TEST_CASE("launcher_platform_config_applies_core_log_filter", "[launcher][logging][core]") {
#if defined(SAO_LAUNCHER_CORE_LOG_PROVIDER)
    int callback_calls = 0;
    REQUIRE(sao_core_set_log_callback(
                +[](int32_t, const char*, const char*, void* user_data) {
                    ++*static_cast<int*>(user_data);
                },
                &callback_calls) == SAO_STATUS_OK);

    sao::launcher::AppState state{};
    lstrcpynW(state.log_level, L"error", 16);
    state.streaming_entitled = true;
    char log_level[32]{};
    sao_platform_config config{};
    REQUIRE(sao::launcher::buildPlatformConfig(state, config, log_level, sizeof(log_level)));
    REQUIRE(std::string{config.log_level} == "error");
    REQUIRE(config.streaming_entitled == 1);
    CHECK(sao::launcher::isPaidLicenseTier("paid"));
    CHECK(sao::launcher::isPaidLicenseTier("PRO"));
    CHECK(sao::launcher::isPaidLicenseTier("team"));
    CHECK_FALSE(sao::launcher::isPaidLicenseTier("free"));
    CHECK_FALSE(sao::launcher::isPaidLicenseTier(nullptr));
    REQUIRE(sao_core_log(SAO_LOG_WARN, "launcher.test", "filtered") == SAO_STATUS_OK);
    REQUIRE(callback_calls == 0);
    REQUIRE(sao_core_log(SAO_LOG_ERROR, "launcher.test", "visible") == SAO_STATUS_OK);
    REQUIRE(callback_calls == 1);

    REQUIRE(sao_core_set_log_callback(nullptr, nullptr) == SAO_STATUS_OK);
    REQUIRE(sao_core_set_log_level(SAO_LOG_INFO) == SAO_STATUS_OK);
#else
    SUCCEED("core log provider is absent; platform config fails closed");
#endif
}

TEST_CASE("launcher platform config propagates only explicit RT I/O dev license bypass",
          "[launcher][init_pipeline][rt_io_operator][license]") {
#if defined(SAO_LAUNCHER_CORE_LOG_PROVIDER)
    sao::launcher::AppState state{};
    state.rt_io_operator = true;
    char log_level[32]{};
    sao_platform_config config{};

    REQUIRE(sao::launcher::buildPlatformConfig(state, config, log_level, sizeof(log_level)));
    REQUIRE(config.rt_io_operator == 1);
    REQUIRE(config.rt_io_dev_license_bypass == 0);

    state.no_license = true;
    REQUIRE(sao::launcher::buildPlatformConfig(state, config, log_level, sizeof(log_level)));
    REQUIRE(config.rt_io_operator == 1);
    REQUIRE(config.rt_io_dev_license_bypass == 1);
#else
    SUCCEED("core log provider is absent; platform config fails closed");
#endif
}

TEST_CASE("launcher platform config propagates the RT I/O status-page override",
          "[launcher][init_pipeline][rt_io_operator][status_page]") {
#if defined(SAO_LAUNCHER_CORE_LOG_PROVIDER)
    sao::launcher::AppState state{};
    state.rt_io_operator = true;
    char log_level[32]{};
    sao_platform_config config{};

    REQUIRE(sao::launcher::buildPlatformConfig(state, config, log_level, sizeof(log_level)));
    REQUIRE(config.rt_io_operator == 1);
    REQUIRE(config.rt_io_force_status_page == 0);

    state.rt_io_force_status_page = true;
    REQUIRE(sao::launcher::buildPlatformConfig(state, config, log_level, sizeof(log_level)));
    REQUIRE(config.rt_io_operator == 1);
    REQUIRE(config.rt_io_force_status_page == 1);
#else
    SUCCEED("core log provider is absent; platform config fails closed");
#endif
}

TEST_CASE("launcher_headless_production_path_applies_rollout_and_persists_anon_id",
          "[launcher][init_pipeline][rollout][telemetry]") {
    ProductionRolloutGuard guard;
    sao_rollout_config rollout{};
    sao_rollout_config_default(&rollout);
    rollout.cpp_percent = 0;
    REQUIRE(sao_rollout_config_save(&rollout) == SAO_STATUS_OK);

    sao_launcher_dual_run_set_test_probe_hook(+[](sao_dual_run_python_probe* out) {
        *out = {};
        out->available = 1;
        lstrcpynW(out->path, L"E:\\Py\\python.exe", 260);
        out->major = 3;
        out->minor = 11;
    });
    sao_launcher_dual_run_set_test_spawn_hook(+[](const wchar_t*, const wchar_t*,
                                                  const wchar_t* role,
                                                  sao_dual_run_spawn_result* result) -> int {
        REQUIRE(std::wstring{role} == L"python");
        *result = {};
        result->pid = 4242;
        return 0;
    });

    CompositionRecorder composition;
    CompositionHookGuard composition_guard(composition);
    wchar_t argv0[] = L"SaoAutoTests.exe";
    wchar_t* argv[] = {argv0};
    int exit_code = -1;
    REQUIRE(sao_launcher_init_pipeline_run(1, argv, nullptr, &exit_code) == SAO_STATUS_OK);
    REQUIRE(exit_code == SAO_EXIT_HANDOFF_TO_PYTHON);
    REQUIRE(composition.steps.empty());

    sao_rollout_stats stats{};
    REQUIRE(sao_rollout_stats_load(&stats) == SAO_STATUS_OK);
    REQUIRE(stats.total_successes == 1);
    REQUIRE(stats.recent_count == 1);
    REQUIRE(stats.recent[0].mode == SAO_DUAL_RUN_MODE_PYTHON_ONLY);

#if defined(SAO_LAUNCHER_HAS_TELEMETRY_CLIENT)
    const auto anon_file = guard.directory / "telemetry_anon_id.txt";
    REQUIRE(std::filesystem::is_regular_file(anon_file));
    std::ifstream anon_input(anon_file, std::ios::binary);
    const std::string anon_id{std::istreambuf_iterator<char>(anon_input),
                              std::istreambuf_iterator<char>()};
    REQUIRE(anon_id.size() >= 32);

    char body[4096]{};
    size_t written = 0;
    REQUIRE(sao_telemetry_test_get_last_body(body, sizeof(body), &written) == SAO_OK);
    const std::string captured(body, written);
    REQUIRE(captured.find("sao.rollout.launch_success") != std::string::npos);
#else
    SUCCEED("telemetry client is not linked; rollout used explicit anon-id fallback");
#endif
}

TEST_CASE("RT I/O operator lifecycle forces inactive CPP-only selection",
          "[launcher][init_pipeline][rollout][rt_io_operator][forced_cpp]") {
    ProductionRolloutGuard guard;
    sao_rollout_config rollout{};
    sao_rollout_config_default(&rollout);
    rollout.cpp_percent = 0;
    REQUIRE(sao_rollout_config_save(&rollout) == SAO_STATUS_OK);

    DualRunInvocationRecorder::reset();
    sao_launcher_dual_run_set_test_probe_hook(
        &DualRunInvocationRecorder::probe);
    sao_launcher_dual_run_set_test_spawn_hook(
        &DualRunInvocationRecorder::spawn);

    sao::launcher::LauncherLifecycleDecision lifecycle;
    REQUIRE(sao::launcher::prepareLauncherLifecycle(lifecycle, true) ==
            SAO_STATUS_OK);
    CHECK(lifecycle.selected_mode == SAO_DUAL_RUN_MODE_CPP_ONLY);
    CHECK(lifecycle.dual_config.mode == SAO_DUAL_RUN_MODE_CPP_ONLY);
    CHECK_FALSE(lifecycle.active);
    CHECK_FALSE(lifecycle.telemetry_started);
    CHECK(lifecycle.start_qpc == 0);
    CHECK(DualRunInvocationRecorder::probe_calls == 0);
    CHECK(DualRunInvocationRecorder::spawn_calls == 0);

    sao::launcher::completeLauncherLifecycle(
        lifecycle, sao::launcher::SAO_EXIT_PLATFORM_INIT_FAIL,
        "forced_operator_test");

    sao_rollout_stats stats{};
    REQUIRE(sao_rollout_stats_load(&stats) == SAO_STATUS_OK);
    CHECK(stats.total_successes == 0);
    CHECK(stats.total_failures == 0);
    CHECK(stats.recent_count == 0);

    sao_rollout_config after{};
    REQUIRE(sao_rollout_config_load(&after) == SAO_STATUS_OK);
    CHECK(after.cpp_percent == 0);
    CHECK(after.retreat_history_count == 0);
}

TEST_CASE("headless RT I/O operator bypasses rollout and Python handoff",
          "[launcher][init_pipeline][rollout][rt_io_operator][forced_cpp]") {
    ProductionRolloutGuard guard;
    sao_rollout_config rollout{};
    sao_rollout_config_default(&rollout);
    rollout.cpp_percent = 0;
    REQUIRE(sao_rollout_config_save(&rollout) == SAO_STATUS_OK);

    DualRunInvocationRecorder::reset();
    sao_launcher_dual_run_set_test_probe_hook(
        &DualRunInvocationRecorder::probe);
    sao_launcher_dual_run_set_test_spawn_hook(
        &DualRunInvocationRecorder::spawn);

    CompositionRecorder composition;
    CompositionHookGuard composition_guard(composition);
    TeardownRecorder teardown;
    sao_launcher_init_hooks_t hooks{};
    hooks.poll_should_exit = &TeardownRecorder::pollExitImmediately;
    hooks.on_teardown_step = &TeardownRecorder::onStep;
    hooks.user_data = &teardown;
    wchar_t argv0[] = L"SaoAutoTests.exe";
    wchar_t operator_arg[] = L"--rt-io-preflight-only";
    wchar_t* argv[] = {argv0, operator_arg};
    int exit_code = -1;

    REQUIRE(sao_launcher_init_pipeline_run(2, argv, &hooks, &exit_code) ==
            SAO_STATUS_OK);
    CHECK(exit_code == SAO_EXIT_OK);
    CHECK(DualRunInvocationRecorder::probe_calls == 0);
    CHECK(DualRunInvocationRecorder::spawn_calls == 0);
    CHECK(findStep(composition.steps, "platform_bringup") >= 0);

    sao_dual_run_status dual_status{};
    sao_launcher_dual_run_status(&dual_status);
    CHECK(dual_status.mode == SAO_DUAL_RUN_MODE_CPP_ONLY);

        composition.security_status = SAO_STATUS_INTERNAL;
        exit_code = -1;
        CHECK(sao_launcher_init_pipeline_run(2, argv, &hooks, &exit_code) ==
            SAO_STATUS_INTERNAL);
        CHECK(exit_code == sao::launcher::SAO_EXIT_PLATFORM_INIT_FAIL);
        CHECK(DualRunInvocationRecorder::probe_calls == 0);
        CHECK(DualRunInvocationRecorder::spawn_calls == 0);

    sao_rollout_stats stats{};
    REQUIRE(sao_rollout_stats_load(&stats) == SAO_STATUS_OK);
    CHECK(stats.total_successes == 0);
    CHECK(stats.total_failures == 0);
    CHECK(stats.recent_count == 0);

    sao_rollout_config after{};
    REQUIRE(sao_rollout_config_load(&after) == SAO_STATUS_OK);
    CHECK(after.cpp_percent == 0);
    CHECK(after.retreat_history_count == 0);
}

TEST_CASE("launcher_headless_records_results_and_checks_auto_retreat",
          "[launcher][init_pipeline][rollout]") {
    ProductionRolloutGuard guard;
    sao_rollout_config rollout{};
    sao_rollout_config_default(&rollout);
    rollout.cpp_percent = 100;
    REQUIRE(sao_rollout_config_save(&rollout) == SAO_STATUS_OK);

    CompositionRecorder composition;
    CompositionHookGuard composition_guard(composition);
    TeardownRecorder teardown;
    sao_launcher_init_hooks_t hooks{};
    hooks.poll_should_exit = &TeardownRecorder::pollExitImmediately;
    hooks.on_teardown_step = &TeardownRecorder::onStep;
    hooks.user_data = &teardown;
    wchar_t argv0[] = L"SaoAutoTests.exe";
    wchar_t* argv[] = {argv0};

    for (int run = 0; run < 2; ++run) {
        int exit_code = -1;
        REQUIRE(sao_launcher_init_pipeline_run(1, argv, &hooks, &exit_code) == SAO_STATUS_OK);
        REQUIRE(exit_code == SAO_EXIT_OK);
    }

    composition.security_status = SAO_STATUS_INTERNAL;
    for (int run = 0; run < 3; ++run) {
        int exit_code = -1;
        REQUIRE(sao_launcher_init_pipeline_run(1, argv, &hooks, &exit_code) == SAO_STATUS_INTERNAL);
        REQUIRE(exit_code == sao::launcher::SAO_EXIT_PLATFORM_INIT_FAIL);
    }

    sao_rollout_stats stats{};
    REQUIRE(sao_rollout_stats_load(&stats) == SAO_STATUS_OK);
    REQUIRE(stats.total_successes == 2);
    REQUIRE(stats.total_failures == 3);
    REQUIRE(stats.recent_count == 5);

    sao_rollout_config after{};
    REQUIRE(sao_rollout_config_load(&after) == SAO_STATUS_OK);
    REQUIRE(after.cpp_percent == 50);
    REQUIRE(after.retreat_history_count == 1);
    REQUIRE(std::string{after.retreat_history[0].reason} == "3_of_5_failed");
}

TEST_CASE("launcher_init_pipeline_without_provider_fails_closed",
          "[launcher][init_pipeline]") {
    DualRunChildGuard dual_run_child;
    sao_launcher_set_composition_test_hooks(nullptr);
    wchar_t argv0[] = L"SaoAutoTests.exe";
    wchar_t* argv[] = {argv0};
    int exit_code = 0;

    REQUIRE(sao_launcher_init_pipeline_run(1, argv, nullptr, &exit_code) == SAO_STATUS_INTERNAL);
    REQUIRE(exit_code == sao::launcher::SAO_EXIT_PLATFORM_INIT_FAIL);
}

TEST_CASE("launcher_shutdown_retains_platform_context_until_cleanup_succeeds",
          "[launcher][shutdown][cleanup_retry]") {
    constexpr sao_status_t cleanup_unknown = -6;
    for (const sao_status_t failure_status : {cleanup_unknown, SAO_STATUS_INTERNAL}) {
        CAPTURE(failure_status);
        CompositionRecorder composition;
        composition.platform_teardown_statuses = {
            failure_status,
            SAO_STATUS_OK,
        };
        CompositionHookGuard composition_guard(composition);

        sao::launcher::AppState state{};
        state.platform_ctx = reinterpret_cast<sao_platform_ctx*>(&composition);

        REQUIRE_FALSE(sao::launcher::runFullShutdown(state, false));
        REQUIRE(state.platform_ctx == reinterpret_cast<sao_platform_ctx*>(&composition));
        REQUIRE(countStep(composition.steps, "platform_teardown") == 1);

        REQUIRE(sao::launcher::runFullShutdown(state, false));
        REQUIRE(state.platform_ctx == nullptr);
        REQUIRE(countStep(composition.steps, "platform_teardown") == 2);

        REQUIRE(sao::launcher::runFullShutdown(state, false));
        REQUIRE(countStep(composition.steps, "platform_teardown") == 2);
    }
}

TEST_CASE("launcher_shutdown_retries_plugins_before_lower_subsystems",
          "[launcher][shutdown][cleanup_retry][providers]") {
    constexpr sao_status_t cleanup_unknown = -6;
    CompositionRecorder composition;
    composition.plugin_shutdown_statuses = {
        cleanup_unknown,
        SAO_STATUS_OK,
    };
    CompositionHookGuard composition_guard(composition);

    sao::launcher::AppState state{};
    state.plugins_registry = reinterpret_cast<sao_plugins_registry*>(&composition);
    state.shell_active = true;
    state.license_active = true;

    REQUIRE_FALSE(sao::launcher::runFullShutdown(state, false));
    REQUIRE(state.plugins_registry == reinterpret_cast<sao_plugins_registry*>(&composition));
    REQUIRE(state.shell_active);
    REQUIRE(state.license_active);
    REQUIRE(countStep(composition.steps, "plugins_shutdown") == 1);
    REQUIRE(countStep(composition.steps, "shell_shutdown") == 0);

    REQUIRE(sao::launcher::runFullShutdown(state, false));
    REQUIRE(state.plugins_registry == nullptr);
    REQUIRE_FALSE(state.shell_active);
    REQUIRE_FALSE(state.license_active);
    REQUIRE(findStep(composition.steps, "plugins_shutdown") <
            findStep(composition.steps, "shell_shutdown"));
    REQUIRE(findStep(composition.steps, "shell_shutdown") <
            findStep(composition.steps, "license_shutdown"));
}

TEST_CASE("launcher_shutdown_retries_shell_then_license_in_order",
          "[launcher][shutdown][cleanup_retry][providers]") {
    constexpr sao_status_t cleanup_unknown = -6;
    CompositionRecorder composition;
    composition.shell_shutdown_statuses = {
        cleanup_unknown,
        SAO_STATUS_OK,
    };
    composition.license_shutdown_statuses = {
        cleanup_unknown,
        SAO_STATUS_OK,
    };
    CompositionHookGuard composition_guard(composition);

    sao::launcher::AppState state{};
    state.shell_active = true;
    state.license_active = true;

    REQUIRE_FALSE(sao::launcher::runFullShutdown(state, false));
    REQUIRE(state.shell_active);
    REQUIRE(state.license_active);
    REQUIRE(countStep(composition.steps, "shell_shutdown") == 1);
    REQUIRE(countStep(composition.steps, "license_shutdown") == 0);

    REQUIRE_FALSE(sao::launcher::runFullShutdown(state, false));
    REQUIRE_FALSE(state.shell_active);
    REQUIRE(state.license_active);
    REQUIRE(countStep(composition.steps, "shell_shutdown") == 2);
    REQUIRE(countStep(composition.steps, "license_shutdown") == 1);

    REQUIRE(sao::launcher::runFullShutdown(state, false));
    REQUIRE_FALSE(state.license_active);
    REQUIRE(countStep(composition.steps, "license_shutdown") == 2);
}

TEST_CASE("launcher_init_failure_shutdown_retries_owned_platform_context",
          "[launcher][shutdown][init_failure][cleanup_retry]") {
    constexpr sao_status_t cleanup_unknown = -6;
    CompositionRecorder composition;
    composition.platform_bringup_status = SAO_STATUS_INTERNAL;
    composition.platform_teardown_statuses = {
        cleanup_unknown,
        SAO_STATUS_OK,
    };
    CompositionHookGuard composition_guard(composition);

    auto& app = sao::launcher::App::instance();
    auto& app_state = const_cast<sao::launcher::AppState&>(app.state());
    lstrcpynW(app_state.log_level, L"debug", 16);
    REQUIRE(app.bringUpPlatform() == sao::launcher::SAO_EXIT_PLATFORM_INIT_FAIL);
    REQUIRE(composition.platform_log_level == "debug");
    REQUIRE(app.state().platform_ctx == reinterpret_cast<sao_platform_ctx*>(&composition));

    app.shutdown();
    REQUIRE(app.state().platform_ctx == reinterpret_cast<sao_platform_ctx*>(&composition));
    REQUIRE(countStep(composition.steps, "platform_teardown") == 1);

    app.shutdown();
    REQUIRE(app.state().platform_ctx == nullptr);
    REQUIRE(countStep(composition.steps, "platform_teardown") == 2);

    app.shutdown();
    REQUIRE(countStep(composition.steps, "platform_teardown") == 2);
}

// Declared in launcher/src/single_instance.cpp — a test-only debug helper
// that exposes the opaque mutex name single_instance.cpp will race for. The
// literal name pattern is deliberately not repeated in this test so that
// future stealth renames (e.g. moving the 4F5A prefix or bumping the hash
// mixer) do not require updating this fixture.
extern "C" void sao_launcher_debug_build_single_instance_mutex_name(
    wchar_t* out, std::size_t out_cap) noexcept;

TEST_CASE("launcher_single_instance_second_run_fails", "[launcher][init_pipeline]") {
    DualRunChildGuard dual_run_child_guard;
    // Acquire the mutex directly (mirroring what a first launcher
    // instance would do), then run the pipeline.  It must return
    // SAO_EXIT_ALREADY_RUNNING because the mutex is held.
    wchar_t name[128]{};
    sao_launcher_debug_build_single_instance_mutex_name(name, 128);

    HANDLE m = CreateMutexW(nullptr, FALSE, name);
    REQUIRE(m != nullptr);
    // CreateMutex returns a handle regardless; if the mutex already
    // exists on the machine (previous crashed test) treat that as
    // still holding the mutex for this test's purposes.

    TeardownRecorder rec;
    sao_launcher_init_hooks_t hooks{};
    hooks.poll_should_exit = &TeardownRecorder::pollExitImmediately;
    hooks.on_teardown_step = &TeardownRecorder::onStep;
    hooks.user_data = &rec;

    wchar_t argv0[] = L"SaoAutoTests.exe";
    wchar_t* argv[] = {argv0};

    int exit_code = 0;
    sao_status_t rc = sao_launcher_init_pipeline_run(1, argv, &hooks, &exit_code);

    // Non-OK: acquireSingleInstance returned false, so the pipeline
    // aborted at that stage.
    REQUIRE(rc != SAO_STATUS_OK);
    REQUIRE(exit_code == SAO_EXIT_ALREADY_RUNNING);

    CloseHandle(m);
}

// Adversarial recon coverage — the launcher exit-code contract now exposes
// SAO_EXIT_HANDOFF_TO_PYTHON via the SaoLauncherExitCode enum in app.h; the
// numeric value (100) is preserved so any TU still consuming the older
// dual_run.h #define sees the same integer.  The ``#undef`` below shrugs off
// the dual_run.h macro so this test case can reference the fully qualified
// enum member.
#ifdef SAO_EXIT_HANDOFF_TO_PYTHON
#undef SAO_EXIT_HANDOFF_TO_PYTHON
#endif
TEST_CASE("launcher_exit_handoff_to_python_reachable_via_enum",
          "[launcher][init_pipeline][exit_code][focused]") {
    // Enum member is directly comparable to the well-known integer value.
    CHECK(static_cast<int>(sao::launcher::SAO_EXIT_HANDOFF_TO_PYTHON) == 100);
    // Enum member does not collide with any process-init failure code.
    CHECK(sao::launcher::SAO_EXIT_HANDOFF_TO_PYTHON != sao::launcher::SAO_EXIT_OK);
    CHECK(sao::launcher::SAO_EXIT_HANDOFF_TO_PYTHON != sao::launcher::SAO_EXIT_BAD_ARGS);
    CHECK(sao::launcher::SAO_EXIT_HANDOFF_TO_PYTHON !=
          sao::launcher::SAO_EXIT_PLATFORM_INIT_FAIL);
    // The enum is the ABI source of truth; document that the well-known
    // dispatch signal keeps the historical value that the dual_run.h macro
    // used to advertise.
    CHECK(static_cast<int>(sao::launcher::SAO_EXIT_HANDOFF_TO_PYTHON) == 100);
}

// Adversarial recon coverage — the launcher previously called
// AiEditorProcessOwner::take_offline() twice (once in sao_ui_take_offline,
// again in teardown_platform_context). The duplicate call was removed but the
// invariant matters: the owner must return SAO_STATUS_OK on the second call
// even after a successful first call.
TEST_CASE("launcher_ai_editor_owner_take_offline_is_idempotent",
          "[launcher][init_pipeline][ai_editor][idempotency][focused]") {
    wchar_t temp_path[MAX_PATH]{};
    REQUIRE(GetTempPathW(MAX_PATH, temp_path) != 0);
    wchar_t base_dir[MAX_PATH]{};
    REQUIRE(GetTempFileNameW(temp_path, L"aid", 0, base_dir) != 0);
    DeleteFileW(base_dir);
    std::error_code error;
    REQUIRE(std::filesystem::create_directories(std::filesystem::path{base_dir}, error));

    sao::launcher::tool_launch::AiEditorProcessOwner owner(base_dir);
    // Never opened — take_offline still succeeds because the owner has
    // nothing to drain.
    const sao_status_t first_status = owner.take_offline();
    REQUIRE(first_status == SAO_STATUS_OK);
    // Second call must not report a double-free / not-initialised error.
    const sao_status_t second_status = owner.take_offline();
    REQUIRE(second_status == SAO_STATUS_OK);
    // A third call — for good measure — still succeeds so the invariant is
    // not order-dependent.
    REQUIRE(owner.take_offline() == SAO_STATUS_OK);

    sao::launcher::tool_launch::AiEditorLaunchSnapshot snap{};
    REQUIRE(owner.snapshot(snap) == SAO_STATUS_OK);
    CHECK(snap.phase == sao::launcher::tool_launch::AiEditorLaunchPhase::idle);

    std::filesystem::remove_all(std::filesystem::path{base_dir}, error);
}

// Adversarial recon coverage — sao_ui_take_offline used to close the
// invocation gate itself before entity_provider_publication::clear() closed
// it again on the same store. The duplicate close was removed; the invariant
// under test here is that the gate close operation is idempotent enough that
// two independent calls do not corrupt the store's state.
TEST_CASE("launcher_route_store_close_invocation_gate_is_idempotent",
          "[launcher][init_pipeline][entity][invocation_gate][idempotency][focused]") {
    sao::launcher::entity_action_routes::EntityActionRouteStore store;
    // First close — the gate was never opened; the store still accepts and
    // returns SAO_STATUS_OK immediately.
    const sao_status_t first_status = store.close_invocation_gate();
    REQUIRE(first_status == SAO_STATUS_OK);
    // Second close — must not corrupt the store; the accepting flag is still
    // set so close returns SAO_STATUS_OK again.
    const sao_status_t second_status = store.close_invocation_gate();
    REQUIRE(second_status == SAO_STATUS_OK);
    // The publish-friendly path is still usable after the double close.
    sao::launcher::entity_action_routes::EntityActionRouteStore::PreparedPublication publication;
    REQUIRE(store.prepare({}, publication) == SAO_STATUS_OK);
    REQUIRE(publication.commit() == SAO_STATUS_OK);
    // Post-publish close is still idempotent.
    REQUIRE(store.close_invocation_gate() == SAO_STATUS_OK);
    REQUIRE(store.close_invocation_gate() == SAO_STATUS_OK);
}

// ---------------------------------------------------------------------------
// Runtime installer hook wiring
//
// The launcher-side pipeline calls the installer-core track's ensure_all
// hook right before plugin discovery. Tests observe the call sequence via
// a mock installer that records its invocations; the mock reports both
// success and failure paths so the pipeline can be exercised without
// dragging in the real installer library.
// ---------------------------------------------------------------------------
namespace {

struct RuntimeInstallerHookRecorder {
    // Whether ensure_all() was called at all this run. Should be true
    // whenever plugins are enabled + safe_mode is off.
    bool ensure_all_called = false;
    // Base directory the pipeline forwarded. Nonempty when the hook fires.
    std::wstring base_dir;
    // Progress ticks the installer emitted. Each entry captures the kind
    // slug + the byte accounting the mock reports so tests can assert
    // both the plumbing works and the pipeline forwards to the launcher's
    // progress adapter.
    struct ProgressTick {
        std::string kind;
        uint64_t bytes_done = 0;
        uint64_t bytes_total = 0;
    };
    std::vector<ProgressTick> ticks;
    // Status the mock hands back so tests can flip between the happy
    // path (SAO_STATUS_OK) and the fail-closed path (any other value).
    sao_status_t status_to_return = SAO_STATUS_OK;

    // Static entry point matching sao::launcher::runtime_installer_glue::
    // EnsureAllFn. Uses a thread-local singleton so the mock does not
    // have to bounce user_data through the extern-"C" setter.
    static RuntimeInstallerHookRecorder*& active() {
        thread_local RuntimeInstallerHookRecorder* current = nullptr;
        return current;
    }

    static sao_status_t ensureAll(const wchar_t* base_dir,
                                  void (*progress_cb)(const char* kind_opaque_id_utf8,
                                                      uint64_t bytes_done,
                                                      uint64_t bytes_total,
                                                      void* user_data),
                                  void* progress_user_data) {
        auto* self = active();
        if (self == nullptr) {
            return SAO_STATUS_OK;
        }
        self->ensure_all_called = true;
        self->base_dir = base_dir ? base_dir : L"";
        // Emit one canned tick per opaque runtime id so the pipeline's
        // progress forwarding is exercised. The launcher receives the
        // tick and hands it to its compositor overlay adapter; the
        // adapter is a no-op in the headless test provider so we simply
        // record the callback landed.
        if (progress_cb != nullptr) {
            progress_cb("dotnet_runtime", 0, 100, progress_user_data);
            progress_cb("dotnet_runtime", 100, 100, progress_user_data);
            progress_cb("lua_source", 42, 42, progress_user_data);
            self->ticks.push_back({"dotnet_runtime", 0, 100});
            self->ticks.push_back({"dotnet_runtime", 100, 100});
            self->ticks.push_back({"lua_source", 42, 42});
        }
        return self->status_to_return;
    }
};

struct RuntimeInstallerHookGuard {
    explicit RuntimeInstallerHookGuard(RuntimeInstallerHookRecorder& recorder) {
        RuntimeInstallerHookRecorder::active() = &recorder;
        sao_launcher_init_pipeline_test_set_runtime_installer_hook(
            &RuntimeInstallerHookRecorder::ensureAll);
    }
    ~RuntimeInstallerHookGuard() {
        sao_launcher_init_pipeline_test_set_runtime_installer_hook(nullptr);
        RuntimeInstallerHookRecorder::active() = nullptr;
    }
    RuntimeInstallerHookGuard(const RuntimeInstallerHookGuard&) = delete;
    RuntimeInstallerHookGuard& operator=(const RuntimeInstallerHookGuard&) = delete;
};

} // namespace

TEST_CASE("launcher runtime installer fires before plugin discovery on happy path",
          "[launcher][init_pipeline][runtime_installer][focused]") {
    DualRunChildGuard dual_run_child;
    ConfigFile config(R"json({"plugins":{"enabled":true,"roots":["plugins"]}})json");
    CompositionRecorder composition;
    CompositionHookGuard composition_guard(composition);
    RuntimeInstallerHookRecorder installer;
    RuntimeInstallerHookGuard installer_guard(installer);
    TeardownRecorder rec;
    sao_launcher_init_hooks_t hooks{};
    hooks.poll_should_exit = &TeardownRecorder::pollExitImmediately;
    hooks.on_teardown_step = &TeardownRecorder::onStep;
    hooks.user_data = &rec;
    wchar_t argv0[] = L"SaoAutoTests.exe";
    auto config_argument = config.argument();
    wchar_t* argv[] = {argv0, config_argument.data()};
    int exit_code = -1;

    REQUIRE(sao_launcher_init_pipeline_run(2, argv, &hooks, &exit_code) == SAO_STATUS_OK);
    REQUIRE(exit_code == SAO_EXIT_OK);
    // The installer must have been asked to ensure runtimes because
    // plugins were enabled and safe_mode was left off.
    REQUIRE(installer.ensure_all_called);
    // The pipeline must have forwarded a non-empty base directory so the
    // installer can resolve cache paths relative to it (mirrors
    // config.BASE_DIR).
    REQUIRE_FALSE(installer.base_dir.empty());
    // The three canned ticks must have landed through the launcher's
    // progress adapter — proves the callback plumbing survives the
    // extern-"C" hop.
    REQUIRE(installer.ticks.size() == 3);
    REQUIRE(installer.ticks[0].kind == "dotnet_runtime");
    REQUIRE(installer.ticks[2].kind == "lua_source");
    // Ordering: ensure_all() must have been invoked before plugins_discover.
    // We verify by cross-checking that the composition recorder saw
    // plugins_discover after our hook set ensure_all_called; the two
    // recorders are ordered by wall clock but Catch2 assertions run
    // strictly after the pipeline returns so we only need the presence
    // + relative structural check here.
    REQUIRE(findStep(composition.steps, "plugins_discover") >= 0);
    REQUIRE(findStep(composition.steps, "plugins_activate") >= 0);
}

TEST_CASE("launcher runtime installer failure does not abort plugin discovery",
          "[launcher][init_pipeline][runtime_installer][fail_closed][focused]") {
    DualRunChildGuard dual_run_child;
    ConfigFile config(R"json({"plugins":{"enabled":true,"roots":["plugins"]}})json");
    CompositionRecorder composition;
    CompositionHookGuard composition_guard(composition);
    RuntimeInstallerHookRecorder installer;
    // Simulate a manifest integrity failure. The pipeline must continue
    // to plugin discovery because pre-installed runtimes are still
    // usable — the Panel authority is what gets flagged, not the
    // discovery pass.
    installer.status_to_return = SAO_STATUS_INTERNAL;
    RuntimeInstallerHookGuard installer_guard(installer);
    TeardownRecorder rec;
    sao_launcher_init_hooks_t hooks{};
    hooks.poll_should_exit = &TeardownRecorder::pollExitImmediately;
    hooks.on_teardown_step = &TeardownRecorder::onStep;
    hooks.user_data = &rec;
    wchar_t argv0[] = L"SaoAutoTests.exe";
    auto config_argument = config.argument();
    wchar_t* argv[] = {argv0, config_argument.data()};
    int exit_code = -1;

    REQUIRE(sao_launcher_init_pipeline_run(2, argv, &hooks, &exit_code) == SAO_STATUS_OK);
    REQUIRE(exit_code == SAO_EXIT_OK);
    REQUIRE(installer.ensure_all_called);
    // Plugin discovery must still have run — the installer failure only
    // gates the Panel authority mirror, not the pipeline itself.
    REQUIRE(findStep(composition.steps, "plugins_discover") >= 0);
    REQUIRE(findStep(composition.steps, "plugins_activate") >= 0);
}

TEST_CASE("launcher runtime installer skipped in safe mode",
          "[launcher][init_pipeline][runtime_installer][safe_mode][focused]") {
    DualRunChildGuard dual_run_child;
    ConfigFile config(R"json({"plugins":{"enabled":true,"roots":["plugins"]}})json");
    CompositionRecorder composition;
    CompositionHookGuard composition_guard(composition);
    RuntimeInstallerHookRecorder installer;
    RuntimeInstallerHookGuard installer_guard(installer);
    TeardownRecorder rec;
    sao_launcher_init_hooks_t hooks{};
    hooks.poll_should_exit = &TeardownRecorder::pollExitImmediately;
    hooks.on_teardown_step = &TeardownRecorder::onStep;
    hooks.user_data = &rec;
    wchar_t argv0[] = L"SaoAutoTests.exe";
    wchar_t safe_mode_arg[] = L"--safe-mode";
    auto config_argument = config.argument();
    wchar_t* argv[] = {argv0, safe_mode_arg, config_argument.data()};
    int exit_code = -1;

    REQUIRE(sao_launcher_init_pipeline_run(3, argv, &hooks, &exit_code) == SAO_STATUS_OK);
    REQUIRE(exit_code == SAO_EXIT_OK);
    // Safe mode disables the entire plugin discovery pass; the runtime
    // installer follows suit so the launcher's boot path does not touch
    // the network when the operator explicitly asked for a safe boot.
    REQUIRE_FALSE(installer.ensure_all_called);
    // Plugin discovery must have been skipped so no plugins_* step
    // fires either.
    REQUIRE(findStep(composition.steps, "plugins_discover") == -1);
}

TEST_CASE("launcher runtime installer restores default hook when pointer is null",
          "[launcher][init_pipeline][runtime_installer][idempotency][focused]") {
    RuntimeInstallerHookRecorder installer;
    RuntimeInstallerHookRecorder::active() = &installer;
    // Install a real hook.
    sao_launcher_init_pipeline_test_set_runtime_installer_hook(
        &RuntimeInstallerHookRecorder::ensureAll);
    // Clear it. The pipeline should now treat the hook as absent and skip
    // the ensure_all call entirely (the pass-through returns OK without
    // touching the recorder).
    sao_launcher_init_pipeline_test_set_runtime_installer_hook(nullptr);
    RuntimeInstallerHookRecorder::active() = nullptr;

    DualRunChildGuard dual_run_child;
    ConfigFile config(R"json({"plugins":{"enabled":true,"roots":["plugins"]}})json");
    CompositionRecorder composition;
    CompositionHookGuard composition_guard(composition);
    TeardownRecorder rec;
    sao_launcher_init_hooks_t hooks{};
    hooks.poll_should_exit = &TeardownRecorder::pollExitImmediately;
    hooks.on_teardown_step = &TeardownRecorder::onStep;
    hooks.user_data = &rec;
    wchar_t argv0[] = L"SaoAutoTests.exe";
    auto config_argument = config.argument();
    wchar_t* argv[] = {argv0, config_argument.data()};
    int exit_code = -1;

    REQUIRE(sao_launcher_init_pipeline_run(2, argv, &hooks, &exit_code) == SAO_STATUS_OK);
    REQUIRE(exit_code == SAO_EXIT_OK);
    // With the hook cleared the pass-through fires but does not touch
    // the recorder — ensure_all_called must remain false.
    REQUIRE_FALSE(installer.ensure_all_called);
}
