#include "memory_viewer_provider.h"

#include "sao/rt_io/status.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <new>
#include <string>
#include <system_error>
#include <utility>

namespace sao::ai_editor::native {
namespace {

constexpr std::size_t kMaximumEnumerationAttempts = 3U;
constexpr std::size_t kMaximumOperationNesting = 8U;
constexpr char kHexDigits[] = "0123456789abcdef";

thread_local std::array<const MemoryViewerProvider*, kMaximumOperationNesting>
    g_active_operations{};
thread_local std::size_t g_active_operation_count{};

MemoryViewerOutcome make_outcome(MemoryViewerError error, sao_status_t rt_io_status) noexcept {
    return MemoryViewerOutcome{error, rt_io_status};
}

MemoryViewerOutcome outcome_from_rt_io(sao_status_t status) noexcept {
    if (status == SAO_STATUS_OK)
        return {};

    switch (status) {
    case SAO_STATUS_ERR_INVALID_ARGUMENT:
        return make_outcome(MemoryViewerError::invalid_argument, status);
    case SAO_STATUS_ERR_NOT_FOUND:
    case SAO_STATUS_ERR_PROCESS_GONE:
    case SAO_RT_IO_ERR_PROCESS_INVALID:
    case SAO_RT_IO_ERR_TARGET_PID_INVALID:
        return make_outcome(MemoryViewerError::exited, status);
    case SAO_STATUS_ERR_READ_FAULT:
    case SAO_STATUS_ERR_ACCESS_DENIED:
    case SAO_STATUS_ERR_CAPABILITY_MISSING:
    case SAO_RT_IO_ERR_INSUFFICIENT_PRIVILEGE:
    case SAO_RT_IO_ERR_READ_FAILED:
    case SAO_RT_IO_ERR_ADDRESS_UNREADABLE:
    case SAO_RT_IO_ERR_TARGET_ADDR_UNREADABLE:
        return make_outcome(MemoryViewerError::unreadable, status);
    case SAO_STATUS_ERR_BUFFER_TOO_SMALL:
    case SAO_RT_IO_ERR_PAYLOAD_TOO_LARGE:
        return make_outcome(MemoryViewerError::limit, status);
    case SAO_STATUS_ERR_CANCELLED:
    case SAO_RT_IO_ERR_PIPE_CANCELLED:
    case SAO_RT_IO_ERR_SESSION_REKEY_REQUIRED:
        return make_outcome(MemoryViewerError::stale, status);
    case SAO_STATUS_ERR_HANDLE_INVALID:
    case SAO_STATUS_ERR_NOT_INITIALIZED:
    case SAO_RT_IO_ERR_PROCESS_NOT_ATTACHED:
    case SAO_RT_IO_ERR_HELPER_NOT_LAUNCHED:
    case SAO_RT_IO_ERR_HELPER_HANDSHAKE_FAIL:
    case SAO_RT_IO_ERR_HELPER_EXITED:
    case SAO_RT_IO_ERR_HELPER_BOOTSTRAP_AUTH:
    case SAO_RT_IO_ERR_HELPER_PROTOCOL_MISMATCH:
    case SAO_RT_IO_ERR_HELPER_BOOTSTRAP_FAIL:
    case SAO_RT_IO_ERR_HELPER_NOT_FOUND:
    case SAO_RT_IO_ERR_HELPER_START_FAILED:
    case SAO_RT_IO_ERR_HELPER_READY_TIMEOUT:
    case SAO_RT_IO_ERR_HELPER_CRASHED:
    case SAO_RT_IO_ERR_SESSION_NOT_RUNNING:
    case SAO_RT_IO_ERR_SESSION_QUIESCING:
    case SAO_RT_IO_ERR_SESSION_CLOSED:
    case SAO_RT_IO_ERR_DRIVER_NOT_LOADED:
        return make_outcome(MemoryViewerError::detached, status);
    default:
        return make_outcome(MemoryViewerError::io_failure, status);
    }
}

MemoryViewerOutcome process_error_from_win32(DWORD error) noexcept {
    switch (error) {
    case ERROR_INVALID_HANDLE:
    case ERROR_INVALID_PARAMETER:
    case ERROR_NOT_FOUND:
        return make_outcome(MemoryViewerError::exited, SAO_STATUS_ERR_PROCESS_GONE);
    case ERROR_ACCESS_DENIED:
        return make_outcome(MemoryViewerError::unreadable, SAO_STATUS_ERR_ACCESS_DENIED);
    default:
        return make_outcome(MemoryViewerError::io_failure, SAO_STATUS_ERR_OS_CALL_FAILED);
    }
}

class ProcessIdentityLease final {
  public:
    ProcessIdentityLease() noexcept = default;

    ~ProcessIdentityLease() noexcept {
        if (handle_ != nullptr)
            CloseHandle(handle_);
    }

    ProcessIdentityLease(const ProcessIdentityLease&) = delete;
    ProcessIdentityLease& operator=(const ProcessIdentityLease&) = delete;

