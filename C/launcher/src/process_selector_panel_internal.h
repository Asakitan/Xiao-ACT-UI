#pragma once

#include "sao/core/status.h"

#include <cstdint>
#include <functional>
#include <memory>
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
    bool visible{};
    FilterMode filter{FilterMode::all};
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
    ~Owner() noexcept;

    Owner(const Owner&) = delete;
    Owner& operator=(const Owner&) = delete;
    Owner(Owner&&) = delete;
    Owner& operator=(Owner&&) = delete;

    sao_status_t open() noexcept;
    sao_status_t close() noexcept;
    sao_status_t refresh() noexcept;
    sao_status_t set_filter(FilterMode filter) noexcept;
    sao_status_t attach(ProcessIdentity identity) noexcept;
    sao_status_t dispatch_action(std::string_view action_id,
                                 std::string_view payload_json = {}) noexcept;
    sao_status_t snapshot(Snapshot& out) const noexcept;

  private:
    struct State;

    sao_status_t require_owner_thread() const noexcept;
    sao_status_t ensure_panel() noexcept;
    sao_status_t publish() noexcept;

    std::unique_ptr<State> state_;
};

} // namespace sao::launcher::process_selector_panel
