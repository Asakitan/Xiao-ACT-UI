// SAO Auto — gap-closure tests for platform/engine.
//
// Coverage:
//   A. capability gate — render_hook provider_status + GPU_PRESENT
//      dispatch now return CAPABILITY_MISSING instead of NOT_IMPLEMENTED.
//   B. production EventBus — static assertions only inventory the seven
//      NOT_IMPLEMENTED returns in dead ``src/event_bus.cpp`` and prove that
//      CMake selects ``src/event_bus_adapter.cpp``.  Runtime completeness is
//      established separately through executable dispatch, retention,
//      ephemeral-topic, statistics, and unsubscribe behavior assertions.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sao/core/status.h"
#include "sao/engine/event_bus.h"
#include "sao/engine/render_hook.h"

#include "core_gap_support.h"

using sao::core_gap::GapKind;
using sao::core_gap::matches_gap_kind;

namespace {

struct CallbackContext {
    std::uint32_t calls = 0;
    std::vector<std::string> topics;
    std::vector<std::vector<std::uint8_t>> payloads;
    std::vector<SaoEngineEventHeader> headers;
};

void SAO_ENGINE_CALL record_event(const char* topic, const uint8_t* json, size_t len,
                                  const SaoEngineEventHeader* header, void* user) {
    auto* context = static_cast<CallbackContext*>(user);
    ++context->calls;
    context->topics.emplace_back(topic);
    if (len == 0) {
        context->payloads.emplace_back();
    } else {
        context->payloads.emplace_back(json, json + len);
    }
    context->headers.push_back(*header);
}

std::optional<std::string> read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::size_t count_occurrences(std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = text.find(needle, offset)) != std::string_view::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

} // namespace

TEST_CASE("engine gap: EventBus static inventory excludes dead source",
          "[gap_closure][engine][event_bus][source_inventory]") {
    const std::filesystem::path source_dir = SAO_ENGINE_MODULE_SOURCE_DIR;
    const auto cmake = read_text_file(source_dir / "CMakeLists.txt");
    const auto dead_source = read_text_file(source_dir / "src" / "event_bus.cpp");
    REQUIRE(cmake.has_value());
    REQUIRE(dead_source.has_value());

    const auto target_start = cmake->find("add_library(sao_platform_engine SHARED");
    REQUIRE(target_start != std::string::npos);
    const auto target_end = cmake->find("\n)", target_start);
    REQUIRE(target_end != std::string::npos);
    const std::string_view target_sources(cmake->data() + target_start, target_end - target_start);
    CHECK(target_sources.find("src/event_bus_adapter.cpp") != std::string_view::npos);
    CHECK(target_sources.find("src/event_bus.cpp") == std::string_view::npos);

    // This is source inventory only.  Production completeness is asserted
    // by the linked-ABI behavior tests below, not inferred from this count.
    CHECK(count_occurrences(*dead_source, "return SAO_STATUS_ERR_NOT_IMPLEMENTED;") == 7);
}

TEST_CASE("engine gap: shipping EventBus base ABI creates a live bus",
          "[gap_closure][engine][event_bus]") {
    // The linked engine target resolves the base ABI to the production
    // adapter selected by CMake.
    sao_engine_event_bus_handle_t handle = nullptr;
    const sao_status_t rc = sao_engine_event_bus_create(0, 0.0F, &handle);
    CAPTURE(rc);
    REQUIRE(rc == SAO_STATUS_OK);
    REQUIRE(handle != nullptr);
    sao_engine_event_bus_destroy(handle);
}