    void reset(HANDLE handle) noexcept {
        if (handle_ != nullptr)
            CloseHandle(handle_);
        handle_ = handle;
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

  private:
    HANDLE handle_{};
};

MemoryViewerOutcome
validate_process_identity(const ProcessIdentityLease& process,
                          const MemoryViewerBindingIdentity& expected) noexcept {
    if (process.get() == nullptr || expected.pid == 0U || expected.start_time_100ns == 0U)
        return make_outcome(MemoryViewerError::invalid_argument, SAO_STATUS_ERR_INVALID_ARGUMENT);

    const DWORD observed_pid = GetProcessId(process.get());
    if (observed_pid == 0U)
        return process_error_from_win32(GetLastError());
    if (observed_pid != expected.pid)
        return make_outcome(MemoryViewerError::stale, SAO_STATUS_ERR_PROCESS_GONE);

    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(process.get(), &created, &exited, &kernel, &user) == FALSE)
        return process_error_from_win32(GetLastError());
    ULARGE_INTEGER created_100ns{};
    created_100ns.LowPart = created.dwLowDateTime;
    created_100ns.HighPart = created.dwHighDateTime;
    if (created_100ns.QuadPart != expected.start_time_100ns)
        return make_outcome(MemoryViewerError::stale, SAO_STATUS_ERR_PROCESS_GONE);

    const DWORD wait_result = WaitForSingleObject(process.get(), 0U);
    if (wait_result == WAIT_OBJECT_0)
        return make_outcome(MemoryViewerError::exited, SAO_STATUS_ERR_PROCESS_GONE);
    if (wait_result != WAIT_TIMEOUT)
        return process_error_from_win32(GetLastError());
    return {};
}

MemoryViewerOutcome open_process_identity(const MemoryViewerBindingIdentity& expected,
                                          ProcessIdentityLease& process) noexcept {
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
                                static_cast<DWORD>(expected.pid));
    if (handle == nullptr)
        return process_error_from_win32(GetLastError());
    process.reset(handle);
    return validate_process_identity(process, expected);
}

bool operation_active_on_current_thread(const MemoryViewerProvider* provider) noexcept {
    for (std::size_t index = 0U; index < g_active_operation_count; ++index) {
        if (g_active_operations[index] == provider)
            return true;
    }
    return false;
}

class OperationThreadScope final {
  public:
    explicit OperationThreadScope(const MemoryViewerProvider* provider) noexcept {
        if (g_active_operation_count == g_active_operations.size())
            return;
        g_active_operations[g_active_operation_count++] = provider;
        active_ = true;
    }

    ~OperationThreadScope() noexcept {
        if (!active_)
            return;
        g_active_operations[--g_active_operation_count] = nullptr;
    }

    OperationThreadScope(const OperationThreadScope&) = delete;
    OperationThreadScope& operator=(const OperationThreadScope&) = delete;

    [[nodiscard]] bool active() const noexcept {
        return active_;
    }

  private:
    bool active_{};
};

bool outcome_invalidates_attachment(const MemoryViewerOutcome& outcome) noexcept {
    return outcome.error == MemoryViewerError::detached ||
           outcome.error == MemoryViewerError::stale ||
            outcome.error == MemoryViewerError::exited ||
           outcome.error == MemoryViewerError::closed;
}

std::string format_hex_u64(std::uint64_t value) {
    std::array<char, 16> digits{};
    const auto converted = std::to_chars(digits.data(), digits.data() + digits.size(), value, 16);
    std::string result{"0x"};
    if (converted.ec == std::errc{})
        result.append(digits.data(), converted.ptr);
    else
        result.push_back('0');
    return result;
}

std::string format_decimal_u64(std::uint64_t value) {
    std::array<char, 20> digits{};
    const auto converted = std::to_chars(digits.data(), digits.data() + digits.size(), value);
    if (converted.ec != std::errc{})
        return "0";
    return std::string(digits.data(), converted.ptr);
}

std::string encode_hex(const std::vector<std::uint8_t>& bytes) {
    std::string encoded(bytes.size() * 2U, '0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        encoded[index * 2U] = kHexDigits[bytes[index] >> 4U];
        encoded[index * 2U + 1U] = kHexDigits[bytes[index] & 0x0fU];
    }
    return encoded;
}

std::string_view memory_viewer_error_message(MemoryViewerError error) noexcept {
    switch (error) {
    case MemoryViewerError::none:
        return "";
    case MemoryViewerError::invalid_argument:
        return "invalid memory viewer request";
    case MemoryViewerError::detached:
        return "memory viewer is detached";
    case MemoryViewerError::stale:
        return "process selection changed during the operation";
    case MemoryViewerError::exited:
        return "selected process is no longer available";
    case MemoryViewerError::unreadable:
        return "requested memory is unreadable";
    case MemoryViewerError::limit:
        return "memory viewer limit exceeded";
    case MemoryViewerError::closed:
        return "memory viewer is closed";
    case MemoryViewerError::io_failure:
        return "memory viewer I/O failed";
    }
    return "memory viewer I/O failed";
}

Json binding_to_json(const MemoryViewerBindingIdentity& binding) {
    if (binding.pid == 0U)
        return nullptr;
    return Json{{"pid", binding.pid},
                {"startTime100ns", format_decimal_u64(binding.start_time_100ns)},
                {"selectionGeneration", format_decimal_u64(binding.selection_generation)}};
}

Json error_to_json(const MemoryViewerOutcome& outcome) {
    if (outcome.ok())
        return nullptr;
    return Json{{"code", std::string(memory_viewer_error_name(outcome.error))},
                {"message", std::string(memory_viewer_error_message(outcome.error))},
                {"rtIoStatus", outcome.rt_io_status}};
}

Json operation_json(const MemoryViewerOutcome& outcome,
                    const MemoryViewerBindingIdentity& binding) {
    return Json{{"ok", outcome.ok()},
                {"status", outcome.ok() ? "ok" : "error"},
                {"binding", binding_to_json(binding)},
                {"error", error_to_json(outcome)}};
}

Json enforce_response_budget(Json response, const MemoryViewerBindingIdentity& binding) {
    if (response.dump().size() < kMemoryViewerMaximumJsonResponseBytes)
        return response;
    Json limited = operation_json(
        make_outcome(MemoryViewerError::limit, SAO_STATUS_ERR_BUFFER_TOO_SMALL), binding);
    limited["maximumResponseBytes"] = kMemoryViewerMaximumJsonResponseBytes - 1U;
    return limited;
}

} // namespace

