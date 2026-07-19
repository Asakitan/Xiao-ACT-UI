#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "sao/sdk/sao_sdk.h"

namespace {

struct MemoryProviderFixture;

struct MemorySessionFixture {
    MemoryProviderFixture* owner = nullptr;
    std::string plugin_id;
    uint32_t process_id = 0;
    bool attached = false;
};

struct MemoryProviderFixture {
    std::atomic<size_t> retain_count{0};
    std::atomic<size_t> release_count{0};
    std::atomic<size_t> open_count{0};
    std::atomic<size_t> close_count{0};
    std::atomic<size_t> attach_count{0};
    std::atomic<size_t> detach_count{0};
    std::atomic<size_t> read_count{0};
    std::atomic<size_t> live_sessions{0};
    sao_sdk_status_t open_status = SAO_SDK_OK;
    sao_sdk_status_t close_status = SAO_SDK_OK;
    sao_sdk_status_t attach_status = SAO_SDK_OK;
    sao_sdk_status_t detach_status = SAO_SDK_OK;
    sao_sdk_status_t read_status = SAO_SDK_OK;
    bool open_returns_session_on_failure = false;
    bool throw_open = false;
    bool throw_close = false;
    bool throw_attach = false;
    bool throw_detach = false;
    bool throw_read = false;
    bool throw_enumerate = false;
    bool short_read = false;
    bool block_read = false;
    bool read_entered = false;
    bool allow_read = false;
    bool reenter_read = false;
    SaoSdkContext* reentry_context = nullptr;
    const SaoSdkMemoryProviderVTable* reentry_replacement = nullptr;
    std::atomic<sao_sdk_status_t> reentry_read_status{SAO_SDK_OK};
    std::atomic<sao_sdk_status_t> reentry_clear_status{SAO_SDK_OK};
    std::atomic<sao_sdk_status_t> reentry_replace_status{SAO_SDK_OK};
    std::atomic<size_t> last_module_stride{0};
    size_t module_count = 2;
    std::mutex mutex;
    std::condition_variable condition;
    std::unordered_map<uint64_t, std::vector<uint8_t>> memory;

