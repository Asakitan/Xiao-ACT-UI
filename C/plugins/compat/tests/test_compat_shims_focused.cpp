#include "sao/plugins/compat/libs_vendor_bridge.h"
#include "sao/plugins/compat/migration.h"
#include "sao/plugins/compat/py_v1_manifest.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace {

namespace fs = std::filesystem;
using namespace sao::plugins::compat;
using sao::plugins::compat::discovered_deps_dirs;
using sao::plugins::loader::engine_kind;

class temporary_tree {
  public:
    temporary_tree() {
        const auto suffix = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        root_ = fs::temp_directory_path() / ("sao_compat_focused_" + std::to_string(suffix));
        REQUIRE(fs::create_directories(root_));
    }

    ~temporary_tree() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    const fs::path& root() const {
        return root_;
    }

  private:
    fs::path root_;
};

void write_text(const fs::path& path, const std::string& content) {
    const bool parent_ready =
        fs::create_directories(path.parent_path()) || fs::is_directory(path.parent_path());
    REQUIRE(parent_ready);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.is_open());
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    REQUIRE(output.good());
}

std::string path_utf8(const fs::path& path) {
#if defined(_WIN32)
    const std::wstring wide = path.wstring();
    const int size =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                            static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    REQUIRE(size > 0);
    std::string output(static_cast<size_t>(size), '\0');
    REQUIRE(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                                static_cast<int>(wide.size()), output.data(), size, nullptr,
                                nullptr) == size);
    return output;
#else
    return path.string();
#endif
}

nlohmann::json scan_report(const fs::path& plugin) {
    char* report = nullptr;
    const std::string input = path_utf8(plugin);
    REQUIRE(sao_plugins_compat_scan_deprecated(input.c_str(), &report) == SAO_OK);
    REQUIRE(report != nullptr);
    const auto parsed = nlohmann::json::parse(report);
    sao_plugins_compat_free_string(report);
    return parsed;
}

std::vector<std::string> old_names(const nlohmann::json& report) {
    std::vector<std::string> output;
    for (const auto& item : report.at("diagnostics")) {
        output.push_back(item.at("old_name").get<std::string>());
    }
    return output;
}

} // namespace

TEST_CASE("compat dependency discovery is canonical, contained and ordered",
          "[plugins][compat][focused]") {
    temporary_tree tree;
    REQUIRE(fs::create_directories(tree.root() / "engine"));
    REQUIRE(fs::create_directories(tree.root() / "libs"));
    REQUIRE(fs::create_directories(tree.root() / "vendor"));

    discovered_deps_dirs dirs;
    REQUIRE(sao_plugins_compat_discover_deps_dirs(tree.root().c_str(), &dirs) == SAO_OK);
    REQUIRE(dirs.engine_dirs.size() == 1);
    REQUIRE(dirs.libs_dirs.size() == 1);
    REQUIRE(dirs.vendor_dirs.size() == 1);
    REQUIRE(dirs.ordered ==
            std::vector<std::wstring>{fs::canonical(tree.root() / "engine").wstring(),
                                      fs::canonical(tree.root() / "libs").wstring(),
                                      fs::canonical(tree.root() / "vendor").wstring()});

    discovered_deps_dirs probed;
    REQUIRE(sao_plugins_compat_libs_vendor_probe(tree.root().c_str(), &probed) == SAO_OK);
    REQUIRE(probed.ordered == dirs.ordered);

    sao_plugins_compat_free_deps_dirs(&dirs);
    REQUIRE(dirs.ordered.empty());
    REQUIRE(dirs.engine_dirs.empty());
    REQUIRE(sao_plugins_compat_discover_deps_dirs(nullptr, &dirs) == SAO_ERR_INVALID_ARGUMENT);
    REQUIRE(sao_plugins_compat_discover_deps_dirs(tree.root().c_str(), nullptr) ==
            SAO_ERR_INVALID_ARGUMENT);
    REQUIRE(sao_plugins_compat_discover_deps_dirs((tree.root() / "missing").c_str(), &dirs) ==
            SAO_ERR_HANDLE_INVALID);
}