std::string_view memory_viewer_state_name(MemoryViewerState state) noexcept {
    switch (state) {
    case MemoryViewerState::detached:
        return "detached";
    case MemoryViewerState::ready:
        return "ready";
    case MemoryViewerState::closing:
        return "closing";
    case MemoryViewerState::closed:
        return "closed";
    }
    return "closed";
}

std::string_view memory_viewer_error_name(MemoryViewerError error) noexcept {
    switch (error) {
    case MemoryViewerError::none:
        return "none";
    case MemoryViewerError::invalid_argument:
        return "invalid_argument";
    case MemoryViewerError::detached:
        return "detached";
    case MemoryViewerError::stale:
        return "stale";
    case MemoryViewerError::exited:
        return "exited";
    case MemoryViewerError::unreadable:
        return "unreadable";
    case MemoryViewerError::limit:
        return "limit";
    case MemoryViewerError::closed:
        return "closed";
    case MemoryViewerError::io_failure:
        return "io_failure";
    }
    return "io_failure";
}

Json memory_viewer_status_to_json(const MemoryViewerStatus& status) {
    return Json{
        {"ok", true},
        {"state", std::string(memory_viewer_state_name(status.state))},
        {"bound", status.bound()},
        {"available", status.state == MemoryViewerState::ready},
        {"readOnly", true},
        {"binding", binding_to_json(status.binding)},
        {"inFlight", status.in_flight},
        {"lastError", error_to_json(status.last_outcome)},
        {"limits", Json{{"maximumReadBytes", kMemoryViewerMaximumReadBytes},
                        {"maximumRegions", kMemoryViewerMaximumRegions},
                        {"maximumResponseBytes", kMemoryViewerMaximumJsonResponseBytes - 1U}}}};
}

Json memory_viewer_regions_to_json(const MemoryViewerRegionsResult& result) {
    MemoryViewerOutcome outcome = result.outcome;
    if (outcome.ok() &&
        (result.page_limit == 0U || result.page_limit > kMemoryViewerMaximumRegions ||
         result.total_regions > kMemoryViewerMaximumRegions ||
         result.regions.size() > result.page_limit ||
         result.page_offset > (std::numeric_limits<std::size_t>::max)() - result.page_limit)) {
        outcome = make_outcome(MemoryViewerError::limit, SAO_STATUS_ERR_BUFFER_TOO_SMALL);
    }
    Json response = operation_json(outcome, result.binding);
    response["page"] = Json{{"offset", result.page_offset},
                            {"limit", result.page_limit},
                            {"count", outcome.ok() ? result.regions.size() : 0U},
                            {"total", result.total_regions}};
    if (outcome.ok()) {
        Json regions = Json::array();
        for (const MemoryViewerRegion& region : result.regions) {
            if (region.size == 0U ||
                region.base > (std::numeric_limits<std::uint64_t>::max)() - region.size) {
                response = operation_json(
                    make_outcome(MemoryViewerError::io_failure, SAO_STATUS_ERR_OS_CALL_FAILED),
                    result.binding);
                response["page"] = Json{{"offset", result.page_offset},
                                        {"limit", result.page_limit},
                                        {"count", 0U},
                                        {"total", result.total_regions}};
                response["regions"] = Json::array();
                return enforce_response_budget(std::move(response), result.binding);
            }
            regions.push_back(Json{{"base", format_hex_u64(region.base)},
                                   {"endExclusive", format_hex_u64(region.base + region.size)},
                                   {"size", format_decimal_u64(region.size)},
                                   {"protect", region.protect},
                                   {"regionType", region.region_type}});
        }
        response["regions"] = std::move(regions);
    } else {
        response["regions"] = Json::array();
    }
    return enforce_response_budget(std::move(response), result.binding);
}

