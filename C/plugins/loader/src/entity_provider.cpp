#include "sao/plugins/loader/entity_provider.h"

#include "entity_provider_internal.h"
#include "plugin_internal.h"

#include "sao/plugins/loader/loader_status.h"
#include "sao_plugins/sao_status.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sao::plugins::loader {

struct entity_provider_state {
    std::shared_ptr<plugin_handle_s> owner;
    std::string provider_id;
    std::string owner_plugin_id;
    uint64_t generation = 0;
    entity_snapshot_callback_fn snapshot = nullptr;
    entity_action_handler_fn action_handler = nullptr;
    void* user_data = nullptr;
    bool has_root_contribution = false;
    std::string contribution_id;
    std::string root_id;
    std::string root_name;
    std::string root_icon;
    double root_priority = 0.0;
    std::mutex mutex;
    std::mutex callback_mutex;
    std::condition_variable idle;
    bool accepting = false;
    bool published = false;
    bool destroyed = false;
    size_t in_flight = 0;
};

namespace {

constexpr uint32_t kMaximumProviderRows = 4096;
constexpr size_t kMaximumProviderIdBytes = 1024;
constexpr size_t kMaximumRootIdBytes = 63;
constexpr size_t kMaximumStringBytes = 16384;
constexpr size_t kMaximumSnapshotStringBytes = 8 * 1024 * 1024;
constexpr size_t kMaximumCatalogProviders = 4096;
constexpr size_t kMaximumCatalogRows = 16384;
constexpr size_t kMaximumCatalogStringBytes = 32 * 1024 * 1024;
constexpr size_t kMaximumInvokePayloadBytes = 1024 * 1024;
constexpr auto kRundownTimeout = std::chrono::seconds(5);
constexpr uint32_t kMaximumSnapshotAttempts = 3;

struct owned_entity_row {
    std::string category_id;
    std::string category_label;
    std::string category_icon;
    double category_priority = 0.0;
    std::string row_label;
    std::string row_icon;
    std::string action_id;
    std::string payload_json;
    bool can_activate = true;
    bool keep_menu_open = false;
    bool close_menu_before = false;
};

struct owned_provider_snapshot {
    std::string provider_id;
    std::string owner_plugin_id;
    uint64_t generation = 0;
    uint64_t revision = 0;
    size_t string_bytes = 0;
    std::vector<owned_entity_row> rows;
    bool has_root_contribution = false;
    std::string contribution_id;
    std::string root_id;
    std::string root_name;
    std::string root_icon;
    double root_priority = 0.0;
};

std::mutex g_catalog_mutex;
std::unordered_map<std::string, std::shared_ptr<entity_provider_state>> g_catalog;
std::unordered_map<std::string, std::shared_ptr<entity_provider_state>> g_attached;
uint64_t g_catalog_revision = 0;
std::atomic_uint64_t g_next_generation{1};
thread_local std::vector<const entity_provider_state*> g_current_providers;

class provider_lease final {
  public:
    provider_lease() noexcept = default;
    ~provider_lease() noexcept {
        release();
    }

    provider_lease(const provider_lease&) = delete;
    provider_lease& operator=(const provider_lease&) = delete;

    int32_t acquire(const std::shared_ptr<entity_provider_state>& state) noexcept {
        if (state == nullptr)
            return SAO_ERR_HANDLE_INVALID;
        if (std::find(g_current_providers.begin(), g_current_providers.end(), state.get()) !=
            g_current_providers.end()) {
            return SAO_PLUGINS_ERR_BUSY;
        }
        try {
            {
                std::lock_guard lock(state->mutex);
                if (!state->accepting || state->destroyed) {
                    return SAO_PLUGINS_ERR_BUSY;
                }
                g_current_providers.push_back(state.get());
                ++state->in_flight;
                state_ = state;
            }
            callback_lock_ = std::unique_lock(state->callback_mutex);
            return SAO_OK;
        } catch (...) {
            return SAO_ERR_OS_CALL_FAILED;
        }
    }

