#pragma once

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

namespace sao::launcher::process_selector_panel {

inline constexpr char kPanelId[] = "sao.launcher.process_selector";
inline constexpr char kPanelTitle[] = "Process Selector";
inline constexpr char kRefreshAction[] = "process_selector.refresh";
inline constexpr char kFilterAction[] = "process_selector.filter";
inline constexpr char kAttachAction[] = "process_selector.attach";

enum class FilterMode : std::uint8_t {
    all,
    likely_game,
};

struct ProcessIdentity {
    std::uint32_t pid{};
    std::uint64_t start_time_100ns{};

    bool operator==(const ProcessIdentity&) const = default;
};

struct ProcessRecord {
    std::uint32_t pid{};
    std::uint32_t parent_pid{};
    std::uint64_t start_time_100ns{};
    std::string image_path_utf8;
    std::string base_name_utf8;

    ProcessIdentity identity() const noexcept {
        return {pid, start_time_100ns};
    }

    bool operator==(const ProcessRecord&) const = default;
};

struct Operations {
    std::function<sao_status_t(std::vector<ProcessRecord>&)> enumerate_snapshot;
    std::function<sao_status_t(std::uint32_t, ProcessRecord&)> query_process;
    std::function<sao_status_t(std::uint32_t)> attach;
    std::uint32_t current_process_id{};
};

struct Snapshot {
    bool panel_created{};
    bool action_handler_attached{};
    bool event_handler_attached{};
    bool visible{};
    bool loading{};
    FilterMode filter{FilterMode::all};
    std::size_t page_index{};
    bool attach_available{};
    sao_status_t last_status{SAO_STATUS_OK};
    std::string status_text;
    std::vector<ProcessRecord> all_processes;
    std::vector<ProcessRecord> visible_processes;
    std::optional<ProcessRecord> attached_process;
    std::string rendered_spec_json;
};

Operations make_default_operations(sao_rt_io_proxy_handle_t proxy);
bool is_likely_game_process(const ProcessRecord& process);

class Owner final {
  public:
    Owner(sao_ui_compositor_handle_t compositor, sao_rt_io_proxy_handle_t proxy);
    Owner(sao_ui_compositor_handle_t compositor, Operations operations);
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
    sao_status_t refresh() noexcept;
    sao_status_t set_filter(FilterMode filter) noexcept;
    sao_status_t attach(ProcessIdentity identity) noexcept;
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
    sao_status_t enqueue_refresh() noexcept;
    sao_status_t set_page(std::size_t page_index) noexcept;

    static void SAO_UI_CALL panel_action_callback(const char* action_id_utf8,
                                                  const std::uint8_t* payload_json_utf8,
                                                  std::size_t payload_len,
                                                  void* user_data) noexcept;
    static void SAO_UI_CALL panel_event_callback(std::int32_t event_kind, void* user_data) noexcept;

    static void defer_state(std::unique_ptr<State> state) noexcept;
    static void drain_deferred_cleanup() noexcept;
    static std::mutex deferred_mutex_;
    static std::vector<std::unique_ptr<State>> deferred_cleanup_;

    std::unique_ptr<State> state_;
};

} // namespace sao::launcher::process_selector_panel