Json memory_viewer_read_to_json(const MemoryViewerReadResult& result) {
    MemoryViewerOutcome outcome = result.outcome;
    if (outcome.ok()) {
        if (result.requested_size == 0U || result.requested_size > kMemoryViewerMaximumReadBytes ||
            result.bytes.size() > kMemoryViewerMaximumReadBytes) {
            outcome = make_outcome(MemoryViewerError::limit, SAO_RT_IO_ERR_PAYLOAD_TOO_LARGE);
        } else if (result.bytes.size() != result.requested_size) {
            outcome = make_outcome(MemoryViewerError::unreadable, SAO_RT_IO_ERR_READ_FAILED);
        }
    }
    Json response = operation_json(outcome, result.binding);
    response["address"] = format_hex_u64(result.address);
    response["requestedSize"] = result.requested_size;
    response["bytesRead"] = outcome.ok() ? result.bytes.size() : 0U;
    response["encoding"] = "hex";
    response["data"] = outcome.ok() ? encode_hex(result.bytes) : std::string{};
    return enforce_response_budget(std::move(response), result.binding);
}

class MemoryViewerProvider::OperationLease final {
  public:
        OperationLease(MemoryViewerProvider& owner,
                                     const MemoryViewerBindingIdentity* expected) noexcept
                : owner_(&owner) {
                begin_outcome_ = owner_->begin_operation(binding_, expected);
        active_ = begin_outcome_.ok();
    }

    ~OperationLease() noexcept {
        if (active_)
            owner_->abandon_operation(binding_);
    }

    OperationLease(const OperationLease&) = delete;
    OperationLease& operator=(const OperationLease&) = delete;

    [[nodiscard]] bool acquired() const noexcept {
        return active_;
    }

    [[nodiscard]] const BindingSnapshot& binding() const noexcept {
        return binding_;
    }

    [[nodiscard]] MemoryViewerOutcome begin_outcome() const noexcept {
        return begin_outcome_;
    }

    [[nodiscard]] MemoryViewerOutcome complete(MemoryViewerOutcome outcome) noexcept {
        if (!active_)
            return begin_outcome_;
        active_ = false;
        return owner_->finish_operation(binding_, outcome);
    }

  private:
    MemoryViewerProvider* owner_{};
    BindingSnapshot binding_{};
    MemoryViewerOutcome begin_outcome_{};
    bool active_{};
};

MemoryViewerProvider::~MemoryViewerProvider() noexcept {
    (void)close();
}

MemoryViewerOutcome MemoryViewerProvider::bind(sao_rt_io_proxy_handle_t proxy, std::uint32_t pid,
                                               std::uint64_t start_time_100ns,
                                               std::uint64_t selection_generation) noexcept {
    if (operation_active_on_current_thread(this))
        return make_outcome(MemoryViewerError::stale, SAO_RT_IO_ERR_SESSION_QUIESCING);

    const MemoryViewerBindingIdentity requested{pid, start_time_100ns, selection_generation};
    try {
        std::lock_guard lifecycle_guard(lifecycle_mutex_);
        OperationThreadScope lifecycle_scope(this);
        if (!lifecycle_scope.active())
            return make_outcome(MemoryViewerError::io_failure, SAO_RT_IO_ERR_INTERNAL_ERROR);
        std::unique_lock io_lock(io_mutex_, std::try_to_lock);
        if (!io_lock.owns_lock())
            return make_outcome(MemoryViewerError::stale, SAO_STATUS_ERR_CANCELLED);
        {
            std::lock_guard lock(mutex_);
            if (state_ == MemoryViewerState::closed)
                return make_outcome(MemoryViewerError::closed, SAO_RT_IO_ERR_SESSION_CLOSED);
            if (proxy == nullptr || pid == 0U || start_time_100ns == 0U ||
                selection_generation == 0U) {
                last_outcome_ = make_outcome(MemoryViewerError::invalid_argument,
                                             SAO_STATUS_ERR_INVALID_ARGUMENT);
                return last_outcome_;
            }
            if (selection_generation < highest_selection_generation_ ||
                (selection_generation == highest_selection_generation_ &&
                 (state_ != MemoryViewerState::ready || proxy_ != proxy ||
                  binding_ != requested))) {
                last_outcome_ =
                    make_outcome(MemoryViewerError::stale, SAO_STATUS_ERR_CANCELLED);
                return last_outcome_;
            }
            if (state_ == MemoryViewerState::ready) {
                if (proxy_ == proxy && binding_ == requested && accepting_ &&
                    attachment_current_) {
                    last_outcome_ = {};
                    return {};
                }
                last_outcome_ =
                    make_outcome(MemoryViewerError::stale, SAO_STATUS_ERR_CANCELLED);
                return last_outcome_;
            }
        }

        ProcessIdentityLease process;
        MemoryViewerOutcome outcome = open_process_identity(requested, process);
        if (!outcome.ok()) {
            std::lock_guard lock(mutex_);
            last_outcome_ = outcome;
            return outcome;
        }

        {
            std::lock_guard lock(mutex_);
            if (!advance_epoch_locked()) {
                last_outcome_ =
                    make_outcome(MemoryViewerError::limit, SAO_STATUS_ERR_BUFFER_TOO_SMALL);
                return last_outcome_;
            }
            proxy_ = proxy;
            binding_ = requested;
            highest_selection_generation_ = selection_generation;
            accepting_ = true;
            state_ = MemoryViewerState::ready;
            attachment_current_ = true;
            in_flight_ = 0U;
            last_outcome_ = {};
        }
        return {};
    } catch (...) {
        return make_outcome(MemoryViewerError::io_failure, SAO_RT_IO_ERR_INTERNAL_ERROR);
    }
}

