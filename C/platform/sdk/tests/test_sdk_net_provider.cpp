#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "sao/sdk/sao_sdk.h"

namespace {

struct NetProviderFixture;

struct NetSessionFixture {
    NetProviderFixture* owner = nullptr;
    std::string plugin_id;
    bool capturing = false;
    sao_sdk_net_packet_callback_t packet_callback = nullptr;
    void* packet_user_data = nullptr;
};

struct NetProviderFixture {
    std::atomic<size_t> retain_count{0};
    std::atomic<size_t> release_count{0};
    std::atomic<size_t> open_count{0};
    std::atomic<size_t> close_count{0};
    std::atomic<size_t> start_count{0};
    std::atomic<size_t> stop_count{0};
    std::atomic<size_t> parse_count{0};
    std::atomic<size_t> live_sessions{0};
    sao_sdk_status_t open_status = SAO_SDK_OK;
    sao_sdk_status_t close_status = SAO_SDK_OK;
    sao_sdk_status_t start_status = SAO_SDK_OK;
    sao_sdk_status_t stop_status = SAO_SDK_OK;
    sao_sdk_status_t parse_status = SAO_SDK_OK;
    bool open_returns_session_on_failure = false;
    bool throw_open = false;
    bool throw_close = false;
    bool throw_retain = false;
    bool throw_release = false;
    bool throw_start = false;
    bool throw_stop = false;
    bool throw_parse = false;
    bool reenter_parse = false;
    bool emit_packet_on_start = false;
    bool emit_invalid_packet_on_start = false;
    bool invalid_result_range = false;
    bool invalid_result_abi = false;
    size_t result_count = 1;
    size_t last_result_stride = 0;
    uint32_t last_snap_length = 0;
    std::string last_source;
    std::string last_filter;
    std::vector<std::string> opened_plugin_ids;
    SaoSdkContext* reentry_context = nullptr;
    const SaoSdkNetProviderVTable* reentry_replacement = nullptr;
    std::atomic<sao_sdk_status_t> reentry_parse_status{SAO_SDK_OK};
    std::atomic<sao_sdk_status_t> reentry_clear_status{SAO_SDK_OK};
    std::atomic<sao_sdk_status_t> reentry_replace_status{SAO_SDK_OK};
};

void SAO_SDK_CALL net_retain(void* user_data) {
    auto* fixture = static_cast<NetProviderFixture*>(user_data);
    if (fixture->throw_retain)
        throw std::runtime_error("net retain fixture");
    ++fixture->retain_count;
}

void SAO_SDK_CALL net_release(void* user_data) {
    auto* fixture = static_cast<NetProviderFixture*>(user_data);
    ++fixture->release_count;
    if (fixture->throw_release)
        throw std::runtime_error("net release fixture");
}

sao_sdk_status_t SAO_SDK_CALL net_open(void* user_data, const char* plugin_id_utf8,
                                       void** out_session) {
    if (plugin_id_utf8 == nullptr || plugin_id_utf8[0] == '\0' || out_session == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    *out_session = nullptr;
    auto* fixture = static_cast<NetProviderFixture*>(user_data);
    ++fixture->open_count;
    fixture->opened_plugin_ids.emplace_back(plugin_id_utf8);
    if (fixture->throw_open)
        throw std::runtime_error("net open fixture");
    if (fixture->open_status != SAO_SDK_OK && !fixture->open_returns_session_on_failure)
        return fixture->open_status;
    *out_session = new NetSessionFixture{fixture, plugin_id_utf8};
    ++fixture->live_sessions;
    return fixture->open_status;
}

sao_sdk_status_t SAO_SDK_CALL net_close(void*, void* session_value) {
    auto* session = static_cast<NetSessionFixture*>(session_value);
    if (session == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    ++session->owner->close_count;
    if (session->owner->throw_close)
        throw std::runtime_error("net close fixture");
    if (session->owner->close_status != SAO_SDK_OK)
        return session->owner->close_status;
    --session->owner->live_sessions;
    delete session;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL net_start(void*, void* session_value,
                                        const SaoSdkNetCaptureConfig* config,
                                        sao_sdk_net_packet_callback_t callback,
                                        void* callback_user_data) {
    auto* session = static_cast<NetSessionFixture*>(session_value);
    if (session == nullptr || config == nullptr || callback == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    auto* fixture = session->owner;
    ++fixture->start_count;
    if (config->struct_size != sizeof(*config) ||
        config->abi_version != SAO_SDK_NET_PROVIDER_ABI_VERSION) {
        return SAO_SDK_ERR_ABI_MISMATCH;
    }
    fixture->last_snap_length = config->snap_length;
    fixture->last_source.assign(config->source_id_utf8, config->source_id_len);
    fixture->last_filter.assign(config->filter_utf8, config->filter_len);
    if (fixture->throw_start)
        throw std::runtime_error("net start fixture");
    if (fixture->start_status != SAO_SDK_OK)
        return fixture->start_status;
    session->capturing = true;
    session->packet_callback = callback;
    session->packet_user_data = callback_user_data;
    if (fixture->emit_packet_on_start) {
        constexpr std::array<uint8_t, 20> packet_bytes{0x45, 0, 0, 20};
        SaoSdkNetPacketView packet{};
        packet.struct_size = sizeof(packet);
        packet.abi_version = SAO_SDK_NET_PROVIDER_ABI_VERSION;
        packet.link_type = SAO_SDK_NET_LINK_RAW_IPV4;
        packet.data = packet_bytes.data();
        packet.data_size = fixture->emit_invalid_packet_on_start ? SAO_SDK_NET_MAX_PACKET_SIZE + 1u
                                                                 : packet_bytes.size();
        packet.timestamp_unix_ns = 123456789;
        callback(&packet, callback_user_data);
    }
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL net_stop(void*, void* session_value) {
    auto* session = static_cast<NetSessionFixture*>(session_value);
    if (session == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    auto* fixture = session->owner;
    ++fixture->stop_count;
    if (fixture->throw_stop)
        throw std::runtime_error("net stop fixture");
    if (fixture->stop_status != SAO_SDK_OK)
        return fixture->stop_status;
    session->capturing = false;
    session->packet_callback = nullptr;
    session->packet_user_data = nullptr;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL net_parse(void*, void* session_value,
                                        const SaoSdkNetPacketView* packet,
                                        SaoSdkNetParsedResult* out_results, size_t capacity,
                                        size_t element_stride, size_t* out_count) {
    if (out_count != nullptr)
        *out_count = 0;
    auto* session = static_cast<NetSessionFixture*>(session_value);
    if (session == nullptr || packet == nullptr || out_count == nullptr ||
        (capacity != 0 && out_results == nullptr)) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    auto* fixture = session->owner;
    ++fixture->parse_count;
    fixture->last_result_stride = element_stride;
    if (packet->struct_size != sizeof(*packet) ||
        packet->abi_version != SAO_SDK_NET_PROVIDER_ABI_VERSION) {
        return SAO_SDK_ERR_ABI_MISMATCH;
    }
    if (fixture->reenter_parse) {
        std::array<uint8_t, 20> nested_bytes{};
        auto nested_packet = *packet;
        nested_packet.data = nested_bytes.data();
        nested_packet.data_size = nested_bytes.size();
        size_t nested_count = 0;
        fixture->reentry_parse_status =
            sao_sdk_net_parse_packet(fixture->reentry_context, &nested_packet, nullptr, 0,
                                     SAO_SDK_NET_PARSED_RESULT_ELEMENT_SIZE, &nested_count);
        fixture->reentry_clear_status =
            sao_sdk_context_configure_net_provider(fixture->reentry_context, nullptr);
        fixture->reentry_replace_status = sao_sdk_context_configure_net_provider(
            fixture->reentry_context, fixture->reentry_replacement);
    }
    if (fixture->throw_parse)
        throw std::runtime_error("net parse fixture");
    if (fixture->parse_status != SAO_SDK_OK)
        return fixture->parse_status;
    *out_count = fixture->result_count;
    if (capacity < fixture->result_count)
        return SAO_SDK_ERR_BUFFER_TOO_SMALL;
    for (size_t index = 0; index < fixture->result_count; ++index) {
        auto* result = reinterpret_cast<SaoSdkNetParsedResult*>(
            reinterpret_cast<uint8_t*>(out_results) + index * element_stride);
        *result = {};
        result->struct_size = fixture->invalid_result_abi ? sizeof(SaoSdkNetParsedResult) - 1u
                                                          : sizeof(SaoSdkNetParsedResult);
        result->abi_version = SAO_SDK_NET_PROVIDER_ABI_VERSION;
        result->layer = 3;
        result->protocol = 6;
        result->header_offset = 0;
        result->header_size = 4;
        result->payload_offset = 4;
        result->payload_size =
            fixture->invalid_result_range ? packet->data_size : packet->data_size - 4;
        result->flow_id = 0x1122334455667788ull + index;
    }
    return SAO_SDK_OK;
}

SaoSdkNetProviderVTable make_net_provider(NetProviderFixture* fixture) {
    SaoSdkNetProviderVTable provider{};
    provider.abi_version = SAO_SDK_NET_PROVIDER_ABI_VERSION;
    provider.struct_size = sizeof(provider);
    provider.user_data = fixture;
    provider.retain = net_retain;
    provider.release = net_release;
    provider.open_session = net_open;
    provider.close_session = net_close;
    provider.capture_start = net_start;
    provider.capture_stop = net_stop;
    provider.parse_packet = net_parse;
    return provider;
}

SaoSdkNetCaptureConfig capture_config() {
    static constexpr char kSource[] = "fixture0";
    static constexpr char kFilter[] = "tcp";
    SaoSdkNetCaptureConfig config{};
    config.struct_size = sizeof(config);
    config.abi_version = SAO_SDK_NET_PROVIDER_ABI_VERSION;
    config.snap_length = 65535;
    config.source_id_utf8 = kSource;
    config.source_id_len = sizeof(kSource) - 1;
    config.filter_utf8 = kFilter;
    config.filter_len = sizeof(kFilter) - 1;
    return config;
}

SaoSdkNetPacketView packet_view(const uint8_t* bytes, size_t size) {
    SaoSdkNetPacketView packet{};
    packet.struct_size = sizeof(packet);
    packet.abi_version = SAO_SDK_NET_PROVIDER_ABI_VERSION;
    packet.link_type = SAO_SDK_NET_LINK_RAW_IPV4;
    packet.data = bytes;
    packet.data_size = size;
    packet.timestamp_unix_ns = 987654321;
    return packet;
}

struct PacketSink {
    size_t callback_count = 0;
    size_t observed_size = 0;
    uint64_t observed_timestamp = 0;
    SaoSdkContext* context = nullptr;
    sao_sdk_status_t reentry_stop_status = SAO_SDK_OK;
};

void SAO_SDK_CALL packet_sink_callback(const SaoSdkNetPacketView* packet, void* user_data) {
    auto* sink = static_cast<PacketSink*>(user_data);
    ++sink->callback_count;
    sink->observed_size = packet->data_size;
    sink->observed_timestamp = packet->timestamp_unix_ns;
    if (sink->context != nullptr)
        sink->reentry_stop_status = sao_sdk_net_capture_stop(sink->context);
}

struct LegacyNetTableV14 {
    decltype(SaoSdkNetTable::set_frame_callback) set_frame_callback = nullptr;
};

static_assert(sizeof(LegacyNetTableV14) == SAO_SDK_NET_TABLE_V1_4_SIZE);

struct NetTableMetadataPrefix {
    decltype(SaoSdkNetTable::set_frame_callback) set_frame_callback = nullptr;
    uint32_t abi_version = 0;
    uint32_t struct_size = 0;
};

static_assert(sizeof(NetTableMetadataPrefix) == offsetof(SaoSdkNetTable, capture_start));

struct ProcessNetProviderReset {
    ~ProcessNetProviderReset() {
        (void)sao_sdk_platform_net_configure_provider(nullptr);
    }
};

struct CoexistMemoryFixture {
    size_t retain_count = 0;
    size_t release_count = 0;
    size_t live_sessions = 0;
    SaoSdkContext* reentry_context = nullptr;
    sao_sdk_status_t destroy_status = SAO_SDK_OK;
    bool reenter_destroy = false;
};

void SAO_SDK_CALL coexist_memory_retain(void* user_data) {
    ++static_cast<CoexistMemoryFixture*>(user_data)->retain_count;
}

void SAO_SDK_CALL coexist_memory_release(void* user_data) {
    ++static_cast<CoexistMemoryFixture*>(user_data)->release_count;
}

sao_sdk_status_t SAO_SDK_CALL coexist_memory_open(void* user_data, const char*,
                                                  void** out_session) {
    if (out_session == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    auto* fixture = static_cast<CoexistMemoryFixture*>(user_data);
    ++fixture->live_sessions;
    *out_session = fixture;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL coexist_memory_close(void*, void* session) {
    if (session == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    --static_cast<CoexistMemoryFixture*>(session)->live_sessions;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL coexist_memory_attach(void*, void*,
                                                    const SaoSdkMemoryTargetIdentity*) {
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL coexist_memory_detach(void*, void*) {
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL coexist_memory_read(void*, void* session, uint64_t, void*, size_t,
                                                  size_t* out_bytes_read) {
    if (out_bytes_read != nullptr)
        *out_bytes_read = 0;
    auto* fixture = static_cast<CoexistMemoryFixture*>(session);
    if (fixture->reenter_destroy)
        fixture->destroy_status = sao_sdk_context_try_destroy(fixture->reentry_context);
    return SAO_SDK_ERR_READ_FAULT;
}

sao_sdk_status_t SAO_SDK_CALL coexist_memory_modules(void*, void*, SaoSdkMemoryModule*, size_t,
                                                     size_t, size_t* out_count) {
    if (out_count != nullptr)
        *out_count = 0;
    return SAO_SDK_OK;
}

SaoSdkMemoryProviderVTable make_coexist_memory_provider(CoexistMemoryFixture* fixture) {
    SaoSdkMemoryProviderVTable provider{};
    provider.abi_version = SAO_SDK_MEMORY_PROVIDER_ABI_VERSION;
    provider.struct_size = sizeof(provider);
    provider.user_data = fixture;
    provider.retain = coexist_memory_retain;
    provider.release = coexist_memory_release;
    provider.open_session = coexist_memory_open;
    provider.close_session = coexist_memory_close;
    provider.attach = coexist_memory_attach;
    provider.detach = coexist_memory_detach;
    provider.read = coexist_memory_read;
    provider.enumerate_modules = coexist_memory_modules;
    return provider;
}

} // namespace

TEST_CASE("net provider is optional and opens one session per context",
          "[sdk][net][provider][session]") {
    SaoSdkContext first{};
    SaoSdkContext second{};
    REQUIRE(sao_sdk_bind_context("net.first", "1.0", &first) == SAO_SDK_OK);
    REQUIRE(sao_sdk_bind_context("net.second", "1.0", &second) == SAO_SDK_OK);
    CHECK(sao_sdk_context_net_provider_status(&first) == SAO_SDK_ERR_UNSUPPORTED);

    auto config = capture_config();
    PacketSink sink;
    CHECK(sao_sdk_net_capture_start(&first, &config, packet_sink_callback, &sink) ==
          SAO_SDK_ERR_UNSUPPORTED);

    NetProviderFixture fixture;
    auto provider = make_net_provider(&fixture);

    auto bad_abi = provider;
    bad_abi.abi_version = 2u << 16;
    CHECK(sao_sdk_context_configure_net_provider(&first, &bad_abi) == SAO_SDK_ERR_ABI_MISMATCH);
    auto incomplete = provider;
    incomplete.parse_packet = nullptr;
    CHECK(sao_sdk_context_configure_net_provider(&first, &incomplete) ==
          SAO_SDK_ERR_INVALID_ARGUMENT);
    CHECK(fixture.open_count == 0);

    REQUIRE(sao_sdk_context_configure_net_provider(&first, &provider) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_configure_net_provider(&second, &provider) == SAO_SDK_OK);
    CHECK(fixture.retain_count == 2);
    CHECK(fixture.open_count == 2);
    CHECK(fixture.live_sessions == 2);
    REQUIRE(fixture.opened_plugin_ids.size() == 2);
    CHECK(fixture.opened_plugin_ids[0] == "net.first");
    CHECK(fixture.opened_plugin_ids[1] == "net.second");

    REQUIRE(sao_sdk_net_capture_start(&first, &config, packet_sink_callback, &sink) == SAO_SDK_OK);
    CHECK(fixture.last_snap_length == 65535);
    CHECK(fixture.last_source == "fixture0");
    CHECK(fixture.last_filter == "tcp");
    CHECK(sao_sdk_net_capture_stop(&first) == SAO_SDK_OK);

    REQUIRE(sao_sdk_context_try_destroy(&first) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_try_destroy(&second) == SAO_SDK_OK);
    CHECK(fixture.close_count == 2);
    CHECK(fixture.release_count == 2);
    CHECK(fixture.live_sessions == 0);
}

TEST_CASE("net ABI 1.5 wrappers reject an ABI 1.4 table before appended reads", "[sdk][net][abi]") {
    LegacyNetTableV14 legacy_table{};
    SaoSdkContext legacy_context{};
    legacy_context.abi_version = (1u << 16) | 4u;
    legacy_context.ctx_impl = reinterpret_cast<void*>(uintptr_t{1});
    legacy_context.net = reinterpret_cast<const SaoSdkNetTable*>(&legacy_table);

    auto config = capture_config();
    std::array<uint8_t, 20> bytes{};
    auto packet = packet_view(bytes.data(), bytes.size());
    size_t result_count = 99;
    CHECK(sao_sdk_net_capture_start(&legacy_context, &config, packet_sink_callback, nullptr) ==
          SAO_SDK_ERR_UNSUPPORTED);
    CHECK(sao_sdk_net_capture_stop(&legacy_context) == SAO_SDK_ERR_UNSUPPORTED);
    CHECK(sao_sdk_net_parse_packet(&legacy_context, &packet, nullptr, 0,
                                   SAO_SDK_NET_PARSED_RESULT_ELEMENT_SIZE,
                                   &result_count) == SAO_SDK_ERR_UNSUPPORTED);
    CHECK(result_count == 0);

    NetTableMetadataPrefix metadata_table{};
    metadata_table.abi_version = SAO_SDK_NET_TABLE_ABI_VERSION;
    metadata_table.struct_size = sizeof(metadata_table);
    legacy_context.abi_version = SAO_SDK_ABI_VERSION;
    legacy_context.net = reinterpret_cast<const SaoSdkNetTable*>(&metadata_table);
    CHECK(sao_sdk_net_capture_stop(&legacy_context) == SAO_SDK_ERR_UNSUPPORTED);

    metadata_table.abi_version = 2u << 16;
    CHECK(sao_sdk_net_capture_stop(&legacy_context) == SAO_SDK_ERR_ABI_MISMATCH);

    SaoSdkNetTable old_minor{};
    old_minor.abi_version = (SAO_SDK_NET_TABLE_ABI_VERSION_MAJOR << 16) | 4u;
    old_minor.struct_size = sizeof(old_minor);
    legacy_context.net = &old_minor;
    CHECK(sao_sdk_net_capture_stop(&legacy_context) == SAO_SDK_ERR_UNSUPPORTED);
}

TEST_CASE("process net owner auto-opens sessions for create and platform bind",
          "[sdk][net][provider][process_owner]") {
    ProcessNetProviderReset reset;
    REQUIRE(sao_sdk_platform_net_configure_provider(nullptr) == SAO_SDK_OK);
    SaoSdkContext existing{};
    REQUIRE(sao_sdk_bind_context("net.owner.existing", "1.0", &existing) == SAO_SDK_OK);

    NetProviderFixture fixture;
    auto provider = make_net_provider(&fixture);
    REQUIRE(sao_sdk_platform_net_configure_provider(&provider) == SAO_SDK_OK);
    SaoSdkContext created{};
    REQUIRE(sao_sdk_bind_context("net.owner.created", "1.0", &created) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_bind_platform_services(&existing) == SAO_SDK_OK);
    CHECK(fixture.retain_count == 3);
    CHECK(fixture.open_count == 2);
    CHECK(fixture.live_sessions == 2);

    REQUIRE(sao_sdk_platform_net_configure_provider(nullptr) == SAO_SDK_OK);
    CHECK(fixture.release_count == 1);
    CHECK(fixture.live_sessions == 2);
    REQUIRE(sao_sdk_context_try_destroy(&created) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_try_destroy(&existing) == SAO_SDK_OK);
    CHECK(fixture.close_count == 2);
    CHECK(fixture.release_count == 3);
    CHECK(fixture.live_sessions == 0);
}

TEST_CASE("net capture callback validates borrowed packet and rejects self teardown",
          "[sdk][net][capture][reentry]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("net.capture", "1.0", &ctx) == SAO_SDK_OK);
    NetProviderFixture fixture;
    fixture.emit_packet_on_start = true;
    auto provider = make_net_provider(&fixture);
    REQUIRE(sao_sdk_context_configure_net_provider(&ctx, &provider) == SAO_SDK_OK);

    auto config = capture_config();
    PacketSink sink;
    sink.context = &ctx;
    REQUIRE(sao_sdk_net_capture_start(&ctx, &config, packet_sink_callback, &sink) == SAO_SDK_OK);
    CHECK(sink.callback_count == 1);
    CHECK(sink.observed_size == 20);
    CHECK(sink.observed_timestamp == 123456789);
    CHECK(sink.reentry_stop_status == SAO_SDK_ERR_BUSY);
    REQUIRE(sao_sdk_net_capture_stop(&ctx) == SAO_SDK_OK);

    fixture.emit_invalid_packet_on_start = true;
    REQUIRE(sao_sdk_net_capture_start(&ctx, &config, packet_sink_callback, &sink) == SAO_SDK_OK);
    CHECK(sink.callback_count == 1);
    REQUIRE(sao_sdk_net_capture_stop(&ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
}

TEST_CASE("net parse validates packet result ownership size stride and short packets",
          "[sdk][net][parse]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("net.parse", "1.0", &ctx) == SAO_SDK_OK);
    NetProviderFixture fixture;
    auto provider = make_net_provider(&fixture);
    REQUIRE(sao_sdk_context_configure_net_provider(&ctx, &provider) == SAO_SDK_OK);

    std::array<uint8_t, 20> bytes{};
    bytes[0] = 0x45;
    auto packet = packet_view(bytes.data(), bytes.size());
    size_t result_count = 0;
    CHECK(sao_sdk_net_parse_packet(&ctx, &packet, nullptr, 0,
                                   SAO_SDK_NET_PARSED_RESULT_ELEMENT_SIZE - 1,
                                   &result_count) == SAO_SDK_ERR_INVALID_ARGUMENT);
    CHECK(fixture.parse_count == 0);

    auto short_packet = packet_view(bytes.data(), bytes.size() - 1);
    CHECK(sao_sdk_net_parse_packet(&ctx, &short_packet, nullptr, 0,
                                   SAO_SDK_NET_PARSED_RESULT_ELEMENT_SIZE,
                                   &result_count) == SAO_SDK_ERR_INVALID_ARGUMENT);
    CHECK(fixture.parse_count == 0);

    CHECK(sao_sdk_net_parse_packet(&ctx, &packet, nullptr, 0,
                                   SAO_SDK_NET_PARSED_RESULT_ELEMENT_SIZE,
                                   &result_count) == SAO_SDK_ERR_BUFFER_TOO_SMALL);
    CHECK(result_count == 1);
    std::vector<SaoSdkNetParsedResult> results(result_count);
    REQUIRE(sao_sdk_net_parse_packet(&ctx, &packet, results.data(), results.size(),
                                     SAO_SDK_NET_PARSED_RESULT_ELEMENT_SIZE,
                                     &result_count) == SAO_SDK_OK);
    REQUIRE(result_count == 1);
    CHECK(results[0].payload_offset == 4);
    CHECK(results[0].payload_size == 16);
    CHECK(results[0].flow_id == 0x1122334455667788ull);

    fixture.invalid_result_range = true;
    CHECK(sao_sdk_net_parse_packet(&ctx, &packet, results.data(), results.size(),
                                   SAO_SDK_NET_PARSED_RESULT_ELEMENT_SIZE,
                                   &result_count) == SAO_SDK_ERR_INTERNAL);
    CHECK(result_count == 0);
    CHECK(results[0].struct_size == 0);
    fixture.invalid_result_range = false;
    fixture.invalid_result_abi = true;
    CHECK(sao_sdk_net_parse_packet(&ctx, &packet, results.data(), results.size(),
                                   SAO_SDK_NET_PARSED_RESULT_ELEMENT_SIZE,
                                   &result_count) == SAO_SDK_ERR_ABI_MISMATCH);
    CHECK(result_count == 0);
    CHECK(results[0].struct_size == 0);
    fixture.invalid_result_abi = false;
    fixture.result_count = 4097;
    CHECK(sao_sdk_net_parse_packet(&ctx, &packet, nullptr, 0,
                                   SAO_SDK_NET_PARSED_RESULT_ELEMENT_SIZE,
                                   &result_count) == SAO_SDK_ERR_INTERNAL);
    CHECK(result_count == 0);
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
}

TEST_CASE("net provider callback reentry fails busy without replacement or self wait",
          "[sdk][net][provider][reentry]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("net.reentry", "1.0", &ctx) == SAO_SDK_OK);
    NetProviderFixture active;
    NetProviderFixture replacement;
    auto active_provider = make_net_provider(&active);
    auto replacement_provider = make_net_provider(&replacement);
    active.reenter_parse = true;
    active.reentry_context = &ctx;
    active.reentry_replacement = &replacement_provider;
    REQUIRE(sao_sdk_context_configure_net_provider(&ctx, &active_provider) == SAO_SDK_OK);

    std::array<uint8_t, 20> bytes{};
    auto packet = packet_view(bytes.data(), bytes.size());
    size_t result_count = 0;
    CHECK(sao_sdk_net_parse_packet(&ctx, &packet, nullptr, 0,
                                   SAO_SDK_NET_PARSED_RESULT_ELEMENT_SIZE,
                                   &result_count) == SAO_SDK_ERR_BUFFER_TOO_SMALL);
    CHECK(active.reentry_parse_status == SAO_SDK_ERR_BUSY);
    CHECK(active.reentry_clear_status == SAO_SDK_ERR_BUSY);
    CHECK(active.reentry_replace_status == SAO_SDK_ERR_BUSY);
    CHECK(replacement.open_count == 0);

    active.reenter_parse = false;
    REQUIRE(sao_sdk_context_configure_net_provider(&ctx, nullptr) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
}

TEST_CASE("net provider replacement is transactional and cleanup failures retry",
          "[sdk][net][provider][replacement][cleanup]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("net.replace", "1.0", &ctx) == SAO_SDK_OK);
    NetProviderFixture original;
    NetProviderFixture replacement;
    auto original_provider = make_net_provider(&original);
    auto replacement_provider = make_net_provider(&replacement);
    REQUIRE(sao_sdk_context_configure_net_provider(&ctx, &original_provider) == SAO_SDK_OK);

    auto config = capture_config();
    PacketSink sink;
    REQUIRE(sao_sdk_net_capture_start(&ctx, &config, packet_sink_callback, &sink) == SAO_SDK_OK);
    original.stop_status = SAO_SDK_ERR_INTERNAL;
    CHECK(sao_sdk_context_configure_net_provider(&ctx, &replacement_provider) ==
          SAO_SDK_ERR_INTERNAL);
    CHECK(sao_sdk_context_net_provider_status(&ctx) == SAO_SDK_ERR_INTERNAL);
    CHECK(original.live_sessions == 1);
    CHECK(original.close_count == 0);
    CHECK(replacement.open_count == 1);
    CHECK(replacement.close_count == 1);
    CHECK(replacement.release_count == 1);

    original.stop_status = SAO_SDK_OK;
    REQUIRE(sao_sdk_context_configure_net_provider(&ctx, &replacement_provider) == SAO_SDK_OK);
    CHECK(original.stop_count == 2);
    CHECK(original.close_count == 1);
    CHECK(original.release_count == 1);
    CHECK(replacement.live_sessions == 1);

    const auto close_count_before_retry = replacement.close_count.load();
    const auto release_count_before_retry = replacement.release_count.load();
    replacement.close_status = SAO_SDK_ERR_INTERNAL;
    CHECK(sao_sdk_context_configure_net_provider(&ctx, nullptr) == SAO_SDK_ERR_INTERNAL);
    CHECK(replacement.live_sessions == 1);
    CHECK(replacement.close_count == close_count_before_retry + 1);
    CHECK(replacement.release_count == release_count_before_retry);
    replacement.close_status = SAO_SDK_OK;
    REQUIRE(sao_sdk_context_configure_net_provider(&ctx, nullptr) == SAO_SDK_OK);
    CHECK(replacement.close_count == close_count_before_retry + 2);
    CHECK(replacement.release_count == release_count_before_retry + 1);
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
}

TEST_CASE("net provider open and cleanup failures preserve ownership for retry",
          "[sdk][net][provider][cleanup][quarantine]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("net.quarantine", "1.0", &ctx) == SAO_SDK_OK);
    NetProviderFixture fixture;
    fixture.open_status = SAO_SDK_ERR_NOT_INITIALIZED;
    fixture.open_returns_session_on_failure = true;
    fixture.close_status = SAO_SDK_ERR_INTERNAL;
    auto provider = make_net_provider(&fixture);

    CHECK(sao_sdk_context_configure_net_provider(&ctx, &provider) == SAO_SDK_ERR_INTERNAL);
    CHECK(fixture.live_sessions == 1);
    CHECK(fixture.release_count == 0);
    CHECK(sao_sdk_context_net_provider_status(&ctx) == SAO_SDK_ERR_INTERNAL);
    fixture.close_status = SAO_SDK_OK;
    REQUIRE(sao_sdk_context_configure_net_provider(&ctx, nullptr) == SAO_SDK_OK);
    CHECK(fixture.live_sessions == 0);
    CHECK(fixture.release_count == 1);
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
}

TEST_CASE("memory and net sessions coexist and destroy retries without touching later providers",
          "[sdk][memory][net][destroy][retry]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("memory.net.destroy.retry", "1.0", &ctx) == SAO_SDK_OK);
    CoexistMemoryFixture memory;
    auto memory_provider = make_coexist_memory_provider(&memory);
    NetProviderFixture net;
    auto net_provider = make_net_provider(&net);
    REQUIRE(sao_sdk_context_configure_memory_provider(&ctx, &memory_provider) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_configure_net_provider(&ctx, &net_provider) == SAO_SDK_OK);

    net.close_status = SAO_SDK_ERR_INTERNAL;
    CHECK(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_ERR_INTERNAL);
    CHECK(ctx.ctx_impl != nullptr);
    CHECK(net.live_sessions == 1);
    CHECK(memory.live_sessions == 1);
    CHECK(memory.release_count == 0);

    net.close_status = SAO_SDK_OK;
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
    CHECK(net.live_sessions == 0);
    CHECK(memory.live_sessions == 0);
    CHECK(memory.release_count == 1);
}

TEST_CASE("destroy preflight sees memory callback before cleaning a coexisting net provider",
          "[sdk][memory][net][destroy][reentry]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("memory.net.destroy.reentry", "1.0", &ctx) == SAO_SDK_OK);
    CoexistMemoryFixture memory;
    memory.reentry_context = &ctx;
    memory.reenter_destroy = true;
    auto memory_provider = make_coexist_memory_provider(&memory);
    NetProviderFixture net;
    auto net_provider = make_net_provider(&net);
    REQUIRE(sao_sdk_context_configure_memory_provider(&ctx, &memory_provider) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_configure_net_provider(&ctx, &net_provider) == SAO_SDK_OK);
    SaoSdkMemoryTargetIdentity identity{};
    identity.struct_size = sizeof(identity);
    identity.abi_version = SAO_SDK_MEMORY_PROVIDER_ABI_VERSION;
    identity.process_id = 404;
    REQUIRE(sao_sdk_mem_attach(&ctx, &identity) == SAO_SDK_OK);

    uint8_t value = 0;
    size_t bytes_read = 0;
    CHECK(sao_sdk_mem_read(&ctx, 0x1000, &value, sizeof(value), &bytes_read) ==
          SAO_SDK_ERR_READ_FAULT);
    CHECK(memory.destroy_status == SAO_SDK_ERR_BUSY);
    CHECK(memory.live_sessions == 1);
    CHECK(net.live_sessions == 1);
    CHECK(net.close_count == 0);

    memory.reenter_destroy = false;
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
    CHECK(memory.live_sessions == 0);
    CHECK(net.live_sessions == 0);
}

TEST_CASE("net provider exceptions map to internal at every C ABI callback",
          "[sdk][net][provider][exception]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("net.exception", "1.0", &ctx) == SAO_SDK_OK);
    NetProviderFixture fixture;
    auto provider = make_net_provider(&fixture);
    REQUIRE(sao_sdk_context_configure_net_provider(&ctx, &provider) == SAO_SDK_OK);

    auto config = capture_config();
    PacketSink sink;
    fixture.throw_start = true;
    CHECK(sao_sdk_net_capture_start(&ctx, &config, packet_sink_callback, &sink) ==
          SAO_SDK_ERR_INTERNAL);
    fixture.throw_start = false;
    REQUIRE(sao_sdk_net_capture_start(&ctx, &config, packet_sink_callback, &sink) == SAO_SDK_OK);

    fixture.throw_stop = true;
    CHECK(sao_sdk_net_capture_stop(&ctx) == SAO_SDK_ERR_INTERNAL);
    fixture.throw_stop = false;
    REQUIRE(sao_sdk_net_capture_stop(&ctx) == SAO_SDK_OK);

    std::array<uint8_t, 20> bytes{};
    auto packet = packet_view(bytes.data(), bytes.size());
    size_t result_count = 0;
    fixture.throw_parse = true;
    CHECK(sao_sdk_net_parse_packet(&ctx, &packet, nullptr, 0,
                                   SAO_SDK_NET_PARSED_RESULT_ELEMENT_SIZE,
                                   &result_count) == SAO_SDK_ERR_INTERNAL);
    CHECK(result_count == 0);
    fixture.throw_parse = false;

    fixture.throw_close = true;
    CHECK(sao_sdk_context_configure_net_provider(&ctx, nullptr) == SAO_SDK_ERR_INTERNAL);
    CHECK(fixture.release_count == 0);
    fixture.throw_close = false;
    fixture.throw_release = true;
    CHECK(sao_sdk_context_configure_net_provider(&ctx, nullptr) == SAO_SDK_ERR_INTERNAL);
    CHECK(fixture.release_count == 1);
    fixture.throw_release = false;
    REQUIRE(sao_sdk_context_configure_net_provider(&ctx, nullptr) == SAO_SDK_OK);
    CHECK(fixture.release_count == 2);
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);

    SaoSdkContext open_ctx{};
    REQUIRE(sao_sdk_bind_context("net.exception.open", "1.0", &open_ctx) == SAO_SDK_OK);
    NetProviderFixture open_fixture;
    open_fixture.throw_open = true;
    auto open_provider = make_net_provider(&open_fixture);
    CHECK(sao_sdk_context_configure_net_provider(&open_ctx, &open_provider) ==
          SAO_SDK_ERR_INTERNAL);
    CHECK(open_fixture.retain_count == 1);
    CHECK(open_fixture.release_count == 1);
    REQUIRE(sao_sdk_context_try_destroy(&open_ctx) == SAO_SDK_OK);

    SaoSdkContext retain_ctx{};
    REQUIRE(sao_sdk_bind_context("net.exception.retain", "1.0", &retain_ctx) == SAO_SDK_OK);
    NetProviderFixture retain_fixture;
    retain_fixture.throw_retain = true;
    auto retain_provider = make_net_provider(&retain_fixture);
    CHECK(sao_sdk_context_configure_net_provider(&retain_ctx, &retain_provider) ==
          SAO_SDK_ERR_INTERNAL);
    CHECK(retain_fixture.open_count == 0);
    CHECK(retain_fixture.live_sessions == 0);
    REQUIRE(sao_sdk_context_try_destroy(&retain_ctx) == SAO_SDK_OK);
}
