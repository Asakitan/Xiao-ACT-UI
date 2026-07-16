// SAO Auto — dc_mutation.  Wave 6 first slice.
//
// 1:1 with `sao_auto/python/render/dc_mutation_coordinator.py` (399 lines).
//
// Serialized, generation-aware display-context mutations for overlay
// HWNDs.  The Python module owns a worker thread that pops mutation
// tasks from a FIFO queue and coalesces duplicates via a
// (hwnd, generation, operation) key.  This C++ slice ports the same
// state machine — without the `mem_probe._dc` bridge — so callers can
// exercise the coordinator's coalescing / invalidation / stale-block
// contract from tests without any Windows dependency.
//
// ── State-machine mapping ────────────────────────────────────
//   Python                      | C++
//   ---------------------------  ----------------------------
//   DcMutationCoordinator       | Coordinator (below)
//   InvalidationBarrier         | Barrier (below)
//   _tokens[hwnd] -> Token       | tokens_ map, opaque uint64 as
//                                 the "generation" side of the key
//   _pending[key] -> _Mutation   | pending_ map keyed by (hwnd, gen, op)
//   _queue (deque)              | queue_ (std::deque)
//   _inflight[hwnd] -> count    | inflight_ map
//   _registration_epochs        | epochs_
//   _invalidating (set)         | invalidating_ set
//   _failed_invalidations       | failed_ map (identity captured at
//                                 fail time — kept even though our
//                                 stub identity source is zero, the
//                                 semantics of "mismatch → clear" are
//                                 preserved for parity)
//
// ── Stub backend semantics ───────────────────────────────────
//   The Python code calls into a `_dc` module for the actual
//   `register_window` / `revoke_window` / method dispatch.  The C++
//   port keeps the same interface but the default backend is a
//   no-op stub: register() returns a fresh opaque token, submit()
//   drives the caller-supplied callback fn directly.  Callers that
//   need a real DirectComposition binding can subclass the backend
//   later — this Wave 6 first slice covers the coordinator plumbing.

#include "sao/ui/dc_mutation.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {

// (hwnd, generation, operation) — the coalescing key.
struct MutationKey {
    uintptr_t hwnd;
    uint64_t  generation;
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

// Opaque token — hands out a monotonically increasing generation per
// register().  Python has `token.generation` — we alias it 1:1.
struct Token {
    uintptr_t hwnd;
    uint64_t  generation;
};

// One queued mutation — the C++ port keeps the payload as the method
// name + optional JSON args (the ABI signature).  The stub backend
// simply records the last dispatched call, so tests can inspect
// coalescing behaviour without a real _dc bridge.
struct Mutation {
    Token       token;
    MutationKey key;
    std::string method_name;
    std::string args_json;
};

struct Barrier {
    std::mutex              mu;
    std::condition_variable cv;
    bool                    done      = false;
    bool                    confirmed = false;
};

struct DispatchRecord {
    uintptr_t hwnd;
    uint64_t  generation;
    std::string op;
    std::string method_name;
    std::string args_json;
};

struct Coordinator {
    // ── Guarded state (cv/mu) ──────────────────────────────────
    std::mutex              mu;
    std::condition_variable cv;
    std::unordered_map<MutationKey, Mutation, MutationKeyHash> pending;
    std::deque<MutationKey> queue;
    std::unordered_set<MutationKey, MutationKeyHash> queued;
    std::unordered_map<uintptr_t, uint32_t> inflight;
    std::unordered_map<uintptr_t, uint64_t> tokens;      // hwnd -> current generation
    std::unordered_map<uintptr_t, uint64_t> epochs;
    std::unordered_set<uintptr_t>           invalidating;
    // failed_ preserves identity semantics — we store std::nullopt
    // when the identity capture failed, matching Python's None case.
    std::unordered_map<uintptr_t,
                       std::optional<std::pair<uint32_t, uint32_t>>> failed;
    uint64_t                next_generation      = 1;
    uint32_t                total_invalidations  = 0;
    uint32_t                failed_invalidations = 0;
    bool                    accepting            = true;
    bool                    stop_requested       = false;

    // ── Diagnostic dispatch log (for tests) ────────────────────
    std::mutex             dispatch_mu;
    std::deque<DispatchRecord> dispatch_log;

    // ── Worker thread ──────────────────────────────────────────
    std::thread worker;