MemoryViewerOutcome MemoryViewerProvider::clear() noexcept {
    if (operation_active_on_current_thread(this))
        return make_outcome(MemoryViewerError::stale, SAO_RT_IO_ERR_SESSION_QUIESCING);

    try {
        std::lock_guard lifecycle_guard(lifecycle_mutex_);
        OperationThreadScope lifecycle_scope(this);
        if (!lifecycle_scope.active())
            return make_outcome(MemoryViewerError::io_failure, SAO_RT_IO_ERR_INTERNAL_ERROR);
        std::unique_lock io_lock(io_mutex_, std::try_to_lock);
        if (!io_lock.owns_lock())
            return make_outcome(MemoryViewerError::stale, SAO_STATUS_ERR_CANCELLED);
        {
            std::lock_guard lock(mutex_);
            if (state_ == MemoryViewerState::closed)
                return make_outcome(MemoryViewerError::closed, SAO_RT_IO_ERR_SESSION_CLOSED);
            if (state_ == MemoryViewerState::detached && proxy_ == nullptr &&
                binding_.pid == 0U && !accepting_ && !attachment_current_) {
                last_outcome_ = {};
                return {};
            }
            if (!advance_epoch_locked()) {
                last_outcome_ =
                    make_outcome(MemoryViewerError::limit, SAO_STATUS_ERR_BUFFER_TOO_SMALL);
                return last_outcome_;
            }
            accepting_ = false;
            state_ = MemoryViewerState::closing;
            proxy_ = nullptr;
            binding_ = {};
            in_flight_ = 0U;
            attachment_current_ = false;
            accepting_ = false;
            state_ = MemoryViewerState::detached;
            last_outcome_ = {};
        }
        return {};
    } catch (...) {
        return make_outcome(MemoryViewerError::io_failure, SAO_RT_IO_ERR_INTERNAL_ERROR);
    }
}

MemoryViewerOutcome MemoryViewerProvider::invalidate() noexcept {
    return clear();
}

MemoryViewerOutcome MemoryViewerProvider::close() noexcept {
    if (operation_active_on_current_thread(this))
        return make_outcome(MemoryViewerError::stale, SAO_RT_IO_ERR_SESSION_QUIESCING);

    try {
        std::lock_guard lifecycle_guard(lifecycle_mutex_);
        OperationThreadScope lifecycle_scope(this);
        if (!lifecycle_scope.active())
            return make_outcome(MemoryViewerError::io_failure, SAO_RT_IO_ERR_INTERNAL_ERROR);
        std::unique_lock io_lock(io_mutex_, std::try_to_lock);
        if (!io_lock.owns_lock())
            return make_outcome(MemoryViewerError::stale, SAO_STATUS_ERR_CANCELLED);
        {
            std::lock_guard lock(mutex_);
            if (state_ == MemoryViewerState::closed)
                return {};
            accepting_ = false;
            state_ = MemoryViewerState::closing;
            (void)advance_epoch_locked();
            proxy_ = nullptr;
            binding_ = {};
            in_flight_ = 0U;
            attachment_current_ = false;
            accepting_ = false;
            state_ = MemoryViewerState::closed;
            last_outcome_ = {};
        }
        return {};
    } catch (...) {
        return make_outcome(MemoryViewerError::io_failure, SAO_RT_IO_ERR_INTERNAL_ERROR);
    }
}

MemoryViewerStatus MemoryViewerProvider::status() const noexcept {
    try {
        BindingSnapshot snapshot{};
        {
            std::lock_guard lock(mutex_);
            const MemoryViewerStatus current{state_, binding_, in_flight_, last_outcome_};
            if (state_ != MemoryViewerState::ready || !accepting_ || proxy_ == nullptr ||
                !current.bound()) {
                return current;
            }
            snapshot = BindingSnapshot{proxy_, binding_, binding_epoch_};
        }

        ProcessIdentityLease process;
        MemoryViewerOutcome identity_outcome =
            open_process_identity(snapshot.identity, process);
        if (identity_outcome.ok())
            identity_outcome = validate_process_identity(process, snapshot.identity);

        std::lock_guard lock(mutex_);
        if (!binding_current_locked(snapshot)) {
            return MemoryViewerStatus{
                MemoryViewerState::detached,
                {},
                in_flight_,
                make_outcome(MemoryViewerError::stale, SAO_STATUS_ERR_CANCELLED),
            };
        }
        if (!identity_outcome.ok()) {
            return MemoryViewerStatus{
                MemoryViewerState::detached,
                {},
                in_flight_,
                identity_outcome,
            };
        }
        return MemoryViewerStatus{state_, binding_, in_flight_, last_outcome_};
    } catch (...) {
        return MemoryViewerStatus{
            MemoryViewerState::closed,
            {},
            0U,
            make_outcome(MemoryViewerError::io_failure, SAO_RT_IO_ERR_INTERNAL_ERROR),
        };
    }
}