TEST_CASE("compat language path arrays own strings and deduplicate stably",
          "[plugins][compat][focused]") {
    discovered_deps_dirs dirs;
    dirs.engine_dirs = {L"C:\\plugin\\engine"};
    dirs.libs_dirs = {L"C:\\plugin\\libs"};
    dirs.vendor_dirs = {L"C:\\plugin\\vendor"};
    dirs.ordered = {L"C:\\plugin\\vendor", L"C:\\plugin\\engine", L"C:\\plugin\\libs\\..\\libs"};
    const auto input_snapshot = dirs;

    for (const auto language :
         {engine_kind::python, engine_kind::emma, engine_kind::angelscript, engine_kind::csharp}) {
        wchar_t** paths = nullptr;
        size_t count = 0;
        REQUIRE(sao_plugins_compat_format_paths_for_language(&dirs, language, &paths, &count) ==
                SAO_OK);
        REQUIRE(count == 3);
        REQUIRE(paths != nullptr);
        REQUIRE(std::wstring(paths[0]) == L"C:\\plugin\\engine");
        REQUIRE(std::wstring(paths[1]) == L"C:\\plugin\\libs");
        REQUIRE(std::wstring(paths[2]) == L"C:\\plugin\\vendor");
        paths[0][0] = L'X';
        REQUIRE(dirs.engine_dirs == input_snapshot.engine_dirs);
        REQUIRE(dirs.libs_dirs == input_snapshot.libs_dirs);
        REQUIRE(dirs.vendor_dirs == input_snapshot.vendor_dirs);
        REQUIRE(dirs.ordered == input_snapshot.ordered);
        sao_plugins_compat_free_paths(paths, count);
    }

    wchar_t** lua_paths = nullptr;
    size_t lua_count = 0;
    REQUIRE(sao_plugins_compat_format_paths_for_language(&dirs, engine_kind::lua, &lua_paths,
                                                         &lua_count) == SAO_OK);
    REQUIRE(lua_count == 6);
    REQUIRE(std::wstring(lua_paths[0]) == L"C:\\plugin\\engine\\?.lua");
    REQUIRE(std::wstring(lua_paths[1]) == L"C:\\plugin\\engine\\?\\init.lua");
    REQUIRE(std::wstring(lua_paths[5]) == L"C:\\plugin\\vendor\\?\\init.lua");
    sao_plugins_compat_free_paths(lua_paths, lua_count);
    sao_plugins_compat_free_paths(nullptr, 99);

    wchar_t** invalid_paths = reinterpret_cast<wchar_t**>(1);
    size_t invalid_count = 99;
    REQUIRE(sao_plugins_compat_format_paths_for_language(&dirs, engine_kind::unknown,
                                                         &invalid_paths, &invalid_count) ==
            SAO_ERR_INVALID_ARGUMENT);
    REQUIRE(invalid_paths == nullptr);
    REQUIRE(invalid_count == 0);
    REQUIRE(sao_plugins_compat_format_paths_for_language(
                &dirs, engine_kind::python, nullptr, &invalid_count) == SAO_ERR_INVALID_ARGUMENT);
}

TEST_CASE("compat legacy formatters reuse ordered deduplicated paths",
          "[plugins][compat][focused]") {
    discovered_deps_dirs dirs;
    dirs.ordered = {L"C:\\plugin\\engine", L"C:\\plugin\\engine", L"C:\\plugin\\libs",
                    L"C:\\plugin\\vendor"};

    wchar_t python[256]{};
    REQUIRE(sao_plugins_compat_format_python_sys_path(&dirs, python, std::size(python)) == SAO_OK);
#if defined(_WIN32)
    REQUIRE(std::wstring(python) == L"C:\\plugin\\engine;C:\\plugin\\libs;C:\\plugin\\vendor");
#else
    REQUIRE(std::wstring(python) == L"C:\\plugin\\engine:C:\\plugin\\libs:C:\\plugin\\vendor");
#endif

    wchar_t lua[512]{};
    REQUIRE(sao_plugins_compat_format_lua_package_path(&dirs, lua, std::size(lua)) == SAO_OK);
    REQUIRE(std::wstring(lua) == L"C:\\plugin\\engine\\?.lua;C:\\plugin\\engine\\?\\init.lua;"
                                 L"C:\\plugin\\libs\\?.lua;C:\\plugin\\libs\\?\\init.lua;"
                                 L"C:\\plugin\\vendor\\?.lua;C:\\plugin\\vendor\\?\\init.lua");
}

TEST_CASE("migration scan reports only structured manifest keys and ctx calls",
          "[plugins][compat][focused]") {
    temporary_tree tree;
    const std::string manifest = R"({
  "id": "legacy.fixture",
  "entry": "src/plugin.py",
  "engine": "python",
  "deps": ["base"],
  "description": "runtime and description_short are ordinary values",
  "sao_menu": {"description_short": "legacy"},
  "capabilities": [{"capability_id": "panel"}]
})";
    const std::string source = R"PY(# ctx.register_script("comment")