    template <typename T> void store(uint64_t address, const T& value) {
        std::vector<uint8_t> bytes(sizeof(value));
        std::memcpy(bytes.data(), &value, sizeof(value));
        memory[address] = std::move(bytes);
    }
};

void SAO_SDK_CALL memory_retain(void* user_data) {
    ++static_cast<MemoryProviderFixture*>(user_data)->retain_count;
}

void SAO_SDK_CALL memory_release(void* user_data) {
    ++static_cast<MemoryProviderFixture*>(user_data)->release_count;
}

sao_sdk_status_t SAO_SDK_CALL memory_open(void* user_data, const char* plugin_id_utf8,
                                          void** out_session) {
    if (plugin_id_utf8 == nullptr || plugin_id_utf8[0] == '\0' || out_session == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    *out_session = nullptr;
    auto* fixture = static_cast<MemoryProviderFixture*>(user_data);
    ++fixture->open_count;
    if (fixture->throw_open)
        throw std::runtime_error("memory open fixture");
    if (fixture->open_status != SAO_SDK_OK && !fixture->open_returns_session_on_failure)
        return fixture->open_status;
    *out_session = new MemorySessionFixture{fixture, plugin_id_utf8};
    ++fixture->live_sessions;
    return fixture->open_status;
}

sao_sdk_status_t SAO_SDK_CALL memory_close(void*, void* session_value) {
    auto* session = static_cast<MemorySessionFixture*>(session_value);
    if (session == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    ++session->owner->close_count;
    if (session->owner->throw_close)
        throw std::runtime_error("memory close fixture");
    if (session->owner->close_status != SAO_SDK_OK)
        return session->owner->close_status;
    --session->owner->live_sessions;
    delete session;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL memory_attach_provider(void*, void* session_value,
                                                     const SaoSdkMemoryTargetIdentity* identity) {
    auto* session = static_cast<MemorySessionFixture*>(session_value);
    if (session == nullptr || identity == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (identity->struct_size != sizeof(*identity) ||
        identity->abi_version != SAO_SDK_MEMORY_PROVIDER_ABI_VERSION) {
        return SAO_SDK_ERR_ABI_MISMATCH;
    }
    if (session->owner->throw_attach)
        throw std::runtime_error("memory attach fixture");
    if (session->owner->attach_status != SAO_SDK_OK)
        return session->owner->attach_status;
    session->process_id = identity->process_id;
    session->attached = true;
    ++session->owner->attach_count;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL memory_detach_provider(void*, void* session_value) {
    auto* session = static_cast<MemorySessionFixture*>(session_value);
    if (session == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (session->owner->throw_detach)
        throw std::runtime_error("memory detach fixture");
    if (session->owner->detach_status != SAO_SDK_OK)
        return session->owner->detach_status;
    session->attached = false;
    session->process_id = 0;
    ++session->owner->detach_count;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL memory_enumerate_provider(void*, void* session_value,
                                                        SaoSdkMemoryModule* out_modules,
                                                        size_t capacity, size_t element_stride,
                                                        size_t* out_count) {
    if (out_count != nullptr)
        *out_count = 0;
    auto* session = static_cast<MemorySessionFixture*>(session_value);
    if (session == nullptr || !session->attached || out_count == nullptr ||
        (capacity != 0 && out_modules == nullptr))
        return SAO_SDK_ERR_NOT_INITIALIZED;
    session->owner->last_module_stride = element_stride;
    if (element_stride != SAO_SDK_MEMORY_MODULE_ELEMENT_SIZE)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (session->owner->throw_enumerate)
        throw std::runtime_error("memory enumerate fixture");
    *out_count = session->owner->module_count;
    if (capacity < session->owner->module_count)
        return SAO_SDK_ERR_BUFFER_TOO_SMALL;
    if (session->owner->module_count != 2)
        return SAO_SDK_OK;
    out_modules[0] = {};
    out_modules[0].struct_size = sizeof(SaoSdkMemoryModule);
    out_modules[0].abi_version = SAO_SDK_MEMORY_PROVIDER_ABI_VERSION;
    out_modules[0].base_address = 0x140000000ull;
    out_modules[0].image_size = 0x210000ull;
    std::memcpy(out_modules[0].name_utf8, "Game.exe", sizeof("Game.exe"));
    out_modules[1] = {};
    out_modules[1].struct_size = sizeof(SaoSdkMemoryModule);
    out_modules[1].abi_version = SAO_SDK_MEMORY_PROVIDER_ABI_VERSION;
    out_modules[1].base_address = 0x180000000ull;
    out_modules[1].image_size = 0x330000ull;
    std::memcpy(out_modules[1].name_utf8, "Engine.dll", sizeof("Engine.dll"));
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL memory_read_provider(void*, void* session_value, uint64_t address,
                                                   void* out_buffer, size_t buffer_size,
                                                   size_t* out_bytes_read) {
    if (out_bytes_read != nullptr)
        *out_bytes_read = 0;
    auto* session = static_cast<MemorySessionFixture*>(session_value);
    if (session == nullptr || !session->attached || out_bytes_read == nullptr ||
        (buffer_size != 0 && out_buffer == nullptr)) {
        return SAO_SDK_ERR_NOT_INITIALIZED;
    }
    auto* fixture = session->owner;
    ++fixture->read_count;
    if (fixture->reenter_read) {
        uint64_t nested_value = 0;
        fixture->reentry_read_status =
            sao_sdk_mem_read_u64(fixture->reentry_context, address, &nested_value);
        fixture->reentry_clear_status =
            sao_sdk_context_configure_memory_provider(fixture->reentry_context, nullptr);
        fixture->reentry_replace_status = sao_sdk_context_configure_memory_provider(
            fixture->reentry_context, fixture->reentry_replacement);
    }
    {
        std::unique_lock<std::mutex> lock(fixture->mutex);
        if (fixture->block_read) {
            fixture->read_entered = true;
            fixture->condition.notify_all();
            fixture->condition.wait(lock, [fixture] { return fixture->allow_read; });
        }
    }
    if (fixture->throw_read)
        throw std::runtime_error("memory read fixture");
    if (fixture->read_status != SAO_SDK_OK)
        return fixture->read_status;

    std::vector<uint8_t> source;
    if (address == 0xF00D && buffer_size == sizeof(session->process_id)) {
        source.resize(sizeof(session->process_id));
        std::memcpy(source.data(), &session->process_id, source.size());
    } else {
        const auto found = fixture->memory.find(address);
        if (found == fixture->memory.end() || found->second.size() < buffer_size)
            return SAO_SDK_ERR_READ_FAULT;
        source = found->second;
    }
    const size_t copied = fixture->short_read && buffer_size != 0 ? buffer_size - 1 : buffer_size;
    if (copied != 0)
        std::memcpy(out_buffer, source.data(), copied);
    *out_bytes_read = copied;
    return SAO_SDK_OK;
}

SaoSdkMemoryProviderVTable make_memory_provider(MemoryProviderFixture* fixture) {
    SaoSdkMemoryProviderVTable provider{};
    provider.abi_version = SAO_SDK_MEMORY_PROVIDER_ABI_VERSION;
    provider.struct_size = sizeof(provider);
    provider.user_data = fixture;
    provider.retain = memory_retain;
    provider.release = memory_release;
    provider.open_session = memory_open;
    provider.close_session = memory_close;
    provider.attach = memory_attach_provider;
    provider.detach = memory_detach_provider;
    provider.enumerate_modules = memory_enumerate_provider;
    provider.read = memory_read_provider;
    return provider;
}

SaoSdkMemoryTargetIdentity process_identity(uint32_t process_id) {
    SaoSdkMemoryTargetIdentity identity{};
    identity.struct_size = sizeof(identity);
    identity.abi_version = SAO_SDK_MEMORY_PROVIDER_ABI_VERSION;
    identity.process_id = process_id;
    return identity;
}

struct LegacyMemTableV14 {
    decltype(SaoSdkMemTable::read) read = nullptr;
    decltype(SaoSdkMemTable::read_u32) read_u32 = nullptr;
    decltype(SaoSdkMemTable::read_u64) read_u64 = nullptr;
    decltype(SaoSdkMemTable::read_ptr_chain) read_ptr_chain = nullptr;
    decltype(SaoSdkMemTable::module_base) module_base = nullptr;
};

static_assert(sizeof(LegacyMemTableV14) == SAO_SDK_MEM_TABLE_V1_4_SIZE);

struct ProcessMemoryProviderReset {
    ~ProcessMemoryProviderReset() {
        (void)sao_sdk_platform_memory_configure_provider(nullptr);
    }
};

} // namespace

TEST_CASE("memory provider wires reads pointer chains and modules", "[sdk][memory][provider]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("memory.provider", "1.0", &ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_memory_provider_status(&ctx) == SAO_SDK_ERR_UNSUPPORTED);

    uint32_t value32 = 99;
    REQUIRE(sao_sdk_mem_read_u32(&ctx, 0x1000, &value32) == SAO_SDK_ERR_UNSUPPORTED);
    REQUIRE(value32 == 0);

    SaoSdkMemoryProviderVTable bad{};
    bad.abi_version = 2u << 16;
    bad.struct_size = sizeof(bad);
    REQUIRE(sao_sdk_context_configure_memory_provider(&ctx, &bad) == SAO_SDK_ERR_ABI_MISMATCH);

    MemoryProviderFixture fixture;
    const uint32_t expected32 = 0x12345678u;
    const uint64_t expected64 = 0x0102030405060708ull;
    fixture.store(0x1000, expected32);
    fixture.store(0x2000, expected64);
    fixture.store<uint64_t>(0x3000, 0x4000);
    fixture.store<uint64_t>(0x4010, 0x5000);
    auto provider = make_memory_provider(&fixture);
    REQUIRE(sao_sdk_context_configure_memory_provider(&ctx, &provider) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_memory_provider_status(&ctx) == SAO_SDK_OK);
    REQUIRE(ctx.mem->abi_version == SAO_SDK_MEM_TABLE_ABI_VERSION);
    REQUIRE(ctx.mem->struct_size == sizeof(SaoSdkMemTable));
    REQUIRE(fixture.retain_count == 1);
    REQUIRE(fixture.open_count == 1);

    REQUIRE(sao_sdk_mem_read_u32(&ctx, 0x1000, &value32) == SAO_SDK_ERR_NOT_INITIALIZED);
    const auto identity = process_identity(4242);
    REQUIRE(sao_sdk_mem_attach(&ctx, &identity) == SAO_SDK_OK);
    uint64_t final_address = 0;
    REQUIRE(sao_sdk_mem_read_ptr_chain(&ctx, 0x3000, nullptr, 0, &final_address) == SAO_SDK_OK);
    REQUIRE(final_address == 0x3000);

    REQUIRE(sao_sdk_mem_read_u32(&ctx, 0x1000, &value32) == SAO_SDK_OK);
    REQUIRE(value32 == expected32);
    uint64_t value64 = 0;
    REQUIRE(sao_sdk_mem_read_u64(&ctx, 0x2000, &value64) == SAO_SDK_OK);
    REQUIRE(value64 == expected64);

    const std::array<int32_t, 2> offsets{0x10, 0x20};
    REQUIRE(sao_sdk_mem_read_ptr_chain(&ctx, 0x3000, offsets.data(), offsets.size(),
                                       &final_address) == SAO_SDK_OK);
    REQUIRE(final_address == 0x5020);

    uint64_t module_base = 0;
    REQUIRE(sao_sdk_mem_module_base(&ctx, "engine.DLL", &module_base) == SAO_SDK_OK);
    REQUIRE(module_base == 0x180000000ull);
    REQUIRE(sao_sdk_mem_module_base(&ctx, "missing.dll", &module_base) == SAO_SDK_ERR_NOT_FOUND);
    size_t module_count = 0;
    REQUIRE(sao_sdk_mem_enumerate_modules(&ctx, nullptr, 0, SAO_SDK_MEMORY_MODULE_ELEMENT_SIZE,
                                          &module_count) == SAO_SDK_ERR_BUFFER_TOO_SMALL);
    REQUIRE(module_count == 2);
    REQUIRE(fixture.last_module_stride == SAO_SDK_MEMORY_MODULE_ELEMENT_SIZE);
    std::vector<SaoSdkMemoryModule> modules(module_count);
    for (auto& module : modules) {
        module.struct_size = sizeof(module);
        module.abi_version = SAO_SDK_MEMORY_PROVIDER_ABI_VERSION;
    }
    REQUIRE(sao_sdk_mem_enumerate_modules(&ctx, modules.data(), modules.size(),
                                          SAO_SDK_MEMORY_MODULE_ELEMENT_SIZE,
                                          &module_count) == SAO_SDK_OK);
    REQUIRE(std::string(modules[0].name_utf8) == "Game.exe");
    REQUIRE(std::string(modules[1].name_utf8) == "Engine.dll");

    fixture.short_read = true;
    std::array<uint8_t, sizeof(uint64_t)> raw{};
    size_t raw_bytes_read = 0;
    REQUIRE(sao_sdk_mem_read(&ctx, 0x2000, raw.data(), raw.size(), &raw_bytes_read) == SAO_SDK_OK);
    REQUIRE(raw_bytes_read == sizeof(uint64_t) - 1);
    REQUIRE(std::memcmp(raw.data(), &expected64, raw_bytes_read) == 0);
    value64 = UINT64_MAX;
    REQUIRE(sao_sdk_mem_read_u64(&ctx, 0x2000, &value64) == SAO_SDK_ERR_READ_FAULT);
    REQUIRE(value64 == 0);
    fixture.short_read = false;

    fixture.store<uint64_t>(0x5000, 0);
    fixture.store<uint64_t>(0x6000, UINT64_MAX);
    const std::array<int32_t, 1> zero_offset{0};
    const std::array<int32_t, 1> overflow_offset{1};
    REQUIRE(sao_sdk_mem_read_ptr_chain(&ctx, 0, nullptr, 0, &final_address) ==
            SAO_SDK_ERR_READ_FAULT);
    REQUIRE(final_address == 0);
    REQUIRE(sao_sdk_mem_read_ptr_chain(&ctx, 0x5000, zero_offset.data(), zero_offset.size(),
                                       &final_address) == SAO_SDK_ERR_READ_FAULT);
    REQUIRE(final_address == 0);
    REQUIRE(sao_sdk_mem_read_ptr_chain(&ctx, 0x6000, overflow_offset.data(), overflow_offset.size(),
                                       &final_address) == SAO_SDK_ERR_READ_FAULT);
    REQUIRE(final_address == 0);

    fixture.read_status = SAO_SDK_ERR_NOT_IMPLEMENTED;
    REQUIRE(sao_sdk_mem_read_u32(&ctx, 0x1000, &value32) == SAO_SDK_ERR_UNSUPPORTED);
    fixture.read_status = SAO_SDK_ERR_UNSUPPORTED;
    REQUIRE(sao_sdk_mem_read_u32(&ctx, 0x1000, &value32) == SAO_SDK_ERR_UNSUPPORTED);
    fixture.read_status = SAO_SDK_OK;

    REQUIRE(sao_sdk_mem_detach(&ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_mem_read_u32(&ctx, 0x1000, &value32) == SAO_SDK_ERR_NOT_INITIALIZED);
    REQUIRE(sao_sdk_mem_read_ptr_chain(&ctx, 0x3000, nullptr, 0, &final_address) ==
            SAO_SDK_ERR_NOT_INITIALIZED);
    REQUIRE(final_address == 0);
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
    REQUIRE(fixture.detach_count == 1);
    REQUIRE(fixture.close_count == 1);
    REQUIRE(fixture.release_count == 1);
    REQUIRE(fixture.live_sessions == 0);
}

TEST_CASE("memory provider replacement is transactional and context local",
          "[sdk][memory][provider][lifecycle]") {
    SaoSdkContext first{};
    SaoSdkContext second{};
    REQUIRE(sao_sdk_bind_context("memory.first", "1.0", &first) == SAO_SDK_OK);
    REQUIRE(sao_sdk_bind_context("memory.second", "1.0", &second) == SAO_SDK_OK);

    MemoryProviderFixture original;
    auto original_provider = make_memory_provider(&original);
    REQUIRE(sao_sdk_context_configure_memory_provider(&first, &original_provider) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_configure_memory_provider(&second, &original_provider) == SAO_SDK_OK);
    const auto first_identity = process_identity(101);
    const auto second_identity = process_identity(202);
    REQUIRE(sao_sdk_mem_attach(&first, &first_identity) == SAO_SDK_OK);
    REQUIRE(sao_sdk_mem_attach(&second, &second_identity) == SAO_SDK_OK);

    uint32_t observed_id = 0;
    REQUIRE(sao_sdk_mem_read_u32(&first, 0xF00D, &observed_id) == SAO_SDK_OK);
    REQUIRE(observed_id == 101);
    REQUIRE(sao_sdk_mem_read_u32(&second, 0xF00D, &observed_id) == SAO_SDK_OK);
    REQUIRE(observed_id == 202);

    auto incomplete = make_memory_provider(&original);
    incomplete.read = nullptr;
    REQUIRE(sao_sdk_context_configure_memory_provider(&first, &incomplete) ==
            SAO_SDK_ERR_INVALID_ARGUMENT);
    REQUIRE(sao_sdk_mem_read_u32(&first, 0xF00D, &observed_id) == SAO_SDK_OK);
    REQUIRE(observed_id == 101);

    MemoryProviderFixture replacement;
    auto replacement_provider = make_memory_provider(&replacement);
    REQUIRE(sao_sdk_context_configure_memory_provider(&first, &replacement_provider) == SAO_SDK_OK);
    REQUIRE(original.detach_count == 1);
    REQUIRE(original.close_count == 1);
    REQUIRE(original.release_count == 1);
    REQUIRE(sao_sdk_mem_read_u32(&first, 0xF00D, &observed_id) == SAO_SDK_ERR_NOT_INITIALIZED);

    MemoryProviderFixture failed;
    failed.open_status = SAO_SDK_ERR_NOT_INITIALIZED;
    auto failed_provider = make_memory_provider(&failed);
    REQUIRE(sao_sdk_context_configure_memory_provider(&first, &failed_provider) ==
            SAO_SDK_ERR_NOT_INITIALIZED);
    REQUIRE(sao_sdk_context_memory_provider_status(&first) == SAO_SDK_OK);
    REQUIRE(failed.retain_count == 1);
    REQUIRE(failed.release_count == 1);

    REQUIRE(sao_sdk_context_configure_memory_provider(&first, nullptr) == SAO_SDK_OK);
    REQUIRE(replacement.close_count == 1);
    REQUIRE(replacement.release_count == 1);
    REQUIRE(sao_sdk_context_memory_provider_status(&first) == SAO_SDK_ERR_UNSUPPORTED);

    sao_sdk_context_destroy(&first);
    sao_sdk_context_destroy(&second);
    REQUIRE(original.detach_count == 2);
    REQUIRE(original.close_count == 2);
    REQUIRE(original.release_count == 2);
    REQUIRE(original.live_sessions == 0);
}

TEST_CASE("memory provider clear waits for an in-flight read",
          "[sdk][memory][provider][concurrency]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("memory.concurrent", "1.0", &ctx) == SAO_SDK_OK);
    MemoryProviderFixture fixture;
    const uint64_t expected = 0xAABBCCDDEEFF0011ull;
    fixture.store(0x7000, expected);
    fixture.block_read = true;
    auto provider = make_memory_provider(&fixture);
    REQUIRE(sao_sdk_context_configure_memory_provider(&ctx, &provider) == SAO_SDK_OK);
    const auto identity = process_identity(303);
    REQUIRE(sao_sdk_mem_attach(&ctx, &identity) == SAO_SDK_OK);

    auto read_future = std::async(std::launch::async, [&ctx] {
        uint64_t value = 0;
        const auto status = sao_sdk_mem_read_u64(&ctx, 0x7000, &value);
        return std::pair{sao_sdk_status_t(status), value};
    });
    {
        std::unique_lock<std::mutex> lock(fixture.mutex);
        fixture.condition.wait(lock, [&fixture] { return fixture.read_entered; });
    }

    auto clear_future = std::async(std::launch::async, [&ctx] {
        return sao_sdk_context_configure_memory_provider(&ctx, nullptr);
    });
    REQUIRE(clear_future.wait_for(std::chrono::milliseconds(25)) == std::future_status::timeout);
    REQUIRE(fixture.close_count == 0);

    {
        std::lock_guard<std::mutex> lock(fixture.mutex);
        fixture.allow_read = true;
    }
    fixture.condition.notify_all();
    const auto [read_status, read_value] = read_future.get();
    REQUIRE(read_status == SAO_SDK_OK);
    REQUIRE(read_value == expected);
    REQUIRE(clear_future.get() == SAO_SDK_OK);
    REQUIRE(fixture.detach_count == 1);
    REQUIRE(fixture.close_count == 1);
    REQUIRE(fixture.release_count == 1);

    uint64_t value = 0;
    REQUIRE(sao_sdk_mem_read_u64(&ctx, 0x7000, &value) == SAO_SDK_ERR_UNSUPPORTED);
    sao_sdk_context_destroy(&ctx);
}

TEST_CASE("memory 1.5 wrappers reject an ABI 1.4 table before reading appended slots",
          "[sdk][memory][abi]") {
    LegacyMemTableV14 legacy_table{};
    SaoSdkContext legacy_context{};
    legacy_context.abi_version = (1u << 16) | 4u;
    legacy_context.ctx_impl = reinterpret_cast<void*>(uintptr_t{1});
    legacy_context.mem = reinterpret_cast<const SaoSdkMemTable*>(&legacy_table);

    const auto identity = process_identity(7);
    size_t module_count = 99;
    CHECK(sao_sdk_mem_attach(&legacy_context, &identity) == SAO_SDK_ERR_UNSUPPORTED);
    CHECK(sao_sdk_mem_detach(&legacy_context) == SAO_SDK_ERR_UNSUPPORTED);
    CHECK(sao_sdk_mem_enumerate_modules(&legacy_context, nullptr, 0,
                                        SAO_SDK_MEMORY_MODULE_ELEMENT_SIZE,
                                        &module_count) == SAO_SDK_ERR_UNSUPPORTED);
    CHECK(module_count == 0);

    SaoSdkMemTable old_minor{};
    old_minor.abi_version = (SAO_SDK_MEM_TABLE_ABI_VERSION_MAJOR << 16) | 4u;
    old_minor.struct_size = sizeof(old_minor);
    legacy_context.abi_version = SAO_SDK_ABI_VERSION;
    legacy_context.mem = &old_minor;
    CHECK(sao_sdk_mem_attach(&legacy_context, &identity) == SAO_SDK_ERR_UNSUPPORTED);
}

TEST_CASE("memory module enumeration rejects provider counts above the SDK limit",
          "[sdk][memory][provider][limit]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("memory.module.limit", "1.0", &ctx) == SAO_SDK_OK);
    MemoryProviderFixture fixture;
    fixture.module_count = SAO_SDK_MEMORY_MAX_MODULE_COUNT + 1u;
    auto provider = make_memory_provider(&fixture);
    REQUIRE(sao_sdk_context_configure_memory_provider(&ctx, &provider) == SAO_SDK_OK);
    const auto identity = process_identity(713);
    REQUIRE(sao_sdk_mem_attach(&ctx, &identity) == SAO_SDK_OK);

    size_t count = 0;
    CHECK(sao_sdk_mem_enumerate_modules(&ctx, nullptr, 0, SAO_SDK_MEMORY_MODULE_ELEMENT_SIZE,
                                        &count) == SAO_SDK_ERR_INTERNAL);
    CHECK(count == 0);
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
}

TEST_CASE("process memory owner auto-opens sessions for create and platform bind",
          "[sdk][memory][provider][process_owner]") {
    ProcessMemoryProviderReset reset;
    REQUIRE(sao_sdk_platform_memory_configure_provider(nullptr) == SAO_SDK_OK);
    SaoSdkContext existing{};
    REQUIRE(sao_sdk_bind_context("memory.owner.existing", "1.0", &existing) == SAO_SDK_OK);

    MemoryProviderFixture fixture;
    auto provider = make_memory_provider(&fixture);
    REQUIRE(sao_sdk_platform_memory_configure_provider(&provider) == SAO_SDK_OK);
    SaoSdkContext created{};
    REQUIRE(sao_sdk_bind_context("memory.owner.created", "1.0", &created) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_bind_platform_services(&existing) == SAO_SDK_OK);
    CHECK(fixture.retain_count == 3);
    CHECK(fixture.open_count == 2);
    CHECK(fixture.live_sessions == 2);

    REQUIRE(sao_sdk_platform_memory_configure_provider(nullptr) == SAO_SDK_OK);
    CHECK(fixture.release_count == 1);
    CHECK(fixture.live_sessions == 2);
    REQUIRE(sao_sdk_context_try_destroy(&created) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_try_destroy(&existing) == SAO_SDK_OK);
    CHECK(fixture.close_count == 2);
    CHECK(fixture.release_count == 3);
    CHECK(fixture.live_sessions == 0);
}

TEST_CASE("memory target identity accepts and normalizes its required ABI prefix",
          "[sdk][memory][abi][identity]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("memory.identity.prefix", "1.0", &ctx) == SAO_SDK_OK);
    MemoryProviderFixture fixture;
    auto provider = make_memory_provider(&fixture);
    REQUIRE(sao_sdk_context_configure_memory_provider(&ctx, &provider) == SAO_SDK_OK);

    struct RequiredIdentityPrefix {
        uint32_t struct_size;
        uint32_t abi_version;
        uint32_t process_id;
    } identity{sizeof(RequiredIdentityPrefix), SAO_SDK_MEMORY_PROVIDER_ABI_VERSION, 711};
    static_assert(sizeof(RequiredIdentityPrefix) == SAO_SDK_MEMORY_TARGET_IDENTITY_REQUIRED_SIZE);
    REQUIRE(sao_sdk_mem_attach(&ctx, reinterpret_cast<const SaoSdkMemoryTargetIdentity*>(
                                         &identity)) == SAO_SDK_OK);
    uint32_t process_id = 0;
    REQUIRE(sao_sdk_mem_read_u32(&ctx, 0xF00D, &process_id) == SAO_SDK_OK);
    CHECK(process_id == 711);

    size_t count = 17;
    CHECK(sao_sdk_mem_enumerate_modules(&ctx, nullptr, 0, SAO_SDK_MEMORY_MODULE_ELEMENT_SIZE - 1,
                                        &count) == SAO_SDK_ERR_INVALID_ARGUMENT);
    CHECK(count == 0);
    CHECK(fixture.last_module_stride == 0);
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
}

TEST_CASE("memory provider callback reentry fails busy without deadlock or replacement",
          "[sdk][memory][provider][reentry]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("memory.reentry", "1.0", &ctx) == SAO_SDK_OK);
    MemoryProviderFixture active;
    MemoryProviderFixture replacement;
    constexpr uint64_t expected = 0x8877665544332211ull;
    active.store(0x8000, expected);
    auto active_provider = make_memory_provider(&active);
    auto replacement_provider = make_memory_provider(&replacement);
    active.reentry_context = &ctx;
    active.reentry_replacement = &replacement_provider;
    active.reenter_read = true;
    REQUIRE(sao_sdk_context_configure_memory_provider(&ctx, &active_provider) == SAO_SDK_OK);
    const auto identity = process_identity(808);
    REQUIRE(sao_sdk_mem_attach(&ctx, &identity) == SAO_SDK_OK);

    uint64_t value = 0;
    REQUIRE(sao_sdk_mem_read_u64(&ctx, 0x8000, &value) == SAO_SDK_OK);
    CHECK(value == expected);
    CHECK(active.reentry_read_status == SAO_SDK_ERR_BUSY);
    CHECK(active.reentry_clear_status == SAO_SDK_ERR_BUSY);
    CHECK(active.reentry_replace_status == SAO_SDK_ERR_BUSY);
    CHECK(replacement.open_count == 0);
    CHECK(active.live_sessions == 1);

    active.reenter_read = false;
    REQUIRE(sao_sdk_context_configure_memory_provider(&ctx, nullptr) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
}

TEST_CASE("memory provider replacement waits for an in-flight callback",
          "[sdk][memory][provider][replacement][race]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("memory.replacement.race", "1.0", &ctx) == SAO_SDK_OK);
    MemoryProviderFixture original;
    MemoryProviderFixture replacement;
    constexpr uint64_t expected = 0x1020304050607080ull;
    original.store(0x9000, expected);
    original.block_read = true;
    auto original_provider = make_memory_provider(&original);
    auto replacement_provider = make_memory_provider(&replacement);
    REQUIRE(sao_sdk_context_configure_memory_provider(&ctx, &original_provider) == SAO_SDK_OK);
    const auto identity = process_identity(909);
    REQUIRE(sao_sdk_mem_attach(&ctx, &identity) == SAO_SDK_OK);

    auto read_future = std::async(std::launch::async, [&ctx] {
        uint64_t value = 0;
        return std::pair{sao_sdk_mem_read_u64(&ctx, 0x9000, &value), value};
    });
    {
        std::unique_lock<std::mutex> lock(original.mutex);
        original.condition.wait(lock, [&original] { return original.read_entered; });
    }
    auto replace_future = std::async(std::launch::async, [&ctx, &replacement_provider] {
        return sao_sdk_context_configure_memory_provider(&ctx, &replacement_provider);
    });
    REQUIRE(replace_future.wait_for(std::chrono::milliseconds(25)) == std::future_status::timeout);
    CHECK(original.close_count == 0);

    {
        std::lock_guard<std::mutex> lock(original.mutex);
        original.allow_read = true;
    }
    original.condition.notify_all();
    const auto [read_status, value] = read_future.get();
    CHECK(read_status == SAO_SDK_OK);
    CHECK(value == expected);
    REQUIRE(replace_future.get() == SAO_SDK_OK);
    CHECK(original.detach_count == 1);
    CHECK(original.close_count == 1);
    CHECK(original.live_sessions == 0);
    CHECK(replacement.live_sessions == 1);
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
}

TEST_CASE("memory cleanup failures retain ownership for retry and quarantine",
          "[sdk][memory][provider][cleanup][retry]") {
    SECTION("detach failure keeps the attached session and context alive") {
        SaoSdkContext ctx{};
        REQUIRE(sao_sdk_bind_context("memory.cleanup.detach", "1.0", &ctx) == SAO_SDK_OK);
        MemoryProviderFixture fixture;
        fixture.detach_status = SAO_SDK_ERR_INTERNAL;
        auto provider = make_memory_provider(&fixture);
        REQUIRE(sao_sdk_context_configure_memory_provider(&ctx, &provider) == SAO_SDK_OK);
        const auto identity = process_identity(1001);
        REQUIRE(sao_sdk_mem_attach(&ctx, &identity) == SAO_SDK_OK);

        CHECK(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_ERR_INTERNAL);
        CHECK(ctx.ctx_impl != nullptr);
        CHECK(fixture.live_sessions == 1);
        CHECK(fixture.close_count == 0);
        CHECK(fixture.release_count == 0);
        CHECK(sao_sdk_context_memory_provider_status(&ctx) == SAO_SDK_ERR_INTERNAL);

        fixture.detach_status = SAO_SDK_OK;
        REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
        CHECK(ctx.ctx_impl == nullptr);
        CHECK(fixture.live_sessions == 0);
        CHECK(fixture.release_count == 1);
    }

    SECTION("close failure preserves the detached session until clear retries") {
        SaoSdkContext ctx{};
        REQUIRE(sao_sdk_bind_context("memory.cleanup.close", "1.0", &ctx) == SAO_SDK_OK);
        MemoryProviderFixture fixture;
        fixture.close_status = SAO_SDK_ERR_INTERNAL;
        auto provider = make_memory_provider(&fixture);
        REQUIRE(sao_sdk_context_configure_memory_provider(&ctx, &provider) == SAO_SDK_OK);

        CHECK(sao_sdk_context_configure_memory_provider(&ctx, nullptr) == SAO_SDK_ERR_INTERNAL);
        CHECK(fixture.live_sessions == 1);
        CHECK(fixture.close_count == 1);
        CHECK(fixture.release_count == 0);
        fixture.close_status = SAO_SDK_OK;
        REQUIRE(sao_sdk_context_configure_memory_provider(&ctx, nullptr) == SAO_SDK_OK);
        CHECK(fixture.close_count == 2);
        CHECK(fixture.live_sessions == 0);
        CHECK(fixture.release_count == 1);
        REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
    }

    SECTION("failed open cleanup remains quarantined until retry") {
        SaoSdkContext ctx{};
        REQUIRE(sao_sdk_bind_context("memory.cleanup.quarantine", "1.0", &ctx) == SAO_SDK_OK);
        MemoryProviderFixture fixture;
        fixture.open_status = SAO_SDK_ERR_NOT_INITIALIZED;
        fixture.open_returns_session_on_failure = true;
        fixture.close_status = SAO_SDK_ERR_INTERNAL;
        auto provider = make_memory_provider(&fixture);

        CHECK(sao_sdk_context_configure_memory_provider(&ctx, &provider) == SAO_SDK_ERR_INTERNAL);
        CHECK(fixture.live_sessions == 1);
        CHECK(fixture.release_count == 0);
        CHECK(sao_sdk_context_memory_provider_status(&ctx) == SAO_SDK_ERR_INTERNAL);
        fixture.close_status = SAO_SDK_OK;
        REQUIRE(sao_sdk_context_configure_memory_provider(&ctx, nullptr) == SAO_SDK_OK);
        CHECK(fixture.live_sessions == 0);
        CHECK(fixture.release_count == 1);
        REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
    }
}

TEST_CASE("memory provider exceptions map to internal without losing ownership",
          "[sdk][memory][provider][exception]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("memory.exception", "1.0", &ctx) == SAO_SDK_OK);
    MemoryProviderFixture fixture;
    constexpr uint64_t expected = 0xCAFEBABEDEADBEEFull;
    fixture.store(0xA000, expected);
    auto provider = make_memory_provider(&fixture);
    REQUIRE(sao_sdk_context_configure_memory_provider(&ctx, &provider) == SAO_SDK_OK);

    fixture.throw_attach = true;
    const auto identity = process_identity(1111);
    CHECK(sao_sdk_mem_attach(&ctx, &identity) == SAO_SDK_ERR_INTERNAL);
    fixture.throw_attach = false;
    REQUIRE(sao_sdk_mem_attach(&ctx, &identity) == SAO_SDK_OK);

    fixture.throw_read = true;
    uint64_t value = UINT64_MAX;
    CHECK(sao_sdk_mem_read_u64(&ctx, 0xA000, &value) == SAO_SDK_ERR_INTERNAL);
    CHECK(value == 0);
    fixture.throw_read = false;

    fixture.throw_enumerate = true;
    size_t module_count = 0;
    CHECK(sao_sdk_mem_enumerate_modules(&ctx, nullptr, 0, SAO_SDK_MEMORY_MODULE_ELEMENT_SIZE,
                                        &module_count) == SAO_SDK_ERR_INTERNAL);
    CHECK(module_count == 0);
    fixture.throw_enumerate = false;

    fixture.throw_detach = true;
    CHECK(sao_sdk_mem_detach(&ctx) == SAO_SDK_ERR_INTERNAL);
    CHECK(fixture.live_sessions == 1);
    fixture.throw_detach = false;
    REQUIRE(sao_sdk_mem_detach(&ctx) == SAO_SDK_OK);

    fixture.throw_close = true;
    CHECK(sao_sdk_context_configure_memory_provider(&ctx, nullptr) == SAO_SDK_ERR_INTERNAL);
    CHECK(fixture.live_sessions == 1);
    CHECK(fixture.release_count == 0);
    fixture.throw_close = false;
    REQUIRE(sao_sdk_context_configure_memory_provider(&ctx, nullptr) == SAO_SDK_OK);
    CHECK(fixture.live_sessions == 0);
    CHECK(fixture.release_count == 1);
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);

    SaoSdkContext open_ctx{};
    REQUIRE(sao_sdk_bind_context("memory.exception.open", "1.0", &open_ctx) == SAO_SDK_OK);
    MemoryProviderFixture open_fixture;
    open_fixture.throw_open = true;
    auto open_provider = make_memory_provider(&open_fixture);
    CHECK(sao_sdk_context_configure_memory_provider(&open_ctx, &open_provider) ==
          SAO_SDK_ERR_INTERNAL);
    CHECK(open_fixture.retain_count == 1);
    CHECK(open_fixture.release_count == 1);
    REQUIRE(sao_sdk_context_try_destroy(&open_ctx) == SAO_SDK_OK);
}

TEST_CASE("void context destroy quarantines cleanup failure for explicit retry",
          "[sdk][memory][destroy][quarantine][retry]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("memory.destroy.quarantine", "1.0", &ctx) == SAO_SDK_OK);
    MemoryProviderFixture fixture;
    fixture.close_status = SAO_SDK_ERR_INTERNAL;
    auto provider = make_memory_provider(&fixture);
    REQUIRE(sao_sdk_context_configure_memory_provider(&ctx, &provider) == SAO_SDK_OK);

    sao_sdk_context_destroy(&ctx);
    CHECK(ctx.ctx_impl != nullptr);
    CHECK(fixture.live_sessions == 1);
    CHECK(fixture.release_count == 0);
    fixture.close_status = SAO_SDK_OK;
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
    CHECK(ctx.ctx_impl == nullptr);
    CHECK(fixture.live_sessions == 0);
    CHECK(fixture.release_count == 1);
}
