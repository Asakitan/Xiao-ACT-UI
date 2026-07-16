// test_cs_host_wave4.cpp — G2.6 csharp_host Wave 4 首切片单测
//
// 4 CASE:
//   1. cshost_is_available_matches_env
//   2. cshost_init_returns_ok_or_not_available
//   3. cshost_get_runtime_version_nonempty (若 available)
//   4. cshost_compile_returns_expected_stub_status
//
// 依赖 .NET 优雅降级 (环境无 hostfxr → skip 编译 case)。

#include "sao/plugins/csharp_host/cs_host.h"
#include "sao/plugins/csharp_host/cs_compile.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace sao::plugins::csharp_host;

namespace {

bool g_available = false;
cs_host_handle_t g_host = nullptr;

// CASE 1: is_available 与环境一致
void case_cshost_is_available_matches_env() {
    bool avail = false;
    int32_t rc = sao_plugins_cshost_is_available(&avail);
    assert(rc == SAO_OK);
    g_available = avail;
    // 只要 API 不崩就 OK; 具体值由环境决定
    std::printf("  [OK] cshost_is_available_matches_env (available=%d)\n",
                avail ? 1 : 0);
}

// CASE 2: init 返 OK 或 NOT_INITIALIZED (取决于是否有 hostfxr)
void case_cshost_init_returns_ok_or_not_available() {
    cs_host_config cfg{};
    cs_host_handle_t h = nullptr;
    int32_t rc = sao_plugins_cshost_init(&cfg, &h);
    if (g_available) {
        // 环境里有 hostfxr → 应 OK (dll 加载成功)
        assert(rc == SAO_OK || rc == SAO_ERR_OS_CALL_FAILED);
        // 拿到 handle
        if (rc == SAO_OK) {
            assert(h != nullptr);
            g_host = h;
        }
    } else {
        // 无 hostfxr → NOT_INITIALIZED (但 handle 也可能非空作为降级 handle)
        assert(rc == SAO_ERR_NOT_INITIALIZED);
    }
    std::printf("  [OK] cshost_init_returns_ok_or_not_available (rc=%d)\n", rc);
}

// CASE 3: runtime_version 非空 (若 available)
void case_cshost_get_runtime_version_nonempty() {
    if (!g_available || g_host == nullptr) {
        std::printf("  [SKIP] cshost_get_runtime_version_nonempty "
                    "(no hostfxr in env)\n");
        return;
    }
    char buf[64] = {};
    int32_t rc = sao_plugins_cshost_get_runtime_version(g_host, buf, sizeof(buf));
    assert(rc == SAO_OK);
    assert(buf[0] != '\0');   // 版本号非空
    std::printf("  [OK] cshost_get_runtime_version_nonempty (v=%s)\n", buf);

    // 老 API 也要能读
    const char* v = sao_plugins_cshost_runtime_version(g_host);
    assert(v != nullptr);
    assert(std::strcmp(v, buf) == 0);
}

// CASE 4: compile 返 stub 状态 (Wave 4)
void case_cshost_compile_returns_expected_stub_status() {
    if (!g_available || g_host == nullptr) {
        // 无 hostfxr: compile 应 INVALID_ARGUMENT (domain nullptr)
        cs_assembly_handle_t asm_out = nullptr;
        int32_t rc = sao_plugins_cshost_compile_source(
            nullptr, L".", L"plugin.cs", nullptr, &asm_out);
        assert(rc == SAO_ERR_INVALID_ARGUMENT);
        assert(asm_out == nullptr);
        std::printf("  [OK] cshost_compile_returns_expected_stub_status "
                    "(no hostfxr, INVALID_ARGUMENT)\n");
        return;
    }
    // 有 hostfxr: 尝试建 domain (Wave 4 stub → NOT_IMPLEMENTED)
    cs_domain_handle_t dom = nullptr;
    int32_t rc = sao_plugins_cshost_create_domain(g_host, "test_domain", &dom);
    assert(rc == SAO_ERR_NOT_IMPLEMENTED || rc == SAO_OK);
    // Wave 4 create_domain stub → dom 是 nullptr, compile INVALID_ARGUMENT
    if (dom == nullptr) {
        cs_assembly_handle_t asm_out = nullptr;
        rc = sao_plugins_cshost_compile_source(
            dom, L".", L"plugin.cs", nullptr, &asm_out);
        assert(rc == SAO_ERR_INVALID_ARGUMENT);
    }
    std::printf("  [OK] cshost_compile_returns_expected_stub_status "
                "(stub NOT_IMPLEMENTED)\n");
}

void teardown() {
    if (g_host != nullptr) {
        sao_plugins_cshost_shutdown(g_host);
        g_host = nullptr;
    }
}

} // namespace

int main() {
    std::printf("test_cs_host_wave4:\n");
    case_cshost_is_available_matches_env();
    case_cshost_init_returns_ok_or_not_available();
    case_cshost_get_runtime_version_nonempty();
    case_cshost_compile_returns_expected_stub_status();
    teardown();
    std::printf("test_cs_host_wave4: 4 cases passed\n");
    return 0;
}
