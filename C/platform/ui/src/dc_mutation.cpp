// SAO Auto — serialized, generation-aware display-context mutations.
//
// Legacy set_window_rect/set_bounds mutations execute on the HWND owner thread.
// Every tagWND mutation executes directly on the coordinator worker through a
// copied physical provider table. A dispatch is recorded only after the
// selected executor reports success.

#include "sao/ui/dc_mutation.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

// (hwnd, generation, operation) — the coalescing key.
struct MutationKey {
    uintptr_t hwnd;
    uint64_t generation;
    std::string op;

    bool operator==(const MutationKey& o) const noexcept {
        return hwnd == o.hwnd && generation == o.generation && op == o.op;
    }
};

struct MutationKeyHash {
    size_t operator()(const MutationKey& k) const noexcept {
        size_t h = std::hash<uintptr_t>{}(k.hwnd);
        h = h * 131u + std::hash<uint64_t>{}(k.generation);
        h = h * 131u + std::hash<std::string>{}(k.op);
        return h;
    }
};

struct GenerationKey {
    uintptr_t hwnd;
    uint64_t generation;

    bool operator==(const GenerationKey& other) const noexcept {
        return hwnd == other.hwnd && generation == other.generation;
    }
};

struct GenerationKeyHash {
    size_t operator()(const GenerationKey& key) const noexcept {
        size_t hash = std::hash<uintptr_t>{}(key.hwnd);
        return hash * 131u + std::hash<uint64_t>{}(key.generation);
    }
};

struct Token {
    uintptr_t hwnd;
    uint64_t generation;
    uint32_t process_id;
    uint32_t thread_id;
};

enum class MutationKind : uint8_t {
    kSetBounds,
    kHideExstyle,
    kHideWindowRect,
    kUnlinkZOrder,
};

struct MutationPayload {
    MutationKind kind = MutationKind::kSetBounds;
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
    uint32_t mask = 0;
    SaoUiDcMutationRect fake_rect{};
    uint32_t settle_ms = 0;
    uint32_t timeout_ms = 0;
};

struct Mutation {
    Token token;
    MutationKey key;
    std::string method_name;
    std::string args_json;
    MutationPayload payload;
};

struct FailedGeneration {
    uint64_t generation = 0;
    std::optional<std::pair<uint32_t, uint32_t>> identity;
};

#if defined(_WIN32)
struct Coordinator;

struct OwnerMutationRequest {
    OwnerMutationRequest(std::shared_ptr<Coordinator> owner, const Mutation& value)
        : coordinator(std::move(owner)), mutation(value),
          hwnd(reinterpret_cast<HWND>(value.key.hwnd)), owner_thread_id(value.token.thread_id) {}

    std::shared_ptr<Coordinator> coordinator;
    Mutation mutation;
    HWND hwnd;
    DWORD owner_thread_id;
    std::atomic<sao_status_t> status{SAO_STATUS_ERR_OS_CALL_FAILED};
    std::atomic_bool started{false};
    std::atomic_bool executed{false};
};

LRESULT CALLBACK owner_mutation_hook(int code, WPARAM wparam, LPARAM lparam);

std::mutex& owner_request_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<uint64_t, std::shared_ptr<OwnerMutationRequest>>& owner_requests() {
    static std::unordered_map<uint64_t, std::shared_ptr<OwnerMutationRequest>> requests;
    return requests;
}

uint64_t next_owner_request_id() {
    static std::atomic<uint64_t> sequence{1};
    return sequence.fetch_add(1, std::memory_order_relaxed);
}

UINT owner_mutation_message() {
    static const UINT message = ::RegisterWindowMessageW(L"SAO.UI.DcMutation.OwnerTransaction.v1");
    return message;
}
#endif

struct Barrier {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    bool confirmed = false;
};

struct DispatchRecord {
    uintptr_t hwnd;
    uint64_t generation;
    std::string op;
    std::string method_name;
    std::string args_json;
};

constexpr size_t kMaxOperationBytes = 63;
constexpr size_t kMaxMethodBytes = 63;
constexpr size_t kMaxArgsBytes = 1024;
constexpr auto kAsyncMutationTimeout = std::chrono::milliseconds(500);
constexpr auto kMutationFrameTick = std::chrono::milliseconds(16);
constexpr uint32_t kPhysicalExStyleTimeoutMs = 2000;

std::optional<std::string_view> bounded_string(const char* value, size_t max_bytes) noexcept {
    if (value == nullptr)
        return std::nullopt;
    size_t length = 0;
    while (length <= max_bytes && value[length] != '\0')
        ++length;
    if (length == 0 || length > max_bytes)
        return std::nullopt;
    return std::string_view(value, length);
}

bool json_int32(const nlohmann::json& value, int32_t* out) {
    if (out == nullptr)
        return false;
    if (value.is_number_unsigned()) {
        const uint64_t parsed = value.get<uint64_t>();
        if (parsed > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
            return false;
        }
        *out = static_cast<int32_t>(parsed);
        return true;
    }
    if (value.is_number_integer()) {
        const int64_t parsed = value.get<int64_t>();
        if (parsed < std::numeric_limits<int32_t>::min() ||
            parsed > std::numeric_limits<int32_t>::max()) {
            return false;
        }
        *out = static_cast<int32_t>(parsed);
        return true;
    }
    return false;
}

