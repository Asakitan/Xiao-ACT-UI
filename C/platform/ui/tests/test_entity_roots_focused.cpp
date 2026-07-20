#include <catch2/catch_test_macros.hpp>

#include "sao/ui/abi.h"
#include "sao/ui/entity_shell.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t kMouseMove = 0x0200;
constexpr uint32_t kLeftButtonDown = 0x0201;
constexpr uint32_t kLeftButtonUp = 0x0202;
constexpr uint32_t kMouseWheel = 0x020A;
constexpr int32_t kMenuPad = 40;
constexpr int32_t kMenuSlot = 70;
constexpr int32_t kRootColumnCenter = 75;
constexpr int32_t kChildRowX = 180;
constexpr int32_t kChildRowY = 62;

struct ActionLog {
    int32_t calls{};
    int32_t last_action{-1};
};

sao_status_t SAO_UI_CALL record_action(SaoUiEntityAction action, void* user_data) {
    auto* log = static_cast<ActionLog*>(user_data);
    ++log->calls;
    log->last_action = static_cast<int32_t>(action);
    return SAO_STATUS_OK;
}

SaoUiEntityShellConfig make_config(ActionLog* log = nullptr) {
    SaoUiEntityShellConfig config{};
    config.width = 640;
    config.height = 720;
    config.origin_x = 100;
    config.origin_y = 200;
    config.action_fn = log == nullptr ? nullptr : &record_action;
    config.action_user_data = log;
    return config;
}

struct RootSpec {
    std::string id;
    std::string label;
    std::string icon;
    int32_t action_id{-1};
    bool can_activate{true};
    std::vector<std::string> child_names;
    std::vector<std::string> child_icons;
    std::vector<SaoUiMenuItem> children;
};

struct RootTree {
    std::vector<RootSpec> specs;
    std::vector<SaoUiEntityRootItem> roots;

    void rebuild() {
        for (auto& spec : specs) {
            spec.children.clear();
            spec.children.reserve(spec.child_names.size());
            for (size_t index = 0; index < spec.child_names.size(); ++index) {
                SaoUiMenuItem child{};
                child.name_utf8 = spec.child_names[index].c_str();
                child.icon_utf8 = spec.child_icons[index].c_str();
                child.action_id = spec.action_id + 1000 + static_cast<int32_t>(index);
                child.can_activate = true;
                spec.children.push_back(child);
            }
        }
        roots.clear();
        roots.reserve(specs.size());
        for (auto& spec : specs) {
            SaoUiEntityRootItem root{};
            root.struct_size = sizeof(SaoUiEntityRootItem);
            root.root_id_utf8 = spec.id.c_str();
            root.name_utf8 = spec.label.c_str();
            root.icon_utf8 = spec.icon.c_str();
            root.action_id = spec.action_id;
            root.can_activate = spec.can_activate;
            root.children = spec.children.empty() ? nullptr : spec.children.data();
            root.child_count = spec.children.size();
            roots.push_back(root);
        }
    }
};

RootTree make_roots(size_t count, bool with_children = false) {
    RootTree tree;
    tree.specs.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        RootSpec spec{};
        spec.id = "root-" + std::to_string(index);
        spec.label = "Root " + std::to_string(index);
        spec.icon = std::string(1, static_cast<char>('A' + index % 26U));
        spec.action_id = 200 + static_cast<int32_t>(index);
        if (with_children && index == 2U) {
            spec.child_names = {"Child 0", "Child 1"};
            spec.child_icons = {">", ">"};
        }
        tree.specs.push_back(std::move(spec));
    }
    tree.rebuild();
    return tree;
}

SaoUiEntityShellSnapshot shell_snapshot(sao_ui_entity_shell_handle_t shell) {
    SaoUiEntityShellSnapshot snapshot{};
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &snapshot) == SAO_STATUS_OK);
    return snapshot;
}

SaoUiEntityRootSnapshot root_snapshot(sao_ui_entity_shell_handle_t shell) {
    SaoUiEntityRootSnapshot snapshot{};
    REQUIRE(sao_ui_entity_shell_get_root_snapshot(shell, &snapshot) == SAO_STATUS_OK);
    return snapshot;
}

