// SAO Auto - platform/ui NOT_IMPLEMENTED classification support.
//
// Isolated STATIC library used ONLY by test_ui_gap.cpp.  Never
// linked into the production sao::ui shared library.  Its purpose is:
//
//   * host the byte-identical classification matrix (Class A capability
//     gate / Class B implementable / Class C legacy) for the 43+ NOT_
//     IMPLEMENTED occurrences that the v3.22 static scan reported in
//     PLAN.md §1.5;
//   * expose small, hermetic mock helpers that make each class assertion
//     compile even on non-Windows hosts where the real capability is
//     absent;
//   * keep every real capability call behind a strict opt-in switch so
//     the ui gap test binary is fully hermetic (no real driver, no real
//     WGC session, no real DXGI Desktop Duplication against the live
//     desktop, no real SendInput, no real ShellNotifyIcon).
//
// The top-level CMake guard forbids iteration-named STATIC implementation
// targets, so this archive uses the `_support_lib` suffix instead — like
// sao_sao_driver_loader_support_lib / sao_sao_user_evasion_support_lib.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "sao/core/status.h"

namespace sao::ui::gap {

// ---------------------------------------------------------------------
// Classification taxonomy - the three stable gap buckets.
// ---------------------------------------------------------------------

enum class GapClass : uint32_t {
    // OS/hardware capability absent.  Return should be
    // SAO_STATUS_ERR_CAPABILITY_MISSING with an explanatory comment.
    CapabilityGate = 1,
    // Implementable against a real API on this build host.  Return
    // should be SAO_STATUS_OK for the covered path.  These sites were
    // covered by production implementations; the matrix records that they
    // are no longer NOT_IMPLEMENTED in the production branch.
    Implementable = 2,
    // Legacy skeleton that intentionally stays NOT_IMPLEMENTED because
    // the public API contract documents that gate (WGL make_current,
    // dcomp GL interop, etc.).  Return stays NOT_IMPLEMENTED.
    LegacySkeleton = 3,
};

// One row of the classification matrix - a single NOT_IMPLEMENTED site.
struct GapRow {
    std::string_view cpp_file;       // "platform/ui/src/<file>.cpp"
    uint32_t         line_number;    // approximate current source-site line
    std::string_view api_symbol;     // the extern "C" entry point
    GapClass         gap_class = GapClass::CapabilityGate;
    std::string_view rationale;      // short human-readable rationale
    std::string_view capability;     // e.g. "user32.dll SendInput",
                                     // "dcomp.dll", "SAPI 5", etc.
};

// The full byte-identical classification matrix.  Rows are added in the
// order they were discovered by the static scan and remain
// stable so the test-side count assertions can be pinned.
std::vector<GapRow> classification_matrix();

// Convenience counters - the totals per class over the whole matrix.
struct MatrixTotals {
    uint32_t total_rows = 0;
    uint32_t capability_gate_rows = 0;
    uint32_t implementable_rows = 0;
    uint32_t legacy_skeleton_rows = 0;
};

MatrixTotals matrix_totals();

// ---------------------------------------------------------------------
// Hermetic mocks used by the focused tests.
//
// Every helper below is deliberately platform-agnostic and MUST NOT
// touch a real driver, HWND, or IPC surface.  The ui gap test binary
// asserts through these mocks so it runs green on any host — Windows or
// non-Windows, headed or headless.
// ---------------------------------------------------------------------

// Record of everything a "real" SendInput would have done.  The
// class-B implementable auto_key tests use this to assert that
// sao_ui_auto_key_arbitrate + the arg validation on
// sao_ui_auto_key_send_key routes work correctly on any host without
// really injecting keystrokes into the OS.
class SendInputLedger {
public:
    struct Event {
        uint32_t virtual_key;
        uint32_t modifiers_mask;
        uint32_t hold_ms;
        bool     is_release;
    };

    void record_press(uint32_t vk, uint32_t mods, uint32_t hold_ms);
    void record_release(uint32_t vk);

    size_t press_count() const noexcept { return presses_; }
    size_t release_count() const noexcept { return releases_; }
    size_t total_events() const noexcept { return events_.size(); }
    const std::vector<Event>& events() const noexcept { return events_; }

    void reset() noexcept;

private:
    std::vector<Event> events_;
    size_t presses_ = 0;
    size_t releases_ = 0;
};

// Deterministic sao_status_t + capability answer that a "real"
// ShellNotifyIcon call would have produced.  The class-B alerts tests
// use this mock exclusively — no real notification is ever posted to
// the OS shell during ctest.
struct ShellNotifyMockResult {
    sao_status_t status = SAO_STATUS_OK;
    bool         icon_added = false;
    bool         icon_removed = false;
    uint32_t     balloon_show_count = 0;
};

class ShellNotifyLedger {
public:
    ShellNotifyMockResult simulate_add_icon(uint32_t id, std::string_view tip);
    ShellNotifyMockResult simulate_modify_icon(uint32_t id, std::string_view tip);
    ShellNotifyMockResult simulate_remove_icon(uint32_t id);
    ShellNotifyMockResult simulate_show_balloon(uint32_t id,
                                                std::string_view title,
                                                std::string_view body);

    size_t total_calls() const noexcept { return calls_; }
    void reset() noexcept;

private:
    size_t calls_ = 0;
    ShellNotifyMockResult latest_{};
};

// Late-bind mock for DirectComposition.  Implementable-path tests use
// this to exercise the fall-through logic when dcomp.dll is present but
// the DCompositionCreateDevice symbol resolution has to be probed.
class DcompLateBindMock {
public:
    // Simulate what the production `resolve_dcomp` helper would answer.
    bool resolve(bool pretend_dll_loaded, bool pretend_symbol_present) noexcept;

    bool last_dll_loaded() const noexcept { return last_dll_loaded_; }
    bool last_symbol_present() const noexcept { return last_symbol_present_; }

    // Classify the outcome the way the ui gap matrix expects.
    GapClass classify_outcome(bool loaded, bool symbol) const noexcept;

private:
    bool last_dll_loaded_ = false;
    bool last_symbol_present_ = false;
};

// ---------------------------------------------------------------------
// Assertion-style helpers.
// ---------------------------------------------------------------------

// Returns true iff `status` is a legitimate capability-gate response.
bool status_is_capability_gate(sao_status_t status) noexcept;

// Returns true iff `status` is a legitimate legacy-skeleton response.
bool status_is_legacy_skeleton(sao_status_t status) noexcept;

// A minimal invariant: the two classification codes must remain
// distinct so the taxonomy has semantic value.
bool taxonomy_codes_are_distinct() noexcept;

} // namespace sao::ui::gap
