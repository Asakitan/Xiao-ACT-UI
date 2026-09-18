#pragma once

// SAO Auto launcher — read-only memory viewer panel.
//
// Native generic compositor panel mirroring the process-selector owner
// lifecycle (borrowed compositor, owner-thread publish, deferred cleanup,
// stale-generation drops).  The viewer never owns the rt_io proxy and never
// attaches or detaches; it observes the attachment handed over by the
// launcher dispatcher and issues bounded read-only calls only from explicit
// user actions.

#include "sao/core/status.h"
#include "sao/ui/abi.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct sao_ui_compositor_s;
typedef struct sao_ui_compositor_s* sao_ui_compositor_handle_t;

struct sao_rt_io_proxy_s;
typedef struct sao_rt_io_proxy_s* sao_rt_io_proxy_handle_t;

namespace sao::launcher::memory_viewer_panel {

inline constexpr char kPanelId[] = "sao.launcher.memory_viewer";
inline constexpr char kPanelTitle[] = "Memory Viewer";
inline constexpr char kRefreshTargetAction[] = "memory_viewer.refresh_target";
inline constexpr char kEnumRegionsAction[] = "memory_viewer.enum_regions";
inline constexpr char kReadAction[] = "memory_viewer.read";
inline constexpr char kAddressInputAction[] = "memory_viewer.address_input";
inline constexpr char kSizeInputAction[] = "memory_viewer.size_input";
inline constexpr char kRegionsPageAction[] = "memory_viewer.regions_page";
inline constexpr char kResetSessionAction[] = "memory_viewer.reset_session";

inline constexpr std::size_t kMaximumReadBytes = 64U * 1024U;
// One enumerated page carries at most this many regions; enumeration stops
// at the bound instead of scanning without a cap.  The bound is also the
// compositor budget: each rendered row is a container plus five badges =
// 6 spec nodes, and kMaximumPanelSpecNodes is 400 — 48 rows = 288 nodes
// leaves headroom for the header/health/read/paging chrome, while 4096
// rows (≈24.6k nodes) would silently truncate the page render.
inline constexpr std::size_t kRegionsPerPage = 48U;

enum class TargetHealth : std::uint8_t {
    ok,
    detached,
    stale,
    exited,
    unreadable,
};

struct TargetBinding {
    // Borrowed; the caller (launcher dispatcher) owns the proxy lifetime,
    // attachment generation ordering, and any drains.  Never destroy here.
    sao_rt_io_proxy_handle_t proxy{};
    std::uint32_t pid{};
    std::uint64_t start_time_100ns{};
    // Monotonic attachment generation published by the handover caller.
    // Stale (older) generations are rejected on every action.
    std::uint64_t generation{};
    std::string image_path_utf8;
    std::string base_name_utf8;

    bool valid() const noexcept {
        return proxy != nullptr && pid != 0U && start_time_100ns != 0U &&
               generation != 0U;
    }

    bool operator==(const TargetBinding&) const = default;
};

struct RegionRecord {
    std::uint64_t base{};
    std::uint64_t size{};
    std::uint32_t protect{};
    std::uint32_t region_type{};

    bool operator==(const RegionRecord&) const = default;
};

struct ReadResult {
    std::uint64_t address{};
    std::size_t requested_size{};
    std::size_t bytes_read{};
    std::vector<std::uint8_t> bytes;
};

struct Snapshot {
    bool panel_created{};
    bool action_handler_attached{};
    bool event_handler_attached{};
    bool visible{};
    bool busy{};
    bool read_busy{};
    bool scan_busy{};
    std::optional<TargetBinding> target;
    TargetHealth target_health{TargetHealth::detached};
    std::uint64_t highest_seen_generation{};
    std::size_t regions_page_index{};
    std::vector<RegionRecord> regions;
    std::size_t regions_total{};
    std::string address_input;
    std::string size_input;
    std::optional<ReadResult> last_read;
    sao_status_t last_status{SAO_STATUS_OK};
    std::string status_text;
    std::string rendered_spec_json;
};

class Owner final {
  public:
    Owner(sao_ui_compositor_handle_t compositor);
    Owner(sao_ui_compositor_handle_t compositor, sao_rt_io_proxy_handle_t borrowed_proxy);
    // Destroy on the compositor owner thread, or complete take_offline()
    // first. A live registration is never silently discarded.
    ~Owner() noexcept;

    Owner(const Owner&) = delete;
    Owner& operator=(const Owner&) = delete;
    Owner(Owner&&) = delete;
    Owner& operator=(Owner&&) = delete;

    void request_shutdown() noexcept;
    sao_status_t open() noexcept;
    sao_status_t close() noexcept;
    sao_status_t service_ui() noexcept;
    // Owner-thread, retryable panel retirement. Any failure preserves the
    // registered panel and callback ownership for a later retry.
    sao_status_t take_offline() noexcept;
    void fail_next_unregister_for_testing(sao_status_t status) noexcept;
    void fail_next_handler_restore_for_testing(sao_status_t action_status,
                                               sao_status_t event_status) noexcept;
    static void drain_deferred_cleanup_for_owner() noexcept;
    static void drain_deferred_cleanup_for_testing() noexcept;

    // Launcher-private handover hook: publishes (or replaces) the borrowed
    // attachment observed by this panel.  `generation` must be monotonic
    // per attachment epoch; a smaller or equal generation for a different
    // identity is treated as stale and rejected.
    sao_status_t bind_target(TargetBinding binding) noexcept;
    sao_status_t clear_target(std::uint64_t generation) noexcept;
    sao_status_t refresh_target() noexcept;
    sao_status_t enum_regions() noexcept;
    sao_status_t read(std::uint64_t address, std::size_t size) noexcept;
    sao_status_t set_address_input(std::string_view text) noexcept;
    sao_status_t set_size_input(std::string_view text) noexcept;
    sao_status_t set_regions_page(std::size_t page_index) noexcept;
    sao_status_t reset_session() noexcept;
    sao_status_t dispatch_action(std::string_view action_id,
                                 std::string_view payload_json = {}) noexcept;
    sao_status_t snapshot(Snapshot& out) const noexcept;

  private:
    struct OperationGuard;
    struct State;
    struct AdoptStateTag {};
    explicit Owner(std::unique_ptr<State> state, AdoptStateTag) noexcept;

    sao_status_t require_owner_thread() const noexcept;
    sao_status_t begin_operation() noexcept;
    void end_operation() noexcept;
    bool begin_callback() noexcept;
    void end_callback() noexcept;
    void handle_panel_event(std::int32_t event_kind) noexcept;
    sao_status_t ensure_panel() noexcept;
    sao_status_t publish() noexcept;
    sao_status_t check_identity_locked(TargetHealth& health_out) noexcept;

    static void SAO_UI_CALL panel_action_callback(const char* action_id_utf8,
                                                  const std::uint8_t* payload_json_utf8,
                                                  std::size_t payload_len,
                                                  void* user_data) noexcept;
    static void SAO_UI_CALL panel_event_callback(std::int32_t event_kind,
                                                 void* user_data) noexcept;

    static void defer_state(std::unique_ptr<State> state) noexcept;
    static void drain_deferred_cleanup() noexcept;
    static std::mutex deferred_mutex_;
    static std::vector<std::unique_ptr<State>> deferred_cleanup_;

    std::unique_ptr<State> state_;
};

} // namespace sao::launcher::memory_viewer_panel
