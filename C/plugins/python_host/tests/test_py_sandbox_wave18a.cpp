// test_py_sandbox_wave18a.cpp — Wave 18 / Agent a
//
// 10 CASE (Catch2):
//   1. arm_creates_interpreter_returns_ok
//   2. arm_null_plugin_id_returns_invalid_argument
//   3. arm_duplicate_plugin_id_returns_already_exists
//   4. arm_registers_math_and_json_but_not_os
//   5. arm_denies_import_ctypes_module
//   6. arm_denies_builtins_open_when_io_deny
//   7. arm_config_allow_extra_module_appends_whitelist
//   8. disarm_releases_interpreter
//   9. disarm_unknown_plugin_returns_not_found
//  10. concurrent_arm_two_plugins_safe
//
// 边界: 全部走**合成脚本**, 不加载任何不受信 Python 源. 需要 Python embed 才
// 真跑, 否则全 SKIP.

#include <catch2/catch_test_macros.hpp>

#include "sao/plugins/python_host/py_host.h"
#include "sao/plugins/python_host/py_sandbox.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace sao::plugins::python_host;

#ifndef SAO_TEST_PYTHON_HOME
#  define SAO_TEST_PYTHON_HOME L""
#endif

namespace {

py_host_handle_t g_host = nullptr;
bool g_host_ready = false;

// 挂 host 一次 (跨 test 复用).
py_host_handle_t ensure_host() {
    if (g_host_ready) return g_host;
    py_host_config cfg{};
    cfg.python_home = SAO_TEST_PYTHON_HOME;
    cfg.register_sao_sdk = true;
    cfg.controlled_test_shim = true;
    int32_t rc = sao_plugins_pyhost_init(&cfg, &g_host);
    if (rc != SAO_OK) {
        std::printf("[wave18a] pyhost_init rc=%d (embed missing?), skip\n", rc);
        g_host_ready = false;
        g_host = nullptr;
        return nullptr;
    }
    g_host_ready = true;
    return g_host;
}

// disarm 若存在, 否则 no-op.
void safe_disarm(const char* pid) {
    (void)sao_plugins_pyhost_sandbox_disarm(pid);
}

py_sandbox_config make_default_cfg() {
    py_sandbox_config c{};
    c.permissions = 0;  // 无 perm → io_deny 生效
    c.import_whitelist = nullptr;
    c.import_whitelist_count = 0;
    c.import_blacklist = nullptr;
    c.import_blacklist_count = 0;
    c.strict_submodule_check = true;
    return c;
}

}  // namespace

TEST_CASE("py_sandbox wave18a :: arm_creates_interpreter_returns_ok",
          "[plugins][python][wave18a]") {
    if (ensure_host() == nullptr) {
        SUCCEED("Python embed missing, skip");
        return;
    }
    py_sandbox_config cfg = make_default_cfg();
    int32_t rc = sao_plugins_pyhost_sandbox_arm("case1_arm_ok", &cfg);
    REQUIRE(rc == SAO_OK);
    REQUIRE(sao_plugins_pyhost_sandbox_active_count() >= 1);
    safe_disarm("case1_arm_ok");
}

TEST_CASE("py_sandbox wave18a :: arm_null_plugin_id_returns_invalid_argument",
          "[plugins][python][wave18a]") {
    if (ensure_host() == nullptr) {
        SUCCEED("Python embed missing, skip");
        return;
    }
    py_sandbox_config cfg = make_default_cfg();
    int32_t rc = sao_plugins_pyhost_sandbox_arm(nullptr, &cfg);
    REQUIRE(rc == SAO_ERR_INVALID_ARGUMENT);
    rc = sao_plugins_pyhost_sandbox_arm("", &cfg);
    REQUIRE(rc == SAO_ERR_INVALID_ARGUMENT);
    rc = sao_plugins_pyhost_sandbox_arm("case2_null_cfg", nullptr);
    REQUIRE(rc == SAO_ERR_INVALID_ARGUMENT);
}

TEST_CASE("py_sandbox wave18a :: arm_duplicate_plugin_id_returns_already_exists",
          "[plugins][python][wave18a]") {
    if (ensure_host() == nullptr) {
        SUCCEED("Python embed missing, skip");
        return;
    }
    py_sandbox_config cfg = make_default_cfg();
    int32_t rc = sao_plugins_pyhost_sandbox_arm("case3_dup", &cfg);
    REQUIRE(rc == SAO_OK);
    rc = sao_plugins_pyhost_sandbox_arm("case3_dup", &cfg);
    REQUIRE(rc == SAO_ERR_HANDLE_INVALID);  // already_exists 语义
    safe_disarm("case3_dup");
}

