// SAO Auto — game-agnostic input arbitration.
//
// See `include/sao/ui/input_arbitration.h` for the ABI contract.
//
// Conflict-detection algorithm:
//   * Every `check(vk, source)` call records `source` in the per-VK
//     observed set.  "user" is filtered out — a physical user press is
//     never a plugin conflict.
//   * `get_conflicts` walks the map and returns every VK where the
//     observed-source set has ≥ 2 distinct entries.  This measures
//     *actual* contention (two plugins really did try to fire the
//     same key), not hypothetical clashes (both plugins theoretically
//     handling the key but only one ever using it).

#include "sao/ui/input_arbitration.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct VkState {
    int32_t                            policy = SAO_UI_AUTO_KEY_POLICY_SHARED;
    std::string                        reservation;   // empty → free
    std::unordered_set<std::string>    observed_sources;
};

struct Arbiter {
    std::mutex                                 mutex;
    std::unordered_map<uint32_t, VkState>      table;
};

Arbiter* as_arb(sao_ui_input_arb_handle_t h) {
    return reinterpret_cast<Arbiter*>(h);
}

bool is_user_source(const char* source_utf8) {
    if (source_utf8 == nullptr) return false;
    return std::strcmp(source_utf8, "user") == 0;
}

}  // namespace

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_input_arb_create(
    const SaoUiInputArbConfig* config,
    sao_ui_input_arb_handle_t* out_handle) {
    if (out_handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto* arb = new Arbiter();
    if (config && config->entries && config->count > 0) {
        for (size_t i = 0; i < config->count; ++i) {
            const auto& e = config->entries[i];
            VkState& s = arb->table[e.virtual_key];
            s.policy = e.policy;
        }
    }
    *out_handle = reinterpret_cast<sao_ui_input_arb_handle_t>(arb);
    return SAO_STATUS_OK;
}

extern "C" SAO_UI_API void SAO_UI_CALL sao_ui_input_arb_destroy(
    sao_ui_input_arb_handle_t handle) {
    if (handle == nullptr) return;
    delete as_arb(handle);
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_input_arb_check(
    sao_ui_input_arb_handle_t handle,
    uint32_t                  virtual_key,
    const char*               source_utf8,
    bool*                     allowed_out) {
    if (handle == nullptr || allowed_out == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    Arbiter* a = as_arb(handle);
    std::lock_guard<std::mutex> lock(a->mutex);
    // Look up state — a missing entry means default SHARED and no
    // reservation, so we don't need to insert.
    auto it = a->table.find(virtual_key);
    int32_t     policy       = SAO_UI_AUTO_KEY_POLICY_SHARED;
    std::string reservation;
    if (it != a->table.end()) {
        policy      = it->second.policy;
        reservation = it->second.reservation;
    }
    const bool is_user = is_user_source(source_utf8);
    // Record observation for plugin sources (not user).  We only care
    // about actual contention, so "user" doesn't count as a conflict
    // participant.
    if (source_utf8 && *source_utf8 && !is_user) {
        VkState& s = a->table[virtual_key];  // create if missing
        s.observed_sources.insert(source_utf8);
    }
    // Reservation trumps policy — with a reservation, only the
    // reserving plugin (or user) may fire.
    if (!reservation.empty()) {
        if (is_user) {
            *allowed_out = true;   // physical input never blocked
            return SAO_STATUS_OK;
        }
        if (source_utf8 && reservation == source_utf8) {
            *allowed_out = true;
            return SAO_STATUS_OK;
        }
        *allowed_out = false;
        return SAO_STATUS_OK;
    }
    // Apply policy.
    switch (policy) {
        case SAO_UI_AUTO_KEY_POLICY_SHARED:
            *allowed_out = true;
            return SAO_STATUS_OK;
        case SAO_UI_AUTO_KEY_POLICY_GAME_ONLY:
            *allowed_out = is_user;
            return SAO_STATUS_OK;
        case SAO_UI_AUTO_KEY_POLICY_PLUGIN_ONLY:
            *allowed_out = !is_user && source_utf8 && *source_utf8;
            return SAO_STATUS_OK;
        case SAO_UI_AUTO_KEY_POLICY_BLOCKED:
            // BLOCKED still admits user input — this is a "no
            // automation" gate, not a full lockout.  A caller who
            // wants a true lockout should hook the physical input
            // at a different layer.
            *allowed_out = is_user;
            return SAO_STATUS_OK;
        default:
            *allowed_out = false;
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_input_arb_register_policy(
    sao_ui_input_arb_handle_t handle,
    uint32_t                  virtual_key,
    int32_t                   policy) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (policy != SAO_UI_AUTO_KEY_POLICY_SHARED &&
        policy != SAO_UI_AUTO_KEY_POLICY_GAME_ONLY &&
        policy != SAO_UI_AUTO_KEY_POLICY_PLUGIN_ONLY &&
        policy != SAO_UI_AUTO_KEY_POLICY_BLOCKED) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    Arbiter* a = as_arb(handle);
    std::lock_guard<std::mutex> lock(a->mutex);
    a->table[virtual_key].policy = policy;
    return SAO_STATUS_OK;
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_input_arb_reserve(
    sao_ui_input_arb_handle_t handle,
    uint32_t                  virtual_key,
    const char*               plugin_id_utf8) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    Arbiter* a = as_arb(handle);
    std::lock_guard<std::mutex> lock(a->mutex);
    if (plugin_id_utf8 == nullptr || plugin_id_utf8[0] == 0) {
        // Release reservation (if any).
        auto it = a->table.find(virtual_key);
        if (it != a->table.end()) it->second.reservation.clear();
        return SAO_STATUS_OK;
    }
    a->table[virtual_key].reservation = plugin_id_utf8;
    return SAO_STATUS_OK;
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_input_arb_get_conflicts(
    sao_ui_input_arb_handle_t handle,
    SaoUiInputArbConflict*    conflicts_out,
    size_t                    capacity,
    size_t*                   count_out) {
    if (handle == nullptr || count_out == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    Arbiter* a = as_arb(handle);
    std::lock_guard<std::mutex> lock(a->mutex);
    // Collect VKs whose observed_sources has size ≥ 2 (plural-plugin
    // contention).  Deterministic ordering — sorted by VK.
    std::vector<uint32_t> vks;
    vks.reserve(a->table.size());
    for (auto& kv : a->table) {
        if (kv.second.observed_sources.size() >= 2) {
            vks.push_back(kv.first);
        }
    }
    std::sort(vks.begin(), vks.end());
    if (conflicts_out == nullptr || capacity == 0) {
        *count_out = vks.size();
        return SAO_STATUS_OK;
    }
    size_t written = 0;
    for (uint32_t vk : vks) {
        if (written >= capacity) break;
        auto& s = a->table[vk];
        SaoUiInputArbConflict& c = conflicts_out[written++];
        c.virtual_key            = vk;
        c.observed_source_count  = static_cast<uint32_t>(
            s.observed_sources.size());
        c.policy                 = s.policy;
        c.reserved               = s.reservation.empty() ? 0u : 1u;
    }
    *count_out = written;
    return SAO_STATUS_OK;
}