Json MemoryViewerProvider::status_json() const {
    return memory_viewer_status_to_json(status());
}

MemoryViewerRegionsResult MemoryViewerProvider::regions(
    std::size_t page_offset, std::size_t page_limit,
    const MemoryViewerBindingIdentity* expected) noexcept {
    MemoryViewerRegionsResult result{};
    result.page_offset = page_offset;
    result.page_limit = page_limit;
    if (operation_active_on_current_thread(this)) {
        result.outcome = make_outcome(MemoryViewerError::stale, SAO_RT_IO_ERR_SESSION_QUIESCING);
        return result;
    }

    try {
        std::unique_lock io_lock(io_mutex_);
        OperationThreadScope operation_scope(this);
        if (!operation_scope.active()) {
            result.outcome =
                make_outcome(MemoryViewerError::io_failure, SAO_RT_IO_ERR_INTERNAL_ERROR);
            return result;
        }

        OperationLease lease(*this, expected);
        result.outcome = lease.begin_outcome();
        result.binding = expected == nullptr ? lease.binding().identity : *expected;
        if (!lease.acquired())
            return result;

        if (page_limit == 0U || page_limit > kMemoryViewerMaximumRegions ||
            page_offset > (std::numeric_limits<std::size_t>::max)() - page_limit) {
            result.outcome = lease.complete(make_outcome(
                page_limit > kMemoryViewerMaximumRegions ? MemoryViewerError::limit
                                                         : MemoryViewerError::invalid_argument,
                page_limit > kMemoryViewerMaximumRegions ? SAO_STATUS_ERR_BUFFER_TOO_SMALL
                                                         : SAO_STATUS_ERR_INVALID_ARGUMENT));
            return result;
        }
        if (!attachment_current_) {
            result.outcome = lease.complete(
                make_outcome(MemoryViewerError::detached, SAO_RT_IO_ERR_PROCESS_NOT_ATTACHED));
            return result;
        }

        try {
            const BindingSnapshot binding = lease.binding();
            ProcessIdentityLease process;
            MemoryViewerOutcome operation_outcome =
                open_process_identity(binding.identity, process);
            const bool identity_opened = operation_outcome.ok();
            std::vector<SaoRtIoRegionInfo> raw_regions;
            bool enumerated = false;

            for (std::size_t attempt = 0U;
                 operation_outcome.ok() && attempt < kMaximumEnumerationAttempts; ++attempt) {
                std::size_t required = 0U;
                const sao_status_t size_status =
                    sao_rt_io_proxy_enum_regions(binding.proxy, nullptr, 0U, &required);
                operation_outcome = outcome_from_rt_io(size_status);
                if (!operation_outcome.ok())
                    break;
                if (required == 0U) {
                    operation_outcome =
                        make_outcome(MemoryViewerError::unreadable, SAO_RT_IO_ERR_READ_FAILED);
                    break;
                }
                if (required > kMemoryViewerMaximumRegions) {
                    operation_outcome =
                        make_outcome(MemoryViewerError::limit, SAO_STATUS_ERR_BUFFER_TOO_SMALL);
                    break;
                }

                raw_regions.assign(required, SaoRtIoRegionInfo{});
                std::size_t received = 0U;
                const sao_status_t fill_status = sao_rt_io_proxy_enum_regions(
                    binding.proxy, raw_regions.data(), raw_regions.size(), &received);
                if (fill_status == SAO_STATUS_ERR_BUFFER_TOO_SMALL) {
                    if (received > kMemoryViewerMaximumRegions) {
                        operation_outcome = make_outcome(MemoryViewerError::limit, fill_status);
                        break;
                    }
                    if (attempt + 1U == kMaximumEnumerationAttempts) {
                        operation_outcome =
                            make_outcome(MemoryViewerError::io_failure, fill_status);
                    }
                    continue;
                }
                operation_outcome = outcome_from_rt_io(fill_status);
                if (!operation_outcome.ok())
                    break;
                if (received > raw_regions.size()) {
                    operation_outcome = make_outcome(MemoryViewerError::io_failure,
                                                     SAO_STATUS_ERR_BUFFER_TOO_SMALL);
                    break;
                }
                raw_regions.resize(received);
                enumerated = true;
                break;
            }

            if (identity_opened) {
                const MemoryViewerOutcome identity_outcome =
                    validate_process_identity(process, binding.identity);
                if (!identity_outcome.ok()) {
                    operation_outcome = identity_outcome;
                    (void)sao_rt_io_proxy_detach(binding.proxy);
                    attachment_current_ = false;
                }
            }
            if (outcome_invalidates_attachment(operation_outcome) && attachment_current_) {
                (void)sao_rt_io_proxy_detach(binding.proxy);
                attachment_current_ = false;
            }
            if (operation_outcome.ok() && !enumerated) {
                operation_outcome =
                    make_outcome(MemoryViewerError::io_failure, SAO_STATUS_ERR_BUFFER_TOO_SMALL);
            }

            std::vector<MemoryViewerRegion> staged_regions;
            const std::size_t total_regions = raw_regions.size();
            if (operation_outcome.ok()) {
                for (const SaoRtIoRegionInfo& region : raw_regions) {
                    if (region.size == 0U ||
                        region.base > (std::numeric_limits<std::uint64_t>::max)() - region.size) {
                        operation_outcome = make_outcome(MemoryViewerError::io_failure,
                                                         SAO_STATUS_ERR_OS_CALL_FAILED);
                        break;
                    }
                }
            }
            if (operation_outcome.ok() && page_offset < total_regions) {
                const std::size_t count = (std::min)(page_limit, total_regions - page_offset);
                staged_regions.reserve(count);
                for (std::size_t index = 0U; index < count; ++index) {
                    const SaoRtIoRegionInfo& region = raw_regions[page_offset + index];
                    staged_regions.push_back(MemoryViewerRegion{
                        region.base,
                        region.size,
                        region.protect,
                        region.region_type,
                    });
                }
            }

            result.outcome = lease.complete(operation_outcome);
            if (result.outcome.ok()) {
                result.total_regions = total_regions;
                result.regions = std::move(staged_regions);
            }
        } catch (const std::bad_alloc&) {
            result.outcome =
                lease.complete(make_outcome(MemoryViewerError::limit, SAO_STATUS_ERR_UNKNOWN));
        } catch (...) {
            result.outcome = lease.complete(
                make_outcome(MemoryViewerError::io_failure, SAO_STATUS_ERR_OS_CALL_FAILED));
        }
    } catch (const std::bad_alloc&) {
        result.outcome = make_outcome(MemoryViewerError::limit, SAO_STATUS_ERR_UNKNOWN);
    } catch (...) {
        result.outcome = make_outcome(MemoryViewerError::io_failure, SAO_RT_IO_ERR_INTERNAL_ERROR);
    }
    return result;
}