std::array<int32_t, 2> root_point(sao_ui_entity_shell_handle_t shell, int32_t physical_slot) {
    const auto snapshot = shell_snapshot(shell);
    return {snapshot.origin_x + snapshot.menu_x + kRootColumnCenter,
            snapshot.origin_y + snapshot.menu_y + kMenuPad + physical_slot * kMenuSlot +
                kMenuSlot / 2};
}

std::array<int32_t, 2> child_point(sao_ui_entity_shell_handle_t shell) {
    const auto snapshot = shell_snapshot(shell);
    return {snapshot.origin_x + snapshot.menu_x + kChildRowX,
            snapshot.origin_y + snapshot.menu_y + kChildRowY};
}

void open_menu(sao_ui_entity_shell_handle_t shell) {
    auto snapshot = shell_snapshot(shell);
    if (!snapshot.menu_visible) {
        REQUIRE(sao_ui_entity_shell_home(shell) == SAO_STATUS_OK);
    }
    REQUIRE(sao_ui_entity_shell_tick(shell, 450) == SAO_STATUS_OK);
}

void click(sao_ui_entity_shell_handle_t shell, const std::array<int32_t, 2>& point) {
    REQUIRE(sao_ui_entity_shell_handle_mouse(shell, kMouseMove, point[0], point[1], -1, 0) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_handle_mouse(shell, kLeftButtonDown, point[0], point[1], 0, 0) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_handle_mouse(shell, kLeftButtonUp, point[0], point[1], 0, 0) ==
            SAO_STATUS_OK);
}

void wheel(sao_ui_entity_shell_handle_t shell, int32_t delta) {
    const auto point = root_point(shell, 0);
    REQUIRE(sao_ui_entity_shell_handle_mouse(shell, kMouseWheel, point[0], point[1], -1, delta) ==
            SAO_STATUS_OK);
}

void destroy_shell(sao_ui_entity_shell_handle_t shell) {
    REQUIRE(sao_ui_entity_shell_take_offline(shell) == SAO_STATUS_OK);
    sao_ui_entity_shell_destroy(shell);
}

std::vector<uint8_t> pixels(sao_ui_entity_shell_handle_t shell) {
    uint32_t width = 0;
    uint32_t height = 0;
    size_t required = 0;
    REQUIRE(sao_ui_entity_shell_snapshot_bgra(shell, nullptr, 0, &width, &height, &required) ==
            SAO_STATUS_ERR_BUFFER_TOO_SMALL);
    std::vector<uint8_t> result(required);
    REQUIRE(sao_ui_entity_shell_snapshot_bgra(shell, result.data(), result.size(), &width, &height,
                                              &required) == SAO_STATUS_OK);
    return result;
}

} // namespace

TEST_CASE("Entity roots default to five entries on UI ABI 1.5", "[ui][entity_roots]") {
    CHECK(SAO_UI_ABI_VERSION_MAJOR == 1U);
    CHECK(SAO_UI_ABI_VERSION_MINOR == 5U);
    CHECK(SAO_UI_ABI_VERSION == 0x00010005U);
    CHECK(sao_ui_abi_version() == SAO_UI_ABI_VERSION);

    const auto config = make_config();
    sao_ui_entity_shell_handle_t shell = nullptr;
    REQUIRE(sao_ui_entity_shell_create(nullptr, &config, &shell) == SAO_STATUS_OK);
    const auto snapshot = root_snapshot(shell);
    CHECK(snapshot.root_count == 5U);
    CHECK(snapshot.first_visible_root_index == 0U);
    CHECK(snapshot.visible_root_count == 5U);
    CHECK(snapshot.root_tree_revision > 0U);
    CHECK(snapshot.active_root_id_utf8[0] == '\0');
    sao_ui_entity_shell_destroy(shell);
}

