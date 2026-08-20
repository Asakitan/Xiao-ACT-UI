#include <catch2/catch_test_macros.hpp>

#include "sao/sdk/sao_sdk_platform_internal.h"
#include "sao/ui/compositor.h"
#include "tool_launch_internal.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifndef SAO_TOOL_LAUNCH_CHILD_FIXTURE
#define SAO_TOOL_LAUNCH_CHILD_FIXTURE ""
#endif

namespace {

struct TempTree {
    std::filesystem::path root;

    TempTree() {
        root = std::filesystem::temp_directory_path() /
               (L"sao_tool_launch_工具_" + std::to_wstring(GetCurrentProcessId()));
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(root / L"runtime");
    }

    ~TempTree() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    void install_fixture(bool runtime = true) const {
        std::filesystem::copy_file(
            std::filesystem::path(SAO_TOOL_LAUNCH_CHILD_FIXTURE),
                runtime ? root / L"runtime" / L"SaoAiEditor.exe"
                    : root / L"SaoAiEditor.exe",
            std::filesystem::copy_options::overwrite_existing);
    }
};

bool wait_for_marker(const std::filesystem::path& marker) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (std::filesystem::is_regular_file(marker)) {
            return true;
        }
        Sleep(10);
    }
    return false;
}

bool wait_for_phase(
    const sao::launcher::tool_launch::AiEditorProcessOwner& owner,
    sao::launcher::tool_launch::AiEditorLaunchPhase expected,
    sao::launcher::tool_launch::AiEditorLaunchSnapshot& snapshot,
    int attempts = 200) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (owner.snapshot(snapshot) == SAO_STATUS_OK &&
            snapshot.phase == expected) {
            return true;
        }
        Sleep(10);
    }
    return false;
}

class ScopedEnvironment final {
public:
    ScopedEnvironment(const wchar_t* name, const wchar_t* value) : name_(name) {
        const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
        if (required > 0) {
            old_value_.resize(required);
            const DWORD copied = GetEnvironmentVariableW(
                name, old_value_.data(), required);
            if (copied > 0 && copied < required) {
                old_value_.resize(copied);
                existed_ = true;
            }
        }
        REQUIRE(SetEnvironmentVariableW(name, value));
    }

    ~ScopedEnvironment() {
        (void)SetEnvironmentVariableW(
            name_.c_str(), existed_ ? old_value_.c_str() : nullptr);
    }

    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

private:
    std::wstring name_;
    std::wstring old_value_;
    bool existed_{};
};

std::size_t marker_line_count(const std::filesystem::path& marker) {
    std::ifstream input(marker);
    std::size_t count = 0;
    std::string line;
    while (std::getline(input, line)) {
        ++count;
    }
    return count;
}

bool wait_for_marker_line_count(const std::filesystem::path& marker,
                                std::size_t expected,
                                int attempts = 200) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (std::filesystem::is_regular_file(marker) &&
            marker_line_count(marker) == expected) {
            return true;
        }
        Sleep(10);
    }
    return false;
}

size_t compositor_layer_count(sao_ui_compositor_handle_t compositor) {
    size_t count = 0;
    REQUIRE(sao_ui_compositor_list_layers(compositor, nullptr, 0, &count) == SAO_STATUS_OK);
    return count;
}

class BoundCompositor final {
  public:
    BoundCompositor() {
        REQUIRE(sao_ui_compositor_create(nullptr, nullptr, &handle_) == SAO_STATUS_OK);
        REQUIRE(sao_sdk_platform_bind_ui_compositor(handle_) == SAO_SDK_OK);
        bound_ = true;
    }

    ~BoundCompositor() {
        if (bound_)
            (void)sao_sdk_platform_unbind_ui_compositor();
        if (handle_ != nullptr)
            (void)sao_ui_compositor_try_destroy(handle_);
    }

    BoundCompositor(const BoundCompositor&) = delete;
    BoundCompositor& operator=(const BoundCompositor&) = delete;

    sao_ui_compositor_handle_t get() const noexcept {
        return handle_;
    }

private:
    sao_ui_compositor_handle_t handle_{};
    bool bound_{};
};

BOOL CALLBACK collect_current_process_windows(HWND window, LPARAM parameter) {
    auto& windows = *reinterpret_cast<std::vector<std::uintptr_t>*>(parameter);
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id == GetCurrentProcessId())
        windows.push_back(reinterpret_cast<std::uintptr_t>(window));
    return TRUE;
}

