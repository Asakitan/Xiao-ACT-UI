#include "sao/plugins/loader/entity_provider.h"

#include "entity_provider_internal.h"
#include "plugin_internal.h"

#include "sao/plugins/loader/loader_status.h"
#include "sao_plugins/sao_status.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace sao::plugins::loader {

struct entity_provider_state {
    std::shared_ptr<plugin_handle_s> owner;
    std::string provider_id;
    std::string owner_plugin_id;
    uint64_t generation = 0;
    uint32_t snapshot_abi_version = 0;
    entity_snapshot_callback_fn snapshot_v1 = nullptr;
    entity_snapshot_callback_v2_fn snapshot_v2 = nullptr;
    entity_action_handler_fn action_handler_v1 = nullptr;
    entity_action_handler_v2_fn action_handler_v2 = nullptr;
    void* snapshot_user_data = nullptr;
    void* action_user_data = nullptr;
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
constexpr size_t kMaximumProviderRawRowBytes = 16 * 1024 * 1024;
constexpr size_t kMaximumCatalogRows = 16384;
constexpr size_t kMaximumCatalogStringBytes = 32 * 1024 * 1024;
constexpr size_t kMaximumInvokePayloadBytes = 1024 * 1024;
constexpr size_t kMaximumJsonNestingDepth = 64;
constexpr size_t kMaximumJsonNodes = 16384;
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
    uint32_t snapshot_abi_version = 0;
    entity_snapshot_content_token_t content_token = kInvalidEntitySnapshotContentToken;
    size_t string_bytes = 0;
    std::vector<owned_entity_row> rows;
    bool has_root_contribution = false;
    std::string contribution_id;
    std::string root_id;
    std::string root_name;
    std::string root_icon;
    double root_priority = 0.0;
};