text = "ctx.add_menu_item('string')"
keyboard.add_hotkey("CTRL+K", callback)
ctx.register_script("panel")
self._ctx.add_hotkey("toggle", callback)
ctx.add_hotkey("duplicate", callback)
)PY";
    write_text(tree.root() / "plugin.json", manifest);
    write_text(tree.root() / "src" / "plugin.py", source);
    write_text(tree.root() / "modules" / "menu.py", "context.add_menu_item('menu')\n");

    char* first = nullptr;
    char* second = nullptr;
    const std::string root_utf8 = path_utf8(tree.root());
    REQUIRE(sao_plugins_compat_scan_deprecated(root_utf8.c_str(), &first) == SAO_OK);
    REQUIRE(sao_plugins_compat_scan_deprecated(root_utf8.c_str(), &second) == SAO_OK);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    REQUIRE(std::string(first) == std::string(second));

    const auto report = nlohmann::json::parse(first);
    sao_plugins_compat_free_string(first);
    sao_plugins_compat_free_string(second);
    REQUIRE(report.at("plugin_id") == "legacy.fixture");
    REQUIRE(report.at("count") == 7);
    REQUIRE(old_names(report) == std::vector<std::string>{"engine", "deps", "description_short",
                                                          "capability_id", "register_script",
                                                          "add_hotkey", "add_menu_item"});
    for (const auto& item : report.at("diagnostics")) {
        REQUIRE(item.at("line").get<size_t>() > 0);
        REQUIRE(item.at("column").get<size_t>() > 0);
        if (item.at("old_name") == "add_menu_item") {
            REQUIRE(item.at("file") == "modules/menu.py");
        }
    }
}

TEST_CASE("migration scan suppresses source noise and blocks entry escape",
          "[plugins][compat][focused]") {
    temporary_tree tree;
    const fs::path plugin = tree.root() / "plugin";
    write_text(
        plugin / "plugin.json",
        R"({"id":"modern","language":"python","entry":"../outside.py","description":"engine deps description_short"})");
    write_text(tree.root() / "outside.py", "ctx.add_hotkey('escape', callback)\n");
    write_text(plugin / "src" / "noise.lua", "--[=[\nctx.register_script('comment')\n]=]\n"
                                             "local text = [==[ctx.add_menu_item('string')]==]\n"
                                             "local other = [=[ctx.add_hotkey('string')]=]\n");
    const auto escaped = scan_report(plugin);
    REQUIRE(escaped.at("count") == 0);

    write_text(plugin / "plugin.json",
               R"({"id":"vendor-noise","language":"python","entry":"vendor/noise.py"})");
    write_text(plugin / "vendor" / "noise.py", "ctx.register_script('dependency')\n");
    const auto dependency = scan_report(plugin / "plugin.json");
    REQUIRE(dependency.at("count") == 0);
}

TEST_CASE("migration scan validates arguments and malformed inputs", "[plugins][compat][focused]") {
    char* report = reinterpret_cast<char*>(1);
    REQUIRE(sao_plugins_compat_scan_deprecated(nullptr, &report) == SAO_ERR_INVALID_ARGUMENT);
    REQUIRE(report == nullptr);
    REQUIRE(sao_plugins_compat_scan_deprecated("", &report) == SAO_ERR_INVALID_ARGUMENT);
    REQUIRE(report == nullptr);
    REQUIRE(sao_plugins_compat_scan_deprecated("missing-plugin", &report) ==
            SAO_ERR_HANDLE_INVALID);
    REQUIRE(report == nullptr);
    REQUIRE(sao_plugins_compat_scan_deprecated("anything", nullptr) == SAO_ERR_INVALID_ARGUMENT);

    temporary_tree tree;
    write_text(tree.root() / "plugin.json", "{not-json");
    const std::string input = path_utf8(tree.root());
    REQUIRE(sao_plugins_compat_scan_deprecated(input.c_str(), &report) == SAO_ERR_INVALID_ARGUMENT);
    REQUIRE(report == nullptr);

    const char invalid_utf8[] = {static_cast<char>(0xC3), '(', '\0'};
    REQUIRE(sao_plugins_compat_scan_deprecated(invalid_utf8, &report) == SAO_ERR_INVALID_ARGUMENT);
    REQUIRE(report == nullptr);
}
