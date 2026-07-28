// SAO Auto — launcher/tests/test_rt_io_operator_focused.cpp

#include <catch2/catch_test_macros.hpp>

#include "sao/core/status.h"
#include "sao/launcher/app.h"
#include "sao/launcher/init_pipeline.h"

#include <string>
#include <vector>

namespace sao::launcher::testing {
bool rt_io_operator_preflight_observations_complete_for_test(
    uint32_t is_admin, uint32_t is_elevated, uint32_t load_driver_privilege_present,
    uint32_t load_driver_privilege_enabled, uint32_t hvci_enabled, uint32_t vbs_enabled,
    uint32_t provider_observable) noexcept;
bool rt_io_operator_preflight_failure_state_ready_for_test(int32_t last_failure_code,
                                                           uint32_t last_failure_stage) noexcept;
} // namespace sao::launcher::testing

namespace {

constexpr uint64_t kDefaultRequestedMask =
    ((uint64_t{1} << 9u) - 1u) |
    (uint64_t{1} << 13u) |
    (uint64_t{1} << 14u) |
    (uint64_t{1} << 15u) |
    (uint64_t{1} << 16u);
constexpr uint64_t kAllRequestedMask = (uint64_t{1} << 17u) - 1u;
constexpr int32_t kNotInitializedStatus = SAO_STATUS_ERR_NOT_INITIALIZED;
constexpr uint32_t kAdmissionMask =
    SAO_LAUNCHER_RT_IO_ADMISSION_R3_MAP |
    SAO_LAUNCHER_RT_IO_ADMISSION_CACHED_WRITE |
    SAO_LAUNCHER_RT_IO_ADMISSION_HID_OWNER |
    SAO_LAUNCHER_RT_IO_ADMISSION_HID_PROBE;
constexpr uint32_t kCapabilityMask =
    SAO_LAUNCHER_RT_IO_CAP_R3_SHARED |
    SAO_LAUNCHER_RT_IO_CAP_MOUSE_PROVENANCE |
    SAO_LAUNCHER_RT_IO_CAP_KEYBOARD_PROVENANCE |
    SAO_LAUNCHER_RT_IO_CAP_OB |
    SAO_LAUNCHER_RT_IO_CAP_WATCHDOG;

struct OperatorRecorder {
    std::vector<std::string> calls;
    std::vector<std::string> lines;
    uint32_t fail_stage = 0u;
    bool fail_with_unknown = false;
    bool retain_cleanup = false;
    uint64_t live_requested_mask = 0u;

    static void fillSuccess(uint32_t stage,
                            sao_launcher_rt_io_operator_report_t* report) {
        report->stage = stage;
        report->status = SAO_STATUS_OK;
        report->operation_status = SAO_STATUS_OK;
        report->failure_classification = SAO_LAUNCHER_RT_IO_FAILURE_NONE;
        report->success = 1u;
        report->complete = 1u;
        report->state_observed = 1u;
        report->call_authenticated = 1u;
        report->call_transport_complete = 1u;
        report->call_request_id_matched = 1u;
        report->call_committed = 1u;
        report->last_failure_code = SAO_STATUS_OK;
        report->last_failure_stage = 0u;
    }

    static sao_status_t finish(OperatorRecorder* self, uint32_t stage,
                               sao_launcher_rt_io_operator_report_t* report) {
        fillSuccess(stage, report);
        if (self->fail_stage != stage)
            return SAO_STATUS_OK;
        report->status = SAO_STATUS_INTERNAL;
        report->operation_status = SAO_STATUS_INTERNAL;
        report->success = 0u;
        report->complete = 0u;
        if (stage == SAO_LAUNCHER_RT_IO_STAGE_PREFLIGHT) {
            report->failure_classification =
                SAO_LAUNCHER_RT_IO_FAILURE_PREFLIGHT_INCOMPLETE;
        } else if (stage == SAO_LAUNCHER_RT_IO_STAGE_INIT) {
            report->failure_classification =
                SAO_LAUNCHER_RT_IO_FAILURE_INIT_INCOMPLETE;
        } else if (stage == SAO_LAUNCHER_RT_IO_STAGE_LIVE) {
            report->failure_classification =
                SAO_LAUNCHER_RT_IO_FAILURE_LIVE_INCOMPLETE;
            if (self->fail_with_unknown)
                report->unknown_step_mask = uint64_t{1} << 8u;
        } else if (stage == SAO_LAUNCHER_RT_IO_STAGE_STATUS) {
            report->failure_classification =
                SAO_LAUNCHER_RT_IO_FAILURE_STATUS_INCONSISTENT;
        } else {
            report->failure_classification =
                SAO_LAUNCHER_RT_IO_FAILURE_CLEANUP_INCOMPLETE;
        }
        return SAO_STATUS_INTERNAL;
    }

