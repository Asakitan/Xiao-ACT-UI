// SAO Auto — platform/engine/src/state_machine.cpp
//
// Wave 6 / Phase 5 — generic state machine.
//
// The engine layer never hard-codes any game concept.  All state names,
// transition events and initial state come in through a JSON config
// blob at create-time — the state machine only enforces the graph
// declared by the caller.
//
// Threading: a single ``std::mutex`` guards state + history; both are
// cheap operations so a shared lock would be overkill.

#include "sao/engine/state.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using json = nlohmann::json;

struct StoredTransition {
    std::string from;
    std::string to;
    std::string event;
    uint64_t    timestamp_ns = 0;
};

// Match table: (state, event) -> destination state.
struct TransitionKey {
    std::string state;
    std::string event;

    bool operator==(const TransitionKey& other) const {
        return state == other.state && event == other.event;
    }
};

struct TransitionKeyHash {
    size_t operator()(const TransitionKey& key) const noexcept {
        // FNV-1a on state + '\0' + event.  Sequential fold is stable
        // across STL implementations, matches the ``sao_core`` helper.
        constexpr uint64_t k_prime = 1099511628211ull;
        uint64_t hash = 1469598103934665603ull;
        for (char c : key.state) {
            hash ^= static_cast<uint8_t>(c);
            hash *= k_prime;
        }
        hash ^= 0u;
        hash *= k_prime;
        for (char c : key.event) {
            hash ^= static_cast<uint8_t>(c);
            hash *= k_prime;
        }
        return static_cast<size_t>(hash);
    }
};

// History cap is fixed — matches the Python primitive's default so
// consumers see the same ceiling.  Callers who need more can copy the
// history into a HistoryRing.
constexpr size_t k_history_cap = 256;

// Total buffer length required to serialise ``entries`` names into the
// history-out buffer.
size_t computeBufferBytes(const std::vector<StoredTransition>& entries) {
    size_t bytes = 0;
    for (const auto& e : entries) {
        bytes += e.from.size();
        bytes += e.to.size();
        bytes += e.event.size();
    }
    return bytes;
}

// Copy ``value`` into a nul-terminated output buffer.  Returns
// SAO_STATUS_ERR_BUFFER_TOO_SMALL when ``capacity`` is 0 or too small
// for value + '\0'.  When ``out_state_utf8`` is nullptr the check is
// skipped (caller only wants to know if the operation would have
// succeeded).
sao_status_t writeStateName(const std::string& value, char* out, size_t capacity) {
    if (out == nullptr) return SAO_STATUS_OK;
    if (capacity == 0) return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    const size_t required = value.size() + 1;
    if (capacity < required) return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    std::memcpy(out, value.data(), value.size());
    out[value.size()] = '\0';
    return SAO_STATUS_OK;
}

}  // namespace

struct sao_engine_state_machine_s {
    mutable std::mutex mtx;

    std::unordered_set<std::string> states;
    std::unordered_map<TransitionKey, std::string, TransitionKeyHash> transitions;