std::vector<std::uintptr_t> current_process_top_level_windows() {
    std::vector<std::uintptr_t> windows;
    REQUIRE(EnumWindows(&collect_current_process_windows, reinterpret_cast<LPARAM>(&windows)));
    std::ranges::sort(windows);
    return windows;
}

bool wait_for_layer_count(sao::launcher::tool_launch::AiEditorProcessOwner& owner,
                          sao_ui_compositor_handle_t compositor, size_t expected,
                          int attempts = 200) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        const sao_status_t status = owner.service_ui();
        if (status == SAO_STATUS_OK && compositor_layer_count(compositor) == expected) {
            return true;
        }
        Sleep(10);
    }
    return false;
}

} // namespace

TEST_CASE("AI Editor tool action preserves API input validation",
        "[launcher][tool_launch][focused]") {
    CHECK(sao::launcher::tool_launch::open_ai_editor(nullptr) ==
          SAO_STATUS_ERR_NOT_INITIALIZED);

    sao::launcher::tool_launch::AiEditorProcessOwner empty(L"");
    CHECK(empty.open() == SAO_STATUS_ERR_INVALID_ARGUMENT);

    sao::launcher::tool_launch::AiEditorProcessOwner relative(
        L"relative-ai-editor-root");
    CHECK(relative.open() == SAO_STATUS_ERR_INVALID_ARGUMENT);
}

#if !defined(SAO_LAUNCHER_HAS_AI_EDITOR_ABI)
TEST_CASE("AI Editor tool action fails closed without the dedicated ABI",
          "[launcher][tool_launch][focused][fail_closed]") {
    TempTree tree;
    sao::launcher::tool_launch::AiEditorProcessOwner owner(tree.root.wstring());
    REQUIRE(owner.open() == SAO_STATUS_ERR_CAPABILITY_MISSING);
    sao::launcher::tool_launch::AiEditorLaunchSnapshot snapshot;
    REQUIRE(owner.snapshot(snapshot) == SAO_STATUS_OK);
    CHECK(snapshot.phase ==
          sao::launcher::tool_launch::AiEditorLaunchPhase::failed);
    CHECK(snapshot.last_status == SAO_STATUS_ERR_CAPABILITY_MISSING);
}
#else

TEST_CASE("AI Editor tool action publishes a missing native child",
          "[launcher][tool_launch][focused][native]") {
    TempTree tree;
    sao::launcher::tool_launch::AiEditorProcessOwner owner(tree.root.wstring());
    REQUIRE(owner.open() == SAO_STATUS_OK);
    sao::launcher::tool_launch::AiEditorLaunchSnapshot snapshot;
    REQUIRE(wait_for_phase(
        owner, sao::launcher::tool_launch::AiEditorLaunchPhase::failed,
        snapshot));
    CHECK(snapshot.last_status == SAO_STATUS_ERR_NOT_FOUND);
}

TEST_CASE("AI Editor tool action uses native handshake without Python mode",
          "[launcher][tool_launch][focused][native]") {
    TempTree tree;
    const auto executable = tree.root / L"runtime" / L"SaoAiEditor.exe";
    std::filesystem::copy_file(
        std::filesystem::path(SAO_TOOL_LAUNCH_CHILD_FIXTURE), executable,
        std::filesystem::copy_options::overwrite_existing);
    ScopedEnvironment python_path(L"PYTHONPATH", nullptr);

    auto owner = std::make_unique<
        sao::launcher::tool_launch::AiEditorProcessOwner>(tree.root.wstring());
    REQUIRE(owner->open() == SAO_STATUS_OK);
    sao::launcher::tool_launch::AiEditorLaunchSnapshot started;
    REQUIRE(wait_for_phase(
        *owner, sao::launcher::tool_launch::AiEditorLaunchPhase::started,
        started));
    REQUIRE(started.process_id != 0);
    CHECK(started.last_status == SAO_STATUS_OK);
    REQUIRE(wait_for_marker(tree.root / L"ai_editor_fixture_marker.txt"));
    CHECK(marker_line_count(tree.root / L"ai_editor_fixture_marker.txt") == 1);

    REQUIRE(owner->open() == SAO_STATUS_OK);
    sao::launcher::tool_launch::AiEditorLaunchSnapshot reused;
    REQUIRE(
        wait_for_phase(*owner, sao::launcher::tool_launch::AiEditorLaunchPhase::started, reused));
    CHECK(reused.process_id == started.process_id);
    CHECK(marker_line_count(tree.root / L"ai_editor_fixture_marker.txt") == 1);

    HANDLE child = OpenProcess(SYNCHRONIZE, FALSE, started.process_id);
    REQUIRE(child != nullptr);
    owner.reset();
    CHECK(WaitForSingleObject(child, 5000) == WAIT_OBJECT_0);
    CloseHandle(child);
}