Json MemoryViewerProvider::regions_json(std::size_t page_offset, std::size_t page_limit) {
    return memory_viewer_regions_to_json(regions(page_offset, page_limit, nullptr));
}

MemoryViewerReadResult MemoryViewerProvider::read(std::uint64_t address,
                                                  std::size_t size,
                                                  const MemoryViewerBindingIdentity* expected) noexcept {
    MemoryViewerReadResult result{};
    result.address = address;
    result.requested_size = size;
    if (operation_active_on_current_thread(this)) {
        result.outcome = make_outcome(MemoryViewerError::stale, SAO_RT_IO_ERR_SESSION_QUIESCING);
        return result;
    }

    try {
        std::unique_lock io_lock(io_mutex_);
        OperationThreadScope operation_scope(this);
        if (!operation_scope.active()) {
            result.outcome =
                make_outcome(MemoryViewerError::io_failure, SAO_RT_IO_ERR_INTERNAL_ERROR);
            return result;
        }

        OperationLease lease(*this, expected);
        result.outcome = lease.begin_outcome();
        result.binding = expected == nullptr ? lease.binding().identity : *expected;
        if (!lease.acquired())
            return result;

        if (size == 0U || size > kMemoryViewerMaximumReadBytes ||
            address >
                (std::numeric_limits<std::uint64_t>::max)() - static_cast<std::uint64_t>(size)) {
            const bool exceeds_limit = size > kMemoryViewerMaximumReadBytes;
            result.outcome = lease.complete(make_outcome(
                exceeds_limit ? MemoryViewerError::limit : MemoryViewerError::invalid_argument,
                exceeds_limit ? SAO_RT_IO_ERR_PAYLOAD_TOO_LARGE : SAO_STATUS_ERR_INVALID_ARGUMENT));
            return result;
        }

        try {
            const BindingSnapshot binding = lease.binding();
            ProcessIdentityLease process;
            MemoryViewerOutcome operation_outcome =
                open_process_identity(binding.identity, process);
            std::vector<std::uint8_t> staged_bytes;
            if (operation_outcome.ok()) {
                staged_bytes.resize(size);
                std::size_t bytes_read = 0U;
                const sao_status_t read_status =
                    sao_rt_io_proxy_read_pid(binding.proxy, binding.identity.pid, address,
                                             staged_bytes.data(), staged_bytes.size(), &bytes_read);
                attachment_current_ = false;
                operation_outcome = outcome_from_rt_io(read_status);
                if (operation_outcome.ok() && bytes_read != size) {
                    operation_outcome =
                        make_outcome(MemoryViewerError::unreadable, SAO_RT_IO_ERR_READ_FAILED);
                }

                const sao_status_t restore_status =
                    sao_rt_io_proxy_attach(binding.proxy, binding.identity.pid);
                attachment_current_ = restore_status == SAO_STATUS_OK;
                if (!attachment_current_)
                    (void)sao_rt_io_proxy_detach(binding.proxy);
                if (operation_outcome.ok() && !attachment_current_)
                    operation_outcome = outcome_from_rt_io(restore_status);

                const MemoryViewerOutcome identity_outcome =
                    validate_process_identity(process, binding.identity);
                if (!identity_outcome.ok()) {
                    operation_outcome = identity_outcome;
                    if (attachment_current_)
                        (void)sao_rt_io_proxy_detach(binding.proxy);
                    attachment_current_ = false;
                }
            }
            if (outcome_invalidates_attachment(operation_outcome) && attachment_current_) {
                (void)sao_rt_io_proxy_detach(binding.proxy);
                attachment_current_ = false;
            }

            result.outcome = lease.complete(operation_outcome);
            if (result.outcome.ok())
                result.bytes = std::move(staged_bytes);
        } catch (const std::bad_alloc&) {
            result.outcome =
                lease.complete(make_outcome(MemoryViewerError::limit, SAO_STATUS_ERR_UNKNOWN));
        } catch (...) {
            result.outcome = lease.complete(
                make_outcome(MemoryViewerError::io_failure, SAO_STATUS_ERR_OS_CALL_FAILED));
        }
    } catch (const std::bad_alloc&) {
        result.outcome = make_outcome(MemoryViewerError::limit, SAO_STATUS_ERR_UNKNOWN);
    } catch (...) {
        result.outcome = make_outcome(MemoryViewerError::io_failure, SAO_RT_IO_ERR_INTERNAL_ERROR);
    }
    return result;
}