    std::string initial;
    std::string current;
    std::deque<StoredTransition> history;
};

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_state_machine_create(
    const char* config_json_utf8,
    sao_engine_state_machine_handle_t* out_handle) {
    if (out_handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    if (config_json_utf8 == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;

    json parsed;
    try {
        parsed = json::parse(config_json_utf8);
    } catch (const json::parse_error&) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (!parsed.is_object()) return SAO_STATUS_ERR_INVALID_ARGUMENT;

    auto sm_owner = std::make_unique<sao_engine_state_machine_s>();
    auto& sm = *sm_owner;

    // ── states ─────────────────────────────────────────────────────
    auto states_it = parsed.find("states");
    if (states_it == parsed.end() || !states_it->is_array()) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    for (const auto& state : *states_it) {
        if (!state.is_string()) return SAO_STATUS_ERR_INVALID_ARGUMENT;
        sm.states.insert(state.get<std::string>());
    }
    if (sm.states.empty()) return SAO_STATUS_ERR_INVALID_ARGUMENT;

    // ── initial ────────────────────────────────────────────────────
    auto initial_it = parsed.find("initial");
    if (initial_it == parsed.end() || !initial_it->is_string()) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    sm.initial = initial_it->get<std::string>();
    if (sm.states.find(sm.initial) == sm.states.end()) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    sm.current = sm.initial;

    // ── transitions ────────────────────────────────────────────────
    auto trans_it = parsed.find("transitions");
    if (trans_it != parsed.end()) {
        if (!trans_it->is_array()) return SAO_STATUS_ERR_INVALID_ARGUMENT;
        for (const auto& edge : *trans_it) {
            if (!edge.is_object()) return SAO_STATUS_ERR_INVALID_ARGUMENT;
            auto from_it = edge.find("from");
            auto to_it   = edge.find("to");
            auto on_it   = edge.find("on");
            if (from_it == edge.end() || !from_it->is_string()
                || to_it == edge.end() || !to_it->is_string()
                || on_it == edge.end() || !on_it->is_array()) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            std::string from_name = from_it->get<std::string>();
            std::string to_name   = to_it->get<std::string>();
            if (sm.states.find(from_name) == sm.states.end()
                || sm.states.find(to_name) == sm.states.end()) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            for (const auto& evt : *on_it) {
                if (!evt.is_string()) return SAO_STATUS_ERR_INVALID_ARGUMENT;
                TransitionKey key{from_name, evt.get<std::string>()};
                // First edge wins on collision — matches the Python
                // primitive's insertion-order semantics.
                sm.transitions.emplace(std::move(key), to_name);
            }
        }
    }

    *out_handle = sm_owner.release();
    return SAO_STATUS_OK;
}

extern "C" void SAO_ENGINE_CALL sao_engine_state_machine_destroy(
    sao_engine_state_machine_handle_t handle) {
    delete handle;
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_state_machine_dispatch(
    sao_engine_state_machine_handle_t handle,
    const char* event_utf8,
    uint64_t timestamp_ns,
    char* out_state_utf8,
    size_t state_capacity) {
    if (handle == nullptr || event_utf8 == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lk(handle->mtx);

    TransitionKey key{handle->current, event_utf8};
    auto it = handle->transitions.find(key);
    if (it == handle->transitions.end()) {
        // Invalid transition — expose the *unchanged* state so callers
        // can update UI without a second get_current() call.
        writeStateName(handle->current, out_state_utf8, state_capacity);
        return SAO_STATUS_ERR_INVALID_TRANSITION;
    }

    const std::string previous = handle->current;
    handle->current = it->second;

    StoredTransition record;
    record.from        = previous;
    record.to          = handle->current;
    record.event       = event_utf8;
    record.timestamp_ns = timestamp_ns;
    handle->history.push_back(std::move(record));
    while (handle->history.size() > k_history_cap) {
        handle->history.pop_front();
    }

    return writeStateName(handle->current, out_state_utf8, state_capacity);
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_state_machine_get_current(
    sao_engine_state_machine_handle_t handle,
    char* out_state_utf8,
    size_t state_capacity) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(handle->mtx);
    return writeStateName(handle->current, out_state_utf8, state_capacity);
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_state_machine_get_history(
    sao_engine_state_machine_handle_t handle,
    uint32_t max_entries,
    SaoEngineStateTransition* out_entries,
    size_t entries_capacity,
    char* out_buffer,
    size_t buffer_capacity,
    uint32_t* out_entry_count,
    size_t* out_buffer_used) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (out_entry_count == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;

    std::lock_guard<std::mutex> lk(handle->mtx);

    const size_t total = handle->history.size();
    const size_t want = (max_entries == 0)
        ? total
        : std::min<size_t>(static_cast<size_t>(max_entries), total);

    // Take the *last* ``want`` entries (chronological order).
    std::vector<StoredTransition> slice;
    slice.reserve(want);
    for (size_t i = total - want; i < total; ++i) {
        slice.push_back(handle->history[i]);
    }

    *out_entry_count = static_cast<uint32_t>(slice.size());

    // Compute buffer usage so callers can size a second call correctly.
    const size_t needed_bytes = computeBufferBytes(slice);
    if (out_buffer_used != nullptr) *out_buffer_used = needed_bytes;

    // Query-only call: report counts and stop.
    if (out_entries == nullptr && out_buffer == nullptr) {
        return SAO_STATUS_OK;
    }

    if (out_entries == nullptr || entries_capacity < slice.size()) {
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    }
    if (out_buffer == nullptr || buffer_capacity < needed_bytes) {
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    }

    size_t cursor = 0;
    for (size_t i = 0; i < slice.size(); ++i) {
        const auto& src = slice[i];
        auto& dst = out_entries[i];
        dst.from_offset   = static_cast<uint32_t>(cursor);
        dst.from_length   = static_cast<uint32_t>(src.from.size());
        std::memcpy(out_buffer + cursor, src.from.data(), src.from.size());
        cursor += src.from.size();

        dst.to_offset     = static_cast<uint32_t>(cursor);
        dst.to_length     = static_cast<uint32_t>(src.to.size());
        std::memcpy(out_buffer + cursor, src.to.data(), src.to.size());
        cursor += src.to.size();

        dst.event_offset  = static_cast<uint32_t>(cursor);
        dst.event_length  = static_cast<uint32_t>(src.event.size());
        std::memcpy(out_buffer + cursor, src.event.data(), src.event.size());
        cursor += src.event.size();

        dst.timestamp_ns = src.timestamp_ns;
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_state_machine_reset(
    sao_engine_state_machine_handle_t handle) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(handle->mtx);
    handle->current = handle->initial;
    handle->history.clear();
    return SAO_STATUS_OK;
}
