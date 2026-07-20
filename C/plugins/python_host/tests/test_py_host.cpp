// Lightweight Python host smoke test for both embedded and fallback builds.

#include "sao/plugins/python_host/py_error.h"
#include "sao/plugins/python_host/py_host.h"

#include <cassert>
#include <cstdio>
#include <cstring>

#ifndef SAO_TEST_PYTHON_HOME
#  define SAO_TEST_PYTHON_HOME L""
#endif

int main() {
    using namespace sao::plugins::python_host;

    py_host_config config{};
    config.python_home = SAO_TEST_PYTHON_HOME;
    py_host_handle_t host = nullptr;
    int32_t rc = sao_plugins_pyhost_init(&config, &host);
#if defined(SAO_HAS_PYTHON_EMBED)
    assert(rc == SAO_OK);
    assert(host != nullptr);
    rc = sao_plugins_pyhost_shutdown(host);
    assert(rc == SAO_OK);
#else
    assert(rc == SAO_ERR_NOT_IMPLEMENTED);
    assert(host == nullptr);
#endif

    assert(std::strcmp(sao_plugins_pyhost_status_to_python_exc(SAO_ERR_INVALID_ARGUMENT),
                       "ValueError") == 0);
    assert(std::strcmp(sao_plugins_pyhost_status_to_python_exc(SAO_ERR_NOT_IMPLEMENTED),
                       "NotImplementedError") == 0);

    std::printf("python_host smoke test passed\n");
    return 0;
}