  private:
    void release() noexcept {
        if (state_ == nullptr)
            return;
        if (callback_lock_.owns_lock())
            callback_lock_.unlock();
        const auto found =
            std::find(g_current_providers.rbegin(), g_current_providers.rend(), state_.get());
        if (found != g_current_providers.rend()) {
            g_current_providers.erase(std::next(found).base());
        }
        {
            std::lock_guard lock(state_->mutex);
            if (state_->in_flight > 0)
                --state_->in_flight;
        }
        state_->idle.notify_all();
        state_.reset();
    }

    std::shared_ptr<entity_provider_state> state_;
    std::unique_lock<std::mutex> callback_lock_;
};

int32_t call_snapshot_cpp(entity_snapshot_callback_fn callback, entity_menu_row* rows,
                          uint32_t capacity, uint32_t* out_count, uint64_t* out_revision,
                          void* user_data) noexcept {
    try {
        return callback(rows, capacity, out_count, out_revision, user_data);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t call_snapshot(entity_snapshot_callback_fn callback, entity_menu_row* rows,
                      uint32_t capacity, uint32_t* out_count, uint64_t* out_revision,
                      void* user_data) noexcept {
#if defined(_MSC_VER)
    __try {
        return call_snapshot_cpp(callback, rows, capacity, out_count, out_revision, user_data);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#else
    return call_snapshot_cpp(callback, rows, capacity, out_count, out_revision, user_data);
#endif
}

int32_t call_action_cpp(entity_action_handler_fn callback, const char* action_id,
                        const char* payload, void* user_data) noexcept {
    try {
        return callback(action_id, payload, user_data);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t call_action(entity_action_handler_fn callback, const char* action_id, const char* payload,
                    void* user_data) noexcept {
#if defined(_MSC_VER)
    __try {
        return call_action_cpp(callback, action_id, payload, user_data);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#else
    return call_action_cpp(callback, action_id, payload, user_data);
#endif
}

int32_t call_catalog_cpp(entity_provider_catalog_callback callback,
                         const entity_provider_catalog_view* catalog, void* user_data) noexcept {
    try {
        return callback(catalog, user_data);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t call_catalog(entity_provider_catalog_callback callback,
                     const entity_provider_catalog_view* catalog, void* user_data) noexcept {
#if defined(_MSC_VER)
    __try {
        return call_catalog_cpp(callback, catalog, user_data);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#else
    return call_catalog_cpp(callback, catalog, user_data);
#endif
}

bool bounded_length(const char* value, size_t maximum_bytes, size_t& out_length) noexcept {
#if defined(_MSC_VER)
    __try {
#endif
        size_t length = 0;
        while (length <= maximum_bytes && value[length] != '\0')
            ++length;
        if (length > maximum_bytes)
            return false;
        out_length = length;
        return true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

bool copy_bytes(char* destination, const char* source, size_t size) noexcept {
#if defined(_MSC_VER)
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::memcpy(destination, source, size);
    return true;
#endif
}

bool valid_utf8(std::string_view value) noexcept {
    size_t offset = 0;
    while (offset < value.size()) {
        const auto first = static_cast<unsigned char>(value[offset]);
        if (first <= 0x7f) {
            ++offset;
            continue;
        }
        size_t continuation_count = 0;
        uint32_t code_point = 0;
        if ((first & 0xe0u) == 0xc0u) {
            continuation_count = 1;
            code_point = first & 0x1fu;
        } else if ((first & 0xf0u) == 0xe0u) {
            continuation_count = 2;
            code_point = first & 0x0fu;
        } else if ((first & 0xf8u) == 0xf0u) {
            continuation_count = 3;
            code_point = first & 0x07u;
        } else {
            return false;
        }
        if (offset + continuation_count >= value.size())
            return false;
        for (size_t index = 1; index <= continuation_count; ++index) {
            const auto next = static_cast<unsigned char>(value[offset + index]);
            if ((next & 0xc0u) != 0x80u)
                return false;
            code_point = (code_point << 6u) | (next & 0x3fu);
        }
        const bool overlong = (continuation_count == 1 && code_point < 0x80u) ||
                              (continuation_count == 2 && code_point < 0x800u) ||
                              (continuation_count == 3 && code_point < 0x10000u);
        if (overlong || code_point > 0x10ffffu ||
            (code_point >= 0xd800u && code_point <= 0xdfffu)) {
            return false;
        }
        offset += continuation_count + 1;
    }
    return true;
}

int32_t copy_bounded_string(const char* value, bool required, size_t maximum_bytes,
                            size_t& total_bytes, std::string& out) {
    if (value == nullptr)
        return required ? SAO_ERR_INVALID_ARGUMENT : SAO_OK;
    size_t length = 0;
    if (!bounded_length(value, maximum_bytes, length) || (required && length == 0) ||
        total_bytes > kMaximumSnapshotStringBytes ||
        length > kMaximumSnapshotStringBytes - total_bytes) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    std::string candidate(length, '\0');
    if (length > 0 && !copy_bytes(candidate.data(), value, length)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const std::string_view view(candidate);
    if (!valid_utf8(view))
        return SAO_ERR_INVALID_ARGUMENT;
    out = std::move(candidate);
    total_bytes += length;
    return SAO_OK;
}

int32_t copy_row(const entity_menu_row& row, size_t& total_bytes, owned_entity_row& out) {
    if (row.struct_size < sizeof(entity_menu_row) || !std::isfinite(row.category_priority)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    owned_entity_row candidate;
    int32_t status = copy_bounded_string(row.category_id_utf8, true, kMaximumStringBytes,
                                         total_bytes, candidate.category_id);
    if (status == SAO_OK) {
        status = copy_bounded_string(row.category_label_utf8, false, kMaximumStringBytes,
                                     total_bytes, candidate.category_label);
    }
    if (status == SAO_OK) {
        status = copy_bounded_string(row.category_icon_utf8, false, kMaximumStringBytes,
                                     total_bytes, candidate.category_icon);
    }
    if (status == SAO_OK) {
        status = copy_bounded_string(row.row_label_utf8, true, kMaximumStringBytes, total_bytes,
                                     candidate.row_label);
    }
    if (status == SAO_OK) {
        status = copy_bounded_string(row.row_icon_utf8, false, kMaximumStringBytes, total_bytes,
                                     candidate.row_icon);
    }
    if (status == SAO_OK) {
        status = copy_bounded_string(row.action_id_utf8, true, kMaximumStringBytes, total_bytes,
                                     candidate.action_id);
    }
    if (status == SAO_OK && row.payload_json_utf8 == nullptr) {
        constexpr std::string_view kDefaultPayload = "{}";
        if (total_bytes > kMaximumSnapshotStringBytes ||
            kDefaultPayload.size() > kMaximumSnapshotStringBytes - total_bytes) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        candidate.payload_json = kDefaultPayload;
        total_bytes += candidate.payload_json.size();
    } else if (status == SAO_OK) {
        status = copy_bounded_string(row.payload_json_utf8, false, kMaximumStringBytes, total_bytes,
                                     candidate.payload_json);
    }
    if (status != SAO_OK)
        return status;
    candidate.category_priority = row.category_priority;
    candidate.can_activate = row.can_activate != 0;
    candidate.keep_menu_open = row.keep_menu_open != 0;
    candidate.close_menu_before = row.close_menu_before != 0;
    out = std::move(candidate);
    return SAO_OK;
}

int32_t copy_provider_snapshot(const std::shared_ptr<entity_provider_state>& state,
                               owned_provider_snapshot& out) {
    provider_lease lease;
    const int32_t lease_status = lease.acquire(state);
    if (lease_status != SAO_OK)
        return lease_status;

    for (uint32_t attempt = 0; attempt < kMaximumSnapshotAttempts; ++attempt) {
        uint32_t required_count = 0;
        uint64_t first_revision = 0;
        int32_t status = call_snapshot(state->snapshot, nullptr, 0, &required_count,
                                       &first_revision, state->user_data);
        if (status != SAO_OK && status != SAO_ERR_BUFFER_TOO_SMALL) {
            return status;
        }
        if (required_count > kMaximumProviderRows) {
            return SAO_ERR_INVALID_ARGUMENT;
        }

        if (required_count == 0) {
            owned_provider_snapshot candidate;
            candidate.provider_id = state->provider_id;
            candidate.owner_plugin_id = state->owner_plugin_id;
            candidate.generation = state->generation;
            candidate.revision = first_revision;
            candidate.has_root_contribution = state->has_root_contribution;
            candidate.contribution_id = state->contribution_id;
            candidate.root_id = state->root_id;
            candidate.root_name = state->root_name;
            candidate.root_icon = state->root_icon;
            candidate.root_priority = state->root_priority;
            candidate.string_bytes = candidate.provider_id.size() +
                                     candidate.owner_plugin_id.size() +
                                     candidate.contribution_id.size() + candidate.root_id.size() +
                                     candidate.root_name.size() + candidate.root_icon.size();
            if (candidate.string_bytes > kMaximumSnapshotStringBytes) {
                return SAO_ERR_INVALID_ARGUMENT;
            }
            out = std::move(candidate);
            return SAO_OK;
        }

        std::vector<entity_menu_row> rows(required_count);
        for (auto& row : rows)
            row.struct_size = sizeof(entity_menu_row);
        uint32_t written_count = required_count;
        uint64_t second_revision = 0;
        status = call_snapshot(state->snapshot, rows.empty() ? nullptr : rows.data(),
                               required_count, &written_count, &second_revision, state->user_data);
        if (status == SAO_ERR_BUFFER_TOO_SMALL || written_count > required_count) {
            continue;
        }
        if (status != SAO_OK)
            return status;
        if (first_revision != second_revision)
            continue;
        rows.resize(written_count);

        owned_provider_snapshot candidate;
        candidate.provider_id = state->provider_id;
        candidate.owner_plugin_id = state->owner_plugin_id;
        candidate.generation = state->generation;
        candidate.revision = second_revision;
        candidate.has_root_contribution = state->has_root_contribution;
        candidate.contribution_id = state->contribution_id;
        candidate.root_id = state->root_id;
        candidate.root_name = state->root_name;
        candidate.root_icon = state->root_icon;
        candidate.root_priority = state->root_priority;
        candidate.rows.reserve(rows.size());
        if (candidate.provider_id.size() > kMaximumSnapshotStringBytes ||
            candidate.owner_plugin_id.size() >
                kMaximumSnapshotStringBytes - candidate.provider_id.size()) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        size_t total_bytes = candidate.provider_id.size() + candidate.owner_plugin_id.size() +
                             candidate.contribution_id.size() + candidate.root_id.size() +
                             candidate.root_name.size() + candidate.root_icon.size();
        for (const auto& row : rows) {
            owned_entity_row copied;
            status = copy_row(row, total_bytes, copied);
            if (status != SAO_OK)
                return status;
            candidate.rows.push_back(std::move(copied));
        }
        candidate.string_bytes = total_bytes;
        out = std::move(candidate);
        return SAO_OK;
    }
    return SAO_PLUGINS_ERR_BUSY;
}

} // namespace

int32_t register_entity_provider(const std::shared_ptr<plugin_handle_s>& owner,
                                 const std::string& owner_plugin_id, const char* provider_id_utf8,
                                 entity_snapshot_callback_fn snapshot,
                                 entity_action_handler_fn action_handler, void* user_data,
                                 const entity_root_contribution_descriptor* root_contribution,
                                 std::shared_ptr<entity_provider_state>& out) noexcept {
    out.reset();
    if (owner == nullptr || owner_plugin_id.empty() || provider_id_utf8 == nullptr ||
        snapshot == nullptr || action_handler == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        size_t provider_id_bytes = 0;
        std::string local_provider_id;
        int32_t status = copy_bounded_string(provider_id_utf8, true, kMaximumProviderIdBytes,
                                             provider_id_bytes, local_provider_id);
        if (status != SAO_OK || local_provider_id.find('/') != std::string::npos) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        auto state = std::make_shared<entity_provider_state>();
        state->owner = owner;
        state->owner_plugin_id = owner_plugin_id;
        state->provider_id = owner_plugin_id + "/" + local_provider_id;
        state->generation = g_next_generation.fetch_add(1, std::memory_order_relaxed);
        if (state->generation == 0)
            return SAO_ERR_OS_CALL_FAILED;
        state->snapshot = snapshot;
        state->action_handler = action_handler;
        state->user_data = user_data;
        if (root_contribution != nullptr) {
            if (root_contribution->struct_size < sizeof(entity_root_contribution_descriptor) ||
                !std::isfinite(root_contribution->priority)) {
                return SAO_ERR_INVALID_ARGUMENT;
            }
            size_t root_bytes = 0;
            status =
                copy_bounded_string(root_contribution->contribution_id_utf8, true,
                                    kMaximumProviderIdBytes, root_bytes, state->contribution_id);
            if (status == SAO_OK) {
                status = copy_bounded_string(root_contribution->root_id_utf8, true,
                                             kMaximumRootIdBytes, root_bytes, state->root_id);
            }
            if (status == SAO_OK) {
                status = copy_bounded_string(root_contribution->name_utf8, true,
                                             kMaximumStringBytes, root_bytes, state->root_name);
            }
            if (status == SAO_OK) {
                status = copy_bounded_string(root_contribution->icon_utf8, false,
                                             kMaximumStringBytes, root_bytes, state->root_icon);
            }
            if (status != SAO_OK)
                return status;
            static constexpr std::string_view kReservedRoots[] = {"Control", "Tools", "Plugins",
                                                                  "Skins", "About"};
            if (std::find(std::begin(kReservedRoots), std::end(kReservedRoots), state->root_id) !=
                    std::end(kReservedRoots) ||
                std::find(std::begin(kReservedRoots), std::end(kReservedRoots), state->root_name) !=
                    std::end(kReservedRoots)) {
                return SAO_PLUGINS_ERR_ALREADY_EXISTS;
            }
            state->root_priority = root_contribution->priority;
            state->has_root_contribution = true;
        }
        {
            std::lock_guard catalog_lock(g_catalog_mutex);
            if (!g_attached.emplace(state->provider_id, state).second) {
                return SAO_PLUGINS_ERR_ALREADY_EXISTS;
            }
        }
        out = std::move(state);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

bool entity_provider_is_current_thread(
    const std::vector<std::shared_ptr<entity_provider_state>>& providers) noexcept {
    return std::any_of(providers.begin(), providers.end(), [](const auto& provider) {
        return provider != nullptr &&
               std::find(g_current_providers.begin(), g_current_providers.end(), provider.get()) !=
                   g_current_providers.end();
    });
}

int32_t activate_entity_providers(
    const std::vector<std::shared_ptr<entity_provider_state>>& providers) noexcept {
    try {
        std::vector<std::shared_ptr<entity_provider_state>> ordered = providers;
        if (std::any_of(ordered.begin(), ordered.end(),
                        [](const auto& provider) { return provider == nullptr; })) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
            return std::less<const entity_provider_state*>{}(left.get(), right.get());
        });
        const auto duplicate = std::adjacent_find(
            ordered.begin(), ordered.end(),
            [](const auto& left, const auto& right) { return left.get() == right.get(); });
        if (duplicate != ordered.end()) {
            return SAO_PLUGINS_ERR_ALREADY_EXISTS;
        }

        std::vector<std::unique_lock<std::mutex>> provider_locks;
        provider_locks.reserve(ordered.size());
        for (const auto& provider : ordered)
            provider_locks.emplace_back(provider->mutex);

        std::unordered_set<std::string_view> provider_ids;
        std::unordered_set<std::string_view> root_ids;
        std::unordered_set<std::string_view> root_names;
        std::unordered_set<std::string> contribution_ids;
        provider_ids.reserve(ordered.size());
        for (const auto& provider : ordered) {
            if (provider->destroyed)
                return SAO_ERR_HANDLE_INVALID;
            if (provider->in_flight != 0)
                return SAO_PLUGINS_ERR_BUSY;
            if (!provider_ids.emplace(provider->provider_id).second) {
                return SAO_PLUGINS_ERR_ALREADY_EXISTS;
            }
            if (provider->has_root_contribution &&
                (!root_ids.emplace(provider->root_id).second ||
                 !root_names.emplace(provider->root_name).second ||
                 !contribution_ids
                      .emplace(provider->owner_plugin_id + "\n" + provider->contribution_id)
                      .second)) {
                return SAO_PLUGINS_ERR_ALREADY_EXISTS;
            }
        }

        std::lock_guard catalog_lock(g_catalog_mutex);
        for (const auto& provider : ordered) {
            const auto found = g_catalog.find(provider->provider_id);
            if (found != g_catalog.end() && found->second != provider) {
                return SAO_PLUGINS_ERR_ALREADY_EXISTS;
            }
        }
        for (const auto& [_, active] : g_catalog) {
            if (!active->has_root_contribution || provider_ids.contains(active->provider_id)) {
                continue;
            }
            if (root_ids.contains(active->root_id) || root_names.contains(active->root_name) ||
                contribution_ids.contains(active->owner_plugin_id + "\n" +
                                          active->contribution_id)) {
                return SAO_PLUGINS_ERR_ALREADY_EXISTS;
            }
        }

        auto candidate = g_catalog;
        candidate.reserve(candidate.size() + ordered.size());
        bool inserted = false;
        for (const auto& provider : ordered) {
            if (!candidate.contains(provider->provider_id)) {
                candidate.emplace(provider->provider_id, provider);
                inserted = true;
            }
        }
        for (const auto& provider : ordered) {
            provider->accepting = true;
            provider->published = true;
        }
        g_catalog.swap(candidate);
        if (inserted)
            ++g_catalog_revision;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t deactivate_entity_providers(
    const std::vector<std::shared_ptr<entity_provider_state>>& providers) noexcept {
    try {
        if (entity_provider_is_current_thread(providers)) {
            return SAO_PLUGINS_ERR_BUSY;
        }

        std::vector<std::shared_ptr<entity_provider_state>> ordered = providers;
        if (std::any_of(ordered.begin(), ordered.end(),
                        [](const auto& provider) { return provider == nullptr; })) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
            return std::less<const entity_provider_state*>{}(left.get(), right.get());
        });
        ordered.erase(std::unique(ordered.begin(), ordered.end(),
                                  [](const auto& left, const auto& right) {
                                      return left.get() == right.get();
                                  }),
                      ordered.end());

        {
            std::vector<std::unique_lock<std::mutex>> provider_locks;
            provider_locks.reserve(ordered.size());
            for (const auto& provider : ordered)
                provider_locks.emplace_back(provider->mutex);
            for (const auto& provider : ordered)
                provider->accepting = false;
        }
        const auto rundown_deadline = std::chrono::steady_clock::now() + kRundownTimeout;
        for (const auto& provider : ordered) {
            std::unique_lock provider_lock(provider->mutex);
            if (!provider->idle.wait_until(provider_lock, rundown_deadline,
                                           [&provider] { return provider->in_flight == 0; })) {
                provider_lock.unlock();
                std::vector<std::unique_lock<std::mutex>> provider_locks;
                provider_locks.reserve(ordered.size());
                for (const auto& item : ordered)
                    provider_locks.emplace_back(item->mutex);
                for (const auto& item : ordered)
                    item->accepting = true;
                return SAO_PLUGINS_ERR_BUSY;
            }
        }
        {
            std::vector<std::unique_lock<std::mutex>> provider_locks;
            provider_locks.reserve(ordered.size());
            for (const auto& provider : ordered)
                provider_locks.emplace_back(provider->mutex);

            std::lock_guard catalog_lock(g_catalog_mutex);
            auto candidate = g_catalog;
            bool removed = false;
            for (const auto& provider : ordered) {
                const auto found = candidate.find(provider->provider_id);
                if (found != candidate.end() && found->second == provider) {
                    candidate.erase(found);
                    removed = true;
                }
                provider->published = false;
            }
            g_catalog.swap(candidate);
            if (removed)
                ++g_catalog_revision;
        }
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t destroy_entity_providers(
    const std::vector<std::shared_ptr<entity_provider_state>>& providers) noexcept {
    const int32_t status = deactivate_entity_providers(providers);
    if (status != SAO_OK)
        return status;
    try {
        for (const auto& provider : providers) {
            std::lock_guard provider_lock(provider->mutex);
            if (provider->in_flight != 0 || provider->published) {
                return SAO_PLUGINS_ERR_BUSY;
            }
            provider->destroyed = true;
            provider->owner.reset();
            provider->snapshot = nullptr;
            provider->action_handler = nullptr;
            provider->user_data = nullptr;
        }
        {
            std::lock_guard catalog_lock(g_catalog_mutex);
            for (const auto& provider : providers) {
                const auto found = g_attached.find(provider->provider_id);
                if (found != g_attached.end() && found->second == provider) {
                    g_attached.erase(found);
                }
            }
        }
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

bool entity_provider_has_id(const std::shared_ptr<entity_provider_state>& provider,
                            const std::string& provider_id) noexcept {
    return provider != nullptr && provider->provider_id == provider_id;
}

extern "C" int32_t SAO_PLUGINS_CALL
sao_plugins_entity_provider_snapshot(entity_provider_catalog_callback callback, void* user_data) {
    if (callback == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        for (uint32_t attempt = 0; attempt < kMaximumSnapshotAttempts; ++attempt) {
            uint64_t catalog_revision = 0;
            std::vector<std::shared_ptr<entity_provider_state>> providers;
            {
                std::lock_guard lock(g_catalog_mutex);
                if (g_catalog.size() > kMaximumCatalogProviders) {
                    return SAO_ERR_INVALID_ARGUMENT;
                }
                catalog_revision = g_catalog_revision;
                providers.reserve(g_catalog.size());
                for (const auto& [_, provider] : g_catalog) {
                    providers.push_back(provider);
                }
            }
            std::sort(providers.begin(), providers.end(), [](const auto& left, const auto& right) {
                return left->provider_id < right->provider_id;
            });

            std::vector<owned_provider_snapshot> snapshots;
            snapshots.reserve(providers.size());
            bool retry = false;
            size_t total_rows = 0;
            size_t total_string_bytes = 0;
            for (const auto& provider : providers) {
                owned_provider_snapshot snapshot;
                const int32_t status = copy_provider_snapshot(provider, snapshot);
                if (status == SAO_PLUGINS_ERR_BUSY) {
                    retry = true;
                    break;
                }
                if (status != SAO_OK)
                    return status;
                if (total_rows > kMaximumCatalogRows ||
                    snapshot.rows.size() > kMaximumCatalogRows - total_rows ||
                    total_string_bytes > kMaximumCatalogStringBytes ||
                    snapshot.string_bytes > kMaximumCatalogStringBytes - total_string_bytes) {
                    return SAO_ERR_INVALID_ARGUMENT;
                }
                total_rows += snapshot.rows.size();
                total_string_bytes += snapshot.string_bytes;
                snapshots.push_back(std::move(snapshot));
            }
            {
                std::lock_guard lock(g_catalog_mutex);
                if (catalog_revision != g_catalog_revision)
                    retry = true;
            }
            if (retry)
                continue;

            std::vector<std::vector<entity_menu_row>> row_views;
            std::vector<entity_provider_view> provider_views;
            std::vector<std::vector<entity_root_action_ref_view>> root_action_views;
            std::vector<entity_root_contribution_view> root_views;
            row_views.resize(snapshots.size());
            provider_views.reserve(snapshots.size());
            root_action_views.reserve(snapshots.size());
            root_views.reserve(snapshots.size());
            for (size_t provider_index = 0; provider_index < snapshots.size(); ++provider_index) {
                auto& snapshot = snapshots[provider_index];
                auto& rows = row_views[provider_index];
                rows.reserve(snapshot.rows.size());
                for (const auto& row : snapshot.rows) {
                    rows.push_back({
                        sizeof(entity_menu_row),
                        row.category_id.c_str(),
                        row.category_label.c_str(),
                        row.category_icon.c_str(),
                        row.category_priority,
                        row.row_label.c_str(),
                        row.row_icon.c_str(),
                        row.action_id.c_str(),
                        row.payload_json.c_str(),
                        static_cast<uint8_t>(row.can_activate),
                        static_cast<uint8_t>(row.keep_menu_open),
                        static_cast<uint8_t>(row.close_menu_before),
                        {},
                    });
                }
                provider_views.push_back({
                    sizeof(entity_provider_view),
                    snapshot.provider_id.c_str(),
                    snapshot.owner_plugin_id.c_str(),
                    snapshot.generation,
                    snapshot.revision,
                    static_cast<uint32_t>(rows.size()),
                    rows.empty() ? nullptr : rows.data(),
                });
                if (snapshot.has_root_contribution) {
                    std::vector<entity_root_action_ref_view> actions;
                    actions.reserve(snapshot.rows.size());
                    for (const auto& row : snapshot.rows) {
                        actions.push_back({
                            sizeof(entity_root_action_ref_view),
                            snapshot.provider_id.c_str(),
                            row.action_id.c_str(),
                        });
                    }
                    root_action_views.push_back(std::move(actions));
                    const auto& stored_actions = root_action_views.back();
                    root_views.push_back({
                        sizeof(entity_root_contribution_view),
                        snapshot.owner_plugin_id.c_str(),
                        snapshot.contribution_id.c_str(),
                        snapshot.root_id.c_str(),
                        snapshot.root_name.c_str(),
                        snapshot.root_icon.c_str(),
                        snapshot.root_priority,
                        static_cast<uint32_t>(stored_actions.size()),
                        stored_actions.empty() ? nullptr : stored_actions.data(),
                    });
                }
            }
            const entity_provider_catalog_view catalog{
                sizeof(entity_provider_catalog_view),
                catalog_revision,
                static_cast<uint32_t>(provider_views.size()),
                provider_views.empty() ? nullptr : provider_views.data(),
                static_cast<uint32_t>(root_views.size()),
                root_views.empty() ? nullptr : root_views.data(),
            };
            const int32_t callback_status = call_catalog(callback, &catalog, user_data);
            {
                std::lock_guard lock(g_catalog_mutex);
                if (catalog_revision != g_catalog_revision)
                    return SAO_PLUGINS_ERR_BUSY;
            }
            return callback_status;
        }
        return SAO_PLUGINS_ERR_BUSY;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" int32_t SAO_PLUGINS_CALL
sao_plugins_entity_provider_invoke(const char* provider_id_utf8, uint64_t expected_generation,
                                   const char* action_id_utf8, const char* payload_json_utf8) {
    if (provider_id_utf8 == nullptr || expected_generation == 0 || action_id_utf8 == nullptr ||
        payload_json_utf8 == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        size_t invoke_bytes = 0;
        std::string provider_id;
        std::string action_id;
        std::string payload_json;
        int32_t status = copy_bounded_string(provider_id_utf8, true, kMaximumStringBytes,
                                             invoke_bytes, provider_id);
        if (status == SAO_OK) {
            status = copy_bounded_string(action_id_utf8, true, kMaximumStringBytes, invoke_bytes,
                                         action_id);
        }
        if (status == SAO_OK) {
            status = copy_bounded_string(payload_json_utf8, false, kMaximumInvokePayloadBytes,
                                         invoke_bytes, payload_json);
        }
        if (status != SAO_OK)
            return status;

        std::shared_ptr<entity_provider_state> provider;
        {
            std::lock_guard lock(g_catalog_mutex);
            const auto found = g_attached.find(provider_id);
            if (found == g_attached.end())
                return SAO_ERR_HANDLE_INVALID;
            provider = found->second;
        }
        if (provider->generation != expected_generation) {
            return SAO_ERR_HANDLE_INVALID;
        }
        provider_lease lease;
        const int32_t lease_status = lease.acquire(provider);
        if (lease_status != SAO_OK)
            return lease_status;
        return call_action(provider->action_handler, action_id.c_str(), payload_json.c_str(),
                           provider->user_data);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

} // namespace sao::plugins::loader
