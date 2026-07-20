// CPython initialization, finalization, and version roundtrip.
//
// Runs only when SAO_HAS_PYTHON_EMBED is defined; otherwise the executable
// reports a skipped capability and exits successfully.

#include "sao/plugins/python_host/py_host.h"

#include <cassert>
#include <cstdio>
#include <string>

using namespace sao::plugins::python_host;

#ifndef SAO_TEST_PYTHON_HOME
#  define SAO_TEST_PYTHON_HOME L""
#endif

#if defined(SAO_HAS_PYTHON_EMBED)

namespace {

// Both checks share one host handle to avoid reinitializing CPython after
// Py_FinalizeEx in the same process.
py_host_handle_t g_test_host = nullptr;

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

    py_host_handle_t h2 = nullptr;
    rc = sao_plugins_pyhost_init(&cfg, &h2);
    assert(rc == SAO_OK);
    assert(h2 == h);

    rc = sao_plugins_pyhost_shutdown(h2);
    assert(rc == SAO_OK);

    g_test_host = h;
    std::printf("  [OK] py_host_init_and_shutdown_roundtrip\n");
}

void case_py_host_reports_correct_version() {
    assert(g_test_host != nullptr);
    py_host_handle_t h = g_test_host;

    const char* ver = sao_plugins_pyhost_version(h);
    assert(ver != nullptr);
    const std::string version(ver);
    const bool supported =
        version.find("3.11") == 0 ||
        version.find("3.12") == 0 ||
        version.find("3.13") == 0 ||
        version.find("3.14") == 0;
    if (!supported) {
        std::fprintf(stderr, "unexpected Python version string: %s\n", ver);
    }
    assert(supported);
    assert(sao_plugins_pyhost_available(SAO_TEST_PYTHON_HOME));

    const int32_t rc = sao_plugins_pyhost_shutdown(h);
    assert(rc == SAO_OK);
    g_test_host = nullptr;

    std::printf("  [OK] py_host_reports_correct_version (%s)\n", ver);
}

} // namespace

int main() {
    std::printf("test_py_host_embed (SAO_HAS_PYTHON_EMBED):\n");
    case_py_host_init_and_shutdown_roundtrip();
    case_py_host_reports_correct_version();
    std::printf("py_host embed: 2 cases passed\n");
    return 0;
}

#else

int main() {
    std::printf("test_py_host_embed: SKIPPED (Python3 embed not found)\n");
    return 0;
}

#endif