    static sao_status_t preflight(
        sao_platform_ctx*, const sao_launcher_rt_io_operator_options_t*,
        sao_launcher_rt_io_operator_report_t* report, void* user) {
        auto* self = static_cast<OperatorRecorder*>(user);
        self->calls.emplace_back("preflight");
        report->residue_gate = 1u;
        report->resources_absent = 1u;
        report->is_admin = 2u;
        report->is_elevated = 2u;
        report->load_driver_privilege_present = 2u;
        report->load_driver_privilege_enabled = 2u;
        report->hvci_enabled = 1u;
        report->vbs_enabled = 1u;
        report->provider_observable = 2u;
        return finish(self, SAO_LAUNCHER_RT_IO_STAGE_PREFLIGHT, report);
    }

    static sao_status_t init(
        sao_platform_ctx*, const sao_launcher_rt_io_operator_options_t*,
        sao_launcher_rt_io_operator_report_t* report, void* user) {
        auto* self = static_cast<OperatorRecorder*>(user);
        self->calls.emplace_back("init");
        report->selected_engine = 5u;
        report->runtime_tier = 5u;
        report->backend = 5u;
        report->selected_backend = 1u;
        report->driver_strategy = 1u;
        report->loaded = 1u;
        report->probe_passed = 1u;
        return finish(self, SAO_LAUNCHER_RT_IO_STAGE_INIT, report);
    }

    static sao_status_t live(
        sao_platform_ctx*, const sao_launcher_rt_io_operator_options_t*,
        sao_launcher_rt_io_operator_report_t* report, void* user) {
        auto* self = static_cast<OperatorRecorder*>(user);
        self->calls.emplace_back("live");
        self->live_requested_mask = report->requested_step_mask;
        report->attempted_step_mask = report->requested_step_mask;
        report->passed_step_mask = report->requested_step_mask;
        report->selected_backend = 1u;
        report->admission_mask = kAdmissionMask;
        report->capability_mask = kCapabilityMask;
        return finish(self, SAO_LAUNCHER_RT_IO_STAGE_LIVE, report);
    }

    static sao_status_t status(
        sao_platform_ctx*, const sao_launcher_rt_io_operator_options_t*,
        sao_launcher_rt_io_operator_report_t* report, void* user) {
        auto* self = static_cast<OperatorRecorder*>(user);
        self->calls.emplace_back("status");
        report->selected_engine = 5u;
        report->runtime_tier = 5u;
        report->backend = 5u;
        report->selected_backend = 1u;
        report->driver_strategy = 1u;
        report->backend_ready = 1u;
        report->attempted_step_mask = report->requested_step_mask;
        report->passed_step_mask = report->requested_step_mask;
        report->admission_mask = kAdmissionMask;
        report->capability_mask = kCapabilityMask;
        return finish(self, SAO_LAUNCHER_RT_IO_STAGE_STATUS, report);
    }

    static sao_status_t cleanup(
        sao_platform_ctx*, const sao_launcher_rt_io_operator_options_t*,
        sao_launcher_rt_io_operator_report_t* report, void* user) {
        auto* self = static_cast<OperatorRecorder*>(user);
        self->calls.emplace_back("cleanup");
        report->resources_absent = 1u;
        report->cleanup_acknowledged = 1u;
        report->cleanup_clean = 1u;
        report->cleanup_keep_running = 0u;
        report->wiper_joined = 1u;
        report->engine_cleanup_confirmed = 1u;
        report->etw_restore_confirmed = 1u;
        if (self->retain_cleanup) {
            self->fail_stage = SAO_LAUNCHER_RT_IO_STAGE_CLEANUP;
            report->provider_retained = 1u;
            report->restore_mask = SAO_LAUNCHER_RT_IO_RESTORE_PROVIDER_RETAINED;
        }
        return finish(self, SAO_LAUNCHER_RT_IO_STAGE_CLEANUP, report);
    }

    static void output(const char* line, void* user) {
        static_cast<OperatorRecorder*>(user)->lines.emplace_back(
            line == nullptr ? "<null>" : line);
    }
};

struct OperatorHookGuard {
    explicit OperatorHookGuard(OperatorRecorder& recorder) {
        hooks.rt_io_operator_preflight = &OperatorRecorder::preflight;
        hooks.rt_io_operator_init = &OperatorRecorder::init;
        hooks.rt_io_operator_live_validate = &OperatorRecorder::live;
        hooks.rt_io_operator_status = &OperatorRecorder::status;
        hooks.rt_io_operator_cleanup = &OperatorRecorder::cleanup;
        hooks.user_data = &recorder;
        sao_launcher_set_composition_test_hooks(&hooks);
    }