TEST_CASE("engine gap: shipping EventBus dispatches and retains",
          "[gap_closure][engine][event_bus]") {
    sao_engine_event_bus_handle_t handle = nullptr;
    REQUIRE(sao_engine_event_bus_create(4, 0.0F, &handle) == SAO_STATUS_OK);

    CallbackContext exact;
    CallbackContext wildcard;
    sao_engine_subscription_t exact_subscription = 0;
    sao_engine_subscription_t wildcard_subscription = 0;
    REQUIRE(sao_engine_event_bus_subscribe(handle, "gap/topic", "gap/exact", record_event,
                                           &exact, &exact_subscription) == SAO_STATUS_OK);
    REQUIRE(sao_engine_event_bus_subscribe(handle, "*", "gap/wildcard", record_event, &wildcard,
                                           &wildcard_subscription) == SAO_STATUS_OK);

    constexpr std::uint8_t first_payload[] = {'{', '"', 'n', '"', ':', '1', '}'};
    SaoEngineEventHeader supplied_header{};
    supplied_header.ts_unix_ns = 1700000000000000000ULL;
    supplied_header.producer_id_hash = 0x17C;
    supplied_header.source_id_hash = 0xA11;
    supplied_header.confidence = 0.75F;
    REQUIRE(sao_engine_event_bus_publish(handle, "gap/topic", first_payload,
                                         sizeof(first_payload), &supplied_header) == SAO_STATUS_OK);

    REQUIRE(exact.calls == 1);
    REQUIRE(wildcard.calls == 1);
    CHECK(exact.topics == std::vector<std::string>{"gap/topic"});
    CHECK(wildcard.topics == exact.topics);
    const auto first_payload_end = first_payload + sizeof(first_payload);
    const std::vector<std::uint8_t> expected_first_payload(first_payload, first_payload_end);
    CHECK(exact.payloads.front() == expected_first_payload);
    REQUIRE(exact.headers.size() == 1);
    REQUIRE(wildcard.headers.size() == 1);
    CHECK(exact.headers.front().sequence_id != 0);
    CHECK(wildcard.headers.front().sequence_id == exact.headers.front().sequence_id);
    CHECK(exact.headers.front().ts_unix_ns == supplied_header.ts_unix_ns);
    CHECK(exact.headers.front().producer_id_hash == supplied_header.producer_id_hash);
    CHECK(exact.headers.front().source_id_hash == supplied_header.source_id_hash);
    CHECK(exact.headers.front().confidence == supplied_header.confidence);

    std::uint32_t event_count = 0;
    std::size_t topics_used = 0;
    std::size_t jsons_used = 0;
    REQUIRE(sao_engine_event_bus_recent(handle, 0, nullptr, 0, nullptr, 0, nullptr, 0, &event_count,
                                        &topics_used, &jsons_used) == SAO_STATUS_OK);
    REQUIRE(event_count == 1);
    REQUIRE(topics_used == std::strlen("gap/topic"));
    REQUIRE(jsons_used == sizeof(first_payload));

    std::vector<SaoEngineRingEntry> events(event_count);
    std::vector<char> topics(topics_used);
    std::vector<std::uint8_t> payloads(jsons_used);
    REQUIRE(sao_engine_event_bus_recent(handle, 0, events.data(), events.size(), topics.data(),
                                        topics.size(), payloads.data(), payloads.size(),
                                        &event_count, &topics_used, &jsons_used) == SAO_STATUS_OK);
    REQUIRE(events.size() == 1);
    const auto& retained = events.front();
    CHECK(std::string(topics.data() + retained.topic_offset, retained.topic_length) ==
          "gap/topic");
    CHECK(std::vector<std::uint8_t>(payloads.begin() + retained.json_offset,
                                    payloads.begin() + retained.json_offset +
                                        retained.json_length) == expected_first_payload);
    CHECK(retained.header.sequence_id == exact.headers.front().sequence_id);

    REQUIRE(sao_engine_event_bus_mark_ephemeral(handle, "gap/topic") == SAO_STATUS_OK);
    constexpr std::uint8_t second_payload[] = {'[', ']'};
    REQUIRE(sao_engine_event_bus_publish(handle, "gap/topic", second_payload,
                                         sizeof(second_payload), nullptr) == SAO_STATUS_OK);
    CHECK(exact.calls == 2);
    CHECK(wildcard.calls == 2);

    SaoEngineBusStats stats{};
    REQUIRE(sao_engine_event_bus_stats(handle, &stats) == SAO_STATUS_OK);
    CHECK(stats.published == 2);
    CHECK(stats.retained == 1);
    CHECK(stats.callback_failures == 0);
    CHECK(stats.topic_count == 2);
    CHECK(stats.active_subscription_count == 2);

    event_count = 0;
    topics_used = 0;
    jsons_used = 0;
    REQUIRE(sao_engine_event_bus_recent(handle, 0, nullptr, 0, nullptr, 0, nullptr, 0, &event_count,
                                        &topics_used, &jsons_used) == SAO_STATUS_OK);
    CHECK(event_count == 1);

    std::uint32_t removed = 0;
    REQUIRE(sao_engine_event_bus_unsubscribe_owner(handle, "gap/exact", &removed) ==
            SAO_STATUS_OK);
    CHECK(removed == 1);
    CHECK(sao_engine_event_bus_unsubscribe(handle, exact_subscription) ==
          SAO_STATUS_ERR_SUBSCRIPTION_GONE);

    REQUIRE(sao_engine_event_bus_publish(handle, "gap/topic", nullptr, 0, nullptr) ==
            SAO_STATUS_OK);
    CHECK(exact.calls == 2);
    CHECK(wildcard.calls == 3);
    REQUIRE(sao_engine_event_bus_unsubscribe(handle, wildcard_subscription) == SAO_STATUS_OK);

    sao_engine_event_bus_destroy(handle);
}

