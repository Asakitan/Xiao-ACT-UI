#pragma once

#include "native_utils.h"

#include "sao/core/status.h"
#include "sao/rt_io/proxy.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <vector>

namespace sao::ai_editor::native {

inline constexpr std::size_t kMemoryViewerMaximumReadBytes = 64U * 1024U;
inline constexpr std::size_t kMemoryViewerMaximumRegions = 4096U;
inline constexpr std::size_t kMemoryViewerMaximumJsonResponseBytes = 4U * 1024U * 1024U;

enum class MemoryViewerState : std::uint8_t {
    detached,
    ready,
    closing,
    closed,
};

enum class MemoryViewerError : std::uint8_t {
    none,
    invalid_argument,
    detached,
    stale,
    exited,
    unreadable,
    limit,
    closed,
    io_failure,
};

struct MemoryViewerOutcome final {
    MemoryViewerError error{MemoryViewerError::none};
    sao_status_t rt_io_status{SAO_STATUS_OK};

    [[nodiscard]] constexpr bool ok() const noexcept {
        return error == MemoryViewerError::none;
    }
};

struct MemoryViewerBindingIdentity final {
    std::uint32_t pid{};
    std::uint64_t start_time_100ns{};
    std::uint64_t selection_generation{};

    bool operator==(const MemoryViewerBindingIdentity&) const = default;
};

struct MemoryViewerStatus final {
    MemoryViewerState state{MemoryViewerState::detached};
    MemoryViewerBindingIdentity binding{};
    std::size_t in_flight{};
    MemoryViewerOutcome last_outcome{};

    [[nodiscard]] bool bound() const noexcept {
        return binding.pid != 0U && binding.start_time_100ns != 0U &&
               binding.selection_generation != 0U;
    }
};

struct MemoryViewerRegion final {
    std::uint64_t base{};
    std::uint64_t size{};
    std::uint32_t protect{};
    std::uint32_t region_type{};
};

struct MemoryViewerRegionsResult final {
    MemoryViewerOutcome outcome{};
    MemoryViewerBindingIdentity binding{};
    std::size_t page_offset{};
    std::size_t page_limit{};
    std::size_t total_regions{};
    std::vector<MemoryViewerRegion> regions;
};

struct MemoryViewerReadResult final {
    MemoryViewerOutcome outcome{};
    MemoryViewerBindingIdentity binding{};
    std::uint64_t address{};
    std::size_t requested_size{};
    std::vector<std::uint8_t> bytes;
};

std::string_view memory_viewer_state_name(MemoryViewerState state) noexcept;
std::string_view memory_viewer_error_name(MemoryViewerError error) noexcept;
Json memory_viewer_status_to_json(const MemoryViewerStatus& status);
Json memory_viewer_regions_to_json(const MemoryViewerRegionsResult& result);
Json memory_viewer_read_to_json(const MemoryViewerReadResult& result);

class MemoryViewerProvider final {
  public:
    MemoryViewerProvider() noexcept = default;
    ~MemoryViewerProvider() noexcept;

    MemoryViewerProvider(const MemoryViewerProvider&) = delete;
    MemoryViewerProvider& operator=(const MemoryViewerProvider&) = delete;
    MemoryViewerProvider(MemoryViewerProvider&&) = delete;
    MemoryViewerProvider& operator=(MemoryViewerProvider&&) = delete;

    // Panel-open entry for the `memory` tool id (open-tool IPC command).
    // The provider owns no window chrome of its own — the launcher's memory
    // viewer panel does — so showing resolves through the process-wide
    // platform panel binding registered by the launcher
    // (init_pipeline.cpp binds "memory" to ctx->memory_viewer_panel->open()).
    // In a process where the binding is absent (the SaoAiEditor child does
    // not install one) the call reports SAO_SDK_ERR_NOT_INITIALIZED so the
    // caller can surface a defined error instead of a silent no-op.
    // Returns an sao_sdk_status_t value.
    [[nodiscard]] static int32_t open_panel() noexcept;

    // The caller keeps proxy alive and serializes attachment changes with lifecycle drain.
    [[nodiscard]] MemoryViewerOutcome bind(sao_rt_io_proxy_handle_t proxy, std::uint32_t pid,
                                           std::uint64_t start_time_100ns,
                                           std::uint64_t selection_generation) noexcept;
    [[nodiscard]] MemoryViewerOutcome clear() noexcept;
    [[nodiscard]] MemoryViewerOutcome invalidate() noexcept;
    [[nodiscard]] MemoryViewerOutcome close() noexcept;

    [[nodiscard]] MemoryViewerStatus status() const noexcept;
    [[nodiscard]] Json status_json() const;

    [[nodiscard]] MemoryViewerRegionsResult regions(std::size_t page_offset,
                                                    std::size_t page_limit,
                                                    const MemoryViewerBindingIdentity* expected =
                                                        nullptr) noexcept;
    [[nodiscard]] Json regions_json(std::size_t page_offset, std::size_t page_limit);

    [[nodiscard]] MemoryViewerReadResult read(
        std::uint64_t address, std::size_t size,
        const MemoryViewerBindingIdentity* expected = nullptr) noexcept;
    [[nodiscard]] Json read_json(std::uint64_t address, std::size_t size);

  private:
    struct BindingSnapshot final {
        sao_rt_io_proxy_handle_t proxy{};
        MemoryViewerBindingIdentity identity{};
        std::uint64_t epoch{};
    };

    class OperationLease;

    [[nodiscard]] MemoryViewerOutcome begin_operation(
        BindingSnapshot& binding,
        const MemoryViewerBindingIdentity* expected) noexcept;
    [[nodiscard]] MemoryViewerOutcome finish_operation(const BindingSnapshot& binding,
                                                       MemoryViewerOutcome outcome) noexcept;
    void abandon_operation(const BindingSnapshot& binding) noexcept;
    [[nodiscard]] bool binding_current_locked(const BindingSnapshot& binding) const noexcept;
    [[nodiscard]] bool advance_epoch_locked() noexcept;

    mutable std::mutex mutex_;
    std::mutex io_mutex_;
    std::mutex lifecycle_mutex_;
    sao_rt_io_proxy_handle_t proxy_{};
    MemoryViewerBindingIdentity binding_{};
    std::uint64_t binding_epoch_{1U};
    std::uint64_t highest_selection_generation_{};
    std::size_t in_flight_{};
    MemoryViewerState state_{MemoryViewerState::detached};
    MemoryViewerOutcome last_outcome_{};
    bool accepting_{};
    bool attachment_current_{};
};

} // namespace sao::ai_editor::native
