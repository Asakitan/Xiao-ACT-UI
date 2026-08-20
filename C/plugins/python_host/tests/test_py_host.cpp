// Lightweight Python host smoke test for both embedded and fallback builds.

#include "sao/plugins/python_host/py_error.h"
#include "sao/plugins/python_host/py_host.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef SAO_TEST_PYTHON_HOME
#  define SAO_TEST_PYTHON_HOME L""
#endif

namespace {
[[noreturn]] void sao_test_assert_fail(const char* file, int line,
                                        const char* expression) {
    std::fprintf(stderr, "%s:%d: assertion failed: %s\n", file, line,
                 expression);
    std::fflush(stderr);
    std::_Exit(EXIT_FAILURE);
}

#define SAO_TEST_ASSERT(...)                                                   \
    do {                                                                       \
        if (!(__VA_ARGS__)) {                                                  \
            sao_test_assert_fail(__FILE__, __LINE__, #__VA_ARGS__);            \
        }                                                                      \
    } while (false)
} // namespace
int main() {
    using namespace sao::plugins::python_host;

    py_host_config config{};
    config.python_home = SAO_TEST_PYTHON_HOME;
    py_host_handle_t host = nullptr;
    int32_t rc = sao_plugins_pyhost_init(&config, &host);
#if defined(SAO_HAS_PYTHON_EMBED)
    SAO_TEST_ASSERT(rc == SAO_OK);
    SAO_TEST_ASSERT(host != nullptr);
    rc = sao_plugins_pyhost_shutdown(host);
    SAO_TEST_ASSERT(rc == SAO_OK);
#else
    SAO_TEST_ASSERT(rc == SAO_ERR_NOT_IMPLEMENTED);
    SAO_TEST_ASSERT(host == nullptr);
#endif

    SAO_TEST_ASSERT(std::strcmp(sao_plugins_pyhost_status_to_python_exc(SAO_ERR_INVALID_ARGUMENT),
                       "ValueError") == 0);
    SAO_TEST_ASSERT(std::strcmp(sao_plugins_pyhost_status_to_python_exc(SAO_ERR_NOT_IMPLEMENTED),
                       "NotImplementedError") == 0);

    std::printf("python_host smoke test passed\n");
    return 0;
}
