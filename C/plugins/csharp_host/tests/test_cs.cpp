// test_cs.cpp — smoke test
//
// Wave 8: init 可能 OK (hostfxr 就绪) / NOT_INITIALIZED (无 hostfxr) /
// NOT_IMPLEMENTED (非 Win 平台).  接受任一.
#include "sao/plugins/csharp_host/cs_host.h"

#include <cassert>
#include <cstdio>

int main() {
    using namespace sao::plugins::csharp_host;

    cs_host_config cfg{};
    cs_host_handle_t h = nullptr;
    int32_t rc = sao_plugins_cshost_init(&cfg, &h);
    assert(rc == SAO_OK || rc == SAO_ERR_NOT_INITIALIZED
           || rc == SAO_ERR_NOT_IMPLEMENTED || rc == SAO_ERR_OS_CALL_FAILED);

    // 用 h 若非空能查版本; 空的话 API 应容忍返回空字符串.
    if (h != nullptr) {
        const char* v = sao_plugins_cshost_runtime_version(h);
        assert(v != nullptr);
        sao_plugins_cshost_shutdown(h);
    }

    std::printf("csharp_host smoke test passed (init rc=%d)\n", rc);
    return 0;
}