bool json_uint32(const nlohmann::json& value, uint32_t* out) {
    if (out == nullptr)
        return false;
    if (value.is_number_unsigned()) {
        const uint64_t parsed = value.get<uint64_t>();
        if (parsed > std::numeric_limits<uint32_t>::max())
            return false;
        *out = static_cast<uint32_t>(parsed);
        return true;
    }
    if (value.is_number_integer()) {
        const int64_t parsed = value.get<int64_t>();
        if (parsed < 0 || parsed > std::numeric_limits<uint32_t>::max()) {
            return false;
        }
        *out = static_cast<uint32_t>(parsed);
        return true;
    }
    return false;
}

sao_status_t parse_mutation(std::string_view operation, std::string_view method,
                            const uint8_t* args_json_utf8, size_t args_len,
                            MutationPayload* out_payload) {
    if (out_payload == nullptr || args_json_utf8 == nullptr || args_len == 0 ||
        args_len > kMaxArgsBytes) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    MutationKind kind{};
    if (operation == "host-rect" && (method == "set_window_rect" || method == "set_bounds")) {
        kind = MutationKind::kSetBounds;
    } else if ((operation == "host-exstyle" || operation == "proxy-exstyle") &&
               method == "hide_exstyle") {
        kind = MutationKind::kHideExstyle;
    } else if (operation == "host-z-order" && method == "hide_z_order") {
        kind = MutationKind::kUnlinkZOrder;
    } else {
        return SAO_STATUS_ERR_NOT_IMPLEMENTED;
    }

    try {
        const auto* begin = reinterpret_cast<const char*>(args_json_utf8);
        const auto document = nlohmann::json::parse(begin, begin + args_len, nullptr, false, true);
        if (document.is_discarded() || !document.is_object()) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }

        MutationPayload payload{};
        payload.kind = kind;
        if (kind == MutationKind::kSetBounds) {
            if (document.size() != 4 || !document.contains("x") || !document.contains("y") ||
                !document.contains("width") || !document.contains("height") ||
                !json_int32(document["x"], &payload.x) || !json_int32(document["y"], &payload.y) ||
                !json_int32(document["width"], &payload.width) ||
                !json_int32(document["height"], &payload.height) || payload.width <= 0 ||
                payload.height <= 0) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            const int64_t right = static_cast<int64_t>(payload.x) + payload.width;
            const int64_t bottom = static_cast<int64_t>(payload.y) + payload.height;
            if (right < std::numeric_limits<int32_t>::min() ||
                right > std::numeric_limits<int32_t>::max() ||
                bottom < std::numeric_limits<int32_t>::min() ||
                bottom > std::numeric_limits<int32_t>::max()) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
        } else if (kind == MutationKind::kUnlinkZOrder) {
            // Optional {"timeout_ms":N}; empty object keeps the default
            // physical transaction budget.  No other keys are meaningful.
            payload.timeout_ms = kPhysicalExStyleTimeoutMs;
            for (const auto& item : document.items()) {
                if (item.key() != "timeout_ms" ||
                    !json_uint32(item.value(), &payload.timeout_ms) ||
                    payload.timeout_ms == 0) {
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                }
            }
        } else {
            if (document.size() != 1 || !document.contains("mask") ||
                !json_uint32(document["mask"], &payload.mask) || payload.mask == 0) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            payload.timeout_ms = kPhysicalExStyleTimeoutMs;
        }
        *out_payload = payload;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
}

std::optional<std::pair<uint32_t, uint32_t>>
capture_window_identity(uintptr_t hwnd_value) noexcept {
#if defined(_WIN32)
    const HWND hwnd = reinterpret_cast<HWND>(hwnd_value);
    if (hwnd == nullptr || !::IsWindow(hwnd))
        return std::nullopt;
    DWORD process_id = 0;
    const DWORD thread_id = ::GetWindowThreadProcessId(hwnd, &process_id);
    if (thread_id == 0 || process_id == 0)
        return std::nullopt;
    return std::pair<uint32_t, uint32_t>{process_id, thread_id};
#else
    (void)hwnd_value;
    return std::nullopt;
#endif
}

sao_status_t win32_error_status() noexcept {
#if defined(_WIN32)
    return ::GetLastError() == ERROR_ACCESS_DENIED ? SAO_STATUS_ERR_ACCESS_DENIED
                                                   : SAO_STATUS_ERR_OS_CALL_FAILED;
#else
    return SAO_STATUS_ERR_CAPABILITY_MISSING;
#endif
}

struct Coordinator : std::enable_shared_from_this<Coordinator> {
    // ── Guarded state (cv/mu) ──────────────────────────────────
    std::mutex mu;
    std::condition_variable cv;
    std::unordered_map<MutationKey, Mutation, MutationKeyHash> pending;
    std::deque<MutationKey> queue;
    std::unordered_set<MutationKey, MutationKeyHash> queued;
    std::unordered_map<MutationKey, std::chrono::steady_clock::time_point, MutationKeyHash>
        ready_at;
    std::unordered_map<GenerationKey, uint32_t, GenerationKeyHash> inflight;
    std::unordered_map<uintptr_t, Token> tokens;
    std::unordered_map<uintptr_t, uint64_t> epochs;
    std::unordered_set<uintptr_t> invalidating;
    std::unordered_map<uintptr_t, FailedGeneration> failed;
    uint64_t next_generation = 1;
    uint32_t total_invalidations = 0;
    uint32_t failed_invalidations = 0;
    bool accepting = true;
    bool stop_requested = false;

    // Copied provider table. user_data remains borrowed until shutdown joins
    // the worker and destroy() returns.  unlink_z_order is only populated
    // when the caller supplied a V3 table; V2-era callers leave it null and
    // z-order unlink submissions fail closed NOT_INITIALIZED.
    SaoUiDcMutationProviderV2 mutation_provider{};
    sao_ui_dc_mutation_unlink_z_order_fn_t unlink_z_order = nullptr;