TEST_CASE("AI Editor tool action resolves native release layouts",
          "[launcher][tool_launch][focused][native]") {
    SECTION("runtime directory wins") {
        TempTree tree;
        std::filesystem::copy_file(
            std::filesystem::path(SAO_TOOL_LAUNCH_CHILD_FIXTURE),
            tree.root / L"runtime" / L"SaoAiEditor.exe",
            std::filesystem::copy_options::overwrite_existing);
        std::filesystem::copy_file(
            std::filesystem::path(SAO_TOOL_LAUNCH_CHILD_FIXTURE),
            tree.root / L"SaoAiEditor.exe",
            std::filesystem::copy_options::overwrite_existing);
        ScopedEnvironment python_path(L"PYTHONPATH", nullptr);
        sao::launcher::tool_launch::AiEditorProcessOwner owner(
            tree.root.wstring());
        REQUIRE(owner.open() == SAO_STATUS_OK);
        sao::launcher::tool_launch::AiEditorLaunchSnapshot snapshot;
        REQUIRE(wait_for_phase(
            owner, sao::launcher::tool_launch::AiEditorLaunchPhase::started,
            snapshot));
        CHECK(snapshot.process_id != 0);
    }

    SECTION("same directory debug layout") {
        TempTree tree;
        std::filesystem::copy_file(
            std::filesystem::path(SAO_TOOL_LAUNCH_CHILD_FIXTURE),
            tree.root / L"SaoAiEditor.exe",
            std::filesystem::copy_options::overwrite_existing);
        ScopedEnvironment python_path(L"PYTHONPATH", nullptr);
        sao::launcher::tool_launch::AiEditorProcessOwner owner(
            tree.root.wstring());
        REQUIRE(owner.open() == SAO_STATUS_OK);
        sao::launcher::tool_launch::AiEditorLaunchSnapshot snapshot;
        REQUIRE(wait_for_phase(
            owner, sao::launcher::tool_launch::AiEditorLaunchPhase::started,
            snapshot));
        CHECK(snapshot.process_id != 0);
    }
}

TEST_CASE("AI Editor native handshake failure is published asynchronously",
          "[launcher][tool_launch][focused][native]") {
    TempTree tree;
    std::filesystem::copy_file(
        std::filesystem::path(SAO_TOOL_LAUNCH_CHILD_FIXTURE),
        tree.root / L"runtime" / L"SaoAiEditor.exe",
        std::filesystem::copy_options::overwrite_existing);
    ScopedEnvironment python_path(L"PYTHONPATH", nullptr);
    ScopedEnvironment early_exit(
        L"SAO_TOOL_LAUNCH_FIXTURE_EXIT_BEFORE_CONNECT", L"1");

    sao::launcher::tool_launch::AiEditorProcessOwner owner(tree.root.wstring());
    REQUIRE(owner.open() == SAO_STATUS_OK);
    sao::launcher::tool_launch::AiEditorLaunchSnapshot snapshot;
    REQUIRE(wait_for_phase(
        owner, sao::launcher::tool_launch::AiEditorLaunchPhase::failed,
        snapshot, 1000));
    CHECK(snapshot.last_status == SAO_STATUS_ERR_PROCESS_GONE);
    CHECK(snapshot.has_exit_code);
    CHECK(snapshot.exit_code == 14);
}

