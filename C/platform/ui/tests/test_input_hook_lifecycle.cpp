#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "sao/ui/input.h"
#include "input_win32_api.h"

namespace {

constexpr std::uintptr_t kMouseHook = 0x101;
constexpr std::uintptr_t kKeyboardHook = 0x202;
constexpr LRESULT kNextHookResult = 73;

struct FakeWin32State {
    std::mutex mutex;
    bool fail_keyboard_install{false};
    std::unordered_map<std::uintptr_t, int> unhook_failures;
    std::vector<int> install_attempts;
    std::vector<std::uintptr_t> unhook_attempts;
};

FakeWin32State* g_fake = nullptr;

HHOOK WINAPI fake_set_windows_hook_ex_w(int hook_id, HOOKPROC, HINSTANCE, DWORD) {
    {
        std::lock_guard<std::mutex> lock(g_fake->mutex);
        g_fake->install_attempts.push_back(hook_id);
    }
    if (hook_id == WH_MOUSE_LL) {
        return reinterpret_cast<HHOOK>(kMouseHook);
    }
    if (hook_id == WH_KEYBOARD_LL && !g_fake->fail_keyboard_install) {
        return reinterpret_cast<HHOOK>(kKeyboardHook);
    }
    return nullptr;
}

BOOL WINAPI fake_unhook_windows_hook_ex(HHOOK hook) {
    const auto value = reinterpret_cast<std::uintptr_t>(hook);
    std::lock_guard<std::mutex> lock(g_fake->mutex);
    g_fake->unhook_attempts.push_back(value);
    auto found = g_fake->unhook_failures.find(value);
    if (found != g_fake->unhook_failures.end() && found->second > 0) {
        --found->second;
        return FALSE;
    }
    return TRUE;
}

LRESULT WINAPI fake_call_next_hook_ex(HHOOK, int, WPARAM, LPARAM) {
    return kNextHookResult;
}

SHORT WINAPI fake_get_async_key_state(int) {
    return 0;
}

DWORD WINAPI fake_get_current_thread_id() {
    return 42;
}

const sao::ui::input_detail::Win32Api kFakeApi{
    &fake_set_windows_hook_ex_w,
    &fake_unhook_windows_hook_ex,
    &fake_call_next_hook_ex,
    &fake_get_async_key_state,
    &fake_get_current_thread_id,
};

class Fixture {
public:
    Fixture() {
        g_fake = &state;
        sao::ui::input_detail::set_win32_api_for_testing(&kFakeApi);
        REQUIRE(sao_ui_input_router_create(
                    reinterpret_cast<sao_ui_overlay_host_handle_t>(1), &router) ==
                SAO_STATUS_OK);
    }

    ~Fixture() {
        if (router != nullptr) {
            sao_ui_input_router_destroy(router);
        }
        sao::ui::input_detail::reset_win32_api_for_testing();
        g_fake = nullptr;
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    FakeWin32State state;
    sao_ui_input_router_handle_t router{nullptr};
};

class LegacyInputGateOff {
public:
    LegacyInputGateOff() {
        const char* current = std::getenv("SAO_UI_LEGACY_TK_INPUT");
        if (current != nullptr) {
            had_previous_ = true;
            previous_ = current;
        }
        (void)_putenv_s("SAO_UI_LEGACY_TK_INPUT", "");
    }

