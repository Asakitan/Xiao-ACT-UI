// SAO AI Editor - kernel-map bridge + tool registry unit tests.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "sao/ai_editor/ai_editor_status.h"
#include "sao/ai_editor/kernel_map_bridge.h"
#include "sao/core/status.h"
#include "sao/rt_io/kernel_map_wire/proxy.h"
#include "sao/rt_io/kernel_map_wire/wire.h"

#include "../src/extension_host.h"
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

// The focused bridge target compiles kernel_map_commands.cpp directly but
// does not link the full extension host.  These seams remain unused here;
// the dedicated runtime tests cover their real implementations.
namespace sao::ai_editor::native {
namespace {
struct CommandFailureInjection final {
    int32_t register_fail_call = 0;
    int32_t register_fail_count = 0;
    int32_t unregister_fail_call = 0;
    int32_t unregister_fail_count = 0;
    int32_t restore_fail_call = 0;
    int32_t restore_fail_count = 0;
    int32_t register_calls = 0;
    int32_t unregister_calls = 0;
    int32_t restore_calls = 0;
};

struct CommandHostState final {
    std::unordered_map<std::string, ExtensionHost::NativeCommandRegistration>
        commands;
};

std::unordered_map<const ExtensionHost*, CommandHostState>& command_hosts() {
    static std::unordered_map<const ExtensionHost*, CommandHostState> states;
    return states;
}

CommandFailureInjection& command_failures() {
    static CommandFailureInjection failures;
    return failures;
}

bool failure_active(int32_t call, int32_t first, int32_t count) {
    return first > 0 && count > 0 && call >= first && call < first + count;
}
}  // namespace

int32_t ExtensionHost::register_native_command(
    std::string_view command_id, NativeCommandHandler handler, uint64_t owner) {
    auto& failures = command_failures();
    ++failures.register_calls;
    if (failure_active(failures.register_calls, failures.register_fail_call,
                       failures.register_fail_count)) {
        return SAO_AI_EDITOR_ERR_BUSY;
    }
    auto& commands = command_hosts()[this].commands;
    const std::string id(command_id);
    const auto found = commands.find(id);
    if (found != commands.end() && found->second.owner != owner &&
        (found->second.owner != 0u || owner != 0u)) {
        return SAO_AI_EDITOR_ERR_BUSY;
    }
    commands[id] =
        NativeCommandRegistration{std::move(handler), owner};
    return SAO_AI_EDITOR_OK;
}

int32_t ExtensionHost::unregister_native_command(std::string_view command_id,
                                                 uint64_t owner) {
    auto& failures = command_failures();
    ++failures.unregister_calls;
    if (failure_active(failures.unregister_calls,
                       failures.unregister_fail_call,
                       failures.unregister_fail_count)) {
        return SAO_AI_EDITOR_ERR_BUSY;
    }
    auto& commands = command_hosts()[this].commands;
    const auto found = commands.find(std::string(command_id));
    if (found == commands.end()) return SAO_AI_EDITOR_ERR_NOT_FOUND;
    if (found->second.owner != owner) return SAO_AI_EDITOR_ERR_BUSY;
    commands.erase(found);
    return SAO_AI_EDITOR_OK;
}

std::optional<ExtensionHost::NativeCommandRegistration>
ExtensionHost::snapshot_native_command(std::string_view command_id) const {
    const auto host = command_hosts().find(this);
    if (host == command_hosts().end()) return std::nullopt;
    const auto found = host->second.commands.find(std::string(command_id));
    if (found == host->second.commands.end()) return std::nullopt;
    return found->second;
}

int32_t ExtensionHost::restore_native_command(
    std::string_view command_id,
    const std::optional<NativeCommandRegistration>& prior,
    uint64_t owner) {
    auto& failures = command_failures();
    ++failures.restore_calls;
    if (failure_active(failures.restore_calls, failures.restore_fail_call,
                       failures.restore_fail_count)) {
        return SAO_AI_EDITOR_ERR_BUSY;
    }
    auto& commands = command_hosts()[this].commands;
    const std::string id(command_id);
    const auto found = commands.find(id);
    if (found != commands.end() && found->second.owner != owner) {
        return SAO_AI_EDITOR_ERR_BUSY;
    }
    if (prior.has_value()) {
        commands[id] = *prior;
    } else if (found != commands.end()) {
        commands.erase(found);
    }
    return SAO_AI_EDITOR_OK;
}

void reset_kernel_map_command_test_host(const ExtensionHost& host) {
    command_hosts().erase(&host);
    command_failures() = CommandFailureInjection{};
}

void configure_kernel_map_command_test_failures(
    int32_t register_fail_call, int32_t register_fail_count,
    int32_t unregister_fail_call, int32_t unregister_fail_count,
    int32_t restore_fail_call, int32_t restore_fail_count) {
    command_failures() = CommandFailureInjection{
        register_fail_call, register_fail_count, unregister_fail_call,
        unregister_fail_count, restore_fail_call, restore_fail_count, 0, 0, 0};
}

size_t kernel_map_command_test_handler_count(const ExtensionHost& host) {
    const auto found = command_hosts().find(&host);
    return found == command_hosts().end() ? 0u : found->second.commands.size();
}
}  // namespace sao::ai_editor::native