TEST_CASE("Entity root viewport scrolls and maps the sixth action", "[ui][entity_roots]") {
    ActionLog actions{};
    const auto config = make_config(&actions);
    sao_ui_entity_shell_handle_t shell = nullptr;
    REQUIRE(sao_ui_entity_shell_create(nullptr, &config, &shell) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_bring_online(shell) == SAO_STATUS_OK);
    auto tree = make_roots(7);
    REQUIRE(sao_ui_entity_shell_set_roots(shell, tree.roots.data(), tree.roots.size()) ==
            SAO_STATUS_OK);
    open_menu(shell);

    auto snapshot = root_snapshot(shell);
    CHECK(snapshot.root_count == 7U);
    CHECK(snapshot.first_visible_root_index == 0U);
    CHECK(snapshot.visible_root_count == 5U);
    wheel(shell, -120);
    snapshot = root_snapshot(shell);
    CHECK(snapshot.first_visible_root_index == 1U);
    click(shell, root_point(shell, 4));
    CHECK(actions.calls == 1);
    CHECK(actions.last_action == 205);
    snapshot = root_snapshot(shell);
    CHECK(std::string(snapshot.active_root_id_utf8) == "root-5");

    wheel(shell, -120 * 20);
    CHECK(root_snapshot(shell).first_visible_root_index == 2U);
    wheel(shell, -120);
    CHECK(root_snapshot(shell).first_visible_root_index == 2U);
    wheel(shell, 120 * 20);
    CHECK(root_snapshot(shell).first_visible_root_index == 0U);
    destroy_shell(shell);
}

TEST_CASE("Entity root stable IDs preserve active root and viewport", "[ui][entity_roots]") {
    const auto config = make_config();
    sao_ui_entity_shell_handle_t shell = nullptr;
    REQUIRE(sao_ui_entity_shell_create(nullptr, &config, &shell) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_bring_online(shell) == SAO_STATUS_OK);
    auto tree = make_roots(6, true);
    REQUIRE(sao_ui_entity_shell_set_roots(shell, tree.roots.data(), tree.roots.size()) ==
            SAO_STATUS_OK);
    open_menu(shell);
    wheel(shell, -120);
    click(shell, root_point(shell, 1));
    REQUIRE(std::string(root_snapshot(shell).active_root_id_utf8) == "root-2");

    std::rotate(tree.specs.begin() + 2, tree.specs.begin() + 3, tree.specs.begin() + 5);
    const auto active = std::find_if(tree.specs.begin(), tree.specs.end(),
                                     [](const RootSpec& root) { return root.id == "root-2"; });
    REQUIRE(active != tree.specs.end());
    active->label = "Renamed root two";
    tree.rebuild();
    REQUIRE(sao_ui_entity_shell_set_roots(shell, tree.roots.data(), tree.roots.size()) ==
            SAO_STATUS_OK);
    auto snapshot = root_snapshot(shell);
    CHECK(std::string(snapshot.active_root_id_utf8) == "root-2");
    CHECK(snapshot.first_visible_root_index == 1U);

    tree.specs.erase(std::remove_if(tree.specs.begin(), tree.specs.end(),
                                    [](const RootSpec& root) { return root.id == "root-2"; }),
                     tree.specs.end());
    tree.rebuild();
    REQUIRE(sao_ui_entity_shell_set_roots(shell, tree.roots.data(), tree.roots.size()) ==
            SAO_STATUS_OK);
    snapshot = root_snapshot(shell);
    CHECK(snapshot.active_root_id_utf8[0] == '\0');
    destroy_shell(shell);
}