    ~LegacyInputGateOff() {
        (void)_putenv_s("SAO_UI_LEGACY_TK_INPUT",
                        had_previous_ ? previous_.c_str() : "");
    }

private:
    bool had_previous_{};
    std::string previous_;
};

std::size_t attempt_count(const FakeWin32State& state, std::uintptr_t hook) {
    return static_cast<std::size_t>(
        std::count(state.unhook_attempts.begin(), state.unhook_attempts.end(), hook));
}

struct CallbackDestroyState {
    sao_ui_input_router_handle_t* router{nullptr};
    int hotkey_calls{0};
    int keyboard_calls{0};
};

void SAO_UI_CALL destroy_router_from_hotkey(uint32_t, void* user_data) {
    auto* state = static_cast<CallbackDestroyState*>(user_data);
    ++state->hotkey_calls;
    sao_ui_input_router_destroy(*state->router);
    *state->router = nullptr;
}

bool SAO_UI_CALL count_keyboard_callback(uint32_t, uint64_t, uint64_t,
                                         void* user_data) {
    ++static_cast<CallbackDestroyState*>(user_data)->keyboard_calls;
    return false;
}

struct BlockingCallbackState {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered{false};
    bool released{false};
};

bool SAO_UI_CALL blocking_mouse_callback(uint32_t, uint64_t, uint64_t,
                                         void* user_data) {
    auto* state = static_cast<BlockingCallbackState*>(user_data);
    std::unique_lock<std::mutex> lock(state->mutex);
    state->entered = true;
    state->condition.notify_all();
    state->condition.wait(lock, [state] { return state->released; });
    return false;
}

}  // namespace

extern "C" void* SAO_UI_CALL sao_ui_overlay_host_hwnd(sao_ui_overlay_host_handle_t) {
    return reinterpret_cast<void*>(1);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_overlay_host_set_input_region(
    sao_ui_overlay_host_handle_t, const SaoOverlayHostInputRect*, size_t) {
    return SAO_STATUS_OK;
}

TEST_CASE("legacy Tk input compatibility shims default off",
        "[ui][input_hook][legacy][tk_mirror][gate]") {
    LegacyInputGateOff gate;
    Fixture fixture;
    const SaoOverlayHostInputRect rect{0, 0, 8, 8};

    CHECK(sao_ui_input_router_set_regions(fixture.router, &rect, 1) ==
        SAO_STATUS_ERR_NOT_IMPLEMENTED);
    CHECK(sao_ui_input_router_rebuild_region(fixture.router) ==
        SAO_STATUS_ERR_NOT_IMPLEMENTED);
    CHECK(sao_ui_input_router_shield_arm_once(
          fixture.router, reinterpret_cast<void*>(2)) ==
        SAO_STATUS_ERR_NOT_IMPLEMENTED);
    CHECK_FALSE(sao_ui_input_router_shield_armed(
      fixture.router, reinterpret_cast<void*>(2)));
    CHECK(sao_ui_input_router_set_layer_cursor(fixture.router, "legacy", 0) ==
        SAO_STATUS_ERR_NOT_IMPLEMENTED);
}

TEST_CASE("input hook rollback retains a mouse handle when rollback fails",
          "[ui][input_hook][lifecycle]") {
    Fixture fixture;
    fixture.state.fail_keyboard_install = true;
    fixture.state.unhook_failures[kMouseHook] = 1;

    REQUIRE(sao_ui_input_router_install_ll_hooks(fixture.router) ==
            SAO_STATUS_ERR_OS_CALL_FAILED);
    REQUIRE(sao::ui::input_detail::mouse_hook_for_testing(fixture.router) == kMouseHook);
    REQUIRE(sao::ui::input_detail::keyboard_hook_for_testing(fixture.router) == 0);
    REQUIRE_FALSE(sao::ui::input_detail::active_router_present_for_testing());

    REQUIRE(sao_ui_input_router_uninstall_ll_hooks(fixture.router) == SAO_STATUS_OK);
    REQUIRE(sao::ui::input_detail::mouse_hook_for_testing(fixture.router) == 0);
    REQUIRE(attempt_count(fixture.state, kMouseHook) == 2);
}

TEST_CASE("input hook uninstall attempts both hooks and retries mouse failure",
          "[ui][input_hook][lifecycle]") {
    Fixture fixture;
    REQUIRE(sao_ui_input_router_install_ll_hooks(fixture.router) == SAO_STATUS_OK);
    fixture.state.unhook_failures[kMouseHook] = 1;

    REQUIRE(sao_ui_input_router_uninstall_ll_hooks(fixture.router) ==
            SAO_STATUS_ERR_OS_CALL_FAILED);
    REQUIRE(attempt_count(fixture.state, kMouseHook) == 1);
    REQUIRE(attempt_count(fixture.state, kKeyboardHook) == 1);
    REQUIRE(sao::ui::input_detail::mouse_hook_for_testing(fixture.router) == kMouseHook);
    REQUIRE(sao::ui::input_detail::keyboard_hook_for_testing(fixture.router) == 0);
    REQUIRE_FALSE(sao::ui::input_detail::active_router_present_for_testing());

    REQUIRE(sao_ui_input_router_uninstall_ll_hooks(fixture.router) == SAO_STATUS_OK);
    REQUIRE(attempt_count(fixture.state, kMouseHook) == 2);
    REQUIRE(attempt_count(fixture.state, kKeyboardHook) == 1);
}

TEST_CASE("input hook uninstall clears active state and retries keyboard failure",
          "[ui][input_hook][lifecycle]") {
    Fixture fixture;
    REQUIRE(sao_ui_input_router_install_ll_hooks(fixture.router) == SAO_STATUS_OK);
    fixture.state.unhook_failures[kKeyboardHook] = 1;

    REQUIRE(sao_ui_input_router_uninstall_ll_hooks(fixture.router) ==
            SAO_STATUS_ERR_OS_CALL_FAILED);
    REQUIRE(attempt_count(fixture.state, kMouseHook) == 1);
    REQUIRE(attempt_count(fixture.state, kKeyboardHook) == 1);
    REQUIRE(sao::ui::input_detail::mouse_hook_for_testing(fixture.router) == 0);
    REQUIRE(sao::ui::input_detail::keyboard_hook_for_testing(fixture.router) == kKeyboardHook);
    REQUIRE_FALSE(sao::ui::input_detail::active_router_present_for_testing());

    REQUIRE(sao_ui_input_router_uninstall_ll_hooks(fixture.router) == SAO_STATUS_OK);
    REQUIRE(attempt_count(fixture.state, kKeyboardHook) == 2);
}

TEST_CASE("input router destroy retires failed hooks without retaining the router",
          "[ui][input_hook][lifecycle]") {
    Fixture fixture;
    REQUIRE(sao_ui_input_router_install_ll_hooks(fixture.router) == SAO_STATUS_OK);
    fixture.state.unhook_failures[kMouseHook] = 2;

    sao_ui_input_router_destroy(fixture.router);
    fixture.router = nullptr;

    REQUIRE_FALSE(sao::ui::input_detail::active_router_present_for_testing());
    REQUIRE(attempt_count(fixture.state, kMouseHook) == 1);
    REQUIRE(attempt_count(fixture.state, kKeyboardHook) == 1);
    REQUIRE_FALSE(sao::ui::input_detail::retry_retired_hooks_for_testing());
    REQUIRE(sao::ui::input_detail::retry_retired_hooks_for_testing());
    REQUIRE(attempt_count(fixture.state, kMouseHook) == 3);
    REQUIRE(attempt_count(fixture.state, kKeyboardHook) == 1);
}

TEST_CASE("hotkey callback can destroy its router without a stale callback",
          "[ui][input_hook][lifecycle]") {
    Fixture fixture;
    CallbackDestroyState state{&fixture.router};
    REQUIRE(sao_ui_input_router_register_global_hotkey(
                fixture.router, 1, 'A', 0, &destroy_router_from_hotkey, &state) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_set_ll_hook_callbacks(
                fixture.router, nullptr, &count_keyboard_callback, &state) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_install_ll_hooks(fixture.router) == SAO_STATUS_OK);

    KBDLLHOOKSTRUCT event{};
    event.vkCode = 'A';
    REQUIRE(sao::ui::input_detail::invoke_keyboard_proc_for_testing(
                HC_ACTION, WM_KEYDOWN, reinterpret_cast<LPARAM>(&event)) ==
            kNextHookResult);
    REQUIRE(fixture.router == nullptr);
    REQUIRE(state.hotkey_calls == 1);
    REQUIRE(state.keyboard_calls == 0);
    REQUIRE_FALSE(sao::ui::input_detail::active_router_present_for_testing());
}

TEST_CASE("input hook uninstall waits for an in-flight callback",
          "[ui][input_hook][lifecycle]") {
    using namespace std::chrono_literals;

    Fixture fixture;
    BlockingCallbackState state;
    REQUIRE(sao_ui_input_router_set_ll_hook_callbacks(
                fixture.router, &blocking_mouse_callback, nullptr, &state) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_input_router_install_ll_hooks(fixture.router) == SAO_STATUS_OK);

    auto callback = std::async(std::launch::async, [] {
        return sao::ui::input_detail::invoke_mouse_proc_for_testing(
            HC_ACTION, WM_MOUSEMOVE, 0);
    });
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        state.condition.wait(lock, [&state] { return state.entered; });
    }

    auto uninstall = std::async(std::launch::async, [&fixture] {
        return sao_ui_input_router_uninstall_ll_hooks(fixture.router);
    });
    REQUIRE(uninstall.wait_for(20ms) == std::future_status::timeout);

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.released = true;
        state.condition.notify_all();
    }
    REQUIRE(callback.get() == kNextHookResult);
    REQUIRE(uninstall.get() == SAO_STATUS_OK);
}

    TEST_CASE("failed explicit uninstall quarantines hooks before another install",
          "[ui][input_hook][lifecycle]") {
        Fixture fixture;
        REQUIRE(sao_ui_input_router_install_ll_hooks(fixture.router) == SAO_STATUS_OK);
        fixture.state.unhook_failures[kMouseHook] = 2;

        REQUIRE(sao_ui_input_router_uninstall_ll_hooks(fixture.router) ==
            SAO_STATUS_ERR_OS_CALL_FAILED);
        const auto install_attempts = fixture.state.install_attempts.size();

        sao_ui_input_router_handle_t second = nullptr;
        REQUIRE(sao_ui_input_router_create(
            reinterpret_cast<sao_ui_overlay_host_handle_t>(1), &second) ==
            SAO_STATUS_OK);
        REQUIRE(sao_ui_input_router_install_ll_hooks(second) ==
            SAO_STATUS_ERR_OS_CALL_FAILED);
        REQUIRE(fixture.state.install_attempts.size() == install_attempts);

        REQUIRE(sao_ui_input_router_uninstall_ll_hooks(fixture.router) == SAO_STATUS_OK);
        REQUIRE(sao_ui_input_router_install_ll_hooks(second) == SAO_STATUS_OK);
        sao_ui_input_router_destroy(second);
    }
