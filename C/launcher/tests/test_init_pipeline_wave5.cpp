// SAO Auto — launcher/tests/test_init_pipeline_wave5.cpp
//
// Wave 5 / Phase 1 — headless init pipeline coverage.
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

#include "sao/launcher/app.h"
#include "sao/launcher/dual_run.h"
#include "sao/launcher/init_pipeline.h"
#include "sao/launcher/provider_config.h"
#include "sao/launcher/rollout.h"
#include "sao/launcher/shutdown.h"
#include "sao/plugins/loader/loader_status.h"
#if defined(SAO_LAUNCHER_CORE_LOG_PROVIDER)
#undef SAO_STATUS_OK
#include "sao/core/logging.h"
#endif

#if defined(SAO_LAUNCHER_HAS_TELEMETRY_CLIENT)
#include "sao/server/freetier/telemetry_client/telemetry_client.h"
#include "sao_core/sao_status.h"
#endif

// The wave5 exit-code constants live in the sao::launcher namespace
// alongside the App state machine.  Pull them in for terser assertions.
using sao::launcher::SAO_EXIT_ALREADY_RUNNING;
using sao::launcher::SAO_EXIT_OK;

extern "C" sao_status_t sao_launcher_init_pipeline_test_apply_streaming_mode_transaction(
    bool enabled, sao_status_t (*acquire)(void*), bool (*get_flow)(void*),
    bool (*get_capture)(void*), sao_status_t (*set_capture)(bool, void*),
    sao_status_t (*set_flow)(bool, void*), sao_status_t (*release)(void*), void* user_data);

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
        return cfg && cfg->enable_anti_debug && cfg->enable_anti_dump ? self->security_status
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
    std::vector<std::string> steps;

    static sao_status_t acquire(void* user_data) {
        auto* self = static_cast<StreamingModeTransactionRecorder*>(user_data);
        self->steps.emplace_back("acquire");
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
        return SAO_STATUS_INTERNAL;
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
    REQUIRE(GetTempFileNameW(temp_path, L"w18", 0, temp_file) != 0);
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

size_t countStep(const std::vector<std::string>& steps, const char* name) {
    size_t count = 0;
    for (const auto& step : steps) {
        if (step == name)
            ++count;
    }
    return count;
}

} // namespace

TEST_CASE("launcher_init_pipeline_run_no_deps_returns_ok", "[launcher][init_pipeline][wave5]") {
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
    CHECK(recorder.steps == std::vector<std::string>{"acquire", "capture_on", "flow_on",
                                                     "release_failed", "capture_off",
                                                     "flow_off"});
}

TEST_CASE("launcher_init_pipeline_teardown_reverse_order", "[launcher][init_pipeline][wave5]") {
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
          "[launcher][license][trust][w18]") {
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
          "[launcher][init_pipeline][wave5]") {
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
          "[launcher][init_pipeline][logging][w18]") {
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

TEST_CASE("launcher_platform_config_applies_core_log_filter", "[launcher][logging][core][w18]") {
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

TEST_CASE("launcher_headless_production_path_applies_rollout_and_persists_anon_id",
          "[launcher][init_pipeline][rollout][telemetry][w18]") {
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

TEST_CASE("launcher_headless_records_results_and_checks_auto_retreat",
          "[launcher][init_pipeline][rollout][w18]") {
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
          "[launcher][init_pipeline][wave5]") {
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

TEST_CASE("launcher_single_instance_second_run_fails", "[launcher][init_pipeline][wave5]") {
    // Acquire the mutex directly (mirroring what a first launcher
    // instance would do), then run the pipeline.  It must return
    // SAO_EXIT_ALREADY_RUNNING because the mutex is held.
    //
    // We reuse the same fnv1a64/GetModuleFileNameW recipe from
    // single_instance.cpp so the mutex name matches exactly.
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    uint64_t h = 1469598103934665603ULL;
    for (const wchar_t* p = exe; *p; ++p) {
        h ^= static_cast<uint64_t>(*p);
        h *= 1099511628211ULL;
    }
    wchar_t name[128]{};
    _snwprintf_s(name, 128, _TRUNCATE, L"Global\\SaoAuto.Instance.%016llx", h);

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