TEST_CASE("production OPEN_AI_EDITOR child completes native handshake",
          "[launcher][tool_launch][integration][production_child]") {
    TempTree tree;
    std::filesystem::copy_file(
        std::filesystem::path(SAO_TOOL_LAUNCH_CHILD_FIXTURE),
        tree.root / L"runtime" / L"SaoAiEditor.exe",
        std::filesystem::copy_options::overwrite_existing);
    ScopedEnvironment python_path(L"PYTHONPATH", nullptr);

    auto owner = std::make_unique<
        sao::launcher::tool_launch::AiEditorProcessOwner>(tree.root.wstring());
    REQUIRE(owner->open() == SAO_STATUS_OK);
    sao::launcher::tool_launch::AiEditorLaunchSnapshot started;
    REQUIRE(wait_for_phase(
        *owner, sao::launcher::tool_launch::AiEditorLaunchPhase::started,
        started, 1000));
    REQUIRE(started.process_id != 0);
    HANDLE child = OpenProcess(SYNCHRONIZE, FALSE, started.process_id);
    REQUIRE(child != nullptr);
    owner.reset();
    CHECK(WaitForSingleObject(child, 5000) == WAIT_OBJECT_0);
    CloseHandle(child);
}

TEST_CASE("AI Editor owner open retires a snapshotted stale panel before relaunch",
          "[launcher][tool_launch][ui_service][child_exit][snapshot][reopen][focused]") {
    TempTree tree;
    tree.install_fixture();
    ScopedEnvironment python_path(L"PYTHONPATH", nullptr);
    BoundCompositor compositor;
    const auto marker = tree.root / L"ai_editor_fixture_marker.txt";

    sao::launcher::tool_launch::AiEditorProcessOwner owner(tree.root.wstring());
    REQUIRE(owner.open() == SAO_STATUS_OK);
    sao::launcher::tool_launch::AiEditorLaunchSnapshot first;
    REQUIRE(wait_for_phase(
        owner, sao::launcher::tool_launch::AiEditorLaunchPhase::started, first));
    REQUIRE(wait_for_layer_count(owner, compositor.get(), 1));
    REQUIRE(wait_for_marker_line_count(marker, 1));

    HANDLE child = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE,
                               first.process_id);
    REQUIRE(child != nullptr);
    REQUIRE(TerminateProcess(child, 27));
    REQUIRE(WaitForSingleObject(child, 5000) == WAIT_OBJECT_0);

    sao::launcher::tool_launch::AiEditorLaunchSnapshot stopped;
    REQUIRE(wait_for_phase(
        owner, sao::launcher::tool_launch::AiEditorLaunchPhase::failed,
        stopped));
    CHECK(stopped.has_exit_code);
    CHECK(stopped.exit_code == 27);
    CHECK(compositor_layer_count(compositor.get()) == 1);

    std::atomic<sao_status_t> foreign_status{SAO_STATUS_OK};
    std::thread foreign([&] { foreign_status.store(owner.open()); });
    foreign.join();
    CHECK(foreign_status.load() == SAO_STATUS_ERR_ACCESS_DENIED);
    CHECK(compositor_layer_count(compositor.get()) == 1);
    CHECK(marker_line_count(marker) == 1);

    REQUIRE(owner.open() == SAO_STATUS_OK);
    sao::launcher::tool_launch::AiEditorLaunchSnapshot second;
    REQUIRE(wait_for_phase(
        owner, sao::launcher::tool_launch::AiEditorLaunchPhase::started,
        second));
    REQUIRE(second.process_id != 0);
    CHECK(second.process_id != first.process_id);
    CloseHandle(child);
    REQUIRE(wait_for_marker_line_count(marker, 2));
    REQUIRE(wait_for_layer_count(owner, compositor.get(), 1));
    CHECK(compositor_layer_count(compositor.get()) == 1);

    REQUIRE(owner.take_offline() == SAO_STATUS_OK);
    CHECK(compositor_layer_count(compositor.get()) == 0);
}

TEST_CASE("AI Editor worker cancels when owner is destroyed",
          "[launcher][tool_launch][focused]") {
    TempTree tree;
    tree.install_fixture();
    HANDLE gate = CreateMutexW(nullptr, TRUE,
                               L"Local\\SAO.Auto.AiEditor.Launch.v1");
    REQUIRE(gate != nullptr);
    {
        sao::launcher::tool_launch::AiEditorProcessOwner owner(
            tree.root.wstring());
        REQUIRE(owner.open() == SAO_STATUS_OK);
        Sleep(100);
    }
    ReleaseMutex(gate);
    CloseHandle(gate);
    Sleep(150);
    CHECK_FALSE(std::filesystem::exists(
        tree.root / L"ai_editor_fixture_marker.txt"));
}
#endif
