// test_angel.cpp — smoke test (无 Catch2)
//
// version 字符串 + gate 存在性检测。真功能测走 test_as_wave3.cpp。

#include "sao/plugins/angel_host/as_host.h"

#include <cassert>
#include <cstdio>
#include <cstring>

// wave3 便利
extern "C" {
    SAO_PLUGINS_API bool SAO_PLUGINS_CALL sao_plugins_ashost_is_available(void);
}

int main() {
    using namespace sao::plugins::angel_host;

    const char* v = sao_plugins_ashost_version();
    assert(v != nullptr);
    assert(std::strlen(v) > 0);

    bool avail = sao_plugins_ashost_is_available();
    if (avail) {
        as_host_config cfg{};
        as_host_handle_t h = nullptr;
        int32_t rc = sao_plugins_ashost_create(&cfg, &h);
        assert(rc == SAO_OK);
        assert(h != nullptr);
        assert(sao_plugins_ashost_engine(h) != nullptr);
        sao_plugins_ashost_destroy(h);
        std::printf("angel_host smoke test passed (real AngelScript %s)\n", v);
    } else {
        as_host_config cfg{};
        as_host_handle_t h = nullptr;
        int32_t rc = sao_plugins_ashost_create(&cfg, &h);
        assert(rc == SAO_ERR_NOT_IMPLEMENTED);
        assert(h == nullptr);
        std::printf("angel_host smoke test passed (stub: %s)\n", v);
    }
    return 0;
}