    // ── Diagnostic dispatch log (for tests) ────────────────────
    std::mutex dispatch_mu;
    std::deque<DispatchRecord> dispatch_log;
    size_t failed_dispatches = 0;
    sao_status_t last_dispatch_status = SAO_STATUS_OK;
    std::atomic<uint64_t> worker_thread_id{0};
    std::atomic<uint64_t> owner_execution_thread_id{0};

    // ── Worker thread ──────────────────────────────────────────
    std::thread worker;

    explicit Coordinator(const SaoUiDcMutationProviderV2* provider,
                         sao_ui_dc_mutation_unlink_z_order_fn_t unlink = nullptr) {
        if (provider != nullptr) {
            mutation_provider = *provider;
        }
        unlink_z_order = unlink;
        worker = std::thread([this] { this->run(); });
    }

    ~Coordinator() {
        shutdown();
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> guard(mu);
            accepting = false;
            stop_requested = true;
            pending.clear();
            queue.clear();
            queued.clear();
            ready_at.clear();
            cv.notify_all();
        }
        if (worker.joinable()) {
            worker.join();
        }
    }

    void run() {
#if defined(_WIN32)
        worker_thread_id.store(static_cast<uint64_t>(::GetCurrentThreadId()),
                               std::memory_order_release);
#endif
        while (true) {
            MutationKey key{};
            std::optional<Mutation> task;
            {
                std::unique_lock<std::mutex> lock(mu);
                for (;;) {
                    cv.wait(lock, [this] { return !queue.empty() || stop_requested; });
                    if (queue.empty()) {
                        return;
                    }
                    key = queue.front();
                    const auto ready = ready_at.find(key);
                    if (ready != ready_at.end() && std::chrono::steady_clock::now() < ready->second) {
                        cv.wait_until(lock, ready->second);
                        continue;
                    }
                    queue.pop_front();
                    queued.erase(key);
                    ready_at.erase(key);
                    auto it = pending.find(key);
                    if (it != pending.end()) {
                        task = std::move(it->second);
                        pending.erase(it);
                        inflight[GenerationKey{key.hwnd, key.generation}] += 1u;
                    }
                    break;
                }
            }

            if (task.has_value()) {
                const sao_status_t status = execute(*task);
                std::lock_guard<std::mutex> log_guard(dispatch_mu);
                last_dispatch_status = status;
                if (status == SAO_STATUS_OK) {
                    dispatch_log.push_back({
                        task->key.hwnd,
                        task->key.generation,
                        task->key.op,
                        task->method_name,
                        task->args_json,
                    });
                } else {
                    ++failed_dispatches;
                }
            }

            if (task.has_value()) {
                std::lock_guard<std::mutex> guard(mu);
                const GenerationKey generation_key{key.hwnd, key.generation};
                auto it = inflight.find(generation_key);
                if (it != inflight.end()) {
                    if (it->second > 1u) {
                        it->second -= 1u;
                    } else {
                        inflight.erase(it);
                    }
                }
                cv.notify_all();
            }
        }
    }

    Token register_hwnd(uintptr_t hwnd) {
        std::lock_guard<std::mutex> guard(mu);
        if (!accepting || invalidating.count(hwnd) != 0) {
            return Token{};
        }
        const auto identity = capture_window_identity(hwnd);
        if (!identity.has_value())
            return Token{};
#if defined(_WIN32)
        if (identity->first != static_cast<uint32_t>(::GetCurrentProcessId())) {
            return Token{};
        }
#endif
        auto failed_it = failed.find(hwnd);
        if (failed_it != failed.end()) {
            const GenerationKey failed_key{hwnd, failed_it->second.generation};
            if (inflight.count(failed_key) != 0 || !failed_it->second.identity.has_value() ||
                failed_it->second.identity.value() == identity.value()) {
                return Token{};
            }
            failed.erase(failed_it);
            epochs.erase(hwnd);
        }
        auto current = tokens.find(hwnd);
        if (current != tokens.end() && current->second.process_id == identity->first &&
            current->second.thread_id == identity->second) {
            return current->second;
        }
        const uint64_t gen = next_generation++;
        const Token token{hwnd, gen, identity->first, identity->second};
        tokens[hwnd] = token;
        return token;
    }

    bool clear_failed(uintptr_t hwnd) {
        std::lock_guard<std::mutex> guard(mu);
        auto it = failed.find(hwnd);
        if (it == failed.end())
            return false;
        if (inflight.count(GenerationKey{hwnd, it->second.generation}) != 0)
            return false;
        failed.erase(it);
        epochs.erase(hwnd);
        cv.notify_all();
        return true;
    }

    sao_status_t submit_dc(uintptr_t hwnd, const std::string& op, const std::string& method_name,
                           const std::string& args_json, const MutationPayload& payload,
                           bool requires_rect_provider) {
        std::lock_guard<std::mutex> guard(mu);
        if (!accepting || invalidating.count(hwnd) != 0 || failed.count(hwnd) != 0) {
            return SAO_STATUS_ERR_ACCESS_DENIED;
        }
        auto tok_it = tokens.find(hwnd);
        if (tok_it == tokens.end()) {
            return SAO_STATUS_ERR_ACCESS_DENIED;
        }
        if (requires_rect_provider && mutation_provider.hide_window_rect == nullptr) {
            return SAO_STATUS_ERR_NOT_INITIALIZED;
        }
        if (payload.kind == MutationKind::kHideExstyle &&
            mutation_provider.hide_exstyle == nullptr) {
            return SAO_STATUS_ERR_NOT_INITIALIZED;
        }
        if (payload.kind == MutationKind::kUnlinkZOrder && unlink_z_order == nullptr) {
            return SAO_STATUS_ERR_NOT_INITIALIZED;
        }
        const Token token = tok_it->second;
        const uint64_t generation = token.generation;
        MutationKey key{hwnd, generation, op};
        Mutation task{
            token, key, method_name, args_json, payload,
        };
        // Coalesce: the map replace keeps the "latest" args for this
        // key; the queued flag prevents double-enqueue.
        pending[key] = std::move(task);
        if (queued.insert(key).second) {
            queue.push_back(key);
            ready_at[key] = std::chrono::steady_clock::now() + kMutationFrameTick;
        }
        cv.notify_all();
        return SAO_STATUS_OK;
    }

    bool token_current(const Token& token) {
        const auto identity = capture_window_identity(token.hwnd);
        if (!identity.has_value() || identity->first != token.process_id ||
            identity->second != token.thread_id) {
            return false;
        }
        std::lock_guard<std::mutex> guard(mu);
        const auto it = tokens.find(token.hwnd);
        return invalidating.count(token.hwnd) == 0 && it != tokens.end() &&
               it->second.generation == token.generation &&
               it->second.process_id == token.process_id && it->second.thread_id == token.thread_id;
    }

    sao_status_t execute_set_bounds_on_owner(const Mutation& task) {
#if !defined(_WIN32)
        (void)task;
        return SAO_STATUS_ERR_CAPABILITY_MISSING;
#else
        if (!token_current(task.token))
            return SAO_STATUS_ERR_HANDLE_INVALID;
        const HWND hwnd = reinterpret_cast<HWND>(task.key.hwnd);
        ::SetLastError(ERROR_SUCCESS);
        if (!::SetWindowPos(hwnd, nullptr, task.payload.x, task.payload.y, task.payload.width,
                            task.payload.height, SWP_NOACTIVATE | SWP_NOZORDER)) {
            return win32_error_status();
        }
        RECT rect{};
        if (!::GetWindowRect(hwnd, &rect))
            return win32_error_status();
        return rect.left == task.payload.x && rect.top == task.payload.y &&
                       rect.right - rect.left == task.payload.width &&
                       rect.bottom - rect.top == task.payload.height
                   ? SAO_STATUS_OK
                   : SAO_STATUS_ERR_OS_CALL_FAILED;
#endif
    }

    sao_status_t execute_hide_window_rect_on_worker(const Mutation& task) {
        // Revalidate immediately before entering borrowed provider code. This
        // is intentionally independent of the owner-thread dispatch hook.
        if (!token_current(task.token))
            return SAO_STATUS_ERR_HANDLE_INVALID;
        const auto callback = mutation_provider.hide_window_rect;
        if (callback == nullptr)
            return SAO_STATUS_ERR_NOT_INITIALIZED;
        try {
            return callback(mutation_provider.user_data, reinterpret_cast<void*>(task.key.hwnd),
                            &task.payload.fake_rect, task.payload.settle_ms,
                            task.payload.timeout_ms);
        } catch (...) {
            return SAO_STATUS_ERR_UNKNOWN;
        }
    }

    sao_status_t execute_hide_exstyle_on_worker(const Mutation& task) {
        if (!token_current(task.token))
            return SAO_STATUS_ERR_HANDLE_INVALID;
        const auto callback = mutation_provider.hide_exstyle;
        if (callback == nullptr)
            return SAO_STATUS_ERR_NOT_INITIALIZED;
        try {
            return callback(mutation_provider.user_data, reinterpret_cast<void*>(task.key.hwnd),
                            task.payload.mask, task.payload.timeout_ms);
        } catch (...) {
            return SAO_STATUS_ERR_UNKNOWN;
        }
    }

    sao_status_t execute_unlink_z_order_on_worker(const Mutation& task) {
        // The unlink is a physical tagWND transaction exactly like the
        // ExStyle/rect scrubs: it runs on the mutation worker, never on the
        // owner thread, and revalidates the token before borrowed code.
        if (!token_current(task.token))
            return SAO_STATUS_ERR_HANDLE_INVALID;
        const auto callback = unlink_z_order;
        if (callback == nullptr)
            return SAO_STATUS_ERR_NOT_INITIALIZED;
        try {
            return callback(mutation_provider.user_data, reinterpret_cast<void*>(task.key.hwnd),
                            task.payload.timeout_ms);
        } catch (...) {
            return SAO_STATUS_ERR_UNKNOWN;
        }
    }

    sao_status_t execute_on_owner_thread(const Mutation& task) {
#if defined(_WIN32)
        if (::GetCurrentThreadId() != task.token.thread_id)
            return SAO_STATUS_ERR_ACCESS_DENIED;
        owner_execution_thread_id.store(static_cast<uint64_t>(::GetCurrentThreadId()),
                                        std::memory_order_release);
#endif
        switch (task.payload.kind) {
        case MutationKind::kSetBounds:
            return execute_set_bounds_on_owner(task);
        case MutationKind::kHideExstyle:
            return SAO_STATUS_ERR_NOT_INITIALIZED;
        case MutationKind::kHideWindowRect:
            return SAO_STATUS_ERR_ACCESS_DENIED;
        case MutationKind::kUnlinkZOrder:
            return SAO_STATUS_ERR_ACCESS_DENIED;
        }
        return SAO_STATUS_ERR_NOT_IMPLEMENTED;
    }

    sao_status_t execute(const Mutation& task) {
        if (task.payload.kind == MutationKind::kHideWindowRect)
            return execute_hide_window_rect_on_worker(task);
        if (task.payload.kind == MutationKind::kHideExstyle) {
            return execute_hide_exstyle_on_worker(task);
        }
        if (task.payload.kind == MutationKind::kUnlinkZOrder) {
            return execute_unlink_z_order_on_worker(task);
        }
#if !defined(_WIN32)
        return execute_on_owner_thread(task);
#else
        if (!token_current(task.token))
            return SAO_STATUS_ERR_HANDLE_INVALID;
        if (::GetCurrentThreadId() == task.token.thread_id)
            return execute_on_owner_thread(task);

        const UINT message = owner_mutation_message();
        if (message == 0)
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        HHOOK hook = ::SetWindowsHookExW(WH_CALLWNDPROC, &owner_mutation_hook, nullptr,
                                         task.token.thread_id);
        if (hook == nullptr)
            return win32_error_status();

        std::shared_ptr<OwnerMutationRequest> request;
        try {
            request = std::make_shared<OwnerMutationRequest>(shared_from_this(), task);
        } catch (...) {
            ::UnhookWindowsHookEx(hook);
            return SAO_STATUS_ERR_UNKNOWN;
        }
        const uint64_t request_id = next_owner_request_id();
        try {
            std::lock_guard<std::mutex> lock(owner_request_mutex());
            owner_requests().emplace(request_id, request);
        } catch (...) {
            ::UnhookWindowsHookEx(hook);
            return SAO_STATUS_ERR_UNKNOWN;
        }

        DWORD_PTR ignored_result = 0;
        ::SetLastError(ERROR_SUCCESS);
        const LRESULT sent = ::SendMessageTimeoutW(
            request->hwnd, message, static_cast<WPARAM>(request_id), 0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, static_cast<UINT>(kAsyncMutationTimeout.count()),
            &ignored_result);
        const DWORD send_error = ::GetLastError();
        ::UnhookWindowsHookEx(hook);
        {
            std::lock_guard<std::mutex> lock(owner_request_mutex());
            owner_requests().erase(request_id);
        }
        if (sent == 0) {
            return send_error == ERROR_TIMEOUT || send_error == ERROR_SUCCESS
                       ? SAO_STATUS_ERR_TIMEOUT
                       : (send_error == ERROR_ACCESS_DENIED ? SAO_STATUS_ERR_ACCESS_DENIED
                                                            : SAO_STATUS_ERR_OS_CALL_FAILED);
        }
        return request->executed.load(std::memory_order_acquire)
                   ? request->status.load(std::memory_order_acquire)
                   : SAO_STATUS_ERR_OS_CALL_FAILED;
#endif
    }

    bool invalidate(uintptr_t hwnd, double timeout_sec, Barrier* barrier) {
        uint64_t invalidated_generation = 0;
        // Snapshot + revoke — same as Python `begin_invalidate`.
        {
            std::lock_guard<std::mutex> guard(mu);
            epochs[hwnd] = epochs[hwnd] + 1;
            invalidating.insert(hwnd);
            const auto token_it = tokens.find(hwnd);
            if (token_it != tokens.end()) {
                invalidated_generation = token_it->second.generation;
            } else {
                const auto failed_it = failed.find(hwnd);
                if (failed_it != failed.end())
                    invalidated_generation = failed_it->second.generation;
            }
            tokens.erase(hwnd);
            // Drop any queued entries for this hwnd — but leave
            // inflight to drain naturally.
            std::deque<MutationKey> keep;
            while (!queue.empty()) {
                auto k = queue.front();
                queue.pop_front();
                if (k.hwnd == hwnd) {
                    pending.erase(k);
                    queued.erase(k);
                    ready_at.erase(k);
                    continue;
                }
                keep.push_back(k);
            }
            queue.swap(keep);
            total_invalidations += 1u;
            cv.notify_all();
        }

        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::microseconds(
                timeout_sec > 0.0 ? static_cast<int64_t>(timeout_sec * 1'000'000.0) : 0);

        bool drained = true;
        {
            std::unique_lock<std::mutex> lock(mu);
            auto in_flight_or_pending = [this, hwnd, invalidated_generation] {
                if (invalidated_generation != 0 &&
                    inflight.count(GenerationKey{hwnd, invalidated_generation}) != 0)
                    return true;
                for (auto const& kv : pending) {
                    if (kv.first.hwnd == hwnd && (invalidated_generation == 0 ||
                                                  kv.first.generation == invalidated_generation))
                        return true;
                }
                return false;
            };
            while (in_flight_or_pending()) {
                const auto now = std::chrono::steady_clock::now();
                if (timeout_sec <= 0.0 || now >= deadline) {
                    drained = false;
                    break;
                }
                cv.wait_until(lock, deadline);
            }
        }

        {
            std::lock_guard<std::mutex> guard(mu);
            invalidating.erase(hwnd);
            if (drained) {
                failed.erase(hwnd);
            } else {
                failed[hwnd] = FailedGeneration{
                    invalidated_generation,
                    capture_window_identity(hwnd),
                };
                failed_invalidations += 1u;
            }
            cv.notify_all();
        }

        if (barrier != nullptr) {
            {
                std::lock_guard<std::mutex> guard(barrier->mu);
                barrier->done = true;
                barrier->confirmed = drained;
            }
            barrier->cv.notify_all();
        }
        return drained;
    }

    bool drain_until_empty(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mu);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!queue.empty() || !pending.empty() || !inflight.empty()) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return false;
            cv.wait_until(lock, deadline);
        }
        return true;
    }

    SaoDcMutationStats stats() {
        std::lock_guard<std::mutex> guard(mu);
        SaoDcMutationStats s{};
        s.registered_hwnds = static_cast<uint32_t>(tokens.size());
        uint32_t inflight_ops = 0;
        for (auto const& kv : inflight)
            inflight_ops += kv.second;
        s.inflight_operations = inflight_ops;
        s.queued_operations = static_cast<uint32_t>(pending.size());
        s.total_invalidations = total_invalidations;
        s.total_failed_invalidations = failed_invalidations;
        s.stale_blocks_active = static_cast<uint32_t>(failed.size());
        return s;
    }
};