struct owned_action_result_v2 {
    bool handled = false;
    bool has_result = false;
    std::string result_json;
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

int32_t call_snapshot_v2_cpp(entity_snapshot_callback_v2_fn callback, void* rows, uint32_t capacity,
                             uint32_t row_stride_bytes, uint32_t* out_count, uint64_t* out_revision,
                             entity_snapshot_content_token_t* out_content_token,
                             uint32_t* out_row_stride_bytes, void* user_data) noexcept {
    try {
        return callback(rows, capacity, row_stride_bytes, out_count, out_revision,
                        out_content_token, out_row_stride_bytes, user_data);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t call_snapshot_v2(entity_snapshot_callback_v2_fn callback, void* rows, uint32_t capacity,
                         uint32_t row_stride_bytes, uint32_t* out_count, uint64_t* out_revision,
                         entity_snapshot_content_token_t* out_content_token,
                         uint32_t* out_row_stride_bytes, void* user_data) noexcept {
#if defined(_MSC_VER)
    __try {
        return call_snapshot_v2_cpp(callback, rows, capacity, row_stride_bytes, out_count,
                                    out_revision, out_content_token, out_row_stride_bytes,
                                    user_data);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#else
    return call_snapshot_v2_cpp(callback, rows, capacity, row_stride_bytes, out_count, out_revision,
                                out_content_token, out_row_stride_bytes, user_data);
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

int32_t call_action_v2_cpp(entity_action_handler_v2_fn callback, const char* action_id,
                           const char* payload, entity_action_result_sink_v2_fn result_sink,
                           void* result_sink_user_data, void* user_data) noexcept {
    try {
        return callback(action_id, payload, result_sink, result_sink_user_data, user_data);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t call_action_v2(entity_action_handler_v2_fn callback, const char* action_id,
                       const char* payload, entity_action_result_sink_v2_fn result_sink,
                       void* result_sink_user_data, void* user_data) noexcept {
#if defined(_MSC_VER)
    __try {
        return call_action_v2_cpp(callback, action_id, payload, result_sink, result_sink_user_data,
                                  user_data);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#else
    return call_action_v2_cpp(callback, action_id, payload, result_sink, result_sink_user_data,
                              user_data);
#endif
}

int32_t call_action_result_cpp(entity_action_result_callback_v2_fn callback,
                               const entity_action_result_v2* result, void* user_data) noexcept {
    try {
        return callback(result, user_data);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t call_action_result(entity_action_result_callback_v2_fn callback,
                           const entity_action_result_v2* result, void* user_data) noexcept {
#if defined(_MSC_VER)
    __try {
        return call_action_result_cpp(callback, result, user_data);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#else
    return call_action_result_cpp(callback, result, user_data);
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

int32_t call_catalog_v2_cpp(entity_provider_catalog_callback_v2 callback,
                            const entity_provider_catalog_view_v2* catalog,
                            void* user_data) noexcept {
    try {
        return callback(catalog, user_data);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t call_catalog_v2(entity_provider_catalog_callback_v2 callback,
                        const entity_provider_catalog_view_v2* catalog, void* user_data) noexcept {
#if defined(_MSC_VER)
    __try {
        return call_catalog_v2_cpp(callback, catalog, user_data);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#else
    return call_catalog_v2_cpp(callback, catalog, user_data);
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

template <typename T>
int32_t copy_external_struct(const T* source, size_t required_prefix_size, T& out) noexcept {
    if (source == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
#if defined(_MSC_VER)
    __try {
#endif
        const size_t struct_size = source->struct_size;
        if (struct_size < required_prefix_size)
            return SAO_PLUGINS_ERR_ABI_MISMATCH;
        out = {};
        std::memcpy(&out, source, (std::min)(struct_size, sizeof(out)));
        return SAO_OK;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
#endif
}

class bounded_json_sax final : public nlohmann::json::json_sax_t {
  public:
    bool null() override {
        return consume_node();
    }
    bool boolean(bool) override {
        return consume_node();
    }
    bool number_integer(number_integer_t) override {
        return consume_node();
    }
    bool number_unsigned(number_unsigned_t) override {
        return consume_node();
    }
    bool number_float(number_float_t, const string_t&) override {
        return consume_node();
    }
    bool string(string_t&) override {
        return consume_node();
    }
    bool binary(binary_t&) override {
        return consume_node();
    }
    bool start_object(std::size_t) override {
        return start_container();
    }
    bool key(string_t&) override {
        return true;
    }
    bool end_object() override {
        return end_container();
    }
    bool start_array(std::size_t) override {
        return start_container();
    }
    bool end_array() override {
        return end_container();
    }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override {
        return false;
    }

  private:
    bool consume_node() noexcept {
        if (nodes_ >= kMaximumJsonNodes)
            return false;
        ++nodes_;
        return true;
    }

    bool start_container() noexcept {
        if (depth_ >= kMaximumJsonNestingDepth || !consume_node())
            return false;
        ++depth_;
        return true;
    }

    bool end_container() noexcept {
        if (depth_ == 0)
            return false;
        --depth_;
        return true;
    }

    size_t depth_ = 0;
    size_t nodes_ = 0;
};

bool valid_json_syntax(std::string_view value) noexcept {
    try {
        bounded_json_sax sax;
        return nlohmann::json::sax_parse(value.begin(), value.end(), &sax);
    } catch (...) {
        return false;
    }
}

bool allocate_generation(uint64_t& out_generation) noexcept {
    auto current = g_next_generation.load(std::memory_order_relaxed);
    for (;;) {
        if (current == 0 || current == (std::numeric_limits<uint64_t>::max)())
            return false;
        const uint64_t next = current + 1;
        if (g_next_generation.compare_exchange_weak(current, next, std::memory_order_relaxed,
                                                    std::memory_order_relaxed)) {
            out_generation = current;
            return true;
        }
    }
}

bool add_snapshot_string_bytes(size_t& total, size_t amount) noexcept {
    if (total > kMaximumSnapshotStringBytes || amount > kMaximumSnapshotStringBytes - total) {
        return false;
    }
    total += amount;
    return true;
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

struct action_result_sink_context {
    uint32_t submissions = 0;
    int32_t status = SAO_OK;
    owned_action_result_v2 result;
};

int32_t SAO_PLUGINS_CALL copy_action_result_v2(const entity_action_result_v2* result,
                                               void* user_data) noexcept {
    auto* context = static_cast<action_result_sink_context*>(user_data);
    if (context == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        if (context->submissions != 0) {
            context->status = SAO_ERR_INVALID_ARGUMENT;
            return context->status;
        }
        ++context->submissions;
        entity_action_result_v2 current{};
        int32_t status =
            copy_external_struct(result, kEntityActionResultV2RequiredPrefixSize, current);
        if (status == SAO_OK && current.abi_version != kEntityActionAbiVersion2)
            status = SAO_PLUGINS_ERR_ABI_MISMATCH;
        if (status == SAO_OK &&
            (current.handled > 1 ||
             std::any_of(std::begin(current.reserved), std::end(current.reserved),
                         [](uint8_t value) { return value != 0; }))) {
            status = SAO_ERR_INVALID_ARGUMENT;
        }
        if (status == SAO_OK && current.handled == 0 && current.result_json_utf8 != nullptr)
            status = SAO_ERR_INVALID_ARGUMENT;

        owned_action_result_v2 candidate;
        candidate.handled = current.handled != 0;
        if (status == SAO_OK && current.result_json_utf8 != nullptr) {
            size_t result_bytes = 0;
            status = copy_bounded_string(current.result_json_utf8, false,
                                         kMaximumEntityActionResultJsonBytes, result_bytes,
                                         candidate.result_json);
            if (status == SAO_OK && !valid_json_syntax(candidate.result_json))
                status = SAO_ERR_INVALID_ARGUMENT;
            candidate.has_result = status == SAO_OK;
        }
        if (status == SAO_OK)
            context->result = std::move(candidate);
        context->status = status;
        return status;
    } catch (...) {
        context->status = SAO_ERR_OS_CALL_FAILED;
        return context->status;
    }
}

int32_t invoke_action_v2_handler(entity_action_handler_v2_fn callback, const char* action_id,
                                 const char* payload, void* user_data,
                                 owned_action_result_v2& out_result) noexcept {
    action_result_sink_context sink_context;
    const int32_t callback_status = call_action_v2(callback, action_id, payload,
                                                   copy_action_result_v2, &sink_context, user_data);
    if (callback_status != SAO_OK)
        return callback_status;
    if (sink_context.submissions != 1)
        return SAO_ERR_INVALID_ARGUMENT;
    if (sink_context.status != SAO_OK)
        return sink_context.status;
    try {
        out_result = std::move(sink_context.result);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t copy_row(const entity_menu_row& row, size_t& total_bytes, owned_entity_row& out) {
    entity_menu_row current{};
    const int32_t copy_status =
        copy_external_struct(&row, kEntityMenuRowRequiredPrefixSize, current);
    if (copy_status != SAO_OK)
        return copy_status;
    if (!std::isfinite(current.category_priority)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    owned_entity_row candidate;
    size_t candidate_total_bytes = total_bytes;
    int32_t status = copy_bounded_string(current.category_id_utf8, true, kMaximumStringBytes,
                                         candidate_total_bytes, candidate.category_id);
    if (status == SAO_OK) {
        status = copy_bounded_string(current.category_label_utf8, false, kMaximumStringBytes,
                                     candidate_total_bytes, candidate.category_label);
    }
    if (status == SAO_OK) {
        status = copy_bounded_string(current.category_icon_utf8, false, kMaximumStringBytes,
                                     candidate_total_bytes, candidate.category_icon);
    }
    if (status == SAO_OK) {
        status = copy_bounded_string(current.row_label_utf8, true, kMaximumStringBytes,
                                     candidate_total_bytes, candidate.row_label);
    }
    if (status == SAO_OK) {
        status = copy_bounded_string(current.row_icon_utf8, false, kMaximumStringBytes,
                                     candidate_total_bytes, candidate.row_icon);
    }
    if (status == SAO_OK) {
        status = copy_bounded_string(current.action_id_utf8, true, kMaximumStringBytes,
                                     candidate_total_bytes, candidate.action_id);
    }
    if (status == SAO_OK && current.payload_json_utf8 == nullptr) {
        constexpr std::string_view kDefaultPayload = "{}";
        if (!add_snapshot_string_bytes(candidate_total_bytes, kDefaultPayload.size())) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        candidate.payload_json = kDefaultPayload;
    } else if (status == SAO_OK) {
        status = copy_bounded_string(current.payload_json_utf8, false, kMaximumStringBytes,
                                     candidate_total_bytes, candidate.payload_json);
    }
    if (status != SAO_OK || !valid_json_syntax(candidate.payload_json))
        return SAO_ERR_INVALID_ARGUMENT;
    candidate.category_priority = current.category_priority;
    candidate.can_activate = current.can_activate != 0;
    candidate.keep_menu_open = current.keep_menu_open != 0;
    candidate.close_menu_before = current.close_menu_before != 0;
    try {
        out = std::move(candidate);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
    total_bytes = candidate_total_bytes;
    return SAO_OK;
}

int32_t copy_row_v2(const entity_menu_row_v2& row, size_t& total_bytes, owned_entity_row& out) {
    entity_menu_row_v2 current{};
    const int32_t copy_status =
        copy_external_struct(&row, kEntityMenuRowV2RequiredPrefixSize, current);
    if (copy_status != SAO_OK)
        return copy_status;
    if (current.can_activate > 1 || current.keep_menu_open > 1 || current.close_menu_before > 1 ||
        std::any_of(std::begin(current.reserved), std::end(current.reserved),
                    [](uint8_t value) { return value != 0; })) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const entity_menu_row canonical{
        sizeof(entity_menu_row),
        current.category_id_utf8,
        current.category_label_utf8,
        current.category_icon_utf8,
        current.category_priority,
        current.row_label_utf8,
        current.row_icon_utf8,
        current.action_id_utf8,
        current.payload_json_utf8,
        current.can_activate,
        current.keep_menu_open,
        current.close_menu_before,
        {},
    };
    return copy_row(canonical, total_bytes, out);
}

class content_hasher final {
  public:
    void add_u8(uint8_t value) noexcept {
        add_bytes(&value, sizeof(value));
    }

    void add_u32(uint32_t value) noexcept {
        for (uint32_t shift = 0; shift < 32; shift += 8)
            add_u8(static_cast<uint8_t>(value >> shift));
    }

    void add_u64(uint64_t value) noexcept {
        for (uint32_t shift = 0; shift < 64; shift += 8)
            add_u8(static_cast<uint8_t>(value >> shift));
    }

    void add_double(double value) noexcept {
        add_u64(value == 0.0 ? 0 : std::bit_cast<uint64_t>(value));
    }

    void add_string(std::string_view value) noexcept {
        add_u64(static_cast<uint64_t>(value.size()));
        add_bytes(value.data(), value.size());
    }

    entity_snapshot_content_token_t finish() const noexcept {
        return value_ == kInvalidEntitySnapshotContentToken ? 1 : value_;
    }

  private:
    void add_bytes(const void* data, size_t size) noexcept {
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (size_t index = 0; index < size; ++index) {
            value_ ^= bytes[index];
            value_ *= 1099511628211ULL;
        }
    }

    uint64_t value_ = 14695981039346656037ULL;
};

void hash_row(content_hasher& hasher, const owned_entity_row& row) noexcept {
    hasher.add_string(row.category_id);
    hasher.add_string(row.category_label);
    hasher.add_string(row.category_icon);
    hasher.add_double(row.category_priority);
    hasher.add_string(row.row_label);
    hasher.add_string(row.row_icon);
    hasher.add_string(row.action_id);
    hasher.add_string(row.payload_json);
    hasher.add_u8(static_cast<uint8_t>(row.can_activate));
    hasher.add_u8(static_cast<uint8_t>(row.keep_menu_open));
    hasher.add_u8(static_cast<uint8_t>(row.close_menu_before));
}

entity_snapshot_content_token_t
compute_provider_content_token(const owned_provider_snapshot& snapshot) noexcept {
    content_hasher hasher;
    hasher.add_string(snapshot.provider_id);
    hasher.add_string(snapshot.owner_plugin_id);
    hasher.add_u64(snapshot.generation);
    hasher.add_u64(static_cast<uint64_t>(snapshot.rows.size()));
    for (const auto& row : snapshot.rows)
        hash_row(hasher, row);
    hasher.add_u64(snapshot.has_root_contribution ? 1 : 0);
    if (snapshot.has_root_contribution) {
        hasher.add_string(snapshot.contribution_id);
        hasher.add_string(snapshot.root_id);
        hasher.add_string(snapshot.root_name);
        hasher.add_string(snapshot.root_icon);
        hasher.add_double(snapshot.root_priority);
        hasher.add_u64(static_cast<uint64_t>(snapshot.rows.size()));
    }
    return hasher.finish();
}

entity_snapshot_content_token_t
compute_catalog_content_token(const std::vector<owned_provider_snapshot>& snapshots) noexcept {
    content_hasher hasher;
    hasher.add_u64(static_cast<uint64_t>(snapshots.size()));
    for (const auto& snapshot : snapshots)
        hasher.add_u64(snapshot.content_token);
    return hasher.finish();
}

bool checked_raw_row_bytes(uint32_t count, uint32_t stride, size_t& out_bytes) noexcept {
    if (stride == 0 || count > kMaximumProviderRawRowBytes / stride)
        return false;
    out_bytes = static_cast<size_t>(count) * stride;
    return out_bytes <= kMaximumProviderRawRowBytes;
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
        int32_t status = call_snapshot(state->snapshot_v1, nullptr, 0, &required_count,
                                       &first_revision, state->snapshot_user_data);
        if (status != SAO_OK && status != SAO_ERR_BUFFER_TOO_SMALL) {
            return status;
        }
        if (required_count > kMaximumProviderRows) {
            return SAO_ERR_INVALID_ARGUMENT;
        }

        if (required_count == 0) {
            if (status != SAO_OK)
                return SAO_ERR_INVALID_ARGUMENT;
            owned_provider_snapshot candidate;
            candidate.provider_id = state->provider_id;
            candidate.owner_plugin_id = state->owner_plugin_id;
            candidate.generation = state->generation;
            candidate.revision = first_revision;
            candidate.snapshot_abi_version = kEntitySnapshotAbiVersion1;
            candidate.has_root_contribution = state->has_root_contribution;
            candidate.contribution_id = state->contribution_id;
            candidate.root_id = state->root_id;
            candidate.root_name = state->root_name;
            candidate.root_icon = state->root_icon;
            candidate.root_priority = state->root_priority;
            size_t total_bytes = 0;
            if (!add_snapshot_string_bytes(total_bytes, candidate.provider_id.size()) ||
                !add_snapshot_string_bytes(total_bytes, candidate.owner_plugin_id.size()) ||
                !add_snapshot_string_bytes(total_bytes, candidate.contribution_id.size()) ||
                !add_snapshot_string_bytes(total_bytes, candidate.root_id.size()) ||
                !add_snapshot_string_bytes(total_bytes, candidate.root_name.size()) ||
                !add_snapshot_string_bytes(total_bytes, candidate.root_icon.size())) {
                return SAO_ERR_INVALID_ARGUMENT;
            }
            candidate.string_bytes = total_bytes;
            candidate.content_token = compute_provider_content_token(candidate);
            out = std::move(candidate);
            return SAO_OK;
        }

        std::vector<entity_menu_row> rows(required_count);
        for (auto& row : rows)
            row.struct_size = sizeof(entity_menu_row);
        uint32_t written_count = required_count;
        uint64_t second_revision = 0;
        status = call_snapshot(state->snapshot_v1, rows.empty() ? nullptr : rows.data(),
                               required_count, &written_count, &second_revision,
                               state->snapshot_user_data);
        if (status != SAO_OK && status != SAO_ERR_BUFFER_TOO_SMALL)
            return status;
        if (status == SAO_ERR_BUFFER_TOO_SMALL || written_count != required_count ||
            first_revision != second_revision) {
            continue;
        }
        rows.resize(written_count);

        owned_provider_snapshot candidate;
        candidate.provider_id = state->provider_id;
        candidate.owner_plugin_id = state->owner_plugin_id;
        candidate.generation = state->generation;
        candidate.revision = second_revision;
        candidate.snapshot_abi_version = kEntitySnapshotAbiVersion1;
        candidate.has_root_contribution = state->has_root_contribution;
        candidate.contribution_id = state->contribution_id;
        candidate.root_id = state->root_id;
        candidate.root_name = state->root_name;
        candidate.root_icon = state->root_icon;
        candidate.root_priority = state->root_priority;
        candidate.rows.reserve(rows.size());
        size_t total_bytes = 0;
        if (!add_snapshot_string_bytes(total_bytes, candidate.provider_id.size()) ||
            !add_snapshot_string_bytes(total_bytes, candidate.owner_plugin_id.size()) ||
            !add_snapshot_string_bytes(total_bytes, candidate.contribution_id.size()) ||
            !add_snapshot_string_bytes(total_bytes, candidate.root_id.size()) ||
            !add_snapshot_string_bytes(total_bytes, candidate.root_name.size()) ||
            !add_snapshot_string_bytes(total_bytes, candidate.root_icon.size())) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        for (const auto& row : rows) {
            owned_entity_row copied;
            status = copy_row(row, total_bytes, copied);
            if (status != SAO_OK)
                return status;
            candidate.rows.push_back(std::move(copied));
        }
        candidate.string_bytes = total_bytes;
        candidate.content_token = compute_provider_content_token(candidate);
        out = std::move(candidate);
        return SAO_OK;
    }
    return SAO_PLUGINS_ERR_BUSY;
}

int32_t copy_provider_snapshot_v2(const std::shared_ptr<entity_provider_state>& state,
                                  owned_provider_snapshot& out) {
    provider_lease lease;
    const int32_t lease_status = lease.acquire(state);
    if (lease_status != SAO_OK)
        return lease_status;

    for (uint32_t attempt = 0; attempt < kMaximumSnapshotAttempts; ++attempt) {
        uint32_t required_count = 0;
        uint64_t first_revision = 0;
        entity_snapshot_content_token_t first_content_token = kInvalidEntitySnapshotContentToken;
        uint32_t required_stride = 0;
        int32_t status =
            call_snapshot_v2(state->snapshot_v2, nullptr, 0, 0, &required_count, &first_revision,
                             &first_content_token, &required_stride,
                             state->snapshot_user_data);
        if (status != SAO_OK && status != SAO_ERR_BUFFER_TOO_SMALL)
            return status;
        if (first_content_token == kInvalidEntitySnapshotContentToken ||
            required_count > kMaximumProviderRows) {
            return SAO_ERR_INVALID_ARGUMENT;
        }

        if (required_count == 0) {
            if (status != SAO_OK || required_stride != 0)
                return SAO_ERR_INVALID_ARGUMENT;
            owned_provider_snapshot candidate;
            candidate.provider_id = state->provider_id;
            candidate.owner_plugin_id = state->owner_plugin_id;
            candidate.generation = state->generation;
            candidate.revision = first_revision;
            candidate.snapshot_abi_version = kEntitySnapshotAbiVersion2;
            candidate.has_root_contribution = state->has_root_contribution;
            candidate.contribution_id = state->contribution_id;
            candidate.root_id = state->root_id;
            candidate.root_name = state->root_name;
            candidate.root_icon = state->root_icon;
            candidate.root_priority = state->root_priority;
            size_t total_bytes = 0;
            if (!add_snapshot_string_bytes(total_bytes, candidate.provider_id.size()) ||
                !add_snapshot_string_bytes(total_bytes, candidate.owner_plugin_id.size()) ||
                !add_snapshot_string_bytes(total_bytes, candidate.contribution_id.size()) ||
                !add_snapshot_string_bytes(total_bytes, candidate.root_id.size()) ||
                !add_snapshot_string_bytes(total_bytes, candidate.root_name.size()) ||
                !add_snapshot_string_bytes(total_bytes, candidate.root_icon.size())) {
                return SAO_ERR_INVALID_ARGUMENT;
            }
            candidate.string_bytes = total_bytes;
            candidate.content_token = compute_provider_content_token(candidate);
            out = std::move(candidate);
            return SAO_OK;
        }

        if (required_stride < kEntityMenuRowV2RequiredPrefixSize)
            return SAO_PLUGINS_ERR_ABI_MISMATCH;
        if (required_stride % alignof(entity_menu_row_v2) != 0)
            return SAO_ERR_INVALID_ARGUMENT;
        size_t raw_row_bytes = 0;
        if (!checked_raw_row_bytes(required_count, required_stride, raw_row_bytes))
            return SAO_ERR_INVALID_ARGUMENT;

        const size_t storage_elements =
            (raw_row_bytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t);
        std::vector<std::max_align_t> raw_storage(storage_elements);
        auto* raw_rows = reinterpret_cast<std::byte*>(raw_storage.data());
        for (uint32_t row_index = 0; row_index < required_count; ++row_index) {
            entity_menu_row_v2 initial{};
            initial.struct_size = sizeof(entity_menu_row_v2);
            std::memcpy(raw_rows + static_cast<size_t>(row_index) * required_stride, &initial,
                        sizeof(initial));
        }

        uint32_t written_count = required_count;
        uint64_t second_revision = 0;
        entity_snapshot_content_token_t second_content_token = kInvalidEntitySnapshotContentToken;
        uint32_t written_stride = 0;
        status = call_snapshot_v2(state->snapshot_v2, raw_rows, required_count, required_stride,
                                  &written_count, &second_revision, &second_content_token,
                                  &written_stride, state->snapshot_user_data);
        if (status != SAO_OK && status != SAO_ERR_BUFFER_TOO_SMALL)
            return status;
        if (status == SAO_ERR_BUFFER_TOO_SMALL || written_count != required_count ||
            second_revision != first_revision || second_content_token != first_content_token ||
            written_stride != required_stride) {
            continue;
        }

        owned_provider_snapshot candidate;
        candidate.provider_id = state->provider_id;
        candidate.owner_plugin_id = state->owner_plugin_id;
        candidate.generation = state->generation;
        candidate.revision = second_revision;
        candidate.snapshot_abi_version = kEntitySnapshotAbiVersion2;
        candidate.has_root_contribution = state->has_root_contribution;
        candidate.contribution_id = state->contribution_id;
        candidate.root_id = state->root_id;
        candidate.root_name = state->root_name;
        candidate.root_icon = state->root_icon;
        candidate.root_priority = state->root_priority;
        candidate.rows.reserve(written_count);
        size_t total_bytes = 0;
        if (!add_snapshot_string_bytes(total_bytes, candidate.provider_id.size()) ||
            !add_snapshot_string_bytes(total_bytes, candidate.owner_plugin_id.size()) ||
            !add_snapshot_string_bytes(total_bytes, candidate.contribution_id.size()) ||
            !add_snapshot_string_bytes(total_bytes, candidate.root_id.size()) ||
            !add_snapshot_string_bytes(total_bytes, candidate.root_name.size()) ||
            !add_snapshot_string_bytes(total_bytes, candidate.root_icon.size())) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        for (uint32_t row_index = 0; row_index < written_count; ++row_index) {
            const auto* source = raw_rows + static_cast<size_t>(row_index) * written_stride;
            uint32_t struct_size = 0;
            std::memcpy(&struct_size, source, sizeof(struct_size));
            if (struct_size < kEntityMenuRowV2RequiredPrefixSize || struct_size > written_stride) {
                return SAO_PLUGINS_ERR_ABI_MISMATCH;
            }
            entity_menu_row_v2 row{};
            std::memcpy(&row, source, (std::min)(static_cast<size_t>(struct_size), sizeof(row)));
            owned_entity_row copied;
            status = copy_row_v2(row, total_bytes, copied);
            if (status != SAO_OK)
                return status;
            candidate.rows.push_back(std::move(copied));
        }
        candidate.string_bytes = total_bytes;
        candidate.content_token = compute_provider_content_token(candidate);
        out = std::move(candidate);
        return SAO_OK;
    }
    return SAO_PLUGINS_ERR_BUSY;
}

int32_t copy_provider_snapshot_for_catalog(const std::shared_ptr<entity_provider_state>& state,
                                           owned_provider_snapshot& out) {
    switch (state->snapshot_abi_version) {
    case kEntitySnapshotAbiVersion1:
        return copy_provider_snapshot(state, out);
    case kEntitySnapshotAbiVersion2:
        return copy_provider_snapshot_v2(state, out);
    default:
        return SAO_PLUGINS_ERR_ABI_MISMATCH;
    }
}

} // namespace

#if defined(SAO_PLUGINS_LOADER_TESTING)
entity_provider_test_counters entity_provider_get_counters_for_testing() noexcept {
    std::lock_guard lock(g_catalog_mutex);
    return {g_next_generation.load(std::memory_order_relaxed), g_catalog_revision};
}

void entity_provider_set_counters_for_testing(entity_provider_test_counters counters) noexcept {
    std::lock_guard lock(g_catalog_mutex);
    g_next_generation.store(counters.next_generation, std::memory_order_relaxed);
    g_catalog_revision = counters.catalog_revision;
}
#endif

namespace {

int32_t register_entity_provider_core(
    const std::shared_ptr<plugin_handle_s>& owner, const std::string& owner_plugin_id,
    const char* provider_id_utf8, entity_snapshot_callback_fn snapshot_v1,
    entity_snapshot_callback_v2_fn snapshot_v2, entity_action_handler_fn action_handler_v1,
    entity_action_handler_v2_fn action_handler_v2, void* snapshot_user_data, void* action_user_data,
    const entity_root_contribution_descriptor* root_contribution,
    std::shared_ptr<entity_provider_state>& out) noexcept {
    out.reset();
    if (owner == nullptr || owner_plugin_id.empty() || provider_id_utf8 == nullptr ||
        (snapshot_v1 == nullptr) == (snapshot_v2 == nullptr) ||
        (action_handler_v1 == nullptr) == (action_handler_v2 == nullptr)) {
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
        if (!allocate_generation(state->generation))
            return SAO_ERR_OS_CALL_FAILED;
        state->snapshot_abi_version =
            snapshot_v1 != nullptr ? kEntitySnapshotAbiVersion1 : kEntitySnapshotAbiVersion2;
        state->snapshot_v1 = snapshot_v1;
        state->snapshot_v2 = snapshot_v2;
        state->action_handler_v1 = action_handler_v1;
        state->action_handler_v2 = action_handler_v2;
        state->snapshot_user_data = snapshot_user_data;
        state->action_user_data = action_user_data;
        if (root_contribution != nullptr) {
            entity_root_contribution_descriptor current_root{};
            status = copy_external_struct(root_contribution,
                                          kEntityRootContributionDescriptorRequiredPrefixSize,
                                          current_root);
            if (status != SAO_OK)
                return status;
            if (!std::isfinite(current_root.priority)) {
                return SAO_ERR_INVALID_ARGUMENT;
            }
            size_t root_bytes = 0;
            status =
                copy_bounded_string(current_root.contribution_id_utf8, true,
                                    kMaximumProviderIdBytes, root_bytes, state->contribution_id);
            if (status == SAO_OK) {
                status = copy_bounded_string(current_root.root_id_utf8, true, kMaximumRootIdBytes,
                                             root_bytes, state->root_id);
            }
            if (status == SAO_OK) {
                status = copy_bounded_string(current_root.name_utf8, true, kMaximumStringBytes,
                                             root_bytes, state->root_name);
            }
            if (status == SAO_OK) {
                status = copy_bounded_string(current_root.icon_utf8, false, kMaximumStringBytes,
                                             root_bytes, state->root_icon);
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
            state->root_priority = current_root.priority;
            state->has_root_contribution = true;
        }
        {
            std::lock_guard catalog_lock(g_catalog_mutex);
            if (g_attached.contains(state->provider_id)) {
                return SAO_PLUGINS_ERR_ALREADY_EXISTS;
            }
            if (g_attached.size() >= kMaximumAttachedEntityProviders)
                return SAO_ERR_INVALID_ARGUMENT;
            g_attached.emplace(state->provider_id, state);
        }
        out = std::move(state);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

} // namespace

int32_t register_entity_provider(const std::shared_ptr<plugin_handle_s>& owner,
                                 const std::string& owner_plugin_id, const char* provider_id_utf8,
                                 entity_snapshot_callback_fn snapshot,
                                 entity_action_handler_fn action_handler, void* user_data,
                                 const entity_root_contribution_descriptor* root_contribution,
                                 std::shared_ptr<entity_provider_state>& out) noexcept {
    return register_entity_provider_core(owner, owner_plugin_id, provider_id_utf8, snapshot,
                                         nullptr, action_handler, nullptr, user_data, user_data,
                                         root_contribution, out);
}

int32_t register_entity_provider_v2(const std::shared_ptr<plugin_handle_s>& owner,
                                    const std::string& owner_plugin_id,
                                    const char* provider_id_utf8,
                                    entity_snapshot_callback_v2_fn snapshot,
                                    entity_action_handler_fn action_handler, void* user_data,
                                    const entity_root_contribution_descriptor* root_contribution,
                                    std::shared_ptr<entity_provider_state>& out) noexcept {
    return register_entity_provider_core(owner, owner_plugin_id, provider_id_utf8, nullptr,
                                         snapshot, action_handler, nullptr, user_data, user_data,
                                         root_contribution, out);
}

int32_t register_entity_provider_v3(const std::shared_ptr<plugin_handle_s>& owner,
                                    const std::string& owner_plugin_id,
                                    const char* provider_id_utf8,
                                    entity_snapshot_callback_v2_fn snapshot,
                                    entity_action_handler_v2_fn action_handler,
                                    void* snapshot_user_data, void* action_user_data,
                                    const entity_root_contribution_descriptor* root_contribution,
                                    std::shared_ptr<entity_provider_state>& out) noexcept {
    return register_entity_provider_core(owner, owner_plugin_id, provider_id_utf8, nullptr,
                                         snapshot, nullptr, action_handler, snapshot_user_data,
                                         action_user_data, root_contribution, out);
}

int32_t replace_entity_provider_action_v2(const std::shared_ptr<entity_provider_state>& provider,
                                          entity_action_handler_v2_fn action_handler,
                                          void* action_user_data) noexcept {
    if (provider == nullptr || action_handler == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    if (std::find(g_current_providers.begin(), g_current_providers.end(), provider.get()) !=
        g_current_providers.end()) {
        return SAO_PLUGINS_ERR_BUSY;
    }
    try {
        std::unique_lock callback_lock(provider->callback_mutex, std::try_to_lock);
        if (!callback_lock.owns_lock())
            return SAO_PLUGINS_ERR_BUSY;
        std::lock_guard lock(provider->mutex);
        if (provider->destroyed)
            return SAO_ERR_HANDLE_INVALID;
        provider->action_handler_v1 = nullptr;
        provider->action_handler_v2 = action_handler;
        provider->action_user_data = action_user_data;
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

        size_t additions = 0;
        for (const auto& provider : ordered) {
            if (!g_catalog.contains(provider->provider_id))
                ++additions;
        }
        if (g_catalog.size() > kMaximumEntityProvidersPerCatalog ||
            additions > kMaximumEntityProvidersPerCatalog - g_catalog.size()) {
            return SAO_ERR_INVALID_ARGUMENT;
        }

        auto candidate = g_catalog;
        candidate.reserve(candidate.size() + additions);
        bool inserted = false;
        for (const auto& provider : ordered) {
            if (!candidate.contains(provider->provider_id)) {
                candidate.emplace(provider->provider_id, provider);
                inserted = true;
            }
        }
        if (inserted && g_catalog_revision == (std::numeric_limits<uint64_t>::max)())
            return SAO_ERR_OS_CALL_FAILED;
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
    std::vector<std::shared_ptr<entity_provider_state>> ordered;
    std::vector<uint8_t> accepting_states;
    std::vector<std::unique_lock<std::mutex>> provider_locks;
    bool accepting_changed = false;
    try {
        if (entity_provider_is_current_thread(providers)) {
            return SAO_PLUGINS_ERR_BUSY;
        }

        ordered = providers;
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

        accepting_states.reserve(ordered.size());
        provider_locks.reserve(ordered.size());

        for (const auto& provider : ordered)
            provider_locks.emplace_back(provider->mutex);
        for (const auto& provider : ordered) {
            accepting_states.push_back(static_cast<uint8_t>(provider->accepting));
            provider->accepting = false;
        }
        accepting_changed = true;
        for (auto& lock : provider_locks)
            lock.unlock();
        const auto rundown_deadline = std::chrono::steady_clock::now() + kRundownTimeout;
        for (const auto& provider : ordered) {
            std::unique_lock provider_lock(provider->mutex);
            if (!provider->idle.wait_until(provider_lock, rundown_deadline,
                                           [&provider] { return provider->in_flight == 0; })) {
                provider_lock.unlock();
                for (auto& lock : provider_locks)
                    lock.lock();
                for (size_t index = 0; index < ordered.size(); ++index)
                    ordered[index]->accepting = accepting_states[index] != 0;
                accepting_changed = false;
                return SAO_PLUGINS_ERR_BUSY;
            }
        }
        {
            for (auto& lock : provider_locks)
                lock.lock();

            std::lock_guard catalog_lock(g_catalog_mutex);
            auto candidate = g_catalog;
            bool removed = false;
            for (const auto& provider : ordered) {
                const auto found = candidate.find(provider->provider_id);
                if (found != candidate.end() && found->second == provider) {
                    candidate.erase(found);
                    removed = true;
                }
            }
            if (removed && g_catalog_revision == (std::numeric_limits<uint64_t>::max)()) {
                for (size_t index = 0; index < ordered.size(); ++index)
                    ordered[index]->accepting = accepting_states[index] != 0;
                accepting_changed = false;
                return SAO_ERR_OS_CALL_FAILED;
            }
            for (const auto& provider : ordered)
                provider->published = false;
            g_catalog.swap(candidate);
            if (removed)
                ++g_catalog_revision;
        }
        accepting_changed = false;
        return SAO_OK;
    } catch (...) {
        if (accepting_changed && ordered.size() == accepting_states.size()) {
            try {
                for (auto& lock : provider_locks) {
                    if (!lock.owns_lock())
                        lock.lock();
                }
                for (size_t index = 0; index < ordered.size(); ++index)
                    ordered[index]->accepting = accepting_states[index] != 0;
            } catch (...) {
                OutputDebugStringA(
                    "SAO loader: failed to restore Entity provider accepting state\n");
            }
        }
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
            provider->snapshot_v1 = nullptr;
            provider->snapshot_v2 = nullptr;
            provider->action_handler_v1 = nullptr;
            provider->action_handler_v2 = nullptr;
            provider->snapshot_user_data = nullptr;
            provider->action_user_data = nullptr;
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
                if (g_catalog.size() > kMaximumEntityProvidersPerCatalog) {
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
                const int32_t status = copy_provider_snapshot_for_catalog(provider, snapshot);
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

extern "C" int32_t SAO_PLUGINS_CALL sao_plugins_entity_provider_snapshot_v2(
    entity_provider_catalog_callback_v2 callback, void* user_data) {
    if (callback == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        for (uint32_t attempt = 0; attempt < kMaximumSnapshotAttempts; ++attempt) {
            uint64_t catalog_revision = 0;
            std::vector<std::shared_ptr<entity_provider_state>> providers;
            {
                std::lock_guard lock(g_catalog_mutex);
                if (g_catalog.size() > kMaximumEntityProvidersPerCatalog) {
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
                const int32_t status = copy_provider_snapshot_for_catalog(provider, snapshot);
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

            std::vector<std::vector<entity_menu_row_v2>> row_views;
            std::vector<entity_provider_view_v2> provider_views;
            std::vector<std::vector<entity_root_action_ref_view_v2>> root_action_views;
            std::vector<entity_root_contribution_view_v2> root_views;
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
                        sizeof(entity_menu_row_v2),
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
                    sizeof(entity_provider_view_v2),
                    snapshot.snapshot_abi_version,
                    snapshot.provider_id.c_str(),
                    snapshot.owner_plugin_id.c_str(),
                    snapshot.generation,
                    snapshot.revision,
                    snapshot.content_token,
                    static_cast<uint32_t>(rows.size()),
                    sizeof(entity_menu_row_v2),
                    rows.empty() ? nullptr : rows.data(),
                });
                if (snapshot.has_root_contribution) {
                    std::vector<entity_root_action_ref_view_v2> actions;
                    actions.reserve(snapshot.rows.size());
                    for (const auto& row : snapshot.rows) {
                        actions.push_back({
                            sizeof(entity_root_action_ref_view_v2),
                            snapshot.provider_id.c_str(),
                            row.action_id.c_str(),
                        });
                    }
                    root_action_views.push_back(std::move(actions));
                    const auto& stored_actions = root_action_views.back();
                    root_views.push_back({
                        sizeof(entity_root_contribution_view_v2),
                        snapshot.owner_plugin_id.c_str(),
                        snapshot.contribution_id.c_str(),
                        snapshot.root_id.c_str(),
                        snapshot.root_name.c_str(),
                        snapshot.root_icon.c_str(),
                        snapshot.root_priority,
                        static_cast<uint32_t>(stored_actions.size()),
                        sizeof(entity_root_action_ref_view_v2),
                        stored_actions.empty() ? nullptr : stored_actions.data(),
                    });
                }
            }
            const entity_provider_catalog_view_v2 catalog{
                sizeof(entity_provider_catalog_view_v2),
                kEntitySnapshotAbiVersion2,
                catalog_revision,
                compute_catalog_content_token(snapshots),
                static_cast<uint32_t>(provider_views.size()),
                sizeof(entity_provider_view_v2),
                provider_views.empty() ? nullptr : provider_views.data(),
                static_cast<uint32_t>(root_views.size()),
                sizeof(entity_root_contribution_view_v2),
                root_views.empty() ? nullptr : root_views.data(),
            };
            const int32_t callback_status = call_catalog_v2(callback, &catalog, user_data);
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

namespace {

int32_t invoke_entity_provider_action(const char* provider_id_utf8, uint64_t expected_generation,
                                      const char* action_id_utf8, const char* payload_json_utf8,
                                      entity_action_result_callback_v2_fn callback,
                                      void* callback_user_data, bool legacy_call) {
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
        if (!valid_json_syntax(payload_json))
            return SAO_ERR_INVALID_ARGUMENT;

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

        owned_action_result_v2 result;
        if (provider->action_handler_v1 != nullptr) {
            status = call_action(provider->action_handler_v1, action_id.c_str(),
                                 payload_json.c_str(), provider->action_user_data);
            if (status != SAO_OK)
                return status;
            result.handled = true;
        } else if (provider->action_handler_v2 != nullptr) {
            status =
                invoke_action_v2_handler(provider->action_handler_v2, action_id.c_str(),
                                         payload_json.c_str(), provider->action_user_data, result);
            if (status != SAO_OK)
                return status;
        } else {
            return SAO_ERR_HANDLE_INVALID;
        }

        if (legacy_call)
            return result.handled ? SAO_OK : SAO_PLUGINS_ERR_NOT_FOUND;

        const entity_action_result_v2 view{
            sizeof(entity_action_result_v2),
            kEntityActionAbiVersion2,
            static_cast<uint8_t>(result.handled),
            {},
            result.has_result ? result.result_json.c_str() : nullptr,
        };
        return call_action_result(callback, &view, callback_user_data);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

} // namespace

extern "C" int32_t SAO_PLUGINS_CALL
sao_plugins_entity_provider_invoke(const char* provider_id_utf8, uint64_t expected_generation,
                                   const char* action_id_utf8, const char* payload_json_utf8) {
    return invoke_entity_provider_action(provider_id_utf8, expected_generation, action_id_utf8,
                                         payload_json_utf8, nullptr, nullptr, true);
}

extern "C" int32_t SAO_PLUGINS_CALL sao_plugins_entity_provider_invoke_v2(
    const char* provider_id_utf8, uint64_t expected_generation, const char* action_id_utf8,
    const char* payload_json_utf8, entity_action_result_callback_v2_fn callback, void* user_data) {
    if (callback == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    return invoke_entity_provider_action(provider_id_utf8, expected_generation, action_id_utf8,
                                         payload_json_utf8, callback, user_data, false);
}

} // namespace sao::plugins::loader
