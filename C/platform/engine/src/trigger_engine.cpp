// SAO Auto — platform/engine/src/trigger_engine.cpp
//
// Wave 6 / Phase 5 — game-agnostic trigger engine.
//
// The engine understands six built-in condition families (event_match,
// state_enter, state_leave, history_pattern, timer, combo) and treats
// every ``condition_params`` payload as opaque JSON.  Every semantic
// mapping ("boss buff appears", "DPS crosses 5M/s") is done by the
// plugin that registered the trigger — the engine only pattern-matches
// on the fields declared per family above.
//
// Threading: a mutex protects the registry.  Dispatch keeps shared snapshots
// alive while callbacks run outside the mutex.

#include "sao/engine/trigger_engine.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using json = nlohmann::json;

struct Condition;

// A recursive condition tree.  ``kind`` names the built-in family and
// ``combo_children`` holds the sub-conditions for ``combo``.
struct Condition {
    int32_t kind = 0;

    // event_match
    std::string event_type;
    std::vector<std::string> payload_contains;

    // state_enter / state_leave
    std::string state;

    // history_pattern
    std::vector<std::string> sequence;
    uint64_t window_ms = 0;
    // Rolling per-condition history of matching events; oldest first.
    // Populated inside evaluate; only meaningful for history_pattern.
    std::deque<std::pair<std::string, uint64_t>> history_seen;

    // timer
    uint64_t interval_ms   = 0;
    uint64_t elapsed_ms    = 0;
    bool     one_shot      = false;
    bool     fired         = false;   // one_shot latch

    // combo
    std::vector<std::unique_ptr<Condition>> combo_children;
};

struct TriggerEntry {
    sao_engine_trigger_handle_t handle = 0;
    uint64_t action_id                 = 0;
    std::unique_ptr<Condition> root;
    sao_engine_trigger_callback_t callback = nullptr;
    void* user_data                        = nullptr;
    std::atomic<bool> active{true};
    // Insertion sequence so evaluate/tick reports action_ids in
    // register order deterministically.
    uint64_t insertion_seq = 0;
};

sao_status_t parseCondition(const json& spec, Condition& out);

sao_status_t populateEventMatch(const json& params, Condition& out) {
    auto ev_it = params.find("event_type");
    if (ev_it == params.end() || !ev_it->is_string()) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    out.event_type = ev_it->get<std::string>();
    auto contains_it = params.find("payload_contains");
    if (contains_it != params.end()) {
        if (!contains_it->is_array()) return SAO_STATUS_ERR_INVALID_ARGUMENT;
        for (const auto& item : *contains_it) {
            if (!item.is_string()) return SAO_STATUS_ERR_INVALID_ARGUMENT;
            out.payload_contains.push_back(item.get<std::string>());
        }
    }
    return SAO_STATUS_OK;
}

sao_status_t populateState(const json& params, Condition& out) {
    auto st_it = params.find("state");
    if (st_it == params.end() || !st_it->is_string()) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    out.state = st_it->get<std::string>();
    return SAO_STATUS_OK;
}

