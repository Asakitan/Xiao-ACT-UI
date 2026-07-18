// test_py_host_wave2.cpp — Py_Initialize + Py_Finalize roundtrip
//
// 只有编译时 SAO_HAS_PYTHON_EMBED 被定义 (find_package 命中 Python3::Python)
// 才真跑, 否则 main 直接打印 skipped 退出 0.
//
// 覆盖:
//   - py_host_init_and_shutdown_roundtrip
//   - py_host_reports_correct_version (Py_GetVersion 含 "3.11" 或 "3.12")
#include "sao/plugins/python_host/py_host.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

using namespace sao::plugins::python_host;

#ifndef SAO_TEST_PYTHON_HOME
#  define SAO_TEST_PYTHON_HOME L""
#endif

#if defined(SAO_HAS_PYTHON_EMBED)

namespace {

// 两个 case 复用同一 host handle, 避免 Py_Finalize 后再 Py_Initialize 的
// CPython "脆弱路径". Wave 3 会加真正的 re-entrant / clean-teardown 测试.

py_host_handle_t g_test_host = nullptr;

// ── CASE 1: init + refcount 复用 + shutdown 全流程 ──
void case_py_host_init_and_shutdown_roundtrip() {
    py_host_config cfg{};
    cfg.python_home = SAO_TEST_PYTHON_HOME;
    cfg.platform_site_dir = nullptr;
    cfg.stdout_callback = nullptr;
    cfg.stderr_callback = nullptr;
    cfg.callback_user_data = nullptr;

    py_host_handle_t h = nullptr;
    int32_t rc = sao_plugins_pyhost_init(&cfg, &h);
    assert(rc == SAO_OK);
    assert(h != nullptr);

    // 二次 init 应复用 handle (进程级单例).
    py_host_handle_t h2 = nullptr;
    rc = sao_plugins_pyhost_init(&cfg, &h2);
    assert(rc == SAO_OK);
    assert(h2 == h);  // 同一 handle

    // 先 shutdown 一次: 因为二次 init 增了引用, 这里不应真 finalize.
    // (ref=2 → 1, singleton 仍在)
    rc = sao_plugins_pyhost_shutdown(h2);
    assert(rc == SAO_OK);

    // 把 host 保留给 case 2 用, 不做最终 finalize.
    g_test_host = h;

    std::printf("  [OK] py_host_init_and_shutdown_roundtrip\n");
}

// ── CASE 2: Py_GetVersion 报 3.11 / 3.12 / 3.13 ──
void case_py_host_reports_correct_version() {
    assert(g_test_host != nullptr);
    py_host_handle_t h = g_test_host;

    const char* ver = sao_plugins_pyhost_version(h);
    assert(ver != nullptr);
    std::string s(ver);
    // 我们只要求 3.11+ (find_package Python3 3.11 保证下限).
    bool ok =
        s.find("3.11") == 0 ||
        s.find("3.12") == 0 ||
        s.find("3.13") == 0 ||
        s.find("3.14") == 0;
    if (!ok) {
        std::fprintf(stderr, "unexpected Python version string: %s\n", ver);
    }
    assert(ok);

    // available() 探测应真.
    bool avail = sao_plugins_pyhost_available(SAO_TEST_PYTHON_HOME);
    assert(avail);

    // 最终 shutdown, 真 Py_FinalizeEx.
    int32_t rc = sao_plugins_pyhost_shutdown(h);
    assert(rc == SAO_OK);
    g_test_host = nullptr;

    std::printf("  [OK] py_host_reports_correct_version (%s)\n", ver);
}

} // namespace

int main() {
    std::printf("test_py_host_wave2 (SAO_HAS_PYTHON_EMBED):\n");
    case_py_host_init_and_shutdown_roundtrip();
    case_py_host_reports_correct_version();
    std::printf("py_host_wave2: 2 cases passed\n");
    return 0;
}

#else // !SAO_HAS_PYTHON_EMBED

int main() {
    std::printf("test_py_host_wave2: SKIPPED (Python3 embed not found)\n");
    // 通过, 让 CI 不因缺 Python 而失败.
    return 0;
}

#endif