    Coordinator() {
        worker = std::thread([this] { this->run(); });
    }

    ~Coordinator() {
        {
            std::lock_guard<std::mutex> guard(mu);
            accepting = false;
            stop_requested = true;
            cv.notify_all();
        }
        if (worker.joinable()) {
            worker.join();
        }
    }

    void run() {
        while (true) {
            MutationKey key{};
            std::optional<Mutation> task;
            {
                std::unique_lock<std::mutex> lock(mu);
                cv.wait(lock, [this] {
                    return !queue.empty() ||
                           (stop_requested && pending.empty());
                });
                if (queue.empty()) {
                    // Only exits here when stop_requested and pending
                    // is empty.  All submits after stop are rejected
                    // by `accepting=false`, so the queue never grows.
                    return;
                }
                key = queue.front();
                queue.pop_front();
                queued.erase(key);
                auto it = pending.find(key);
                if (it != pending.end()) {
                    task = std::move(it->second);
                    pending.erase(it);
                }
                inflight[key.hwnd] += 1u;
            }

            if (task.has_value()) {
                DispatchRecord rec{
                    task->key.hwnd,
                    task->key.generation,
                    task->key.op,
                    task->method_name,
                    task->args_json,
                };
                std::lock_guard<std::mutex> log_guard(dispatch_mu);
                dispatch_log.push_back(std::move(rec));
            }

            {
                std::lock_guard<std::mutex> guard(mu);
                auto it = inflight.find(key.hwnd);
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
            return Token{0, 0};
        }
        if (failed.count(hwnd) != 0) {
            // Stub identity source always returns "same" → keep the
            // block active until an explicit clear.  Matches the
            // Python `_is_blocked_by_stale_failure_locked` branch that
            // returns True when identity doesn't change.
            return Token{0, 0};
        }
        const uint64_t gen = next_generation++;
        tokens[hwnd] = gen;
        return Token{hwnd, gen};
    }

    bool clear_failed(uintptr_t hwnd) {
        std::lock_guard<std::mutex> guard(mu);
        auto it = failed.find(hwnd);
        if (it == failed.end()) return false;
        failed.erase(it);
        epochs.erase(hwnd);
        cv.notify_all();
        return true;
    }

    // NOTE: submit_dc dispatches to the stub backend on the worker
    // thread.  The stub simply records the call — no user-provided
    // function pointer is required.  A real backend would subclass
    // Coordinator or thread a function-table through the handle.
    bool submit_dc(uintptr_t hwnd, const std::string& op,
                   const std::string& method_name,
                   const std::string& args_json) {
        std::lock_guard<std::mutex> guard(mu);
        if (!accepting || invalidating.count(hwnd) != 0 ||
            failed.count(hwnd) != 0) {
            return false;
        }
        auto tok_it = tokens.find(hwnd);
        if (tok_it == tokens.end()) {
            return false;
        }
        const uint64_t generation = tok_it->second;
        MutationKey key{hwnd, generation, op};
        Mutation task{
            Token{hwnd, generation},
            key,
            method_name,
            args_json,
        };
        // Coalesce: the map replace keeps the "latest" args for this
        // key; the queued flag prevents double-enqueue.
        pending[key] = std::move(task);
        if (queued.insert(key).second) {
            queue.push_back(key);
        }
        cv.notify_all();
        return true;
    }

    bool invalidate(uintptr_t hwnd, double timeout_sec, Barrier* barrier) {
        // Snapshot + revoke — same as Python `begin_invalidate`.
        {
            std::lock_guard<std::mutex> guard(mu);
            epochs[hwnd] = epochs[hwnd] + 1;
            invalidating.insert(hwnd);
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
                    continue;
                }
                keep.push_back(k);
            }
            queue.swap(keep);
            total_invalidations += 1u;
            cv.notify_all();
        }

        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::microseconds(
                timeout_sec > 0.0 ?
                static_cast<int64_t>(timeout_sec * 1'000'000.0) : 0);

        bool drained = true;
        {
            std::unique_lock<std::mutex> lock(mu);
            auto in_flight_or_pending = [this, hwnd] {
                if (inflight.count(hwnd) != 0) return true;
                for (auto const& kv : pending) {
                    if (kv.first.hwnd == hwnd) return true;
                }
                return false;
            };
            while (in_flight_or_pending()) {
                if (timeout_sec <= 0.0) {
                    cv.wait(lock);
                    continue;
                }
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) {
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
                // Same as Python `_record_failed_locked` — mark hwnd
                // as failed with a zero identity (no user32 available
                // in this slice).
                failed[hwnd] = std::optional<std::pair<uint32_t, uint32_t>>{};
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
        while (!pending.empty() || !inflight.empty()) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return false;
            cv.wait_until(lock, deadline);
        }
        return true;
    }