sao_status_t populateHistoryPattern(const json& params, Condition& out) {
    auto seq_it = params.find("sequence");
    auto win_it = params.find("window_ms");
    if (seq_it == params.end() || !seq_it->is_array()
        || win_it == params.end() || !win_it->is_number()) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    for (const auto& item : *seq_it) {
        if (!item.is_string()) return SAO_STATUS_ERR_INVALID_ARGUMENT;
        out.sequence.push_back(item.get<std::string>());
    }
    if (out.sequence.empty()) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const double window_raw = win_it->get<double>();
    if (window_raw < 0.0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    out.window_ms = static_cast<uint64_t>(window_raw);
    return SAO_STATUS_OK;
}

sao_status_t populateTimer(const json& params, Condition& out) {
    auto iv_it = params.find("interval_ms");
    if (iv_it == params.end() || !iv_it->is_number()) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    if (iv_it->is_number_unsigned()) {
        out.interval_ms = iv_it->get<uint64_t>();
        if (out.interval_ms == 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    } else if (iv_it->is_number_integer()) {
        const auto interval_raw = iv_it->get<int64_t>();
        if (interval_raw <= 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
        out.interval_ms = static_cast<uint64_t>(interval_raw);
    } else {
        constexpr double kUint64UpperBound = 18446744073709551616.0;
        const double interval_raw = iv_it->get<double>();
        if (!std::isfinite(interval_raw) || interval_raw < 1.0
            || std::trunc(interval_raw) != interval_raw
            || interval_raw >= kUint64UpperBound) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        out.interval_ms = static_cast<uint64_t>(interval_raw);
    }
    auto one_it = params.find("one_shot");
    if (one_it != params.end()) {
        if (!one_it->is_boolean()) return SAO_STATUS_ERR_INVALID_ARGUMENT;
        out.one_shot = one_it->get<bool>();
    }
    return SAO_STATUS_OK;
}

sao_status_t populateCombo(const json& params, Condition& out) {
    auto conds_it = params.find("conditions");
    if (conds_it == params.end() || !conds_it->is_array()) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    for (const auto& child_json : *conds_it) {
        if (!child_json.is_object()) return SAO_STATUS_ERR_INVALID_ARGUMENT;
        auto child = std::make_unique<Condition>();
        auto rc = parseCondition(child_json, *child);
        if (rc != SAO_STATUS_OK) return rc;
        out.combo_children.push_back(std::move(child));
    }
    if (out.combo_children.empty()) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    return SAO_STATUS_OK;
}

sao_status_t parseCondition(const json& spec, Condition& out) {
    auto kind_it = spec.find("condition_type");
    if (kind_it == spec.end() || !kind_it->is_number()) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    out.kind = kind_it->get<int32_t>();

    auto params_it = spec.find("condition_params");
    json params_obj = json::object();
    if (params_it != spec.end()) {
        if (params_it->is_string()) {
            try {
                params_obj = json::parse(params_it->get<std::string>());
            } catch (const json::parse_error&) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
        } else if (params_it->is_object()) {
            params_obj = *params_it;
        } else {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
    }

    switch (out.kind) {
        case SAO_ENGINE_TRIGGER_EVENT_MATCH:
            return populateEventMatch(params_obj, out);
        case SAO_ENGINE_TRIGGER_STATE_ENTER:
        case SAO_ENGINE_TRIGGER_STATE_LEAVE:
            return populateState(params_obj, out);
        case SAO_ENGINE_TRIGGER_HISTORY_PATTERN:
            return populateHistoryPattern(params_obj, out);
        case SAO_ENGINE_TRIGGER_TIMER:
            return populateTimer(params_obj, out);
        case SAO_ENGINE_TRIGGER_COMBO:
            return populateCombo(params_obj, out);
        default:
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
}

bool evalCondition(Condition& c, const SaoEngineTriggerEvent& evt);

bool evalEventMatch(const Condition& c, const SaoEngineTriggerEvent& evt) {
    if (evt.event_type_utf8 == nullptr) return false;
    if (c.event_type != evt.event_type_utf8) return false;
    if (c.payload_contains.empty()) return true;
    const std::string_view payload = evt.payload_utf8 != nullptr
        ? std::string_view(evt.payload_utf8)
        : std::string_view();
    for (const auto& needle : c.payload_contains) {
        if (payload.find(needle) == std::string_view::npos) return false;
    }
    return true;
}

bool evalStateEnter(const Condition& c, const SaoEngineTriggerEvent& evt) {
    if (evt.state_to_utf8 == nullptr) return false;
    return c.state == evt.state_to_utf8;
}

bool evalStateLeave(const Condition& c, const SaoEngineTriggerEvent& evt) {
    if (evt.state_from_utf8 == nullptr) return false;
    return c.state == evt.state_from_utf8;
}

bool evalHistoryPattern(Condition& c, const SaoEngineTriggerEvent& evt) {
    // Only event_type-carrying events feed the history.
    if (evt.event_type_utf8 == nullptr) return false;

    // Record every event and prune outside the window.
    c.history_seen.emplace_back(evt.event_type_utf8, evt.timestamp_ms);
    while (!c.history_seen.empty()) {
        const auto& front = c.history_seen.front();
        if (evt.timestamp_ms - front.second > c.window_ms) {
            c.history_seen.pop_front();
        } else {
            break;
        }
    }

    // Walk the history and try to match the sequence in order.
    if (c.history_seen.size() < c.sequence.size()) return false;
    size_t seq_idx = 0;
    for (const auto& item : c.history_seen) {
        if (item.first == c.sequence[seq_idx]) {
            seq_idx += 1;
            if (seq_idx == c.sequence.size()) return true;
        }
    }
    return false;
}

bool evalCombo(Condition& c, const SaoEngineTriggerEvent& evt) {
    for (auto& child : c.combo_children) {
        if (!evalCondition(*child, evt)) return false;
    }
    return true;
}

bool evalCondition(Condition& c, const SaoEngineTriggerEvent& evt) {
    switch (c.kind) {
        case SAO_ENGINE_TRIGGER_EVENT_MATCH:     return evalEventMatch(c, evt);
        case SAO_ENGINE_TRIGGER_STATE_ENTER:     return evalStateEnter(c, evt);
        case SAO_ENGINE_TRIGGER_STATE_LEAVE:     return evalStateLeave(c, evt);
        case SAO_ENGINE_TRIGGER_HISTORY_PATTERN: return evalHistoryPattern(c, evt);
        // timer never fires from evaluate() — only from tick().
        case SAO_ENGINE_TRIGGER_TIMER:           return false;
        case SAO_ENGINE_TRIGGER_COMBO:           return evalCombo(c, evt);
        default:                                 return false;
    }
}

// Timer advance.  Returns true when the timer fired during this tick.
// A timer with ``one_shot`` fires at most once, then latches.
bool advanceTimers(Condition& c, uint64_t dt_ms) {
    bool fired = false;
    switch (c.kind) {
        case SAO_ENGINE_TRIGGER_TIMER: {
            if (c.one_shot && c.fired) return false;
            c.elapsed_ms += dt_ms;
            while (c.elapsed_ms >= c.interval_ms) {
                fired = true;
                if (c.one_shot) {
                    c.fired = true;
                    c.elapsed_ms = 0;
                    break;
                }
                c.elapsed_ms -= c.interval_ms;
            }
            return fired;
        }
        case SAO_ENGINE_TRIGGER_COMBO: {
            // A combo's tick fires only when every child (including
            // timers) is currently satisfied.  Non-timer children pass
            // through their steady-state answer for this tick — the
            // combo just tracks its timers.
            //
            // For a combo without any timer children, ticking does
            // nothing (evaluate() covers non-time-based combos).
            bool any_timer = false;
            for (auto& child : c.combo_children) {
                if (child->kind == SAO_ENGINE_TRIGGER_TIMER
                    || child->kind == SAO_ENGINE_TRIGGER_COMBO) {
                    any_timer = true;
                    break;
                }
            }
            if (!any_timer) return false;

            bool all_satisfied = true;
            bool timer_progressed = false;
            for (auto& child : c.combo_children) {
                if (child->kind == SAO_ENGINE_TRIGGER_TIMER
                    || child->kind == SAO_ENGINE_TRIGGER_COMBO) {
                    if (advanceTimers(*child, dt_ms)) timer_progressed = true;
                    // Timer combo membership requires the child to have
                    // just fired *this* tick to count as satisfied.
                    if (!timer_progressed) all_satisfied = false;
                } else {
                    // Non-time-based children — combo can't retroactively
                    // check them here.  Assume unsatisfied unless the
                    // caller runs an evaluate() pairing.
                    all_satisfied = false;
                }
            }
            return all_satisfied && timer_progressed;
        }
        default:
            return false;
    }
}

}  // namespace

struct sao_engine_trigger_engine_s {
    mutable std::mutex mtx;
    std::vector<std::shared_ptr<TriggerEntry>> triggers;
    std::atomic<sao_engine_trigger_handle_t> next_handle{1};
    std::atomic<uint64_t> next_seq{1};
};

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_trigger_create(
    sao_engine_trigger_engine_handle_t* out_handle) {
    if (out_handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        *out_handle = new sao_engine_trigger_engine_s{};
    } catch (const std::bad_alloc&) {
        *out_handle = nullptr;
        return SAO_STATUS_ERR_UNKNOWN;
    }
    return SAO_STATUS_OK;
}

extern "C" void SAO_ENGINE_CALL sao_engine_trigger_destroy(
    sao_engine_trigger_engine_handle_t handle) {
    delete handle;
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_trigger_register(
    sao_engine_trigger_engine_handle_t handle,
    const SaoEngineTriggerSpec* spec,
    sao_engine_trigger_callback_t callback,
    void* user_data,
    sao_engine_trigger_handle_t* out_trigger) {
    if (handle == nullptr || spec == nullptr || out_trigger == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_trigger = 0;

    auto entry = std::make_shared<TriggerEntry>();
    entry->root = std::make_unique<Condition>();
    entry->callback = callback;
    entry->user_data = user_data;
    entry->action_id = spec->action_id;

    // Build a synthetic spec object so the recursive parser can also
    // consume combo children uniformly.
    json spec_obj;
    spec_obj["condition_type"] = spec->condition_type;
    if (spec->condition_params_json != nullptr
        && *spec->condition_params_json != '\0') {
        try {
            spec_obj["condition_params"] =
                json::parse(spec->condition_params_json);
        } catch (const json::exception&) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
    } else {
        spec_obj["condition_params"] = json::object();
    }

    auto rc = parseCondition(spec_obj, *entry->root);
    if (rc != SAO_STATUS_OK) return rc;

    entry->handle       = handle->next_handle.fetch_add(1, std::memory_order_relaxed);
    entry->insertion_seq = handle->next_seq.fetch_add(1, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lk(handle->mtx);
        handle->triggers.push_back(std::move(entry));
        *out_trigger = handle->triggers.back()->handle;
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_trigger_unregister(
    sao_engine_trigger_engine_handle_t handle,
    sao_engine_trigger_handle_t trigger) {
    if (handle == nullptr || trigger == 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(handle->mtx);
    auto it = std::find_if(handle->triggers.begin(), handle->triggers.end(),
                           [trigger](const std::shared_ptr<TriggerEntry>& e) {
                               return e && e->handle == trigger;
                           });
    if (it == handle->triggers.end()) return SAO_STATUS_ERR_NOT_FOUND;
    (*it)->active.store(false, std::memory_order_release);
    handle->triggers.erase(it);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_trigger_evaluate(
    sao_engine_trigger_engine_handle_t handle,
    const SaoEngineTriggerEvent* event,
    uint64_t* out_action_ids,
    uint32_t capacity,
    uint32_t* out_count) {
    if (handle == nullptr || event == nullptr || out_count == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_count = 0;

    // Keep every snapshotted entry alive while callbacks execute outside
    // the registry lock.  This closes the unregister-after-check lifetime
    // race while still allowing callbacks to mutate the registry.
    std::vector<std::shared_ptr<TriggerEntry>> snapshot;
    {
        std::lock_guard<std::mutex> lk(handle->mtx);
        snapshot.reserve(handle->triggers.size());
        for (const auto& t : handle->triggers) snapshot.push_back(t);
    }
    // Preserve registration order — evaluate walks the snapshot in
    // registration order (insertion_seq ascending) for deterministic
    // action-id emission.
    std::sort(snapshot.begin(), snapshot.end(),
              [](const auto& a, const auto& b) {
                  return a->insertion_seq < b->insertion_seq;
              });

    uint32_t matched = 0;
    for (const auto& t : snapshot) {
        if (!t->active.load(std::memory_order_acquire)) continue;

        if (!evalCondition(*t->root, *event)) continue;

        matched += 1;
        if (out_action_ids != nullptr && matched <= capacity) {
            out_action_ids[matched - 1] = t->action_id;
        }
        if (t->callback != nullptr) {
            t->callback(t->action_id, event, t->user_data);
        }
    }

    *out_count = matched;
    if (out_action_ids != nullptr && matched > capacity) {
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_trigger_tick(
    sao_engine_trigger_engine_handle_t handle,
    uint64_t dt_ms,
    uint64_t* out_action_ids,
    uint32_t capacity,
    uint32_t* out_count) {
    if (handle == nullptr || out_count == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_count = 0;

    std::vector<std::shared_ptr<TriggerEntry>> snapshot;
    {
        std::lock_guard<std::mutex> lk(handle->mtx);
        snapshot.reserve(handle->triggers.size());
        for (const auto& t : handle->triggers) snapshot.push_back(t);
    }
    std::sort(snapshot.begin(), snapshot.end(),
              [](const auto& a, const auto& b) {
                  return a->insertion_seq < b->insertion_seq;
              });

    uint32_t matched = 0;
    for (const auto& t : snapshot) {
        if (!t->active.load(std::memory_order_acquire)) continue;
        if (!advanceTimers(*t->root, dt_ms)) continue;

        matched += 1;
        if (out_action_ids != nullptr && matched <= capacity) {
            out_action_ids[matched - 1] = t->action_id;
        }
        if (t->callback != nullptr) {
            SaoEngineTriggerEvent tick_evt{};
            tick_evt.event_type_utf8 = "__tick__";
            tick_evt.timestamp_ms    = dt_ms;
            t->callback(t->action_id, &tick_evt, t->user_data);
        }
    }
    *out_count = matched;
    if (out_action_ids != nullptr && matched > capacity) {
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    }
    return SAO_STATUS_OK;
}
