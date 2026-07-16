// test_compat_runtime_wave4.cpp — G2.8 compat runtime shim 首切片单测
//
// 5 CASE:
//   1. compat_ctx_v1_register_alias_lookup
//   2. compat_ctx_v1_add_hotkey_alias_hits
//   3. compat_libs_vendor_probe_finds_dirs
//   4. compat_format_python_sys_path_uses_pathsep
//   5. compat_format_lua_package_path_pattern

#include "sao/plugins/compat/py_v1_ctx_shim.h"
#include "sao/plugins/compat/libs_vendor_bridge.h"
#include "sao/sdk/sao_sdk.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <string>

using namespace sao::plugins::compat;

namespace {

// method_id 常量 (对齐 sdk_binding::sdk_method_id 但脱离编译依赖):
//   method_register_hotkey = 34 (对齐枚举顺序; 也可直接引 sdk_method_id)
// 为独立性起见, 用几个数字模拟 method_id 语义。
constexpr uint16_t k_method_add_hotkey = 34;
constexpr uint16_t k_method_register_ui_panel = 30;
constexpr uint16_t k_method_publish_event = 15;

// CASE 1: register + lookup 单向
void case_compat_ctx_v1_register_alias_lookup() {
    sao_plugins_compat_ctx_v1_clear_aliases();
    // 注册两个别名
    int32_t rc = sao_plugins_compat_ctx_v1_register_alias(
        "add_hotkey", k_method_add_hotkey);
    assert(rc == SAO_OK);
    rc = sao_plugins_compat_ctx_v1_register_alias(
        "register_script", k_method_register_ui_panel);
    assert(rc == SAO_OK);

    // 查一下
    uint16_t id = sao_plugins_compat_ctx_v1_lookup_alias("add_hotkey");
    assert(id == k_method_add_hotkey);
    id = sao_plugins_compat_ctx_v1_lookup_alias("register_script");
    assert(id == k_method_register_ui_panel);

    // 未注册的返 0xFFFF
    id = sao_plugins_compat_ctx_v1_lookup_alias("nonexistent_method");
    assert(id == 0xFFFF);

    // 空 / null 参数
    id = sao_plugins_compat_ctx_v1_lookup_alias(nullptr);
    assert(id == 0xFFFF);
    id = sao_plugins_compat_ctx_v1_lookup_alias("");
    assert(id == 0xFFFF);

    rc = sao_plugins_compat_ctx_v1_register_alias(nullptr, 0);
    assert(rc == SAO_ERR_INVALID_ARGUMENT);
    rc = sao_plugins_compat_ctx_v1_register_alias("", 0);
    assert(rc == SAO_ERR_INVALID_ARGUMENT);

    std::printf("  [OK] compat_ctx_v1_register_alias_lookup\n");
}

// CASE 2: add_hotkey alias 命中 + wrap
void case_compat_ctx_v1_add_hotkey_alias_hits() {
    sao_plugins_compat_ctx_v1_clear_aliases();
    // 老 name "add_hotkey" → 新 method ADD_HOTKEY
    int32_t rc = sao_plugins_compat_ctx_v1_register_alias(
        "add_hotkey", k_method_add_hotkey);
    assert(rc == SAO_OK);

    // 老 name "emit" 也是 v1 (对齐 old event_bus)
    rc = sao_plugins_compat_ctx_v1_register_alias(
        "publish", k_method_publish_event);
    assert(rc == SAO_OK);

    uint16_t hit1 = sao_plugins_compat_ctx_v1_lookup_alias("add_hotkey");
    assert(hit1 == k_method_add_hotkey);
    uint16_t hit2 = sao_plugins_compat_ctx_v1_lookup_alias("publish");
    assert(hit2 == k_method_publish_event);

    // wrap: 只接受真实现代 SDK context, 不接受任意 non-null 指针
    SaoSdkContext sdk_ctx{};
    assert(sao_sdk_bind_context("compat.wave4", "1.0", &sdk_ctx) == SAO_SDK_OK);
    plugin_context_ptr fake_ctx =
        reinterpret_cast<plugin_context_ptr>(&sdk_ctx);
    plugin_context_ptr wrapped = nullptr;
    rc = sao_plugins_compat_ctx_v1_wrap(fake_ctx, &wrapped);
    assert(rc == SAO_OK);
    assert(wrapped == fake_ctx);   // 同一现代 context handle

    // wrap null 应 INVALID_ARGUMENT
    rc = sao_plugins_compat_ctx_v1_wrap(nullptr, &wrapped);
    assert(rc == SAO_ERR_INVALID_ARGUMENT);

    sao_sdk_context_destroy(&sdk_ctx);

    std::printf("  [OK] compat_ctx_v1_add_hotkey_alias_hits\n");
}

// CASE 3: libs/vendor probe 找目录
void case_compat_libs_vendor_probe_finds_dirs() {
    // 造两个临时目录 (libs + vendor 都建, engine 不建)
    namespace fs = std::filesystem;
    fs::path root = fs::temp_directory_path() / "sao_wave4_probe_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    fs::create_directories(root / "libs", ec);
    fs::create_directories(root / "vendor", ec);
    // engine/ 故意不建

    discovered_deps_dirs dirs{};
    int32_t rc = sao_plugins_compat_libs_vendor_probe(
        root.wstring().c_str(), &dirs);
    assert(rc == SAO_OK);
    assert(dirs.libs_dirs.size() == 1);
    assert(dirs.vendor_dirs.size() == 1);
    assert(dirs.engine_dirs.size() == 0);   // 不存在的目录不加入
    assert(dirs.ordered.size() == 2);       // libs + vendor
    // 顺序: engine (缺) → libs → vendor
    assert(dirs.ordered[0].find(L"libs") != std::wstring::npos);
    assert(dirs.ordered[1].find(L"vendor") != std::wstring::npos);

    // null 参数
    rc = sao_plugins_compat_libs_vendor_probe(nullptr, &dirs);
    assert(rc == SAO_ERR_INVALID_ARGUMENT);
    rc = sao_plugins_compat_libs_vendor_probe(root.wstring().c_str(), nullptr);
    assert(rc == SAO_ERR_INVALID_ARGUMENT);

    // 建 engine 再扫: 顺序应是 engine → libs → vendor
    fs::create_directories(root / "engine", ec);
    discovered_deps_dirs dirs2{};
    rc = sao_plugins_compat_libs_vendor_probe(root.wstring().c_str(), &dirs2);
    assert(rc == SAO_OK);
    assert(dirs2.engine_dirs.size() == 1);
    assert(dirs2.ordered.size() == 3);
    assert(dirs2.ordered[0].find(L"engine") != std::wstring::npos);
    assert(dirs2.ordered[1].find(L"libs") != std::wstring::npos);
    assert(dirs2.ordered[2].find(L"vendor") != std::wstring::npos);

    fs::remove_all(root, ec);
    std::printf("  [OK] compat_libs_vendor_probe_finds_dirs\n");
}

// CASE 4: python sys.path 用 os.pathsep 拼接
void case_compat_format_python_sys_path_uses_pathsep() {
    discovered_deps_dirs dirs{};
    dirs.ordered.push_back(L"C:\\a\\engine");
    dirs.ordered.push_back(L"C:\\a\\libs");
    dirs.ordered.push_back(L"C:\\a\\vendor");

    wchar_t buf[512] = {};
    int32_t rc = sao_plugins_compat_format_python_sys_path(
        &dirs, buf, sizeof(buf) / sizeof(wchar_t));
    assert(rc == SAO_OK);
    std::wstring out(buf);
#if defined(_WIN32)
    // Windows: ';' 分隔
    assert(out == L"C:\\a\\engine;C:\\a\\libs;C:\\a\\vendor");
#else
    assert(out == L"C:\\a\\engine:C:\\a\\libs:C:\\a\\vendor");
#endif

    // 空 dirs → 空串
    discovered_deps_dirs empty{};
    rc = sao_plugins_compat_format_python_sys_path(
        &empty, buf, sizeof(buf) / sizeof(wchar_t));
    assert(rc == SAO_OK);
    assert(std::wstring(buf).empty());

    // buffer 太小 → BUFFER_TOO_SMALL
    wchar_t tiny[4] = {};
    rc = sao_plugins_compat_format_python_sys_path(&dirs, tiny, 4);
    assert(rc == SAO_ERR_BUFFER_TOO_SMALL);

    std::printf("  [OK] compat_format_python_sys_path_uses_pathsep\n");
}

// CASE 5: lua package.path 用 "?.lua" pattern
void case_compat_format_lua_package_path_pattern() {
    discovered_deps_dirs dirs{};
    dirs.ordered.push_back(L"C:\\a\\libs");
    dirs.ordered.push_back(L"C:\\a\\vendor");

    wchar_t buf[1024] = {};
    int32_t rc = sao_plugins_compat_format_lua_package_path(
        &dirs, buf, sizeof(buf) / sizeof(wchar_t));
    assert(rc == SAO_OK);
    std::wstring out(buf);
    // 每目录都应展开为 "<dir>\?.lua" + "<dir>\?\init.lua"
    assert(out.find(L"C:\\a\\libs\\?.lua") != std::wstring::npos);
    assert(out.find(L"C:\\a\\libs\\?\\init.lua") != std::wstring::npos);
    assert(out.find(L"C:\\a\\vendor\\?.lua") != std::wstring::npos);
    assert(out.find(L"C:\\a\\vendor\\?\\init.lua") != std::wstring::npos);

    // 空 dirs → 空串
    discovered_deps_dirs empty{};
    rc = sao_plugins_compat_format_lua_package_path(
        &empty, buf, sizeof(buf) / sizeof(wchar_t));
    assert(rc == SAO_OK);
    assert(std::wstring(buf).empty());

    // null dirs
    rc = sao_plugins_compat_format_lua_package_path(
        nullptr, buf, sizeof(buf) / sizeof(wchar_t));
    assert(rc == SAO_ERR_INVALID_ARGUMENT);

    std::printf("  [OK] compat_format_lua_package_path_pattern\n");
}

} // namespace

int main() {
    std::printf("test_compat_runtime_wave4:\n");
    case_compat_ctx_v1_register_alias_lookup();
    case_compat_ctx_v1_add_hotkey_alias_hits();
    case_compat_libs_vendor_probe_finds_dirs();
    case_compat_format_python_sys_path_uses_pathsep();
    case_compat_format_lua_package_path_pattern();
    std::printf("test_compat_runtime_wave4: 5 cases passed\n");
    return 0;
}