namespace km = sao::ai_editor::kernel_map;
namespace nt = sao::ai_editor::native;

namespace {

using Json = nlohmann::json;

nt::ExtensionHost& command_test_host() {
    return *reinterpret_cast<nt::ExtensionHost*>(static_cast<uintptr_t>(0x1000));
}

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
std::shared_ptr<km::Bridge> make_bridge() {
    return std::make_shared<km::Bridge>();
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
    km::register_kernel_map_tools(registry, bridge);

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

TEST_CASE("kernel_map_tools: exec kernelMap.status reaches bridge",
          "[plugins][ai_editor][kernel_map]") {
    WireRecorder recorder;
    SaoRtIoKmStatusReply reply{};
    reply.active = 1;
    reply.map_count = 9;
    recorder.next_status_reply.resize(sizeof(reply));
    std::memcpy(recorder.next_status_reply.data(), &reply, sizeof(reply));
    ShimGuard guard(recorder);

    nt::ScopeStore scopes;
    nt::NativeToolRegistry registry(scopes, 0u, 0u);
    auto bridge = make_bridge();
    km::register_kernel_map_tools(registry, bridge);

    Json result;
    const int32_t status =
        registry.execute("chat", "kernelMap.status",
                          Json::object(), result);
    REQUIRE(status == SAO_AI_EDITOR_OK);
    REQUIRE(result.is_object());
    REQUIRE(result.value("ok", false) == true);
    REQUIRE(result.value("active", false) == true);
    REQUIRE(result.value("map_count", 0u) == 9u);
    REQUIRE(recorder.hops.size() == 1u);
    REQUIRE(recorder.hops.front().cmd == SAO_RT_IO_KMOP_STATUS);
}

    TEST_CASE("kernel_map_tools: registration retains bridge until unregister",
          "[plugins][ai_editor][kernel_map][registration]") {
    nt::ScopeStore scopes;
    nt::NativeToolRegistry registry(scopes, 0u, 0u);
    auto bridge = std::make_shared<km::Bridge>();
    std::weak_ptr<km::Bridge> weak = bridge;
    REQUIRE(km::register_kernel_map_tools(registry, bridge) == SAO_AI_EDITOR_OK);
    bridge.reset();
    REQUIRE_FALSE(weak.expired());
    auto owner = weak.lock();
    REQUIRE(owner != nullptr);
    REQUIRE(km::unregister_kernel_map_tools(registry, owner) == SAO_AI_EDITOR_OK);
    owner.reset();
    REQUIRE(weak.expired());
}

TEST_CASE("kernel_map_tools: mutating callbacks preserve mode gates",
          "[plugins][ai_editor][kernel_map]") {
        WireRecorder recorder;
        ShimGuard guard(recorder);

        nt::ScopeStore scopes;
        nt::NativeToolRegistry registry(scopes, 0u, 0u);
        auto bridge = make_bridge();
        km::register_kernel_map_tools(registry, bridge);

        Json result;
        const Json arguments{{"driver_path", "C:\\fixture.sys"}};
        REQUIRE(registry.execute("ask", "kernelMap.map", arguments, result) ==
            SAO_AI_EDITOR_ERR_PERMISSION_DENIED);
        REQUIRE(recorder.hops.empty());

        result = Json::object();
        REQUIRE(registry.execute("plan", "kernelMap.map", arguments, result) ==
            SAO_AI_EDITOR_ERR_CONFIRMATION_REQUIRED);
        REQUIRE(result.value("confirmationRequired", false) == true);
        REQUIRE(recorder.hops.empty());
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

TEST_CASE("kernel_map_commands: canonical uint64 parser rejects malformed input",
          "[plugins][ai_editor][kernel_map]") {
    uint64_t value = 0;
    REQUIRE(km::parse_kernel_map_uint64_text("0x10", value));
    REQUIRE(value == 16);
    REQUIRE(km::parse_kernel_map_uint64_text("10", value));
    REQUIRE(value == 10);
    for (const std::string text : {"", " 10", "10 ", "+10", "-1",
                                   "0x", "0x10junk",
                                   "18446744073709551616"}) {
        REQUIRE_FALSE(km::parse_kernel_map_uint64_text(text, value));
    }
    REQUIRE_FALSE(km::parse_kernel_map_uint64(Json(10), value));
    REQUIRE_FALSE(km::parse_kernel_map_uint64(Json(1.5), value));
    REQUIRE_FALSE(km::parse_kernel_map_uint64(Json(true), value));
}

TEST_CASE("kernel_map_commands: wrong argument types do not reach bridge",
          "[plugins][ai_editor][kernel_map]") {
    WireRecorder recorder;
    ShimGuard guard(recorder);
    Json out;
    REQUIRE(km::handle_kernel_map_command("sao.kernelMap.status",
                                           Json::array(), out) ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
    REQUIRE(km::handle_kernel_map_command(
                "sao.kernelMap.unmap", Json{{"target_base", 4096}}, out) ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
    REQUIRE(recorder.hops.empty());
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


TEST_CASE("kernel_map_commands: partial unregister retains owner for retry",
          "[plugins][ai_editor][kernel_map][registration][recovery]") {
    auto& host = command_test_host();
    nt::reset_kernel_map_command_test_host(host);
    auto bridge = make_bridge();
    REQUIRE(km::register_kernel_map_commands(host, bridge) == SAO_AI_EDITOR_OK);
    REQUIRE(nt::kernel_map_command_test_handler_count(host) == 6u);

    nt::configure_kernel_map_command_test_failures(
        0, 0, 2, 1, 0, 0);
    REQUIRE(km::unregister_kernel_map_commands(host) ==
            SAO_AI_EDITOR_ERR_BUSY);
    REQUIRE(nt::kernel_map_command_test_handler_count(host) != 0u);

    nt::configure_kernel_map_command_test_failures(0, 0, 0, 0, 0, 0);
    REQUIRE(km::unregister_kernel_map_commands(host) == SAO_AI_EDITOR_OK);
    REQUIRE(nt::kernel_map_command_test_handler_count(host) == 0u);
    REQUIRE(km::unregister_kernel_map_commands(host) ==
            SAO_AI_EDITOR_ERR_NOT_FOUND);
    nt::reset_kernel_map_command_test_host(host);
}

TEST_CASE("kernel_map_commands: final abandonment releases failed owner for address reuse",
          "[plugins][ai_editor][kernel_map][registration][teardown]") {
    auto& host = command_test_host();
    nt::reset_kernel_map_command_test_host(host);
    auto bridge = make_bridge();
    std::weak_ptr<km::Bridge> weak = bridge;
    REQUIRE(km::register_kernel_map_commands(host, bridge) == SAO_AI_EDITOR_OK);

    nt::configure_kernel_map_command_test_failures(
        0, 0, 2, 1, 0, 0);
    REQUIRE(km::unregister_kernel_map_commands(host) ==
            SAO_AI_EDITOR_ERR_BUSY);
    bridge.reset();
    REQUIRE_FALSE(weak.expired());

    km::abandon_kernel_map_commands(host);
    km::abandon_kernel_map_commands(host);
    nt::reset_kernel_map_command_test_host(host);
    REQUIRE(weak.expired());
    REQUIRE(km::unregister_kernel_map_commands(host) ==
            SAO_AI_EDITOR_ERR_NOT_FOUND);

    auto replacement_bridge = make_bridge();
    REQUIRE(km::register_kernel_map_commands(host, replacement_bridge) ==
            SAO_AI_EDITOR_OK);
    REQUIRE(nt::kernel_map_command_test_handler_count(host) == 6u);
    REQUIRE(km::unregister_kernel_map_commands(host) == SAO_AI_EDITOR_OK);
    nt::reset_kernel_map_command_test_host(host);
}

TEST_CASE("kernel_map_commands: failed registration rollback is retryable",
          "[plugins][ai_editor][kernel_map][registration][recovery]") {
    auto& host = command_test_host();
    nt::reset_kernel_map_command_test_host(host);
    auto bridge = make_bridge();

    nt::configure_kernel_map_command_test_failures(
        4, 1, 0, 0, 1, 1);
    REQUIRE(km::register_kernel_map_commands(host, bridge) ==
            SAO_AI_EDITOR_ERR_BUSY);
    REQUIRE(nt::kernel_map_command_test_handler_count(host) != 0u);

    nt::configure_kernel_map_command_test_failures(0, 0, 0, 0, 0, 0);
    REQUIRE(km::register_kernel_map_commands(host, bridge) ==
            SAO_AI_EDITOR_OK);
    REQUIRE(nt::kernel_map_command_test_handler_count(host) == 6u);
    REQUIRE(km::unregister_kernel_map_commands(host) == SAO_AI_EDITOR_OK);
    REQUIRE(nt::kernel_map_command_test_handler_count(host) == 0u);
    nt::reset_kernel_map_command_test_host(host);
}

TEST_CASE("kernel_map_commands: foreign conflict remains intact after rollback",
          "[plugins][ai_editor][kernel_map][registration][recovery]") {
    auto& host = command_test_host();
    nt::reset_kernel_map_command_test_host(host);
    constexpr uint64_t foreign_owner = 0xA11CEu;
    REQUIRE(host.register_native_command(
                "sao.kernelMap.map",
                [](const Json&, Json&) { return SAO_AI_EDITOR_OK; },
                foreign_owner) == SAO_AI_EDITOR_OK);

    auto bridge = make_bridge();
    REQUIRE(km::register_kernel_map_commands(host, bridge) ==
            SAO_AI_EDITOR_ERR_BUSY);
    REQUIRE(nt::kernel_map_command_test_handler_count(host) == 1u);
    const auto retained =
        host.snapshot_native_command("sao.kernelMap.map");
    REQUIRE(retained.has_value());
    REQUIRE(retained->owner == foreign_owner);
    REQUIRE(km::unregister_kernel_map_commands(host) ==
            SAO_AI_EDITOR_ERR_NOT_FOUND);
    REQUIRE(host.unregister_native_command("sao.kernelMap.map",
                                           foreign_owner) ==
            SAO_AI_EDITOR_OK);
    nt::reset_kernel_map_command_test_host(host);
}

TEST_CASE("kernel_map_tools: partial unregister retains BUSY tool for retry",
          "[plugins][ai_editor][kernel_map][registration][recovery]") {
    nt::ScopeStore scopes;
    nt::NativeToolRegistry registry(scopes, 0u, 0u);
    auto bridge = make_bridge();
    km::clear_kernel_map_tool_failure_injection();
    REQUIRE(km::register_kernel_map_tools(registry, bridge) ==
            SAO_AI_EDITOR_OK);

    km::set_kernel_map_tool_failure_injection(0, 0, 2, 1,
                                              SAO_AI_EDITOR_ERR_BUSY);
    REQUIRE(km::unregister_kernel_map_tools(registry, bridge) ==
            SAO_AI_EDITOR_ERR_BUSY);
    REQUIRE(registry.describe("chat").size() > 4u);

    km::clear_kernel_map_tool_failure_injection();
    REQUIRE(km::unregister_kernel_map_tools(registry, bridge) ==
            SAO_AI_EDITOR_OK);
    REQUIRE(km::unregister_kernel_map_tools(registry, bridge) ==
            SAO_AI_EDITOR_ERR_NOT_FOUND);
}

TEST_CASE("kernel_map_tools: final abandonment releases failed owner for address reuse",
          "[plugins][ai_editor][kernel_map][registration][teardown]") {
    nt::ScopeStore scopes;
    std::aligned_storage_t<sizeof(nt::NativeToolRegistry),
                           alignof(nt::NativeToolRegistry)>
        storage;
    auto* registry = ::new (static_cast<void*>(&storage))
        nt::NativeToolRegistry(scopes, 0u, 0u);

    auto bridge = make_bridge();
    std::weak_ptr<km::Bridge> weak = bridge;
    km::clear_kernel_map_tool_failure_injection();
    REQUIRE(km::register_kernel_map_tools(*registry, bridge) ==
            SAO_AI_EDITOR_OK);

    km::set_kernel_map_tool_failure_injection(0, 0, 2, 1,
                                              SAO_AI_EDITOR_ERR_BUSY);
    REQUIRE(km::unregister_kernel_map_tools(*registry, bridge) ==
            SAO_AI_EDITOR_ERR_BUSY);
    bridge.reset();
    REQUIRE_FALSE(weak.expired());

    km::abandon_kernel_map_tools(*registry);
    km::abandon_kernel_map_tools(*registry);
    registry->~NativeToolRegistry();
    REQUIRE(weak.expired());

    km::clear_kernel_map_tool_failure_injection();
    registry = ::new (static_cast<void*>(&storage))
        nt::NativeToolRegistry(scopes, 0u, 0u);
    auto replacement_bridge = make_bridge();
    REQUIRE(km::register_kernel_map_tools(*registry, replacement_bridge) ==
            SAO_AI_EDITOR_OK);
    REQUIRE(km::unregister_kernel_map_tools(*registry, replacement_bridge) ==
            SAO_AI_EDITOR_OK);
    registry->~NativeToolRegistry();
}

TEST_CASE("kernel_map_tools: partial registration rollback retries cleanly",
          "[plugins][ai_editor][kernel_map][registration][recovery]") {
    nt::ScopeStore scopes;
    nt::NativeToolRegistry registry(scopes, 0u, 0u);
    auto bridge = make_bridge();
    km::clear_kernel_map_tool_failure_injection();
    km::set_kernel_map_tool_failure_injection(3, 1, 1, 1,
                                              SAO_AI_EDITOR_ERR_BUSY);
    REQUIRE(km::register_kernel_map_tools(registry, bridge) ==
            SAO_AI_EDITOR_ERR_BUSY);

    km::clear_kernel_map_tool_failure_injection();
    REQUIRE(km::register_kernel_map_tools(registry, bridge) ==
            SAO_AI_EDITOR_OK);
    REQUIRE(km::unregister_kernel_map_tools(registry, bridge) ==
            SAO_AI_EDITOR_OK);
    REQUIRE(km::unregister_kernel_map_tools(registry, bridge) ==
            SAO_AI_EDITOR_ERR_NOT_FOUND);
}