TEST_CASE("py_sandbox wave18a :: arm_registers_math_and_json_but_not_os",
          "[plugins][python][wave18a]") {
    if (ensure_host() == nullptr) {
        SUCCEED("Python embed missing, skip");
        return;
    }
    py_sandbox_config cfg = make_default_cfg();
    int32_t rc = sao_plugins_pyhost_sandbox_arm("case4_whitelist", &cfg);
    REQUIRE(rc == SAO_OK);

    // math 允许.
    bool raised = false;
    char err[64] = {};
    rc = sao_plugins_pyhost_sandbox_exec_test(
        "case4_whitelist", "import math\nresult=math.sqrt(4.0)\n",
        &raised, err, sizeof(err));
    REQUIRE(rc == SAO_OK);
    REQUIRE_FALSE(raised);

    // json 允许.
    raised = false; err[0] = '\0';
    rc = sao_plugins_pyhost_sandbox_exec_test(
        "case4_whitelist", "import json\nresult=json.dumps({'x':1})\n",
        &raised, err, sizeof(err));
    REQUIRE(rc == SAO_OK);
    REQUIRE_FALSE(raised);

    // os 拒 (不在 whitelist, permissions=0).
    raised = false; err[0] = '\0';
    rc = sao_plugins_pyhost_sandbox_exec_test(
        "case4_whitelist", "import os\n",
        &raised, err, sizeof(err));
    REQUIRE(rc == SAO_OK);
    REQUIRE(raised);
    REQUIRE(std::strcmp(err, "ImportError") == 0);

    safe_disarm("case4_whitelist");
}

TEST_CASE("py_sandbox wave18a :: arm_denies_import_ctypes_module",
          "[plugins][python][wave18a]") {
    if (ensure_host() == nullptr) {
        SUCCEED("Python embed missing, skip");
        return;
    }
    py_sandbox_config cfg = make_default_cfg();
    // 即便加了 unsafe, ctypes 在默认 blacklist, 应仍拒.
    cfg.permissions = static_cast<uint32_t>(permission_flag::unsafe);
    int32_t rc = sao_plugins_pyhost_sandbox_arm("case5_ctypes", &cfg);
    REQUIRE(rc == SAO_OK);

    bool raised = false;
    char err[64] = {};
    rc = sao_plugins_pyhost_sandbox_exec_test(
        "case5_ctypes", "import ctypes\n", &raised, err, sizeof(err));
    REQUIRE(rc == SAO_OK);
    REQUIRE(raised);
    REQUIRE(std::strcmp(err, "ImportError") == 0);

    // subprocess 同样.
    raised = false; err[0] = '\0';
    rc = sao_plugins_pyhost_sandbox_exec_test(
        "case5_ctypes", "import subprocess\n", &raised, err, sizeof(err));
    REQUIRE(rc == SAO_OK);
    REQUIRE(raised);
    REQUIRE(std::strcmp(err, "ImportError") == 0);

    safe_disarm("case5_ctypes");
}

TEST_CASE("py_sandbox wave18a :: arm_denies_builtins_open_when_io_deny",
          "[plugins][python][wave18a]") {
    if (ensure_host() == nullptr) {
        SUCCEED("Python embed missing, skip");
        return;
    }
    py_sandbox_config cfg = make_default_cfg();
    cfg.permissions = 0;  // 无 fs → io_deny
    int32_t rc = sao_plugins_pyhost_sandbox_arm("case6_open", &cfg);
    REQUIRE(rc == SAO_OK);

    bool raised = false;
    char err[64] = {};
    // 尝试打开一个绝对不存在的路径; 若沙箱生效, 先被 PermissionError 拦.
    // 不该到达 FileNotFoundError.
    rc = sao_plugins_pyhost_sandbox_exec_test(
        "case6_open", "open('sao_sandbox_probe_does_not_exist.txt', 'r')\n",
        &raised, err, sizeof(err));
    REQUIRE(rc == SAO_OK);
    REQUIRE(raised);
    REQUIRE(std::strcmp(err, "PermissionError") == 0);

    safe_disarm("case6_open");
}

TEST_CASE("py_sandbox wave18a :: arm_config_allow_extra_module_appends_whitelist",
          "[plugins][python][wave18a]") {
    if (ensure_host() == nullptr) {
        SUCCEED("Python embed missing, skip");
        return;
    }
    // 追加 stat (不在默认 whitelist, 纯常量模块, 无深层依赖), 应允.
    const char* extras[] = { "stat" };
    py_sandbox_config cfg = make_default_cfg();
    cfg.import_whitelist = extras;
    cfg.import_whitelist_count = 1;

    int32_t rc = sao_plugins_pyhost_sandbox_arm("case7_extra", &cfg);
    REQUIRE(rc == SAO_OK);

    bool raised = false;
    char err[64] = {};
    rc = sao_plugins_pyhost_sandbox_exec_test(
        "case7_extra", "import stat\nresult=stat.S_IFDIR\n",
        &raised, err, sizeof(err));
    REQUIRE(rc == SAO_OK);
    REQUIRE_FALSE(raised);

    // 未追加的 hashlib 也允 (默认 whitelist), 只是为 sanity.
    raised = false; err[0] = '\0';
    rc = sao_plugins_pyhost_sandbox_exec_test(
        "case7_extra", "import hashlib\n",
        &raised, err, sizeof(err));
    REQUIRE(rc == SAO_OK);
    REQUIRE_FALSE(raised);

    // socket 仍拒.
    raised = false; err[0] = '\0';
    rc = sao_plugins_pyhost_sandbox_exec_test(
        "case7_extra", "import socket\n",
        &raised, err, sizeof(err));
    REQUIRE(rc == SAO_OK);
    REQUIRE(raised);
    REQUIRE(std::strcmp(err, "ImportError") == 0);

    safe_disarm("case7_extra");
}

