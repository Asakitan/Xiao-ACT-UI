// binding_engine_mem.cpp — 反射引擎面 mem 组：覆盖 SaoSdkMemTable 全部槽位
// （read / read_u32 / read_u64 / read_ptr_chain / module_base / attach /
// detach / enumerate_modules）加 provider status 导出。
// 目录项契约见 binding_engine.h；mem vtable ABI 见 sao_sdk_mem.h。

#include "sao/plugins/sdk_binding/binding_engine.h"

#include "sao/sdk/sao_sdk_mem.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace sao::plugins::sdk_binding {
namespace {

// mem.read 单次读取上限（binding JSON 总量上限对齐）。
constexpr uint64_t kMaxMemReadBytes = 8ull * 1024ull * 1024ull;

// ── 槽位探测 ─────────────────────────────────────────────────────
// vtable 槽可独立为 null：v1.4 前缀槽直接查指针，v1.5 追加槽先走
// sao_sdk_mem_v1_5_status 做 abi/struct_size 门控再查槽。

bool mem_group_available(const SaoSdkContext* ctx) noexcept {
    return ctx != nullptr && ctx->mem != nullptr;
}

bool mem_read_available(const SaoSdkContext* ctx) noexcept {
    return mem_group_available(ctx) && ctx->mem->read != nullptr;
}

bool mem_read_u32_available(const SaoSdkContext* ctx) noexcept {
    return mem_group_available(ctx) && ctx->mem->read_u32 != nullptr;
}

bool mem_read_u64_available(const SaoSdkContext* ctx) noexcept {
    return mem_group_available(ctx) && ctx->mem->read_u64 != nullptr;
}

bool mem_read_ptr_chain_available(const SaoSdkContext* ctx) noexcept {
    return mem_group_available(ctx) && ctx->mem->read_ptr_chain != nullptr;
}

bool mem_module_base_available(const SaoSdkContext* ctx) noexcept {
    return mem_group_available(ctx) && ctx->mem->module_base != nullptr;
}

bool mem_v1_5_ready(const SaoSdkContext* ctx) noexcept {
    return mem_group_available(ctx) && sao_sdk_mem_v1_5_status(ctx) == SAO_SDK_OK;
}

bool mem_attach_available(const SaoSdkContext* ctx) noexcept {
    return mem_v1_5_ready(ctx) && ctx->mem->attach != nullptr;
}

bool mem_detach_available(const SaoSdkContext* ctx) noexcept {
    return mem_v1_5_ready(ctx) && ctx->mem->detach != nullptr;
}

bool mem_enumerate_modules_available(const SaoSdkContext* ctx) noexcept {
    return mem_v1_5_ready(ctx) && ctx->mem->enumerate_modules != nullptr;
}

// ── 调用器 ───────────────────────────────────────────────────────

int32_t invoke_mem_read(const SaoSdkContext* ctx, const engine_json& args,
                        sdk_context_call_request* request) {
    uint64_t address = 0;
    uint32_t size = 0;
    if (!engine_arg_u64(args, "address", &address) ||
        !engine_arg_u32(args, "size", &size) || size == 0 ||
        size > kMaxMemReadBytes) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    std::vector<uint8_t> buffer(size);
    size_t bytes_read = 0;
    const sao_sdk_status_t status =
        sao_sdk_mem_read(ctx, address, buffer.data(), buffer.size(), &bytes_read);
    if (status != SAO_SDK_OK) return status;
    if (bytes_read > buffer.size()) return SAO_ERR_OS_CALL_FAILED;
    engine_json result = engine_json::object();
    result["bytes_b64"] = engine_b64_encode(buffer.data(), bytes_read);
    result["bytes_read"] = bytes_read;
    return engine_result(request, result);
}

int32_t invoke_mem_read_u32(const SaoSdkContext* ctx, const engine_json& args,
                            sdk_context_call_request* request) {
    uint64_t address = 0;
    if (!engine_arg_u64(args, "address", &address)) return SAO_ERR_INVALID_ARGUMENT;
    uint32_t value = 0;
    const sao_sdk_status_t status = sao_sdk_mem_read_u32(ctx, address, &value);
    if (status != SAO_SDK_OK) return status;
    return engine_result(request, value);
}

int32_t invoke_mem_read_u64(const SaoSdkContext* ctx, const engine_json& args,
                            sdk_context_call_request* request) {
    uint64_t address = 0;
    if (!engine_arg_u64(args, "address", &address)) return SAO_ERR_INVALID_ARGUMENT;
    uint64_t value = 0;
    const sao_sdk_status_t status = sao_sdk_mem_read_u64(ctx, address, &value);
    if (status != SAO_SDK_OK) return status;
    return engine_result(request, value);
}

int32_t invoke_mem_read_ptr_chain(const SaoSdkContext* ctx, const engine_json& args,
                                  sdk_context_call_request* request) {
    uint64_t base = 0;
    std::vector<int32_t> offsets;
    if (!engine_arg_u64(args, "base", &base) ||
        !engine_arg_i32_array(args, "offsets", &offsets) || offsets.empty()) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    uint64_t final_address = 0;
    const sao_sdk_status_t status = sao_sdk_mem_read_ptr_chain(
        ctx, base, offsets.data(), offsets.size(), &final_address);
    if (status != SAO_SDK_OK) return status;
    return engine_result(request, final_address);
}

int32_t invoke_mem_module_base(const SaoSdkContext* ctx, const engine_json& args,
                               sdk_context_call_request* request) {
    std::string module;
    if (!engine_arg_string(args, "module", &module)) return SAO_ERR_INVALID_ARGUMENT;
    uint64_t base = 0;
    const sao_sdk_status_t status = sao_sdk_mem_module_base(ctx, module.c_str(), &base);
    if (status != SAO_SDK_OK) return status;
    return engine_result(request, base);
}

int32_t invoke_mem_attach(const SaoSdkContext* ctx, const engine_json& args,
                          sdk_context_call_request* request) {
    uint32_t process_id = 0;
    std::string image_name;
    uint64_t start_time_100ns = 0;
    if (!engine_arg_u32(args, "process_id", &process_id) ||
        !engine_arg_string(args, "image_name_utf8", &image_name, true) ||
        !engine_arg_u64(args, "process_start_time_100ns", &start_time_100ns, 0, true)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (process_id == 0 && image_name.empty()) return SAO_ERR_INVALID_ARGUMENT;
    if (image_name.size() >= SAO_SDK_MEMORY_NAME_CAPACITY) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    SaoSdkMemoryTargetIdentity identity{};
    identity.struct_size = sizeof(identity);
    identity.abi_version = SAO_SDK_MEMORY_PROVIDER_ABI_VERSION;
    identity.process_id = process_id;
    identity.process_start_time_100ns = start_time_100ns;
    if (!image_name.empty()) {
        std::memcpy(identity.image_name_utf8, image_name.data(), image_name.size());
    }
    const sao_sdk_status_t status = sao_sdk_mem_attach(ctx, &identity);
    if (status != SAO_SDK_OK) return status;
    return engine_result(request, engine_json(nullptr));
}

int32_t invoke_mem_detach(const SaoSdkContext* ctx, const engine_json&,
                          sdk_context_call_request* request) {
    const sao_sdk_status_t status = sao_sdk_mem_detach(ctx);
    if (status != SAO_SDK_OK) return status;
    return engine_result(request, engine_json(nullptr));
}

int32_t invoke_mem_enumerate_modules(const SaoSdkContext* ctx, const engine_json&,
                                     sdk_context_call_request* request) {
    // 两调容量模式：capacity=0 探测真实数量，再分配读取。
    size_t count = 0;
    sao_sdk_status_t status = sao_sdk_mem_enumerate_modules(
        ctx, nullptr, 0, SAO_SDK_MEMORY_MODULE_ELEMENT_SIZE, &count);
    if (status != SAO_SDK_OK && status != SAO_SDK_ERR_BUFFER_TOO_SMALL) return status;
    if (count == 0) return engine_result(request, engine_json::array());
    if (count > SAO_SDK_MEMORY_MAX_MODULE_COUNT) return SAO_ERR_OS_CALL_FAILED;
    std::vector<SaoSdkMemoryModule> modules(count);
    status = sao_sdk_mem_enumerate_modules(ctx, modules.data(), modules.size(),
                                           SAO_SDK_MEMORY_MODULE_ELEMENT_SIZE, &count);
    if (status != SAO_SDK_OK) return status;
    if (count > modules.size()) return SAO_ERR_OS_CALL_FAILED;
    engine_json result = engine_json::array();
    for (size_t index = 0; index < count; ++index) {
        const SaoSdkMemoryModule& module = modules[index];
        const auto* terminator = static_cast<const char*>(
            std::memchr(module.name_utf8, '\0', sizeof(module.name_utf8)));
        if (terminator == nullptr) return SAO_ERR_OS_CALL_FAILED;
        engine_json entry = engine_json::object();
        entry["name"] = std::string(module.name_utf8, terminator);
        entry["base"] = module.base_address;
        entry["size"] = module.image_size;
        result.push_back(std::move(entry));
    }
    return engine_result(request, result);
}

int32_t invoke_mem_provider_status(const SaoSdkContext* ctx, const engine_json&,
                                   sdk_context_call_request* request) {
    const sao_sdk_status_t status = sao_sdk_context_memory_provider_status(ctx);
    return engine_result(request, status);
}

// ── 目录表 ───────────────────────────────────────────────────────

const char* const kArgsMemRead[] = {"address", "size"};
const char* const kArgsMemAddress[] = {"address"};
const char* const kArgsMemPtrChain[] = {"base", "offsets"};
const char* const kArgsMemModuleBase[] = {"module"};
const char* const kArgsMemAttach[] = {"process_id", "image_name_utf8"};

const sdk_engine_function_desc kEngineMemDescs[] = {
    {"mem.read", kArgsMemRead, 2, &invoke_mem_read, &mem_read_available},
    {"mem.read_u32", kArgsMemAddress, 1, &invoke_mem_read_u32, &mem_read_u32_available},
    {"mem.read_u64", kArgsMemAddress, 1, &invoke_mem_read_u64, &mem_read_u64_available},
    {"mem.read_ptr_chain", kArgsMemPtrChain, 2, &invoke_mem_read_ptr_chain,
     &mem_read_ptr_chain_available},
    {"mem.module_base", kArgsMemModuleBase, 1, &invoke_mem_module_base,
     &mem_module_base_available},
    {"mem.attach", kArgsMemAttach, 2, &invoke_mem_attach, &mem_attach_available},
    {"mem.detach", nullptr, 0, &invoke_mem_detach, &mem_detach_available},
    {"mem.enumerate_modules", nullptr, 0, &invoke_mem_enumerate_modules,
     &mem_enumerate_modules_available},
    {"mem.provider_status", nullptr, 0, &invoke_mem_provider_status, nullptr},
};

} // namespace

const sdk_engine_group_table kEngineGroupMem = {
    kEngineMemDescs,
    sizeof(kEngineMemDescs) / sizeof(kEngineMemDescs[0]),
    &mem_group_available,
};

} // namespace sao::plugins::sdk_binding
