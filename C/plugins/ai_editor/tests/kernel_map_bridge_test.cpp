// SAO AI Editor - kernel-map bridge + tool registry unit tests.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "sao/ai_editor/ai_editor_status.h"
#include "sao/ai_editor/kernel_map_bridge.h"
#include "sao/core/status.h"
#include "sao/rt_io/kernel_map_wire/proxy.h"
#include "sao/rt_io/kernel_map_wire/wire.h"

#include "../src/kernel_map_commands.h"
#include "../src/kernel_map_tools.h"
#include "../src/native_tool_registry.h"
#include "../src/scope_store.h"

// Stub definitions for the four gpu_hunt_bridge entry points that
// native_tool_registry.cpp hard-depends on.  The kernel_map test
// target compiles the registry directly (so the rt_io shim can reach
// the bridge inside the same static library instance) and would
// otherwise fail to link against gpu_hunt symbols.  Since these tests
// never exercise gpu_hunt.* tools, the stubs are trivial.
namespace sao::ai_editor::native {
bool is_gpu_hunt_tool_name(std::string_view) noexcept { return false; }
nlohmann::json gpu_hunt_tool_schema(std::string_view) {
    return nlohmann::json{};
}
void append_gpu_hunt_tool_descriptors(nlohmann::json&) {}
int32_t dispatch_gpu_hunt_tool(std::string_view,
                                const nlohmann::json&,
                                nlohmann::json&) {
    return SAO_AI_EDITOR_ERR_NOT_FOUND;
}
}  // namespace sao::ai_editor::native

namespace km = sao::ai_editor::kernel_map;
namespace nt = sao::ai_editor::native;

namespace {

using Json = nlohmann::json;

// Recorded wire hop the shim captures on every proxy call so tests
// can assert opcode + payload byte-by-byte.
struct WireHop {
    uint8_t cmd = 0;
    std::vector<uint8_t> payload;
};

// Recorder is captured by the shim through the user pointer.  We
// keep the mutex + vector inline so multiple tests can install / reset
// a fresh recorder without static state bleeding between cases.
struct WireRecorder {
    std::mutex mutex;
    std::vector<WireHop> hops;