#if defined(_WIN32)
LRESULT CALLBACK owner_mutation_hook(int code, WPARAM wparam, LPARAM lparam) {
    try {
        if (code >= 0 && lparam != 0) {
            const auto* message = reinterpret_cast<const CWPSTRUCT*>(lparam);
            if (message->message == owner_mutation_message()) {
                std::shared_ptr<OwnerMutationRequest> request;
                {
                    std::lock_guard<std::mutex> lock(owner_request_mutex());
                    const auto it = owner_requests().find(static_cast<uint64_t>(message->wParam));
                    if (it != owner_requests().end())
                        request = it->second;
                }
                bool expected = false;
                if (request != nullptr && request->hwnd == message->hwnd &&
                    request->owner_thread_id == ::GetCurrentThreadId() &&
                    request->started.compare_exchange_strong(expected, true,
                                                             std::memory_order_acq_rel)) {
                    sao_status_t status = SAO_STATUS_ERR_UNKNOWN;
                    try {
                        status = request->coordinator->execute_on_owner_thread(request->mutation);
                    } catch (...) {
                    }
                    request->status.store(status, std::memory_order_release);
                    request->executed.store(true, std::memory_order_release);
                }
            }
        }
    } catch (...) {
    }
    return ::CallNextHookEx(nullptr, code, wparam, lparam);
}
#endif