TEST_CASE("Entity root validation is fail closed and semantic no-op is stable",
          "[ui][entity_roots]") {
    const auto config = make_config();
    sao_ui_entity_shell_handle_t shell = nullptr;
    REQUIRE(sao_ui_entity_shell_create(nullptr, &config, &shell) == SAO_STATUS_OK);
    auto tree = make_roots(2);
    REQUIRE(sao_ui_entity_shell_set_roots(shell, tree.roots.data(), tree.roots.size()) ==
            SAO_STATUS_OK);
    const uint64_t revision = root_snapshot(shell).root_tree_revision;
    REQUIRE(sao_ui_entity_shell_set_roots(shell, tree.roots.data(), tree.roots.size()) ==
            SAO_STATUS_OK);
    CHECK(root_snapshot(shell).root_tree_revision == revision);

    auto duplicate = tree.roots;
    duplicate[1].root_id_utf8 = duplicate[0].root_id_utf8;
    CHECK(sao_ui_entity_shell_set_roots(shell, duplicate.data(), duplicate.size()) ==
          SAO_STATUS_ERR_ALREADY_EXISTS);

        auto duplicate_name = tree.roots;
        duplicate_name[1].name_utf8 = duplicate_name[0].name_utf8;
        CHECK(sao_ui_entity_shell_set_roots(shell, duplicate_name.data(), duplicate_name.size()) ==
                    SAO_STATUS_ERR_ALREADY_EXISTS);

    auto bad_size = tree.roots;
    bad_size[0].struct_size = sizeof(SaoUiEntityRootItem) - 1U;
    CHECK(sao_ui_entity_shell_set_roots(shell, bad_size.data(), bad_size.size()) ==
          SAO_STATUS_ERR_ABI_MISMATCH);

    auto bad_utf8 = tree.roots;
    const std::array<char, 2> invalid_utf8{static_cast<char>(0xC3), '\0'};
    bad_utf8[0].name_utf8 = invalid_utf8.data();
    CHECK(sao_ui_entity_shell_set_roots(shell, bad_utf8.data(), bad_utf8.size()) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);

    auto bad_children = tree.roots;
    bad_children[0].children = nullptr;
    bad_children[0].child_count = 1;
    CHECK(sao_ui_entity_shell_set_roots(shell, bad_children.data(), bad_children.size()) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);

    std::vector<SaoUiEntityRootItem> excessive(SAO_UI_ENTITY_ROOT_MAX_COUNT + 1U, tree.roots[0]);
    CHECK(sao_ui_entity_shell_set_roots(shell, excessive.data(), excessive.size()) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(root_snapshot(shell).root_tree_revision == revision);
    CHECK(root_snapshot(shell).root_count == 2U);
    sao_ui_entity_shell_destroy(shell);
}

TEST_CASE("Empty Entity root publication is a semantic tree", "[ui][entity_roots]") {
    const auto config = make_config();
    sao_ui_entity_shell_handle_t shell = nullptr;
    REQUIRE(sao_ui_entity_shell_create(nullptr, &config, &shell) == SAO_STATUS_OK);
    const uint64_t initial_revision = root_snapshot(shell).root_tree_revision;

    REQUIRE(sao_ui_entity_shell_set_roots(shell, nullptr, 0) == SAO_STATUS_OK);
    const auto empty = root_snapshot(shell);
    CHECK(empty.root_count == 0U);
    CHECK(empty.first_visible_root_index == 0U);
    CHECK(empty.visible_root_count == 0U);
    CHECK(empty.active_root_id_utf8[0] == '\0');
    CHECK(empty.root_tree_revision == initial_revision + 1U);

    REQUIRE(sao_ui_entity_shell_set_roots(shell, nullptr, 0) == SAO_STATUS_OK);
    CHECK(root_snapshot(shell).root_tree_revision == empty.root_tree_revision);
    sao_ui_entity_shell_destroy(shell);
}

TEST_CASE("Entity root publication enforces string and child budgets", "[ui][entity_roots]") {
    const auto config = make_config();
    sao_ui_entity_shell_handle_t shell = nullptr;
    REQUIRE(sao_ui_entity_shell_create(nullptr, &config, &shell) == SAO_STATUS_OK);
    auto tree = make_roots(1);
    REQUIRE(sao_ui_entity_shell_set_roots(shell, tree.roots.data(), tree.roots.size()) ==
            SAO_STATUS_OK);
    const uint64_t revision = root_snapshot(shell).root_tree_revision;

    auto oversized_label = tree.roots;
    const std::string long_label(4097U, 'L');
    oversized_label[0].name_utf8 = long_label.c_str();
    CHECK(sao_ui_entity_shell_set_roots(shell, oversized_label.data(), oversized_label.size()) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);

    const SaoUiMenuItem child{"Child", ">", 701, true, {false, false, false}};
    const std::vector<SaoUiMenuItem> too_many_per_root(257U, child);
    auto excessive_children = tree.roots;
    excessive_children[0].children = too_many_per_root.data();
    excessive_children[0].child_count = too_many_per_root.size();
    CHECK(sao_ui_entity_shell_set_roots(shell, excessive_children.data(),
                                        excessive_children.size()) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);

    const std::vector<SaoUiMenuItem> maximum_children(256U, child);
    std::array<std::string, 5> ids{"total-0", "total-1", "total-2", "total-3", "total-4"};
    std::array<SaoUiEntityRootItem, 5> excessive_total{};
    for (size_t index = 0; index < excessive_total.size(); ++index) {
        excessive_total[index].struct_size = sizeof(SaoUiEntityRootItem);
        excessive_total[index].root_id_utf8 = ids[index].c_str();
        excessive_total[index].name_utf8 = ids[index].c_str();
        excessive_total[index].icon_utf8 = "T";
        excessive_total[index].action_id = 800 + static_cast<int32_t>(index);
        excessive_total[index].can_activate = true;
        excessive_total[index].children = maximum_children.data();
        excessive_total[index].child_count = maximum_children.size();
    }
    CHECK(sao_ui_entity_shell_set_roots(shell, excessive_total.data(), excessive_total.size()) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
        CHECK(root_snapshot(shell).root_tree_revision == revision);
        CHECK(root_snapshot(shell).root_count == 1U);

    std::array<std::string, 5> legacy_ids{"legacy-0", "legacy-1", "legacy-2", "legacy-3",
                                          "legacy-4"};
    std::array<SaoUiEntityRootItem, 5> legacy_total{};
    for (size_t index = 0; index < legacy_total.size(); ++index) {
        legacy_total[index].struct_size = sizeof(SaoUiEntityRootItem);
        legacy_total[index].root_id_utf8 = legacy_ids[index].c_str();
        legacy_total[index].name_utf8 = legacy_ids[index].c_str();
        legacy_total[index].icon_utf8 = "L";
        legacy_total[index].action_id = 900 + static_cast<int32_t>(index);
        legacy_total[index].can_activate = true;
        if (index > 0U) {
            legacy_total[index].children = maximum_children.data();
            legacy_total[index].child_count = maximum_children.size();
        }
    }
    REQUIRE(sao_ui_entity_shell_set_roots(shell, legacy_total.data(), legacy_total.size()) ==
            SAO_STATUS_OK);
        const uint64_t legacy_revision = root_snapshot(shell).root_tree_revision;
    CHECK(sao_ui_entity_shell_set_children(shell, "legacy-0", &child, 1) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);

    const auto unchanged = root_snapshot(shell);
        CHECK(unchanged.root_count == 5U);
        CHECK(unchanged.root_tree_revision == legacy_revision);

    const std::string maximum_label(4096U, 'U');
    const SaoUiMenuItem maximum_label_child{maximum_label.c_str(), "", 990, true,
                                             {false, false, false}};
    const std::vector<SaoUiMenuItem> near_utf8_budget(15U, maximum_label_child);
    std::array<SaoUiEntityRootItem, 2> utf8_roots{};
    utf8_roots[0] = {sizeof(SaoUiEntityRootItem), "utf-0", "utf-0", "U", 990, true,
                     {false, false, false}, nullptr, 0};
    utf8_roots[1] = {sizeof(SaoUiEntityRootItem), "utf-1", "utf-1", "U", 991, true,
                     {false, false, false}, near_utf8_budget.data(), near_utf8_budget.size()};
    REQUIRE(sao_ui_entity_shell_set_roots(shell, utf8_roots.data(), utf8_roots.size()) ==
            SAO_STATUS_OK);
    const uint64_t utf8_revision = root_snapshot(shell).root_tree_revision;
    CHECK(sao_ui_entity_shell_set_children(shell, "utf-0", &maximum_label_child, 1) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(root_snapshot(shell).root_tree_revision == utf8_revision);
    sao_ui_entity_shell_destroy(shell);
}

TEST_CASE("Active Entity root survives viewport projection", "[ui][entity_roots]") {
    ActionLog actions{};
    const auto config = make_config(&actions);
    sao_ui_entity_shell_handle_t shell = nullptr;
    REQUIRE(sao_ui_entity_shell_create(nullptr, &config, &shell) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_bring_online(shell) == SAO_STATUS_OK);
    auto tree = make_roots(7);
    REQUIRE(sao_ui_entity_shell_set_roots(shell, tree.roots.data(), tree.roots.size()) ==
            SAO_STATUS_OK);
    open_menu(shell);
    click(shell, root_point(shell, 0));
    REQUIRE(std::string(root_snapshot(shell).active_root_id_utf8) == "root-0");

    wheel(shell, -240);
    auto snapshot = root_snapshot(shell);
    CHECK(snapshot.first_visible_root_index == 2U);
    CHECK(std::string(snapshot.active_root_id_utf8) == "root-0");
    wheel(shell, 240);
    snapshot = root_snapshot(shell);
    CHECK(snapshot.first_visible_root_index == 0U);
    CHECK(std::string(snapshot.active_root_id_utf8) == "root-0");
    destroy_shell(shell);
}

TEST_CASE("Hiding the Entity menu clears the active root", "[ui][entity_roots]") {
    ActionLog actions{};
    const auto config = make_config(&actions);
    sao_ui_entity_shell_handle_t shell = nullptr;
    REQUIRE(sao_ui_entity_shell_create(nullptr, &config, &shell) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_bring_online(shell) == SAO_STATUS_OK);
    open_menu(shell);
    click(shell, root_point(shell, 4));
    CHECK_FALSE(shell_snapshot(shell).menu_visible);
    CHECK(root_snapshot(shell).active_root_id_utf8[0] == '\0');
    destroy_shell(shell);
}

TEST_CASE("Legacy Entity children survive root projection", "[ui][entity_roots]") {
    ActionLog actions{};
    const auto config = make_config(&actions);
    sao_ui_entity_shell_handle_t shell = nullptr;
    REQUIRE(sao_ui_entity_shell_create(nullptr, &config, &shell) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_bring_online(shell) == SAO_STATUS_OK);

    auto tree = make_roots(6);
    tree.specs[2].id = "plugins-id";
    tree.specs[2].label = "Plugins";
    tree.rebuild();
    REQUIRE(sao_ui_entity_shell_set_roots(shell, tree.roots.data(), tree.roots.size()) ==
            SAO_STATUS_OK);
    const SaoUiMenuItem plugin_row{"A2 Plugins", "P", 9123, true, {false, false, false}};
    REQUIRE(sao_ui_entity_shell_set_children(shell, "Plugins", &plugin_row, 1) == SAO_STATUS_OK);
    open_menu(shell);
    wheel(shell, -120);
    wheel(shell, 120);
    click(shell, root_point(shell, 2));
    REQUIRE(sao_ui_entity_shell_tick(shell, 200) == SAO_STATUS_OK);
    click(shell, child_point(shell));
    CHECK(actions.calls == 1);
    CHECK(actions.last_action == 9123);
    destroy_shell(shell);
}

TEST_CASE("Entity set_roots is owner-thread only", "[ui][entity_roots]") {
    const auto config = make_config();
    sao_ui_entity_shell_handle_t shell = nullptr;
    REQUIRE(sao_ui_entity_shell_create(nullptr, &config, &shell) == SAO_STATUS_OK);
    auto tree = make_roots(1);
    sao_status_t status = SAO_STATUS_OK;
    std::thread non_owner([&] {
        status = sao_ui_entity_shell_set_roots(shell, tree.roots.data(), tree.roots.size());
    });
    non_owner.join();
    CHECK(status == SAO_STATUS_ERR_ACCESS_DENIED);
    CHECK(root_snapshot(shell).root_count == 5U);
    sao_ui_entity_shell_destroy(shell);
}

TEST_CASE("Dynamic Unicode Entity roots rasterize", "[ui][entity_roots]") {
    const auto config = make_config();
    sao_ui_entity_shell_handle_t shell = nullptr;
    REQUIRE(sao_ui_entity_shell_create(nullptr, &config, &shell) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_bring_online(shell) == SAO_STATUS_OK);
    auto tree = make_roots(1);
    tree.specs[0].label = "动态根节点";
    tree.specs[0].icon = "界";
    tree.rebuild();
    REQUIRE(sao_ui_entity_shell_set_roots(shell, tree.roots.data(), tree.roots.size()) ==
            SAO_STATUS_OK);
    open_menu(shell);
    const auto raster = pixels(shell);
    CHECK(std::any_of(raster.begin() + 3, raster.end(), [](uint8_t value) { return value != 0; }));
    destroy_shell(shell);
}