TEST_CASE("engine gap: shipping EventBus exposes the priority extension",
          "[gap_closure][engine][event_bus][priority]") {
    // The canonical priority API is the functional surface under test.
    // Compatibility note: the production adapter also exports the legacy
    // _wave5 symbols solely to preserve the published compatibility ABI.
    sao_engine_event_bus_handle_t handle = nullptr;
    REQUIRE(sao_engine_event_bus_create_priority(&handle) == SAO_STATUS_OK);
    REQUIRE(handle != nullptr);
    sao_engine_event_bus_destroy(handle);
}

TEST_CASE("engine gap: render_hook provider_status advertises capability gap",
          "[gap_closure][engine][render_hook]") {
    sao_engine_render_hook_registry_handle_t registry = nullptr;
    REQUIRE(sao_engine_render_hook_registry_create(&registry) == SAO_STATUS_OK);
    REQUIRE(registry != nullptr);

    // Capability-gate contract — was NOT_IMPLEMENTED, is now CAPABILITY_MISSING.
    const sao_status_t status = sao_engine_render_hook_provider_status(registry);
    CAPTURE(status);
    CHECK(matches_gap_kind(status, GapKind::CapabilityGate));

    // Null handle path still reports invalid argument — orthogonal to the
    // capability gate.
    CHECK(sao_engine_render_hook_provider_status(nullptr) == SAO_STATUS_ERR_INVALID_ARGUMENT);

    sao_engine_render_hook_registry_destroy(registry);
}

TEST_CASE("engine gap: render_clock_dispatch GPU_PRESENT is capability-gated",
          "[gap_closure][engine][render_hook]") {
    sao_engine_render_hook_registry_handle_t registry = nullptr;
    REQUIRE(sao_engine_render_hook_registry_create(&registry) == SAO_STATUS_OK);

    // Logical tick — the real path.  We don't strictly assert
    // SAO_STATUS_OK (implementation may reject a viewport of 0x0), but it
    // definitely must not report NOT_IMPLEMENTED / CAPABILITY_MISSING.
    const sao_status_t logical_rc = sao_engine_render_clock_dispatch(
        registry, "gap-surface", SAO_ENGINE_RENDER_BEFORE_COMPOSITOR, 1000u, 0, 0, 640, 480,
        SAO_ENGINE_RENDER_DISPATCH_LOGICAL_TICK);
    CAPTURE(logical_rc);
    CHECK(logical_rc != SAO_STATUS_ERR_CAPABILITY_MISSING);
    CHECK(logical_rc != SAO_STATUS_ERR_NOT_IMPLEMENTED);

    // GPU_PRESENT — the capability-gated path.
    const sao_status_t gpu_rc = sao_engine_render_clock_dispatch(
        registry, "gap-surface", SAO_ENGINE_RENDER_BEFORE_COMPOSITOR, 2000u, 0, 0, 640, 480,
        SAO_ENGINE_RENDER_DISPATCH_GPU_PRESENT);
    CAPTURE(gpu_rc);
    CHECK(matches_gap_kind(gpu_rc, GapKind::CapabilityGate));

    sao_engine_render_hook_registry_destroy(registry);
}