// ── Handle plumbing ────────────────────────────────────────────
// The ABI hands out opaque struct* pointers that wrap Coordinator /
// Barrier instances.  Placement-new inside a static aligned buffer is
// unnecessary since we own the lifetime with new/delete.
struct CoordinatorHandle {
    std::shared_ptr<Coordinator> coord;
};
struct BarrierHandle {
    Barrier* bar;
};

} // namespace

extern "C" sao_status_t SAO_UI_CALL sao_ui_dc_mutation_coordinator_create_ex(
    const SaoUiDcMutationProvider* provider, sao_ui_dc_mutation_coordinator_handle_t* out_handle) {
    SaoUiDcMutationProviderV2 provider_v2{};
    const SaoUiDcMutationProviderV2* selected_provider = nullptr;
    if (provider != nullptr) {
        provider_v2.struct_size = sizeof(provider_v2);
        provider_v2.hide_window_rect = provider->hide_window_rect;
        provider_v2.user_data = provider->user_data;
        selected_provider = &provider_v2;
    }
    return sao_ui_dc_mutation_coordinator_create_ex_v2(selected_provider, out_handle);
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_dc_mutation_coordinator_create_ex_v2(const SaoUiDcMutationProviderV2* provider,
                                            sao_ui_dc_mutation_coordinator_handle_t* out_handle) {
    if (provider != nullptr &&
        (provider->struct_size != sizeof(SaoUiDcMutationProviderV2) || provider->reserved != 0)) {
        if (out_handle != nullptr)
            *out_handle = nullptr;
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    SaoUiDcMutationProviderV3 provider_v3{};
    const SaoUiDcMutationProviderV3* selected_provider = nullptr;
    if (provider != nullptr) {
        provider_v3.struct_size = sizeof(provider_v3);
        provider_v3.hide_window_rect = provider->hide_window_rect;
        provider_v3.hide_exstyle = provider->hide_exstyle;
        provider_v3.user_data = provider->user_data;
        provider_v3.unlink_z_order = nullptr;
        selected_provider = &provider_v3;
    }
    return sao_ui_dc_mutation_coordinator_create_ex_v3(selected_provider, out_handle);
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_dc_mutation_coordinator_create_ex_v3(const SaoUiDcMutationProviderV3* provider,
                                            sao_ui_dc_mutation_coordinator_handle_t* out_handle) {
    if (out_handle == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_handle = nullptr;
    if (provider != nullptr &&
        (provider->struct_size != sizeof(SaoUiDcMutationProviderV3) || provider->reserved != 0)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        SaoUiDcMutationProviderV2 provider_v2{};
        const SaoUiDcMutationProviderV2* selected_v2 = nullptr;
        sao_ui_dc_mutation_unlink_z_order_fn_t unlink = nullptr;
        if (provider != nullptr) {
            provider_v2.struct_size = sizeof(provider_v2);
            provider_v2.hide_window_rect = provider->hide_window_rect;
            provider_v2.hide_exstyle = provider->hide_exstyle;
            provider_v2.user_data = provider->user_data;
            selected_v2 = &provider_v2;
            unlink = provider->unlink_z_order;
        }
        auto coordinator = std::make_shared<Coordinator>(selected_v2, unlink);
        auto handle = std::make_unique<CoordinatorHandle>();
        handle->coord = std::move(coordinator);
        *out_handle = reinterpret_cast<sao_ui_dc_mutation_coordinator_handle_t>(handle.release());
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_dc_mutation_coordinator_create(sao_ui_dc_mutation_coordinator_handle_t* out_handle) {
    return sao_ui_dc_mutation_coordinator_create_ex_v2(nullptr, out_handle);
}

extern "C" void SAO_UI_CALL
sao_ui_dc_mutation_coordinator_destroy(sao_ui_dc_mutation_coordinator_handle_t handle) {
    if (handle == nullptr)
        return;
    auto* h = reinterpret_cast<CoordinatorHandle*>(handle);
    h->coord->shutdown();
    h->coord.reset();
    delete h;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dc_mutation_coordinator_register(
    sao_ui_dc_mutation_coordinator_handle_t handle, void* hwnd, void** out_token) {
    if (out_token != nullptr)
        *out_token = nullptr;
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (hwnd == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        auto* h = reinterpret_cast<CoordinatorHandle*>(handle);
        const Token t = h->coord->register_hwnd(reinterpret_cast<uintptr_t>(hwnd));
        if (t.generation == 0)
            return SAO_STATUS_ERR_ACCESS_DENIED;
        if (out_token != nullptr) {
            *out_token = reinterpret_cast<void*>(static_cast<uintptr_t>(t.generation));
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dc_mutation_coordinator_submit_dc(
    sao_ui_dc_mutation_coordinator_handle_t handle, void* hwnd, const char* operation_utf8,
    const char* method_name_utf8, const uint8_t* args_json_utf8, size_t args_len) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (hwnd == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if ((args_json_utf8 == nullptr) != (args_len == 0)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const auto operation = bounded_string(operation_utf8, kMaxOperationBytes);
    const auto method = bounded_string(method_name_utf8, kMaxMethodBytes);
    if (!operation.has_value() || !method.has_value()) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    MutationPayload payload{};
    const sao_status_t parse_status =
        parse_mutation(*operation, *method, args_json_utf8, args_len, &payload);
    if (parse_status != SAO_STATUS_OK)
        return parse_status;
#if !defined(_WIN32)
    return SAO_STATUS_ERR_CAPABILITY_MISSING;
#else
    try {
        auto* h = reinterpret_cast<CoordinatorHandle*>(handle);
        const std::string op(*operation);
        const std::string method_name(*method);
        const std::string args_json(reinterpret_cast<const char*>(args_json_utf8), args_len);
        return h->coord->submit_dc(reinterpret_cast<uintptr_t>(hwnd), op, method_name, args_json,
                                   payload, false);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
#endif
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dc_mutation_coordinator_submit_hide_window_rect(
    sao_ui_dc_mutation_coordinator_handle_t handle, void* hwnd,
    const SaoUiDcMutationRect* fake_rect, uint32_t settle_ms, uint32_t timeout_ms) {
    if (handle == nullptr || hwnd == nullptr || fake_rect == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (fake_rect->right <= fake_rect->left || fake_rect->bottom <= fake_rect->top)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        MutationPayload payload{};
        payload.kind = MutationKind::kHideWindowRect;
        payload.fake_rect = *fake_rect;
        payload.settle_ms = settle_ms;
        payload.timeout_ms = timeout_ms;
        const std::string args_json = "{\"left\":" + std::to_string(fake_rect->left) +
                                      ",\"top\":" + std::to_string(fake_rect->top) +
                                      ",\"right\":" + std::to_string(fake_rect->right) +
                                      ",\"bottom\":" + std::to_string(fake_rect->bottom) +
                                      ",\"settle_ms\":" + std::to_string(settle_ms) +
                                      ",\"timeout_ms\":" + std::to_string(timeout_ms) + "}";
        auto* h = reinterpret_cast<CoordinatorHandle*>(handle);
        return h->coord->submit_dc(reinterpret_cast<uintptr_t>(hwnd), "host-rect-scrub",
                                   "hide_window_rect", args_json, payload, true);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dc_mutation_coordinator_submit_unlink_z_order(
    sao_ui_dc_mutation_coordinator_handle_t handle, void* hwnd, uint32_t timeout_ms) {
    if (handle == nullptr || hwnd == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        MutationPayload payload{};
        payload.kind = MutationKind::kUnlinkZOrder;
        payload.timeout_ms = timeout_ms != 0 ? timeout_ms : kPhysicalExStyleTimeoutMs;
        const std::string args_json =
            "{\"timeout_ms\":" + std::to_string(payload.timeout_ms) + "}";
        auto* h = reinterpret_cast<CoordinatorHandle*>(handle);
        return h->coord->submit_dc(reinterpret_cast<uintptr_t>(hwnd), "host-z-order",
                                   "hide_z_order", args_json, payload, false);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" bool SAO_UI_CALL sao_ui_dc_mutation_coordinator_invalidate(
    sao_ui_dc_mutation_coordinator_handle_t handle, void* hwnd, double timeout_sec) {
    if (handle == nullptr || hwnd == nullptr)
        return false;
    auto* h = reinterpret_cast<CoordinatorHandle*>(handle);
    return h->coord->invalidate(reinterpret_cast<uintptr_t>(hwnd), timeout_sec, nullptr);
}

extern "C" bool SAO_UI_CALL sao_ui_dc_mutation_coordinator_clear_failed(
    sao_ui_dc_mutation_coordinator_handle_t handle, void* hwnd) {
    if (handle == nullptr || hwnd == nullptr)
        return false;
    auto* h = reinterpret_cast<CoordinatorHandle*>(handle);
    return h->coord->clear_failed(reinterpret_cast<uintptr_t>(hwnd));
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dc_mutation_barrier_wait(
    sao_ui_dc_mutation_barrier_handle_t barrier, uint32_t wait_ms, bool* out_confirmed) {
    if (out_confirmed != nullptr)
        *out_confirmed = false;
    if (barrier == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto* h = reinterpret_cast<BarrierHandle*>(barrier);
    if (h->bar == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::unique_lock<std::mutex> lock(h->bar->mu);
    if (wait_ms == 0u) {
        if (!h->bar->done)
            return SAO_STATUS_ERR_TIMEOUT;
    } else {
        const bool ok = h->bar->cv.wait_for(lock, std::chrono::milliseconds(wait_ms),
                                            [h] { return h->bar->done; });
        if (!ok)
            return SAO_STATUS_ERR_TIMEOUT;
    }
    if (out_confirmed != nullptr)
        *out_confirmed = h->bar->confirmed;
    return SAO_STATUS_OK;
}

extern "C" bool SAO_UI_CALL
sao_ui_dc_mutation_barrier_done(sao_ui_dc_mutation_barrier_handle_t barrier) {
    if (barrier == nullptr)
        return false;
    auto* h = reinterpret_cast<BarrierHandle*>(barrier);
    if (h->bar == nullptr)
        return false;
    std::lock_guard<std::mutex> guard(h->bar->mu);
    return h->bar->done;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dc_mutation_coordinator_stats(
    sao_ui_dc_mutation_coordinator_handle_t handle, SaoDcMutationStats* out_stats) {
    if (handle == nullptr || out_stats == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto* h = reinterpret_cast<CoordinatorHandle*>(handle);
    *out_stats = h->coord->stats();
    return SAO_STATUS_OK;
}

// ── Mutation-coordinator test-only helpers (SAO_UI_API exports
// them; header declarations live in the individual test files as
// forward externs to avoid churning the public ABI headers) ─────
extern "C" SAO_UI_API size_t SAO_UI_CALL
sao_ui_dc_mut_test_dispatch_count(sao_ui_dc_mutation_coordinator_handle_t handle) {
    if (handle == nullptr)
        return 0;
    auto* h = reinterpret_cast<CoordinatorHandle*>(handle);
    std::lock_guard<std::mutex> guard(h->coord->dispatch_mu);
    return h->coord->dispatch_log.size();
}

extern "C" SAO_UI_API bool SAO_UI_CALL
sao_ui_dc_mut_test_drain(sao_ui_dc_mutation_coordinator_handle_t handle, uint32_t timeout_ms) {
    if (handle == nullptr)
        return false;
    auto* h = reinterpret_cast<CoordinatorHandle*>(handle);
    return h->coord->drain_until_empty(std::chrono::milliseconds(timeout_ms));
}

extern "C" SAO_UI_API bool SAO_UI_CALL sao_ui_dc_mut_test_last_op(
    sao_ui_dc_mutation_coordinator_handle_t handle, char* out_op, size_t out_op_cap,
    char* out_method, size_t out_method_cap, char* out_args, size_t out_args_cap) {
    if (handle == nullptr)
        return false;
    auto* h = reinterpret_cast<CoordinatorHandle*>(handle);
    std::lock_guard<std::mutex> guard(h->coord->dispatch_mu);
    if (h->coord->dispatch_log.empty())
        return false;
    const auto& rec = h->coord->dispatch_log.back();
    auto copy = [](char* dst, size_t cap, const std::string& s) {
        if (dst == nullptr || cap == 0u)
            return;
        const size_t n = s.size() < cap - 1u ? s.size() : cap - 1u;
        std::memcpy(dst, s.data(), n);
        dst[n] = '\0';
    };
    copy(out_op, out_op_cap, rec.op);
    copy(out_method, out_method_cap, rec.method_name);
    copy(out_args, out_args_cap, rec.args_json);
    return true;
}

extern "C" SAO_UI_API size_t SAO_UI_CALL
sao_ui_dc_mut_test_failed_dispatch_count(sao_ui_dc_mutation_coordinator_handle_t handle) {
    if (handle == nullptr)
        return 0;
    auto* h = reinterpret_cast<CoordinatorHandle*>(handle);
    std::lock_guard<std::mutex> guard(h->coord->dispatch_mu);
    return h->coord->failed_dispatches;
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_dc_mut_test_last_dispatch_status(sao_ui_dc_mutation_coordinator_handle_t handle) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    auto* h = reinterpret_cast<CoordinatorHandle*>(handle);
    std::lock_guard<std::mutex> guard(h->coord->dispatch_mu);
    return h->coord->last_dispatch_status;
}

extern "C" SAO_UI_API uint64_t SAO_UI_CALL
sao_ui_dc_mut_test_worker_thread_id(sao_ui_dc_mutation_coordinator_handle_t handle) {
    if (handle == nullptr)
        return 0;
    auto* h = reinterpret_cast<CoordinatorHandle*>(handle);
    return h->coord->worker_thread_id.load(std::memory_order_acquire);
}

extern "C" SAO_UI_API uint64_t SAO_UI_CALL
sao_ui_dc_mut_test_owner_execution_thread_id(sao_ui_dc_mutation_coordinator_handle_t handle) {
    if (handle == nullptr)
        return 0;
    auto* h = reinterpret_cast<CoordinatorHandle*>(handle);
    return h->coord->owner_execution_thread_id.load(std::memory_order_acquire);
}

extern "C" SAO_UI_API size_t SAO_UI_CALL sao_ui_dc_mut_test_owner_request_count(void) {
#if !defined(_WIN32)
    return 0;
#else
    std::lock_guard<std::mutex> lock(owner_request_mutex());
    return owner_requests().size();
#endif
}