    // Optional per-cmd reply builder.  Each entry returns the payload
    // bytes for the given cmd; nullopt = empty reply.
    std::vector<uint8_t> next_status_reply;    // KMOP_STATUS reply body
    std::vector<uint8_t> next_map_reply;       // KMOP_MAP reply body
    std::vector<uint8_t> next_enumerate_reply; // KMOP_ENUMERATE reply body
    // status byte returned for the next call (default = OK)
    uint8_t next_status_byte = 0;
};

sao_status_t SAO_RT_IO_CALL recording_shim(uint8_t cmd,
                                            const uint8_t* payload,
                                            size_t payload_size,
                                            uint8_t* out_status,
                                            uint8_t* out_payload,
                                            size_t out_cap,
                                            size_t* out_used,
                                            void* user) {
    auto* recorder = static_cast<WireRecorder*>(user);
    if (recorder == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    // Record the raw hop first so tests that only care about the
    // request payload can inspect it regardless of what the shim
    // decides to reply.
    WireHop hop;
    hop.cmd = cmd;
    if (payload != nullptr && payload_size > 0) {
        hop.payload.assign(payload, payload + payload_size);
    }

    // Build the reply body for the recorded opcode; the caller
    // provided an output buffer + cap and expects us to report how
    // many bytes we wrote through out_used.
    const std::vector<uint8_t>* reply_body = nullptr;
    switch (cmd) {
    case SAO_RT_IO_KMOP_STATUS:
        reply_body = &recorder->next_status_reply;
        break;
    case SAO_RT_IO_KMOP_MAP:
        reply_body = &recorder->next_map_reply;
        break;
    case SAO_RT_IO_KMOP_ENUMERATE:
        reply_body = &recorder->next_enumerate_reply;
        break;
    default:
        reply_body = nullptr;
        break;
    }

    if (out_status != nullptr) {
        *out_status = recorder->next_status_byte;
    }
    size_t written = 0;
    if (reply_body != nullptr && !reply_body->empty()) {
        if (out_payload == nullptr || out_cap < reply_body->size()) {
            std::lock_guard<std::mutex> guard(recorder->mutex);
            recorder->hops.push_back(std::move(hop));
            return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        }
        std::memcpy(out_payload, reply_body->data(), reply_body->size());
        written = reply_body->size();
    }
    if (out_used != nullptr) {
        *out_used = written;
    }

    {
        std::lock_guard<std::mutex> guard(recorder->mutex);
        recorder->hops.push_back(std::move(hop));
    }
    return SAO_STATUS_OK;
}

class ShimGuard final {
public:
    explicit ShimGuard(WireRecorder& recorder) : recorder_(&recorder) {
        sao_rt_io_kernel_map_proxy_test_install(&recording_shim, &recorder);
    }
    ~ShimGuard() {
        sao_rt_io_kernel_map_proxy_test_install(nullptr, nullptr);
    }
    ShimGuard(const ShimGuard&) = delete;
    ShimGuard& operator=(const ShimGuard&) = delete;

private:
    WireRecorder* recorder_;
};

// Fresh bridge per test so the internal "last activation snapshot"
// does not leak between cases.  The shared_bridge() singleton would
// tie all tests together and turn idempotency assertions into a
// serialisation puzzle.
std::unique_ptr<km::Bridge> make_bridge() {
    return std::make_unique<km::Bridge>();
}

// Interpret the raw payload for KMOP_ACTIVATE.
SaoRtIoKmActivateReq read_activate(const std::vector<uint8_t>& payload) {
    REQUIRE(payload.size() == sizeof(SaoRtIoKmActivateReq));
    SaoRtIoKmActivateReq req{};
    std::memcpy(&req, payload.data(), sizeof(req));
    return req;
}

// Interpret the raw payload for KMOP_UNMAP.
SaoRtIoKmUnmapReq read_unmap(const std::vector<uint8_t>& payload) {
    REQUIRE(payload.size() == sizeof(SaoRtIoKmUnmapReq));
    SaoRtIoKmUnmapReq req{};
    std::memcpy(&req, payload.data(), sizeof(req));
    return req;
}

// Interpret the raw payload for KMOP_MAP header + inline body.
std::pair<SaoRtIoKmMapReqHeader, std::vector<uint8_t>> read_map(
    const std::vector<uint8_t>& payload) {
    REQUIRE(payload.size() >= sizeof(SaoRtIoKmMapReqHeader));
    SaoRtIoKmMapReqHeader header{};
    std::memcpy(&header, payload.data(), sizeof(header));
    std::vector<uint8_t> body(
        payload.begin() + sizeof(SaoRtIoKmMapReqHeader), payload.end());
    return {header, std::move(body)};
}

}  // namespace

TEST_CASE("kernel_map_bridge: activate forwards to proxy",
          "[plugins][ai_editor][kernel_map]") {
    WireRecorder recorder;
    ShimGuard guard(recorder);

    auto bridge = make_bridge();
    const int32_t status = bridge->activate(
        /*invoke_result_slot_va=*/0x1122334455667788ull,
        /*idle_timeout_ms=*/45000u,
        /*pool_tag_seed=*/0xA5A5A5A5A5A5A5A5ull);
    REQUIRE(status == SAO_AI_EDITOR_OK);

    REQUIRE(recorder.hops.size() == 1);
    const auto& hop = recorder.hops.front();
    REQUIRE(hop.cmd == SAO_RT_IO_KMOP_ACTIVATE);
    const auto req = read_activate(hop.payload);
    REQUIRE(req.invoke_result_slot_va == 0x1122334455667788ull);
    REQUIRE(req.idle_timeout_ms == 45000u);
    REQUIRE(req.pool_tag_seed == 0xA5A5A5A5A5A5A5A5ull);
}

TEST_CASE("kernel_map_bridge: idempotent activate",
          "[plugins][ai_editor][kernel_map]") {
    WireRecorder recorder;
    ShimGuard guard(recorder);

    auto bridge = make_bridge();
    REQUIRE(bridge->activate(0xdeadbeefull, 12345u, 42u) == SAO_AI_EDITOR_OK);
    // Same params -> no second wire hop, still OK.
    REQUIRE(bridge->activate(0xdeadbeefull, 12345u, 42u) == SAO_AI_EDITOR_OK);
    REQUIRE(recorder.hops.size() == 1);

    // Different params -> new wire call.
    REQUIRE(bridge->activate(0xdeadbeefull, 12345u, 43u) == SAO_AI_EDITOR_OK);
    REQUIRE(recorder.hops.size() == 2);
    REQUIRE(recorder.hops.back().cmd == SAO_RT_IO_KMOP_ACTIVATE);
}

TEST_CASE("kernel_map_bridge: status translates reply",
          "[plugins][ai_editor][kernel_map]") {
    WireRecorder recorder;
    // Preload a KMOP_STATUS reply: active=1, map_count=7.
    SaoRtIoKmStatusReply reply{};
    reply.active = 1;
    reply.map_count = 7;
    recorder.next_status_reply.resize(sizeof(reply));
    std::memcpy(recorder.next_status_reply.data(), &reply, sizeof(reply));
    ShimGuard guard(recorder);

    auto bridge = make_bridge();
    km::BridgeStatus snap{};
    const int32_t status = bridge->status(snap);
    REQUIRE(status == SAO_AI_EDITOR_OK);
    REQUIRE(snap.active == true);
    REQUIRE(snap.map_count == 7u);

    REQUIRE(recorder.hops.size() == 1);
    REQUIRE(recorder.hops.front().cmd == SAO_RT_IO_KMOP_STATUS);
    REQUIRE(recorder.hops.front().payload.empty());
}

TEST_CASE("kernel_map_bridge: map enforces NO_INVOKE_ENTRY on-wire",
          "[plugins][ai_editor][kernel_map]") {
    WireRecorder recorder;
    // Preload a KMOP_MAP reply body so the bridge can decode
    // target_base + entry_status.
    SaoRtIoKmMapReply reply{};
    reply.target_base = 0xFFFFC00012345000ull;
    reply.entry_status = 0;  // NO_INVOKE_ENTRY -> mapper reports 0
    recorder.next_map_reply.resize(sizeof(reply));
    std::memcpy(recorder.next_map_reply.data(), &reply, sizeof(reply));
    ShimGuard guard(recorder);

    auto bridge = make_bridge();
    const std::vector<uint8_t> driver_bytes(4096, 0x42);
    km::MapResult result{};
    const int32_t status = bridge->map(
        driver_bytes.data(),
        static_cast<uint32_t>(driver_bytes.size()), result);
    REQUIRE(status == SAO_AI_EDITOR_OK);
    REQUIRE(result.target_base == 0xFFFFC00012345000ull);
    REQUIRE(result.entry_status == 0);

    REQUIRE(recorder.hops.size() == 1);
    const auto& hop = recorder.hops.front();
    REQUIRE(hop.cmd == SAO_RT_IO_KMOP_MAP);
    const auto [header, body] = read_map(hop.payload);
    // The NO_INVOKE_ENTRY bit MUST be set in the flags field the
    // bridge emits on-wire.  The proxy layer also ORs the same
    // constant on unconditionally so this assertion holds even if the
    // bridge accidentally forgot to set it.  Value 0x2 mirrors
    // SAO_MMD_FLAG_NO_INVOKE_ENTRY from
    // sao_security/driver_loader/manual_map_driver.h.
    REQUIRE((header.flags & 0x2u) == 0x2u);
    REQUIRE(header.driver_len == driver_bytes.size());
    REQUIRE(body.size() == driver_bytes.size());
    REQUIRE(body == driver_bytes);
}

TEST_CASE("kernel_map_bridge: map rejects oversized driver",
          "[plugins][ai_editor][kernel_map]") {
    WireRecorder recorder;
    ShimGuard guard(recorder);

    auto bridge = make_bridge();
    // 32 MiB + 1 byte - one over the wire-layer cap.
    const uint32_t oversized =
        SAO_RT_IO_KMOP_MAX_DRIVER_BYTES + 1u;
    // Only stub the size check with a single-byte buffer + oversized
    // length; the bridge must reject before dereferencing beyond the
    // real buffer.
    const std::vector<uint8_t> tiny_buffer(1, 0x00);
    km::MapResult result{};
    const int32_t status =
        bridge->map(tiny_buffer.data(), oversized, result);
    REQUIRE(status == SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
    // Shim must not have been called - the rejection is pre-wire.
    REQUIRE(recorder.hops.empty());
}

TEST_CASE("kernel_map_bridge: unmap forwards target_base",
          "[plugins][ai_editor][kernel_map]") {
    WireRecorder recorder;
    ShimGuard guard(recorder);

    auto bridge = make_bridge();
    const uint64_t base = 0xFFFF800012345678ull;
    REQUIRE(bridge->unmap(base) == SAO_AI_EDITOR_OK);

    REQUIRE(recorder.hops.size() == 1);
    REQUIRE(recorder.hops.front().cmd == SAO_RT_IO_KMOP_UNMAP);
    const auto req = read_unmap(recorder.hops.front().payload);
    REQUIRE(req.target_base == base);

    // Zero base must be rejected before the wire.
    recorder.hops.clear();
    REQUIRE(bridge->unmap(0) == SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
    REQUIRE(recorder.hops.empty());
}

TEST_CASE("kernel_map_bridge: enumerate returns bases",
          "[plugins][ai_editor][kernel_map]") {
    WireRecorder recorder;
    // The bridge issues a count-only call first, then a real call.
    // Simplest way to satisfy both is to preload a reply payload
    // that carries header {count=3, _pad=0} + three u64 bases.  The
    // recording shim replays the SAME reply for both hops, which is
    // fine here because both share the same body shape (the
    // implementation of the wire layer decides which fields are
    // consulted on each hop).
    SaoRtIoKmEnumerateReplyHeader header{};
    header.count = 3;
    header._pad = 0;
    const std::vector<uint64_t> synthetic_bases = {
        0xFFFFC00000000010ull,
        0xFFFFC00000000020ull,
        0xFFFFC00000000030ull,
    };
    recorder.next_enumerate_reply.resize(
        sizeof(header) + synthetic_bases.size() * sizeof(uint64_t));
    std::memcpy(recorder.next_enumerate_reply.data(),
                &header, sizeof(header));
    std::memcpy(
        recorder.next_enumerate_reply.data() + sizeof(header),
        synthetic_bases.data(),
        synthetic_bases.size() * sizeof(uint64_t));
    ShimGuard guard(recorder);

    auto bridge = make_bridge();
    std::vector<uint64_t> out;
    const int32_t status = bridge->enumerate(out);
    // Some wire-layer implementations answer the count-only pass by
    // decoding the reply fully; either way the bridge must return
    // OK and hand back the three synthetic bases when the shim
    // provides them.  We accept both "one hop" and "two hops"
    // implementations so the test does not over-constrain the wire
    // contract.
    REQUIRE(status == SAO_AI_EDITOR_OK);
    REQUIRE(out.size() == synthetic_bases.size());
    for (size_t i = 0; i < out.size(); ++i) {
        REQUIRE(out[i] == synthetic_bases[i]);
    }
    // At least one hop happened, and every hop that did happen was
    // KMOP_ENUMERATE.
    REQUIRE(!recorder.hops.empty());
    for (const auto& hop : recorder.hops) {
        REQUIRE(hop.cmd == SAO_RT_IO_KMOP_ENUMERATE);
        REQUIRE(hop.payload.empty());
    }
}

TEST_CASE("kernel_map_tools: registration creates four tools",
          "[plugins][ai_editor][kernel_map]") {
    // The registry validates + persists the four descriptors via
    // register_custom, which is a thin no-side-effect
    // in-memory map.  Building it with an empty ScopeStore is fine
    // for this test because we never call execute() on a builtin;
    // describe() only inspects the descriptor tables.
    nt::ScopeStore scopes;
    nt::NativeToolRegistry registry(scopes, 0u, 0u);

    auto bridge = make_bridge();
    km::register_kernel_map_tools(registry, *bridge);

    const Json descriptors = registry.describe("chat");
    REQUIRE(descriptors.is_array());

    std::vector<std::string> observed_names;
    for (const auto& entry : descriptors) {
        if (!entry.is_object() || !entry.contains("name")) {
            continue;
        }
        const std::string name = entry["name"].get<std::string>();
        if (name.rfind("kernelMap.", 0) == 0) {
            observed_names.push_back(name);
        }
    }

    const std::vector<std::string> expected = {
        "kernelMap.status",
        "kernelMap.enumerate",
        "kernelMap.map",
        "kernelMap.unmap",
    };
    REQUIRE(observed_names.size() == expected.size());
    for (const auto& name : expected) {
        bool found = false;
        for (const auto& observed : observed_names) {
            if (observed == name) {
                found = true;
                break;
            }
        }
        REQUIRE(found);
    }
}

TEST_CASE("kernel_map_tools: registration is idempotent",
          "[plugins][ai_editor][kernel_map]") {
    nt::ScopeStore scopes;
    nt::NativeToolRegistry registry(scopes, 0u, 0u);
    auto bridge = make_bridge();

    km::register_kernel_map_tools(registry, *bridge);
    km::register_kernel_map_tools(registry, *bridge);

    const Json descriptors = registry.describe("chat");
    size_t count = 0;
    for (const auto& entry : descriptors) {
        if (!entry.is_object() || !entry.contains("name")) {
            continue;
        }
        const std::string name = entry["name"].get<std::string>();
        if (name.rfind("kernelMap.", 0) == 0) {
            ++count;
        }
    }
    REQUIRE(count == 4);
}

TEST_CASE("kernel_map_tools: exec kernelMap.status intentionally not "
          "reachable through registry",
          "[plugins][ai_editor][kernel_map]") {
    // NativeToolRegistry::execute() has no custom-exec seam today (see
    // the header TODO in kernel_map_tools.h).  Calling a custom tool
    // returns an echo of the request payload, NOT a live bridge call.
    // We assert the current behaviour so the follow-up ticket can
    // flip this test into "goes through the bridge" once the seam
    // lands, and any accidental introduction of a live path breaks
    // this case loud.
    nt::ScopeStore scopes;
    nt::NativeToolRegistry registry(scopes, 0u, 0u);
    auto bridge = make_bridge();
    km::register_kernel_map_tools(registry, *bridge);

    Json result;
    const int32_t status =
        registry.execute("chat", "kernelMap.status",
                          Json::object(), result);
    REQUIRE(status == SAO_AI_EDITOR_OK);
    REQUIRE(result.is_object());
    // Echo passthrough behaviour: the response carries
    // `{custom: true, name: "kernelMap.status", arguments: {...}}`.
    REQUIRE(result.value("custom", false) == true);
    REQUIRE(result.value("name", std::string{}) == "kernelMap.status");
}

TEST_CASE("kernel_map_commands: status returns bridge snapshot",
          "[plugins][ai_editor][kernel_map]") {
    WireRecorder recorder;
    SaoRtIoKmStatusReply reply{};
    reply.active = 1;
    reply.map_count = 5;
    recorder.next_status_reply.resize(sizeof(reply));
    std::memcpy(recorder.next_status_reply.data(), &reply, sizeof(reply));
    ShimGuard guard(recorder);

    // Use shared_bridge() because handle_kernel_map_command() falls
    // back to it when nothing was explicitly registered.  Clear the
    // adapter state first via a fresh deactivate() so a previously
    // cached activation snapshot in another test does not bleed in.
    (void)km::shared_bridge().deactivate();

    Json out;
    const int32_t status =
        km::handle_kernel_map_command("sao.kernelMap.status",
                                       Json::object(), out);
    REQUIRE(status == SAO_AI_EDITOR_OK);
    REQUIRE(out.value("ok", false) == true);
    REQUIRE(out.value("active", false) == true);
    REQUIRE(out.value("map_count", 0u) == 5u);
}

TEST_CASE("kernel_map_commands: enumerate reports hex bases",
          "[plugins][ai_editor][kernel_map]") {
    WireRecorder recorder;
    SaoRtIoKmEnumerateReplyHeader header{};
    header.count = 2;
    header._pad = 0;
    const std::vector<uint64_t> bases = {
        0xFFFFC00000000001ull,
        0xFFFFC00000000002ull,
    };
    recorder.next_enumerate_reply.resize(
        sizeof(header) + bases.size() * sizeof(uint64_t));
    std::memcpy(recorder.next_enumerate_reply.data(),
                &header, sizeof(header));
    std::memcpy(recorder.next_enumerate_reply.data() + sizeof(header),
                bases.data(), bases.size() * sizeof(uint64_t));
    ShimGuard guard(recorder);

    Json out;
    const int32_t status =
        km::handle_kernel_map_command("sao.kernelMap.enumerate",
                                       Json::object(), out);
    REQUIRE(status == SAO_AI_EDITOR_OK);
    REQUIRE(out.value("ok", false) == true);
    REQUIRE(out.contains("bases"));
    REQUIRE(out["bases"].is_array());
    REQUIRE(out["bases"].size() == bases.size());
    // Each entry is a lowercase hex string.
    REQUIRE(out["bases"][0].get<std::string>() ==
            std::string("0xffffc00000000001"));
    REQUIRE(out["bases"][1].get<std::string>() ==
            std::string("0xffffc00000000002"));
}

TEST_CASE("kernel_map_commands: unknown command returns NOT_FOUND",
          "[plugins][ai_editor][kernel_map]") {
    Json out;
    const int32_t status =
        km::handle_kernel_map_command("sao.kernelMap.does_not_exist",
                                       Json::object(), out);
    REQUIRE(status == SAO_AI_EDITOR_ERR_NOT_FOUND);
    REQUIRE(out.value("ok", true) == false);
}

TEST_CASE("kernel_map_commands: unmap parses hex + decimal target_base",
          "[plugins][ai_editor][kernel_map]") {
    WireRecorder recorder;
    ShimGuard guard(recorder);

    Json out;
    Json args_hex{{"target_base", "0xFFFFC00012345678"}};
    REQUIRE(km::handle_kernel_map_command(
                "sao.kernelMap.unmap", args_hex, out) == SAO_AI_EDITOR_OK);
    REQUIRE(recorder.hops.size() == 1);
    REQUIRE(recorder.hops.front().cmd == SAO_RT_IO_KMOP_UNMAP);
    auto req = read_unmap(recorder.hops.front().payload);
    REQUIRE(req.target_base == 0xFFFFC00012345678ull);

    recorder.hops.clear();
    Json args_dec{{"target_base", "4096"}};
    REQUIRE(km::handle_kernel_map_command(
                "sao.kernelMap.unmap", args_dec, out) == SAO_AI_EDITOR_OK);
    REQUIRE(recorder.hops.size() == 1);
    req = read_unmap(recorder.hops.front().payload);
    REQUIRE(req.target_base == 4096u);

    // Missing target_base -> INVALID_ARGUMENT before the wire.
    recorder.hops.clear();
    Json args_missing = Json::object();
    REQUIRE(km::handle_kernel_map_command(
                "sao.kernelMap.unmap", args_missing, out) ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
    REQUIRE(recorder.hops.empty());
}