    ~OperatorHookGuard() {
        sao_launcher_set_composition_test_hooks(nullptr);
    }

    sao_launcher_composition_test_hooks_t hooks{};
};

sao_launcher_rt_io_operator_options_t makeOptions() {
    sao_launcher_rt_io_operator_options_t options{};
    options.struct_size = sizeof(options);
    options.timeout_ms = 1000u;
    return options;
}

sao_status_t runOperator(OperatorRecorder& recorder,
                         const sao_launcher_rt_io_operator_options_t& options,
                         int32_t& ready) {
    OperatorHookGuard guard(recorder);
    return sao_launcher_rt_io_operator_run(
        reinterpret_cast<sao_platform_ctx*>(&recorder), &options,
        &OperatorRecorder::output, &recorder, &ready);
}

bool contains(const std::string& value, const char* needle) {
    return value.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("RT I/O operator runs the complete typed sequence and cleans up",
          "[launcher][rt_io_operator][orchestration]") {
    OperatorRecorder recorder;
    const auto options = makeOptions();
    int32_t ready = -1;

    REQUIRE(runOperator(recorder, options, ready) == SAO_STATUS_OK);
    CHECK(ready == 1);
    CHECK(recorder.live_requested_mask == kDefaultRequestedMask);
    CHECK(recorder.calls == std::vector<std::string>{
        "preflight", "init", "live", "status", "cleanup"});
    REQUIRE(recorder.lines.size() == 5u);
    CHECK(contains(recorder.lines[0], "\"stage\":\"preflight\""));
    CHECK(contains(recorder.lines[1], "\"stage\":\"init\""));
    CHECK(contains(recorder.lines[2], "\"stage\":\"live\""));
    CHECK(contains(recorder.lines[3], "\"stage\":\"status\""));
    CHECK(contains(recorder.lines[4], "\"stage\":\"cleanup\""));
    CHECK(contains(recorder.lines[4], "\"cleanup_clean\":true"));
    CHECK(contains(recorder.lines[4], "\"wiper_joined\":true"));
    CHECK(contains(recorder.lines[4],
                   "\"engine_cleanup_confirmed\":true"));
    CHECK(contains(recorder.lines[4],
                   "\"etw_restore_confirmed\":true"));
    CHECK_FALSE(contains(recorder.lines[0], "RT_IO_READY"));
}

TEST_CASE("RT I/O optional diagnostics are explicit and reflected in JSON",
          "[launcher][rt_io_operator][options]") {
    OperatorRecorder recorder;
    auto options = makeOptions();
    options.input_checks = 1u;
    options.r5_check = 1u;
    options.mf_check = 1u;
    options.exit_after_validation = 1u;
    int32_t ready = 0;

    REQUIRE(runOperator(recorder, options, ready) == SAO_STATUS_OK);
    CHECK(ready == 1);
    CHECK(recorder.live_requested_mask == kAllRequestedMask);
    REQUIRE(recorder.lines.size() == 5u);
    CHECK(contains(recorder.lines[2], "\"input_checks\":true"));
    CHECK(contains(recorder.lines[2], "\"r5_check\":true"));
    CHECK(contains(recorder.lines[2], "\"mf_check\":true"));
    CHECK(contains(recorder.lines[2], "\"exit_after_validation\":true"));
    CHECK(contains(recorder.lines[2],
                   "\"requested_step_mask\":131071"));
}

TEST_CASE("RT I/O preflight-only performs no mutating operator stage",
          "[launcher][rt_io_operator][preflight]") {
    OperatorRecorder recorder;
    auto options = makeOptions();
    options.preflight_only = 1u;
    int32_t ready = -1;

    REQUIRE(runOperator(recorder, options, ready) == SAO_STATUS_OK);
    CHECK(ready == 0);
    CHECK(recorder.calls == std::vector<std::string>{"preflight"});
    REQUIRE(recorder.lines.size() == 1u);
    CHECK(contains(recorder.lines[0], "\"preflight_only\":true"));
}

TEST_CASE("RT I/O preflight accepts a present but disabled load-driver privilege",
          "[launcher][rt_io_operator][preflight][privilege]") {
    using sao::launcher::testing::rt_io_operator_preflight_observations_complete_for_test;

    CHECK(rt_io_operator_preflight_observations_complete_for_test(2u, 2u, 2u, 1u, 1u, 2u, 2u));
    CHECK_FALSE(
        rt_io_operator_preflight_observations_complete_for_test(2u, 2u, 2u, 0u, 1u, 2u, 2u));
    CHECK_FALSE(
        rt_io_operator_preflight_observations_complete_for_test(2u, 2u, 1u, 1u, 1u, 2u, 2u));
}

TEST_CASE("RT I/O preflight accepts only clean or never-initialized provider history",
          "[launcher][rt_io_operator][preflight][provider_history]") {
    using sao::launcher::testing::rt_io_operator_preflight_failure_state_ready_for_test;

    CHECK(rt_io_operator_preflight_failure_state_ready_for_test(SAO_STATUS_OK, 0u));
    CHECK(rt_io_operator_preflight_failure_state_ready_for_test(kNotInitializedStatus, 0u));
    CHECK_FALSE(rt_io_operator_preflight_failure_state_ready_for_test(kNotInitializedStatus, 1u));
    CHECK_FALSE(
        rt_io_operator_preflight_failure_state_ready_for_test(SAO_STATUS_INVALID_ARGUMENT, 0u));
}

TEST_CASE("RT I/O unknown live state fails and still performs cleanup",
          "[launcher][rt_io_operator][failure][cleanup]") {
    OperatorRecorder recorder;
    recorder.fail_stage = SAO_LAUNCHER_RT_IO_STAGE_LIVE;
    recorder.fail_with_unknown = true;
    const auto options = makeOptions();
    int32_t ready = 1;

    REQUIRE(runOperator(recorder, options, ready) != SAO_STATUS_OK);
    CHECK(ready == 0);
    CHECK(recorder.calls == std::vector<std::string>{
        "preflight", "init", "live", "cleanup"});
    REQUIRE(recorder.lines.size() == 5u);
    CHECK(contains(recorder.lines[2],
                   "\"failure_classification\":\"live_incomplete\""));
    CHECK(contains(recorder.lines[3],
                   "\"failure_classification\":\"not_submitted\""));
    CHECK(contains(recorder.lines[4], "\"stage\":\"cleanup\""));
}

TEST_CASE("RT I/O retained provider state blocks readiness",
          "[launcher][rt_io_operator][failure][restore]") {
    OperatorRecorder recorder;
    recorder.retain_cleanup = true;
    const auto options = makeOptions();
    int32_t ready = 1;

    REQUIRE(runOperator(recorder, options, ready) != SAO_STATUS_OK);
    CHECK(ready == 0);
    REQUIRE(recorder.lines.size() == 5u);
    CHECK(contains(recorder.lines.back(), "\"provider_retained\":true"));
    CHECK(contains(recorder.lines.back(),
                   "\"failure_classification\":\"cleanup_incomplete\""));
}

TEST_CASE("RT I/O operator JSON is deterministic and sanitized",
          "[launcher][rt_io_operator][output]") {
    sao_launcher_rt_io_operator_report_t report{};
    report.struct_size = sizeof(report);
    report.stage = SAO_LAUNCHER_RT_IO_STAGE_STATUS;
    report.status = SAO_STATUS_OK;
    report.operation_status = SAO_STATUS_OK;
    report.success = 1u;
    report.complete = 1u;
    report.selected_engine = 5u;
    report.runtime_tier = 5u;
    report.backend = 5u;
    report.selected_backend = 1u;
    report.failure_classification = SAO_LAUNCHER_RT_IO_FAILURE_NONE;
    char first[2048]{};
    char second[2048]{};
    size_t first_size = 0u;
    size_t second_size = 0u;

    REQUIRE(sao_launcher_rt_io_operator_format_json(
        &report, first, sizeof(first), &first_size) == SAO_STATUS_OK);
    REQUIRE(sao_launcher_rt_io_operator_format_json(
        &report, second, sizeof(second), &second_size) == SAO_STATUS_OK);
    CHECK(std::string{first} == std::string{second});
    CHECK(first_size == second_size);
    CHECK(std::string{first}.find('\n') == std::string::npos);
    CHECK_FALSE(contains(first, "handle"));
    CHECK_FALSE(contains(first, "address"));
    CHECK_FALSE(contains(first, "sha256"));
    CHECK_FALSE(contains(first, "path"));
}

TEST_CASE("READY remains ordinary and RT_IO_READY requires full operator success",
          "[launcher][rt_io_operator][smoke][full_stack]") {
    sao::launcher::AppState ordinary_smoke{};
    ordinary_smoke.smoke_mode = true;
    ordinary_smoke.exit_after_init = true;
    CHECK_FALSE(sao::launcher::shouldEmitRtIoReady(ordinary_smoke, true));

    sao::launcher::AppState operator_state{};
    operator_state.rt_io_operator = true;
    CHECK_FALSE(sao::launcher::shouldEmitRtIoReady(operator_state, false));
    CHECK(sao::launcher::shouldEmitRtIoReady(operator_state, true));

    operator_state.rt_io_preflight_only = true;
    CHECK_FALSE(sao::launcher::shouldEmitRtIoReady(operator_state, true));
}