Json MemoryViewerProvider::read_json(std::uint64_t address, std::size_t size) {
    return memory_viewer_read_to_json(read(address, size, nullptr));
}

MemoryViewerOutcome MemoryViewerProvider::begin_operation(
    BindingSnapshot& binding,
    const MemoryViewerBindingIdentity* expected) noexcept {
    try {
        std::lock_guard lock(mutex_);
        if (state_ == MemoryViewerState::closed)
            return make_outcome(MemoryViewerError::closed, SAO_RT_IO_ERR_SESSION_CLOSED);
        if (state_ == MemoryViewerState::closing)
            return make_outcome(MemoryViewerError::stale, SAO_STATUS_ERR_CANCELLED);
        if (state_ != MemoryViewerState::ready || !accepting_ || proxy_ == nullptr ||
            binding_.pid == 0U || binding_.start_time_100ns == 0U ||
            binding_.selection_generation == 0U) {
            return make_outcome(MemoryViewerError::detached, SAO_STATUS_ERR_NOT_INITIALIZED);
        }
        if (expected != nullptr && binding_ != *expected)
            return make_outcome(MemoryViewerError::stale, SAO_STATUS_ERR_CANCELLED);
        if (in_flight_ == (std::numeric_limits<std::size_t>::max)())
            return make_outcome(MemoryViewerError::limit, SAO_STATUS_ERR_BUFFER_TOO_SMALL);

        binding = BindingSnapshot{proxy_, binding_, binding_epoch_};
        ++in_flight_;
        return {};
    } catch (...) {
        return make_outcome(MemoryViewerError::io_failure, SAO_RT_IO_ERR_INTERNAL_ERROR);
    }
}

MemoryViewerOutcome MemoryViewerProvider::finish_operation(const BindingSnapshot& binding,
                                                           MemoryViewerOutcome outcome) noexcept {
    try {
        std::lock_guard lock(mutex_);
        if (!binding_current_locked(binding)) {
            outcome = make_outcome(MemoryViewerError::stale, SAO_STATUS_ERR_CANCELLED);
        } else {
            if (!attachment_current_ && outcome.ok()) {
                outcome =
                    make_outcome(MemoryViewerError::detached, SAO_RT_IO_ERR_PROCESS_NOT_ATTACHED);
            }
            last_outcome_ = outcome;
            if (!attachment_current_) {
                proxy_ = nullptr;
                binding_ = {};
                accepting_ = false;
                state_ = MemoryViewerState::detached;
                (void)advance_epoch_locked();
            }
        }
        if (in_flight_ != 0U)
            --in_flight_;
        return outcome;
    } catch (...) {
        return make_outcome(MemoryViewerError::io_failure, SAO_RT_IO_ERR_INTERNAL_ERROR);
    }
}

void MemoryViewerProvider::abandon_operation(const BindingSnapshot& binding) noexcept {
    try {
        std::lock_guard lock(mutex_);
        if (binding_current_locked(binding)) {
            last_outcome_ = make_outcome(MemoryViewerError::io_failure, SAO_STATUS_ERR_UNKNOWN);
            if (!attachment_current_) {
                proxy_ = nullptr;
                binding_ = {};
                accepting_ = false;
                state_ = MemoryViewerState::detached;
                (void)advance_epoch_locked();
            }
        }
        if (in_flight_ != 0U)
            --in_flight_;
    } catch (...) {
    }
}

bool MemoryViewerProvider::binding_current_locked(const BindingSnapshot& binding) const noexcept {
    return state_ == MemoryViewerState::ready && accepting_ && proxy_ == binding.proxy &&
           binding_ == binding.identity && binding_epoch_ == binding.epoch;
}

bool MemoryViewerProvider::advance_epoch_locked() noexcept {
    if (binding_epoch_ == (std::numeric_limits<std::uint64_t>::max)())
        return false;
    ++binding_epoch_;
    return true;
}

} // namespace sao::ai_editor::native