    SaoDcMutationStats stats() {
        std::lock_guard<std::mutex> guard(mu);
        SaoDcMutationStats s{};
        s.registered_hwnds = static_cast<uint32_t>(tokens.size());
        uint32_t inflight_ops = 0;
        for (auto const& kv : inflight) inflight_ops += kv.second;
        s.inflight_operations = inflight_ops;
        s.queued_operations = static_cast<uint32_t>(pending.size());
        s.total_invalidations = total_invalidations;
        s.total_failed_invalidations = failed_invalidations;
        s.stale_blocks_active = static_cast<uint32_t>(failed.size());
        return s;
    }
};

// ── Handle plumbing ────────────────────────────────────────────
// The ABI hands out opaque struct* pointers that wrap Coordinator /
// Barrier instances.  Placement-new inside a static aligned buffer is
// unnecessary since we own the lifetime with new/delete.
struct CoordinatorHandle { Coordinator* coord; };
struct BarrierHandle { Barrier* bar; };

}  // namespace

extern "C" sao_status_t SAO_UI_CALL sao_ui_dc_mutation_coordinator_create(
    sao_ui_dc_mutation_coordinator_handle_t* out_handle) {
    if (out_handle == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto* h = new CoordinatorHandle{new Coordinator()};
    *out_handle = reinterpret_cast<sao_ui_dc_mutation_coordinator_handle_t>(h);
    return SAO_STATUS_OK;
}

extern "C" void SAO_UI_CALL sao_ui_dc_mutation_coordinator_destroy(
    sao_ui_dc_mutation_coordinator_handle_t handle) {
    if (handle == nullptr) return;
    auto* h = reinterpret_cast<CoordinatorHandle*>(handle);
    delete h->coord;
    delete h;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dc_mutation_coordinator_register(
    sao_ui_dc_mutation_coordinator_handle_t handle,
    void* hwnd,
    void** out_token) {
    if (out_token != nullptr) *out_token = nullptr;
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (hwnd == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto* h = reinterpret_cast<CoordinatorHandle*>(handle);
    const Token t = h->coord->register_hwnd(reinterpret_cast<uintptr_t>(hwnd));
    if (t.generation == 0) {
        return SAO_STATUS_ERR_ACCESS_DENIED;
    }
    if (out_token != nullptr) {
        // Encode (hwnd, generation) into an opaque uintptr_t.  Callers
        // are told the value is opaque so we just need something unique
        // per (hwnd, generation) pair.  We use `generation` directly
        // because it is monotonic across the process.
        *out_token = reinterpret_cast<void*>(
            static_cast<uintptr_t>(t.generation));
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dc_mutation_coordinator_submit_dc(
    sao_ui_dc_mutation_coordinator_handle_t handle,
    void* hwnd,
    const char* operation_utf8,
    const char* method_name_utf8,
    const uint8_t* args_json_utf8,
    size_t args_len) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (hwnd == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (operation_utf8 == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (method_name_utf8 == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto* h = reinterpret_cast<CoordinatorHandle*>(handle);
    std::string op(operation_utf8);
    std::string method(method_name_utf8);
    std::string args_json;
    if (args_json_utf8 != nullptr && args_len > 0u) {
        args_json.assign(reinterpret_cast<const char*>(args_json_utf8),
                         args_len);
    }
    const bool ok = h->coord->submit_dc(
        reinterpret_cast<uintptr_t>(hwnd), op, method, args_json);
    return ok ? SAO_STATUS_OK : SAO_STATUS_ERR_ACCESS_DENIED;
}

extern "C" bool SAO_UI_CALL sao_ui_dc_mutation_coordinator_invalidate(
    sao_ui_dc_mutation_coordinator_handle_t handle,
    void* hwnd,
    double timeout_sec) {
    if (handle == nullptr || hwnd == nullptr) return false;
    auto* h = reinterpret_cast<CoordinatorHandle*>(handle);
    return h->coord->invalidate(
        reinterpret_cast<uintptr_t>(hwnd), timeout_sec, nullptr);
}

extern "C" bool SAO_UI_CALL sao_ui_dc_mutation_coordinator_clear_failed(
    sao_ui_dc_mutation_coordinator_handle_t handle,
    void* hwnd) {
    if (handle == nullptr || hwnd == nullptr) return false;
    auto* h = reinterpret_cast<CoordinatorHandle*>(handle);
    return h->coord->clear_failed(reinterpret_cast<uintptr_t>(hwnd));
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dc_mutation_barrier_wait(
    sao_ui_dc_mutation_barrier_handle_t barrier,
    uint32_t wait_ms,
    bool* out_confirmed) {
    if (out_confirmed != nullptr) *out_confirmed = false;
    if (barrier == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto* h = reinterpret_cast<BarrierHandle*>(barrier);
    if (h->bar == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::unique_lock<std::mutex> lock(h->bar->mu);
    if (wait_ms == 0u) {
        if (!h->bar->done) return SAO_STATUS_ERR_TIMEOUT;
    } else {
        const bool ok = h->bar->cv.wait_for(
            lock, std::chrono::milliseconds(wait_ms),
            [h] { return h->bar->done; });
        if (!ok) return SAO_STATUS_ERR_TIMEOUT;
    }
    if (out_confirmed != nullptr) *out_confirmed = h->bar->confirmed;
    return SAO_STATUS_OK;
}

extern "C" bool SAO_UI_CALL sao_ui_dc_mutation_barrier_done(
    sao_ui_dc_mutation_barrier_handle_t barrier) {
    if (barrier == nullptr) return false;
    auto* h = reinterpret_cast<BarrierHandle*>(barrier);
    if (h->bar == nullptr) return false;
    std::lock_guard<std::mutex> guard(h->bar->mu);
    return h->bar->done;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_dc_mutation_coordinator_stats(
    sao_ui_dc_mutation_coordinator_handle_t handle,
    SaoDcMutationStats* out_stats) {
    if (handle == nullptr || out_stats == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto* h = reinterpret_cast<CoordinatorHandle*>(handle);
    *out_stats = h->coord->stats();
    return SAO_STATUS_OK;
}

// ── Wave 6 test-only helpers (marked SAO_UI_API so the DLL exports
// them; header declarations live in the individual test files as
// forward externs to avoid churning the public ABI headers) ─────
extern "C" SAO_UI_API size_t SAO_UI_CALL sao_ui_dc_mut_test_dispatch_count(
    sao_ui_dc_mutation_coordinator_handle_t handle) {
    if (handle == nullptr) return 0;
    auto* h = reinterpret_cast<CoordinatorHandle*>(handle);
    std::lock_guard<std::mutex> guard(h->coord->dispatch_mu);
    return h->coord->dispatch_log.size();
}

extern "C" SAO_UI_API bool SAO_UI_CALL sao_ui_dc_mut_test_drain(
    sao_ui_dc_mutation_coordinator_handle_t handle,
    uint32_t timeout_ms) {
    if (handle == nullptr) return false;
    auto* h = reinterpret_cast<CoordinatorHandle*>(handle);
    return h->coord->drain_until_empty(std::chrono::milliseconds(timeout_ms));
}

extern "C" SAO_UI_API bool SAO_UI_CALL sao_ui_dc_mut_test_last_op(
    sao_ui_dc_mutation_coordinator_handle_t handle,
    char* out_op, size_t out_op_cap,
    char* out_method, size_t out_method_cap,
    char* out_args, size_t out_args_cap) {
    if (handle == nullptr) return false;
    auto* h = reinterpret_cast<CoordinatorHandle*>(handle);
    std::lock_guard<std::mutex> guard(h->coord->dispatch_mu);
    if (h->coord->dispatch_log.empty()) return false;
    const auto& rec = h->coord->dispatch_log.back();
    auto copy = [](char* dst, size_t cap, const std::string& s) {
        if (dst == nullptr || cap == 0u) return;
        const size_t n = s.size() < cap - 1u ? s.size() : cap - 1u;
        std::memcpy(dst, s.data(), n);
        dst[n] = '\0';
    };
    copy(out_op, out_op_cap, rec.op);
    copy(out_method, out_method_cap, rec.method_name);
    copy(out_args, out_args_cap, rec.args_json);
    return true;
}