TEST_CASE("py_sandbox wave18a :: disarm_releases_interpreter",
          "[plugins][python][wave18a]") {
    if (ensure_host() == nullptr) {
        SUCCEED("Python embed missing, skip");
        return;
    }
    py_sandbox_config cfg = make_default_cfg();
    int32_t rc = sao_plugins_pyhost_sandbox_arm("case8_disarm", &cfg);
    REQUIRE(rc == SAO_OK);
    size_t before = sao_plugins_pyhost_sandbox_active_count();
    REQUIRE(before >= 1);
    rc = sao_plugins_pyhost_sandbox_disarm("case8_disarm");
    REQUIRE(rc == SAO_OK);
    size_t after = sao_plugins_pyhost_sandbox_active_count();
    REQUIRE(after == before - 1);
    // 二次 disarm → HANDLE_INVALID (not_found 语义).
    rc = sao_plugins_pyhost_sandbox_disarm("case8_disarm");
    REQUIRE(rc == SAO_ERR_HANDLE_INVALID);
}

TEST_CASE("py_sandbox wave18a :: disarm_unknown_plugin_returns_not_found",
          "[plugins][python][wave18a]") {
    if (ensure_host() == nullptr) {
        SUCCEED("Python embed missing, skip");
        return;
    }
    int32_t rc = sao_plugins_pyhost_sandbox_disarm("case9_never_armed");
    REQUIRE(rc == SAO_ERR_HANDLE_INVALID);  // not_found 语义
    rc = sao_plugins_pyhost_sandbox_disarm(nullptr);
    REQUIRE(rc == SAO_ERR_INVALID_ARGUMENT);
    rc = sao_plugins_pyhost_sandbox_disarm("");
    REQUIRE(rc == SAO_ERR_INVALID_ARGUMENT);
}

TEST_CASE("py_sandbox wave18a :: concurrent_arm_two_plugins_safe",
          "[plugins][python][wave18a]") {
    if (ensure_host() == nullptr) {
        SUCCEED("Python embed missing, skip");
        return;
    }
    // 两个独立 plugin 都 arm, 都能各自 exec, 各自 disarm.
    py_sandbox_config cfg = make_default_cfg();

    int32_t rc1 = sao_plugins_pyhost_sandbox_arm("case10_a", &cfg);
    int32_t rc2 = sao_plugins_pyhost_sandbox_arm("case10_b", &cfg);
    REQUIRE(rc1 == SAO_OK);
    REQUIRE(rc2 == SAO_OK);

    bool raised = false;
    char err[64] = {};
    rc1 = sao_plugins_pyhost_sandbox_exec_test(
        "case10_a", "import math\nresult=math.pi\n", &raised, err, sizeof(err));
    REQUIRE(rc1 == SAO_OK);
    REQUIRE_FALSE(raised);

    raised = false; err[0] = '\0';
    rc2 = sao_plugins_pyhost_sandbox_exec_test(
        "case10_b", "import json\nresult=json.dumps({'k':2})\n", &raised, err, sizeof(err));
    REQUIRE(rc2 == SAO_OK);
    REQUIRE_FALSE(raised);

    // 各自互不感知: a 里 exec 只影响 a 的 subinterp.
    // 用一个变量 __sao_isolation_probe__ = 1 验证.
    raised = false; err[0] = '\0';
    (void)sao_plugins_pyhost_sandbox_exec_test(
        "case10_a", "__sao_isolation_probe__ = 12345\n", &raised, err, sizeof(err));
    REQUIRE_FALSE(raised);

    // b 里查同名变量应 NameError.
    raised = false; err[0] = '\0';
    (void)sao_plugins_pyhost_sandbox_exec_test(
        "case10_b", "_ = __sao_isolation_probe__\n", &raised, err, sizeof(err));
    REQUIRE(raised);
    REQUIRE(std::strcmp(err, "NameError") == 0);

    safe_disarm("case10_a");
    safe_disarm("case10_b");
}
