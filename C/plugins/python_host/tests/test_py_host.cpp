// test_py_host.cpp — smoke test (Wave2 后: 兼容 stub 与 embed 双模式)
//
// Wave 1 时 py_host 是纯 stub, init 返 NOT_IMPLEMENTED. Wave 2 加了 Python
// embed 实装, 若 CMake 找到 Python3::Python 则 init 真跑, 否则仍走 stub.
// 本 smoke test 用 SAO_HAS_PYTHON_EMBED 编译时分支两种情况都覆盖.
#include "sao/plugins/python_host/py_host.h"
#include "sao/plugins/python_host/py_error.h"

#include <cassert>
#include <cstdio>
#include <cstring>

int main() {
    using namespace sao::plugins::python_host;

    py_host_config cfg{};
    py_host_handle_t h = nullptr;
    int32_t rc = sao_plugins_pyhost_init(&cfg, &h);
#if defined(SAO_HAS_PYTHON_EMBED)
    // Wave 2 实装: init 应成功.
    assert(rc == SAO_OK);
    assert(h != nullptr);
    rc = sao_plugins_pyhost_shutdown(h);
    assert(rc == SAO_OK);
#else
    // stub: NOT_IMPLEMENTED, handle 保持 nullptr.
    assert(rc == SAO_ERR_NOT_IMPLEMENTED);
    assert(h == nullptr);
#endif

    // 状态 → exception 名映射稳定 (与 embed 无关)
    assert(std::strcmp(sao_plugins_pyhost_status_to_python_exc(SAO_ERR_INVALID_ARGUMENT),
                       "ValueError") == 0);
    assert(std::strcmp(sao_plugins_pyhost_status_to_python_exc(SAO_ERR_NOT_IMPLEMENTED),
                       "NotImplementedError") == 0);

    std::printf("python_host smoke test passed\n");
    return 0;
}
