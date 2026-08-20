#include "workshop_panel_internal.h"

#include "sao/server/freetier/workshop_client/workshop_client.h"
#include "sao_core/sao_status.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace sao::launcher::workshop_panel {
namespace {

using json = nlohmann::json;

constexpr std::size_t kMaximumTaskQueue = 32;
constexpr std::size_t kMaximumActionPayloadBytes = 1024;
constexpr std::size_t kMaximumPanelSpecBytes = 128U * 1024U;
constexpr std::size_t kMaximumBasePathBytes = 24U * 1024U;
constexpr std::size_t kMaximumPluginIdBytes = 63;
constexpr std::size_t kMaximumNameBytes = 127;
constexpr std::size_t kMaximumVersionBytes = 31;
constexpr std::size_t kMaximumTagBytes = 63;
constexpr std::size_t kMaximumAuthorBytes = 63;
constexpr std::size_t kMaximumDescriptionBytes = 1023;
constexpr std::size_t kMaximumSignatureAlgorithmBytes = 15;
constexpr std::uint32_t kMaximumPage = 1'000'000;
constexpr std::uint32_t kMaximumTotal = 1'000'000;
constexpr std::size_t kDownloadPathCapacity = 32U * 1024U;

bool valid_utf8(std::string_view value) noexcept {
    std::size_t offset = 0;
    while (offset < value.size()) {
        const auto first = static_cast<unsigned char>(value[offset]);
        if (first <= 0x7fU) {
            ++offset;
            continue;
        }
        std::size_t continuation_count = 0;
        std::uint32_t code_point = 0;
        if ((first & 0xe0U) == 0xc0U) {
            continuation_count = 1;
            code_point = first & 0x1fU;
        } else if ((first & 0xf0U) == 0xe0U) {
            continuation_count = 2;
            code_point = first & 0x0fU;
        } else if ((first & 0xf8U) == 0xf0U) {
            continuation_count = 3;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (offset + continuation_count >= value.size())
            return false;
        for (std::size_t index = 1; index <= continuation_count; ++index) {
            const auto next = static_cast<unsigned char>(value[offset + index]);
            if ((next & 0xc0U) != 0x80U)
                return false;
            code_point = (code_point << 6U) | (next & 0x3fU);
        }
        const bool overlong = (continuation_count == 1 && code_point < 0x80U) ||
                              (continuation_count == 2 && code_point < 0x800U) ||
                              (continuation_count == 3 && code_point < 0x10000U);
        if (overlong || code_point > 0x10ffffU ||
            (code_point >= 0xd800U && code_point <= 0xdfffU)) {
            return false;
        }
        offset += continuation_count + 1;
    }
    return true;
}

bool valid_string(std::string_view value, std::size_t maximum, bool required) noexcept {
    return (!required || !value.empty()) && value.size() <= maximum &&
           value.find('\0') == std::string_view::npos && valid_utf8(value);
}

std::optional<std::string_view> bounded_c_text(const char* value,
                                               std::size_t maximum) noexcept {
    if (value == nullptr)
        return std::nullopt;
    const void* terminator = std::memchr(value, '\0', maximum + 1U);
    if (terminator == nullptr)
        return std::nullopt;
    const auto length =
        static_cast<std::size_t>(static_cast<const char*>(terminator) - value);
    const std::string_view result(value, length);
    return valid_string(result, maximum, true) ? std::optional<std::string_view>(result)
                                                : std::nullopt;
}

bool valid_plugin_id(std::string_view id) noexcept {
    if (!valid_string(id, kMaximumPluginIdBytes, true) || id == "." || id == ".." ||
        id.starts_with('.') || id.ends_with('.') || id.find("..") != std::string_view::npos) {
        return false;
    }
    return std::ranges::all_of(id, [](unsigned char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '.' || character == '-' ||
               character == '_';
    });
}

bool valid_sha256(std::string_view value) noexcept {
    return value.size() == 64 && std::ranges::all_of(value, [](unsigned char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f') ||
                      (character >= 'A' && character <= 'F');
           });
}

sao_status_t validate_summary(const PluginSummary& summary) noexcept {
    if (!valid_plugin_id(summary.id) || !valid_string(summary.name, kMaximumNameBytes, true) ||
        !valid_string(summary.version, kMaximumVersionBytes, true) ||
        !valid_string(summary.tag, kMaximumTagBytes, false) ||
        !valid_string(summary.author, kMaximumAuthorBytes, false) || summary.rating > 500) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    return SAO_STATUS_OK;
}

sao_status_t validate_page(const PluginPage& page, std::uint32_t requested_page,
                           std::uint32_t requested_size) noexcept {
    if (requested_page == 0 || requested_page > kMaximumPage || requested_size == 0 ||
        requested_size > 100 || page.page != requested_page || page.size != requested_size ||
        page.total > kMaximumTotal || page.items.size() > requested_size ||
        page.items.size() > page.total) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    for (const auto& item : page.items) {
        const sao_status_t status = validate_summary(item);
        if (status != SAO_STATUS_OK)
            return status;
    }
    return SAO_STATUS_OK;
}

sao_status_t validate_detail(const PluginDetail& detail, std::string_view requested_id) noexcept {
    const sao_status_t summary_status = validate_summary(detail.summary);
    if (summary_status != SAO_STATUS_OK || detail.summary.id != requested_id ||
        !valid_string(detail.description, kMaximumDescriptionBytes, false) ||
        !valid_sha256(detail.sha256_hex) ||
        !valid_string(detail.signature_algorithm, kMaximumSignatureAlgorithmBytes, true)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    return SAO_STATUS_OK;
}

std::string path_to_utf8(const std::filesystem::path& path) {
    const std::u8string value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::filesystem::path path_from_utf8(std::string_view value) {
    std::u8string converted;
    converted.reserve(value.size());
    for (const char character : value)
        converted.push_back(static_cast<char8_t>(static_cast<unsigned char>(character)));
    return std::filesystem::path(converted);
}

bool valid_base_dir(const std::filesystem::path& base_dir) {
    if (base_dir.empty() || !base_dir.is_absolute())
        return false;
    const std::string value = path_to_utf8(base_dir);
    return valid_string(value, kMaximumBasePathBytes, true);
}

std::string bounded_text(std::string value, std::size_t maximum) {
    if (value.size() <= maximum)
        return value;
    if (maximum <= 3U)
        return value.substr(0, maximum);
    std::size_t end = maximum - 3U;
    while (end > 0U && (static_cast<unsigned char>(value[end]) & 0xc0U) == 0x80U)
        --end;
    value.resize(end);
    value.append("...");
    return value;
}

bool path_is_within(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    if (!root.is_absolute() || !candidate.is_absolute())
        return false;
    const auto normalized_root = root.lexically_normal();
    const auto normalized_candidate = candidate.lexically_normal();
    auto root_part = normalized_root.begin();
    auto candidate_part = normalized_candidate.begin();
    for (; root_part != normalized_root.end(); ++root_part, ++candidate_part) {
        if (candidate_part == normalized_candidate.end() || *root_part != *candidate_part)
            return false;
    }
    return true;
}

template <std::size_t Capacity> std::string copy_c_field(const char (&value)[Capacity]) {
    const auto* end = static_cast<const char*>(std::memchr(value, '\0', Capacity));
    return std::string(value, end == nullptr ? Capacity : static_cast<std::size_t>(end - value));
}

PluginSummary copy_summary(const sao_workshop_plugin_summary_t& source) {
    PluginSummary result;
    result.id = copy_c_field(source.id);
    result.name = copy_c_field(source.name);
    result.version = copy_c_field(source.version);
    result.tag = copy_c_field(source.tag);
    result.author = copy_c_field(source.author);
    result.updated_ms = source.updated_ms;
    result.rating = source.rating;
    result.downloads = source.downloads;
    return result;
}

PluginDetail copy_detail(const sao_workshop_plugin_detail_t& source) {
    PluginDetail result;
    result.summary = copy_summary(source.summary);
    result.description = copy_c_field(source.description);
    result.sha256_hex = copy_c_field(source.sha256_hex);
    result.signature_algorithm = copy_c_field(source.signature_alg);
    result.size_bytes = source.size_bytes;
    result.min_client_version_major = source.min_client_version_major;
    result.min_client_version_minor = source.min_client_version_minor;
    result.min_client_version_patch = source.min_client_version_patch;
    return result;
}

sao_status_t map_workshop_status(std::int32_t status) noexcept {
    switch (status) {
    case SAO_OK:
        return SAO_STATUS_OK;
    case SAO_ERR_INVALID_ARGUMENT:
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    case SAO_ERR_NOT_INITIALIZED:
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    case SAO_ERR_HANDLE_INVALID:
        return SAO_STATUS_ERR_HANDLE_INVALID;
    case SAO_ERR_BUFFER_TOO_SMALL:
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    case SAO_ERR_OS_CALL_FAILED:
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    case SAO_ERR_NOT_IMPLEMENTED:
        return SAO_STATUS_ERR_NOT_IMPLEMENTED;
    case SAO_ERR_NOT_FOUND:
        return SAO_STATUS_ERR_NOT_FOUND;
    case SAO_ERR_ACCESS_DENIED:
        return SAO_STATUS_ERR_ACCESS_DENIED;
    case SAO_ERR_READ_FAULT:
        return SAO_STATUS_ERR_READ_FAULT;
    default:
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

std::string status_text(sao_status_t status) {
    switch (status) {
    case SAO_STATUS_OK:
        return "OK";
    case SAO_STATUS_ERR_INVALID_ARGUMENT:
        return "invalid data";
    case SAO_STATUS_ERR_NOT_INITIALIZED:
        return "not initialized";
    case SAO_STATUS_ERR_HANDLE_INVALID:
        return "invalid package";
    case SAO_STATUS_ERR_BUFFER_TOO_SMALL:
        return "buffer limit exceeded";
    case SAO_STATUS_ERR_NOT_IMPLEMENTED:
        return "operation unavailable";
    case SAO_STATUS_ERR_TIMEOUT:
        return "timed out";
    case SAO_STATUS_ERR_CANCELLED:
        return "cancelled";
    case SAO_STATUS_ERR_CAPABILITY_MISSING:
        return "capability missing";
    case SAO_STATUS_ERR_OS_CALL_FAILED:
        return "system operation failed";
    case SAO_STATUS_ERR_ACCESS_DENIED:
        return "access denied";
    case SAO_STATUS_ERR_NOT_FOUND:
        return "not found";
    case SAO_STATUS_ERR_ALREADY_EXISTS:
        return "already exists";
    default:
        return "unknown error";
    }
}

json text_node(std::string text, std::string_view style = "value", std::int32_t height = 24) {
    return json{{"type", "text"}, {"text", bounded_text(std::move(text), 4096U)}, {"style", style}, {"height", height}};
}

json badge_node(std::string text, std::string_view style) {
    return json{{"type", "badge"}, {"text", bounded_text(std::move(text), 256U)}, {"style", style}, {"height", 22}};
}

json row_node(json children) {
    return json{{"type", "row"}, {"align", "left"}, {"children", std::move(children)}};
}

json card_node(std::string title, json children) {
    return json{{"type", "card"},
                {"title", bounded_text(std::move(title), 512U)},
                {"children", std::move(children)}};
}

json section_node(std::string title, json children) {
    return json{{"type", "section"},
                {"title", bounded_text(std::move(title), 512U)},
                {"children", std::move(children)}};
}

json button_node(std::string id, std::string label, std::string action, json payload,
                 std::string_view style = "default", bool disabled = false) {
    json node{{"type", "button"},
              {"id", std::move(id)},
              {"label", bounded_text(std::move(label), 256U)},
              {"action", std::move(action)},
              {"style", style},
              {"height", 28}};
    if (!payload.empty())
        node["payload"] = std::move(payload);
    if (disabled)
        node["disabled"] = true;
    return node;
}

std::string format_rating(std::uint32_t rating) {
    return std::to_string(rating / 100U) + "." + std::to_string((rating / 10U) % 10U) + " / 5";
}

std::string format_bytes(std::uint64_t size) {
    constexpr std::uint64_t kib = 1024;
    constexpr std::uint64_t mib = kib * 1024;
    if (size >= mib)
        return std::to_string(size / mib) + " MiB";
    if (size >= kib)
        return std::to_string(size / kib) + " KiB";
    return std::to_string(size) + " B";
}

bool next_page_available(std::uint32_t page, std::uint32_t size, std::uint32_t total) noexcept {
    return static_cast<std::uint64_t>(page) * size < total;
}

sao_status_t parse_plugin_id(std::string_view payload_json, std::string& id_out) {
    if (payload_json.empty() || payload_json.size() > kMaximumActionPayloadBytes ||
        !valid_utf8(payload_json) || payload_json.find('\0') != std::string_view::npos) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const json payload = json::parse(payload_json, nullptr, false, false);
    if (payload.is_discarded() || !payload.is_object())
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const auto id = payload.find("id");
    if (id == payload.end() || !id->is_string())
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    id_out = id->get<std::string>();
    return valid_plugin_id(id_out) ? SAO_STATUS_OK : SAO_STATUS_ERR_INVALID_ARGUMENT;
}

} // namespace

bool item_count_within_capacity_for_testing(std::uint32_t item_count,
                                            std::uint32_t item_cap,
                                            std::uint32_t requested_size) noexcept {
    return item_count <= item_cap && item_count <= requested_size;
}

bool Operations::complete() const noexcept {
    return static_cast<bool>(list) && static_cast<bool>(detail) && static_cast<bool>(download) &&
           static_cast<bool>(verify) && static_cast<bool>(install) && static_cast<bool>(uninstall);
}

Operations make_production_operations() {
    Operations operations;
    operations.list = [](std::stop_token stop, std::uint32_t page, std::uint32_t size,
                         PluginPage& output) -> sao_status_t {
        if (stop.stop_requested())
            return SAO_STATUS_ERR_CANCELLED;
        std::vector<sao_workshop_plugin_summary_t> items(size);
        sao_workshop_page_t raw{};
        raw.items = items.data();
        raw.item_cap = size;
        const sao_status_t status =
            map_workshop_status(sao_workshop_list_plugins(page, size, nullptr, &raw));
        if (status != SAO_STATUS_OK || stop.stop_requested())
            return stop.stop_requested() ? SAO_STATUS_ERR_CANCELLED : status;
        if (!item_count_within_capacity_for_testing(raw.item_count, raw.item_cap, size) ||
            raw.item_count > items.size()) {
            return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        }
        PluginPage candidate;
        candidate.page = raw.page;
        candidate.size = raw.size;
        candidate.total = raw.total;
        candidate.items.reserve(raw.item_count);
        for (std::uint32_t index = 0; index < raw.item_count; ++index)
            candidate.items.push_back(copy_summary(items[index]));
        output = std::move(candidate);
        return SAO_STATUS_OK;
    };
    operations.detail = [](std::stop_token stop, std::string_view id,
                           PluginDetail& output) -> sao_status_t {
        if (stop.stop_requested())
            return SAO_STATUS_ERR_CANCELLED;
        const std::string stable_id(id);
        sao_workshop_plugin_detail_t raw{};
        const sao_status_t status =
            map_workshop_status(sao_workshop_get_plugin_detail(stable_id.c_str(), &raw));
        if (status != SAO_STATUS_OK || stop.stop_requested())
            return stop.stop_requested() ? SAO_STATUS_ERR_CANCELLED : status;
        output = copy_detail(raw);
        return SAO_STATUS_OK;
    };
    operations.download = [](std::stop_token stop, std::string_view id,
                             const std::filesystem::path& target_dir,
                             std::filesystem::path& output) -> sao_status_t {
        if (stop.stop_requested())
            return SAO_STATUS_ERR_CANCELLED;
        std::error_code error;
        std::filesystem::create_directories(target_dir, error);
        if (error)
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        const std::string stable_id(id);
        const std::string target = path_to_utf8(target_dir);
        std::vector<char> path_buffer(kDownloadPathCapacity);
        const sao_status_t status = map_workshop_status(sao_workshop_download_plugin(
            stable_id.c_str(), target.c_str(), path_buffer.data(), path_buffer.size()));
        if (status != SAO_STATUS_OK || stop.stop_requested())
            return stop.stop_requested() ? SAO_STATUS_ERR_CANCELLED : status;
        const auto terminator = std::ranges::find(path_buffer, '\0');
        const std::size_t length = static_cast<std::size_t>(terminator - path_buffer.begin());
        if (length == 0 || terminator == path_buffer.end())
            return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        output = path_from_utf8(std::string_view(path_buffer.data(), length));
        return SAO_STATUS_OK;
    };
    operations.verify = [](std::stop_token stop, const std::filesystem::path& package,
                           std::string_view id) -> sao_status_t {
        if (stop.stop_requested())
            return SAO_STATUS_ERR_CANCELLED;
        const std::string package_utf8 = path_to_utf8(package);
        const std::string stable_id(id);
        const sao_status_t status = map_workshop_status(
            sao_workshop_verify_plugin(package_utf8.c_str(), stable_id.c_str()));
        return stop.stop_requested() ? SAO_STATUS_ERR_CANCELLED : status;
    };
    operations.install = [](std::stop_token stop, const std::filesystem::path& package,
                            const std::filesystem::path& plugins_dir) -> sao_status_t {
        if (stop.stop_requested())
            return SAO_STATUS_ERR_CANCELLED;
        std::error_code error;
        std::filesystem::create_directories(plugins_dir, error);
        if (error)
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        const std::string package_utf8 = path_to_utf8(package);
        const std::string plugins_utf8 = path_to_utf8(plugins_dir);
        const sao_status_t status = map_workshop_status(
            sao_workshop_install_plugin(package_utf8.c_str(), plugins_utf8.c_str()));
        return stop.stop_requested() ? SAO_STATUS_ERR_CANCELLED : status;
    };
    operations.uninstall = [](std::stop_token stop, std::string_view id,
                              const std::filesystem::path& plugins_dir,
                              bool keep_backup) -> sao_status_t {
        if (stop.stop_requested())
            return SAO_STATUS_ERR_CANCELLED;
        const std::string stable_id(id);
        const std::string plugins_utf8 = path_to_utf8(plugins_dir);
        const sao_status_t status = map_workshop_status(sao_workshop_uninstall_plugin(
            stable_id.c_str(), plugins_utf8.c_str(), keep_backup ? 1 : 0));
        return stop.stop_requested() ? SAO_STATUS_ERR_CANCELLED : status;
    };
    return operations;
}

const char* theme_override_json() noexcept {
    return nullptr;
}

SaoPanelDescriptor panel_descriptor() noexcept {
    SaoPanelDescriptor descriptor{};
    descriptor.struct_size = sizeof(SaoPanelDescriptor);
    descriptor.panel_id_utf8 = kPanelId;
    descriptor.title_utf8 = kPanelTitle;
    descriptor.anchor = SAO_UI_PANEL_ANCHOR_CENTER;
    descriptor.default_width_px = 980;
    descriptor.default_height_px = 760;
    descriptor.min_width_px = 680;
    descriptor.min_height_px = 480;
    descriptor.movable = true;
    descriptor.resizable = true;
    descriptor.show_titlebar = true;
    descriptor.show_close_button = true;
    descriptor.visible = false;
    descriptor.remember_geometry = true;
    descriptor.z_class = SAO_UI_PANEL_Z_NORMAL;
    descriptor.theme_override_json_utf8 = nullptr;
    descriptor.initial_opacity = 1.0F;
    descriptor.auto_scroll = true;
    return descriptor;
}

class Owner::Impl final {
  public:
    static std::mutex deferred_mutex_;
    static std::vector<std::unique_ptr<Impl>> deferred_cleanup_;

  public:
    enum class TaskKind {
        List,
        Detail,
        Install,
        Uninstall,
    };

    struct Task {
        TaskKind kind{TaskKind::List};
        std::uint64_t sequence{};
        std::uint32_t page{1};
        std::string id;
    };

    struct Completion {
        Task task;
        sao_status_t status{SAO_STATUS_OK};
        std::string progress;
        std::uint32_t progress_percent{};
        bool terminal{};
        PluginPage page;
        std::optional<PluginDetail> detail;
    };

    Impl(sao_ui_compositor_handle_t compositor, std::filesystem::path base_dir,
         Operations operations)
        : compositor_(compositor), base_dir_(std::move(base_dir)),
          plugins_dir_(base_dir_ / "plugins"), cache_dir_(base_dir_ / "cache" / "workshop"),
          operations_(std::move(operations)) {
        try {
            base_dir_ = base_dir_.lexically_normal();
            plugins_dir_ = base_dir_ / "plugins";
            cache_dir_ = base_dir_ / "cache" / "workshop";
            if (compositor_ == nullptr || !valid_base_dir(base_dir_) || !operations_.complete()) {
                startup_status_ = SAO_STATUS_ERR_INVALID_ARGUMENT;
                accepting_ = false;
                online_ = false;
                return;
            }
            worker_ = std::jthread([this](std::stop_token stop) { worker_main(stop); });
            online_ = true;
        } catch (...) {
            startup_status_ = SAO_STATUS_ERR_OS_CALL_FAILED;
            accepting_ = false;
            online_ = false;
        }
    }

    ~Impl() {
        stop_and_join();
    }

    bool shutdown_noexcept() noexcept {
        if (panel_handle() == nullptr) {
            stop_and_join();
            return true;
        }
        if (require_owner_thread() != SAO_STATUS_OK)
            return false;
        bool worker_stopped = false;
        for (int attempt = 0; attempt < 1 && panel_handle() != nullptr; ++attempt) {
            if (claim_retirement() != SAO_STATUS_OK)
                return false;
            if (!worker_stopped) {
                stop_and_join();
                worker_stopped = true;
            }
            const sao_status_t status = retire_panel_claimed();
            release_retirement_claim(status);
            if (status == SAO_STATUS_OK)
                break;
        }
        return panel_handle() == nullptr;
    }

    static void defer_cleanup(std::unique_ptr<Impl> state) noexcept;
    static void drain_deferred_cleanup() noexcept;

    sao_status_t open() {
        const sao_status_t owner_status = require_owner_thread();
        if (owner_status != SAO_STATUS_OK)
            return owner_status;
        {
            std::lock_guard lock(mutex_);
            if (startup_status_ != SAO_STATUS_OK)
                return startup_status_;
            if (!online_ || !accepting_)
                return SAO_STATUS_ERR_NOT_INITIALIZED;
        }
        sao_status_t status = ensure_panel();
        if (status != SAO_STATUS_OK)
            return status;
        const sao_ui_panel_handle_t panel = panel_handle();
        if (panel == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        const bool was_visible = query_visible(panel);
        status = sao_ui_panel_show(panel);
        if (status == SAO_STATUS_OK)
            status = sao_ui_panel_bring_to_front(panel);
        if (status != SAO_STATUS_OK)
            return status;
        status = sync_visibility(panel);
        if (status != SAO_STATUS_OK)
            return status;
        if (!was_visible)
            return enqueue_list(1);
        return SAO_STATUS_OK;
    }

    sao_status_t hide() {
        const sao_status_t owner_status = require_owner_thread();
        if (owner_status != SAO_STATUS_OK)
            return owner_status;
        const sao_ui_panel_handle_t panel = panel_handle();
        if (panel == nullptr)
            return SAO_STATUS_OK;
        const sao_status_t status = sao_ui_panel_hide(panel);
        if (status == SAO_STATUS_OK)
            return sync_visibility(panel);
        return status;
    }

    sao_status_t service_ui() {
        const sao_status_t owner_status = require_owner_thread();
        if (owner_status != SAO_STATUS_OK)
            return owner_status;
        sao_ui_panel_handle_t panel = nullptr;
        {
            std::lock_guard lock(mutex_);
            if (!online_ && panel_ == nullptr)
                return SAO_STATUS_ERR_NOT_INITIALIZED;
            if (retiring_)
                return SAO_UI_PANEL_STATUS_ERR_BUSY;
            panel = panel_;
        }
        if (panel == nullptr)
            return SAO_STATUS_OK;
        sao_status_t status = sync_visibility(panel);
        if (status != SAO_STATUS_OK)
            return status;
        if (!owns_panel(panel))
            return SAO_STATUS_OK;
        apply_completions();
        bool hide_requested = false;
        {
            std::lock_guard lock(mutex_);
            hide_requested = hide_requested_;
            hide_requested_ = false;
        }
        if (hide_requested) {
            const sao_status_t hide_status = sao_ui_panel_hide(panel);
            if (hide_status != SAO_STATUS_OK)
                return hide_status;
            status = sync_visibility(panel);
            if (status != SAO_STATUS_OK)
                return status;
            if (!owns_panel(panel))
                return SAO_STATUS_OK;
        }
        const sao_status_t publish_status = publish_spec(false);
        if (publish_status != SAO_STATUS_OK)
            return publish_status;
        return sync_visibility(panel);
    }

    sao_status_t try_take_offline() {
        const sao_status_t owner_status = require_owner_thread();
        if (owner_status != SAO_STATUS_OK)
            return owner_status;
        const sao_status_t claim_status = claim_retirement();
        if (claim_status != SAO_STATUS_OK)
            return claim_status;
        stop_and_join();
        const sao_status_t status = retire_panel_claimed();
        release_retirement_claim(status);
        return status;
    }

    void set_visibility_changed_callback(VisibilityChangedCallback callback) {
        VisibilityChangedCallback notify;
        bool visible = false;
        {
            std::lock_guard lock(mutex_);
            visibility_callback_ = std::move(callback);
            if (visibility_callback_ && visibility_known_) {
                notify = visibility_callback_;
                visible = visible_;
            }
        }
        invoke_visibility_callback(notify, visible);
    }

    sao_status_t dispatch_action(std::string_view action, std::string_view payload) {
        if (!valid_string(action, 64, true) || payload.size() > kMaximumActionPayloadBytes ||
            payload.find('\0') != std::string_view::npos || !valid_utf8(payload)) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        if (action == "workshop.refresh") {
            std::uint32_t page = 1;
            {
                std::lock_guard lock(mutex_);
                page = current_page_;
            }
            return enqueue_list(page);
        }
        if (action == "workshop.page.previous") {
            std::uint32_t page = 1;
            {
                std::lock_guard lock(mutex_);
                if (current_page_ <= 1)
                    return SAO_STATUS_ERR_NOT_FOUND;
                page = current_page_ - 1;
            }
            return enqueue_list(page);
        }
        if (action == "workshop.page.next") {
            std::uint32_t page = 1;
            {
                std::lock_guard lock(mutex_);
                if (!next_page_available(current_page_, page_size_, total_))
                    return SAO_STATUS_ERR_NOT_FOUND;
                page = current_page_ + 1;
            }
            return enqueue_list(page);
        }
        if (action == "workshop.close") {
            return request_hide();
        }

        std::string id;
        const sao_status_t payload_status = parse_plugin_id(payload, id);
        if (payload_status != SAO_STATUS_OK)
            return payload_status;
        if (action == "workshop.plugin.detail")
            return enqueue_task(Task{TaskKind::Detail, 0, 0, std::move(id)});
        if (action == "workshop.plugin.install")
            return enqueue_task(Task{TaskKind::Install, 0, 0, std::move(id)});
        if (action == "workshop.plugin.uninstall")
            return enqueue_task(Task{TaskKind::Uninstall, 0, 0, std::move(id)});
        return SAO_STATUS_ERR_NOT_FOUND;
    }

    sao_status_t dispatch_action_for_testing(std::string_view action,
                                             std::string_view payload_json) {
        CallbackLease lease(this);
        return lease ? dispatch_action(action, payload_json) : SAO_UI_PANEL_STATUS_ERR_BUSY;
    }

    sao_status_t dispatch_panel_event_for_testing(std::int32_t event_kind) {
        CallbackLease lease(this);
        return lease ? handle_panel_event(event_kind) : SAO_UI_PANEL_STATUS_ERR_BUSY;
    }

    void fail_next_unregister_for_testing(sao_status_t status) noexcept {
        std::lock_guard lock(mutex_);
        fail_next_unregister_status_ = status;
    }

    Snapshot snapshot() const {
        std::lock_guard lock(mutex_);
        Snapshot result;
        result.panel = panel_;
        result.online = online_;
        result.visible = visible_;
        result.busy = worker_active_ || !tasks_.empty();
        result.page = current_page_;
        result.page_size = page_size_;
        result.total = total_;
        result.queued_operations = queued_operations_;
        result.completed_operations = completed_operations_;
        result.last_status = last_status_;
        result.status_text = status_text_;
        result.progress_text = progress_text_;
        result.error_text = error_text_;
        result.last_spec = last_spec_;
        result.items = items_;
        result.detail = detail_;
        return result;
    }

  private:
    struct CallbackLease final {
        explicit CallbackLease(Impl* candidate) noexcept : state(candidate) {
            if (state == nullptr)
                return;
            std::lock_guard lock(state->mutex_);
            if (!state->callback_accepting_ || state->retiring_ || state->panel_ == nullptr)
                return;
            ++state->callbacks_in_flight_;
            active = true;
        }

        ~CallbackLease() {
            if (!active)
                return;
            std::lock_guard lock(state->mutex_);
            --state->callbacks_in_flight_;
        }

        explicit operator bool() const noexcept {
            return active;
        }

        Impl* state{};
        bool active{};
    };

    static void SAO_UI_CALL panel_action_callback(const char* action_key,
                                                  const std::uint8_t* payload,
                                                  std::size_t payload_len, void* user_data) {
        auto* self = static_cast<Impl*>(user_data);
        const auto action = bounded_c_text(action_key, 64U);
        CallbackLease lease(self);
        if (!lease || !action.has_value() || (payload == nullptr && payload_len != 0U))
            return;
        try {
            const std::string_view payload_view(
                payload == nullptr ? "" : reinterpret_cast<const char*>(payload), payload_len);
            (void)self->dispatch_action(*action, payload_view);
        } catch (...) {
        }
    }

    static void SAO_UI_CALL panel_event_callback(std::int32_t event_kind, void* user_data) {
        auto* self = static_cast<Impl*>(user_data);
        CallbackLease lease(self);
        if (!lease)
            return;
        (void)self->handle_panel_event(event_kind);
    }

    sao_status_t handle_panel_event(std::int32_t event_kind) noexcept {
        if (event_kind == SAO_UI_PANEL_EVENT_CLOSE)
            return request_hide();
        return SAO_STATUS_OK;
    }

    sao_status_t request_hide() noexcept {
        std::lock_guard lock(mutex_);
        if (!online_ || panel_ == nullptr)
            return SAO_STATUS_ERR_NOT_INITIALIZED;
        hide_requested_ = true;
        return SAO_STATUS_OK;
    }

    sao_status_t require_owner_thread() const noexcept {
        if (compositor_ == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        return sao_ui_compositor_require_owner_thread(compositor_);
    }

    sao_status_t ensure_panel() {
        {
            std::lock_guard lock(mutex_);
            if (panel_ != nullptr)
                return panel_ready_ ? SAO_STATUS_OK : SAO_STATUS_ERR_NOT_INITIALIZED;
        }
        const SaoPanelDescriptor descriptor = panel_descriptor();
        sao_ui_panel_handle_t panel = nullptr;
        sao_ui_panel_body_handle_t body = nullptr;
        sao_status_t status = sao_ui_panel_register(compositor_, &descriptor, &panel, &body);
        if (status != SAO_STATUS_OK)
            return status;
        {
            std::lock_guard lock(mutex_);
            panel_ = panel;
            body_ = body;
            panel_ready_ = false;
            action_handler_attached_ = false;
            event_handler_attached_ = false;
            callback_accepting_ = false;
        }
        status = sao_ui_panel_set_action_handler(panel, &panel_action_callback, this);
        if (status == SAO_STATUS_OK) {
            std::lock_guard lock(mutex_);
            action_handler_attached_ = true;
        }
        if (status == SAO_STATUS_OK)
            status = sao_ui_panel_set_event_handler(panel, &panel_event_callback, this);
        if (status == SAO_STATUS_OK) {
            std::lock_guard lock(mutex_);
            event_handler_attached_ = true;
            callback_accepting_ = true;
        }
        if (status == SAO_STATUS_OK)
            status = publish_spec(true);
        if (status == SAO_STATUS_OK) {
            std::lock_guard lock(mutex_);
            if (panel_ != panel)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            panel_ready_ = true;
            return SAO_STATUS_OK;
        }
        if (status != SAO_STATUS_OK) {
            const sao_status_t event_status =
                sao_ui_panel_set_event_handler(panel, nullptr, nullptr);
            if (panel_is_gone(event_status)) {
                clear_retired_panel(panel);
                return status;
            }
            if (event_status == SAO_STATUS_OK) {
                std::lock_guard lock(mutex_);
                if (panel_ == panel) {
                    event_handler_attached_ = false;
                    callback_accepting_ = false;
                }
            }
            const sao_status_t action_status =
                sao_ui_panel_set_action_handler(panel, nullptr, nullptr);
            if (panel_is_gone(action_status)) {
                clear_retired_panel(panel);
                return status;
            }
            if (action_status == SAO_STATUS_OK) {
                std::lock_guard lock(mutex_);
                if (panel_ == panel) {
                    action_handler_attached_ = false;
                    callback_accepting_ = false;
                }
            }
            const sao_status_t unregister_status = sao_ui_panel_unregister(panel);
            if (unregister_status == SAO_STATUS_OK || panel_is_gone(unregister_status))
                clear_retired_panel(panel);
            return status;
        }
        return status;
    }

    sao_ui_panel_handle_t panel_handle() const noexcept {
        std::lock_guard lock(mutex_);
        return panel_;
    }

    bool owns_panel(sao_ui_panel_handle_t panel) const noexcept {
        std::lock_guard lock(mutex_);
        return panel_ == panel;
    }

    static bool query_visible(sao_ui_panel_handle_t panel) noexcept {
        if (panel == nullptr)
            return false;
        SaoPanelState state{};
        return sao_ui_panel_get_state(panel, &state) == SAO_STATUS_OK && state.visible;
    }

    static void invoke_visibility_callback(const VisibilityChangedCallback& callback,
                                           bool visible) noexcept {
        if (!callback)
            return;
        try {
            callback(visible);
        } catch (...) {
        }
    }

    sao_status_t sync_visibility(sao_ui_panel_handle_t panel) {
        if (panel == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        SaoPanelState state{};
        const sao_status_t status = sao_ui_panel_get_state(panel, &state);
        if (status != SAO_STATUS_OK)
            return status;
        VisibilityChangedCallback callback;
        bool changed = false;
        {
            std::lock_guard lock(mutex_);
            if (panel_ != panel)
                return SAO_STATUS_OK;
            changed = !visibility_known_ || visible_ != state.visible;
            visibility_known_ = true;
            visible_ = state.visible;
            if (changed)
                callback = visibility_callback_;
        }
        if (changed)
            invoke_visibility_callback(callback, state.visible);
        return SAO_STATUS_OK;
    }

    sao_status_t enqueue_list(std::uint32_t page) {
        if (page == 0 || page > kMaximumPage)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        return enqueue_task(Task{TaskKind::List, 0, page, {}});
    }

    sao_status_t enqueue_task(Task task) {
        {
            std::lock_guard lock(mutex_);
            if (!accepting_ || !online_ || startup_status_ != SAO_STATUS_OK)
                return SAO_STATUS_ERR_NOT_INITIALIZED;
            if (tasks_.size() >= kMaximumTaskQueue)
                return SAO_UI_PANEL_STATUS_ERR_BUSY;
            task.sequence = next_sequence_++;
            tasks_.push_back(std::move(task));
            ++queued_operations_;
            status_text_ = "Operation queued";
            progress_text_ = "Waiting to start";
            progress_percent_ = 0;
            error_text_.clear();
            dirty_ = true;
        }
        worker_cv_.notify_all();
        return SAO_STATUS_OK;
    }

    void worker_main(std::stop_token stop) noexcept {
        while (!stop.stop_requested()) {
            Task task;
            {
                std::unique_lock lock(mutex_);
                const bool ready =
                    worker_cv_.wait(lock, stop, [this] { return !tasks_.empty() || !accepting_; });
                if (!ready || stop.stop_requested() || !accepting_)
                    break;
                task = std::move(tasks_.front());
                tasks_.pop_front();
                worker_active_ = true;
            }
            run_task(stop, task);
            {
                std::lock_guard lock(mutex_);
                worker_active_ = false;
            }
        }
    }

    void push_progress(const Task& task, std::string progress, std::uint32_t percent) {
        Completion completion;
        completion.task = task;
        completion.progress = std::move(progress);
        completion.progress_percent = std::min(percent, 100U);
        std::lock_guard lock(mutex_);
        completions_.push_back(std::move(completion));
    }

    void push_terminal(Completion completion) {
        completion.terminal = true;
        std::lock_guard lock(mutex_);
        completions_.push_back(std::move(completion));
    }

    void run_task(std::stop_token stop, const Task& task) noexcept {
        Completion completion;
        completion.task = task;
        try {
            if (stop.stop_requested()) {
                completion.status = SAO_STATUS_ERR_CANCELLED;
                push_terminal(std::move(completion));
                return;
            }
            switch (task.kind) {
            case TaskKind::List:
                push_progress(task, "Loading workshop page " + std::to_string(task.page), 20);
                completion.status = operations_.list(stop, task.page, kPageSize, completion.page);
                if (completion.status == SAO_STATUS_OK)
                    completion.status = validate_page(completion.page, task.page, kPageSize);
                break;
            case TaskKind::Detail: {
                push_progress(task, "Loading detail for " + task.id, 35);
                PluginDetail detail;
                completion.status = operations_.detail(stop, task.id, detail);
                if (completion.status == SAO_STATUS_OK)
                    completion.status = validate_detail(detail, task.id);
                if (completion.status == SAO_STATUS_OK)
                    completion.detail = std::move(detail);
                break;
            }
            case TaskKind::Install: {
                std::filesystem::path package;
                push_progress(task, "Downloading " + task.id, 20);
                completion.status = operations_.download(stop, task.id, cache_dir_, package);
                if (completion.status != SAO_STATUS_OK)
                    break;
                if (stop.stop_requested()) {
                    completion.status = SAO_STATUS_ERR_CANCELLED;
                    break;
                }
                if (!path_is_within(cache_dir_, package)) {
                    completion.status = SAO_STATUS_ERR_ACCESS_DENIED;
                    break;
                }
                push_progress(task, "Verifying signature and payload id for " + task.id, 60);
                completion.status = operations_.verify(stop, package, task.id);
                if (completion.status != SAO_STATUS_OK)
                    break;
                if (stop.stop_requested()) {
                    completion.status = SAO_STATUS_ERR_CANCELLED;
                    break;
                }
                push_progress(task, "Installing " + task.id, 85);
                completion.status = operations_.install(stop, package, plugins_dir_);
                break;
            }
            case TaskKind::Uninstall:
                push_progress(task, "Uninstalling " + task.id, 50);
                completion.status = operations_.uninstall(stop, task.id, plugins_dir_, true);
                break;
            }
        } catch (...) {
            completion.status = SAO_STATUS_ERR_OS_CALL_FAILED;
        }
        if (stop.stop_requested() && completion.status == SAO_STATUS_OK)
            completion.status = SAO_STATUS_ERR_CANCELLED;
        push_terminal(std::move(completion));
    }

    void apply_completions() {
        std::deque<Completion> local;
        {
            std::lock_guard lock(mutex_);
            local.swap(completions_);
        }
        if (local.empty())
            return;
        std::lock_guard lock(mutex_);
        while (!local.empty()) {
            Completion completion = std::move(local.front());
            local.pop_front();
            progress_text_ = std::move(completion.progress);
            progress_percent_ = completion.progress_percent;
            if (!completion.terminal) {
                dirty_ = true;
                continue;
            }
            ++completed_operations_;
            last_status_ = completion.status;
            progress_percent_ = completion.status == SAO_STATUS_OK ? 100U : progress_percent_;
            if (completion.status != SAO_STATUS_OK) {
                error_text_ =
                    operation_name(completion.task) + " failed: " + status_text(completion.status);
                status_text_ = "Workshop operation failed";
                dirty_ = true;
                continue;
            }
            error_text_.clear();
            switch (completion.task.kind) {
            case TaskKind::List:
                current_page_ = completion.page.page;
                page_size_ = completion.page.size;
                total_ = completion.page.total;
                items_ = std::move(completion.page.items);
                detail_.reset();
                status_text_ = "Loaded " + std::to_string(items_.size()) + " plugin entries";
                progress_text_ = "Catalog is up to date";
                break;
            case TaskKind::Detail:
                detail_ = std::move(completion.detail);
                status_text_ = "Detail loaded for " + completion.task.id;
                progress_text_ = "Plugin metadata is ready";
                break;
            case TaskKind::Install:
                status_text_ = "Installed " + completion.task.id;
                progress_text_ = "Download, verification, and installation completed";
                break;
            case TaskKind::Uninstall:
                status_text_ = "Uninstalled " + completion.task.id;
                progress_text_ = "Plugin directory moved to its backup";
                break;
            }
            dirty_ = true;
        }
    }

    static std::string operation_name(const Task& task) {
        switch (task.kind) {
        case TaskKind::List:
            return "List";
        case TaskKind::Detail:
            return "Detail";
        case TaskKind::Install:
            return "Download / verify / install";
        case TaskKind::Uninstall:
            return "Uninstall";
        }
        return "Workshop operation";
    }
    std::string build_spec() const {
        json nodes = json::array();
        const bool busy = worker_active_ || !tasks_.empty();
        json controls = json::array();
        json status_badges = json::array();
        status_badges.push_back(badge_node(online_ ? "Online" : "Offline", online_ ? "ok" : "bad"));
        status_badges.push_back(badge_node("Page " + std::to_string(current_page_), "accent"));
        status_badges.push_back(badge_node(std::to_string(total_) + " plugins", "gold"));
        controls.push_back(row_node(std::move(status_badges)));
        json navigation = json::array();
        navigation.push_back(button_node("workshop.refresh", error_text_.empty() ? "Refresh" : "Retry",
                                         "workshop.refresh", json::object(), "primary", busy));
        navigation.push_back(button_node("workshop.previous", "Previous", "workshop.page.previous",
                                         json::object(), "default", busy || current_page_ <= 1));
        navigation.push_back(button_node("workshop.next", "Next", "workshop.page.next", json::object(),
                                         "default", busy || !next_page_available(current_page_, page_size_, total_)));
        navigation.push_back(button_node("workshop.close", "Close", "workshop.close",
                                         json::object(), "ghost", false));
        controls.push_back(row_node(std::move(navigation)));
        nodes.push_back(section_node("Controls", std::move(controls)));

        json catalog = json::array();
        if (items_.empty()) {
            catalog.push_back(text_node(busy ? "Loading plugins..." : "No plugins on this page.", "muted", 38));
            if (!busy)
                catalog.push_back(button_node("workshop.catalog-retry", "Refresh", "workshop.refresh",
                                              json::object(), "primary"));
        } else {
            for (std::size_t index = 0; index < items_.size(); ++index) {
                const PluginSummary& item = items_[index];
                json details = json::array();
                json metadata = json::array();
                metadata.push_back(badge_node("v" + item.version, "accent"));
                metadata.push_back(badge_node(item.tag.empty() ? "untagged" : item.tag, "muted"));
                metadata.push_back(badge_node(item.author.empty() ? item.id : item.author, "muted"));
                details.push_back(row_node(std::move(metadata)));
                json metrics = json::array();
                metrics.push_back(badge_node(format_rating(item.rating), "gold"));
                metrics.push_back(badge_node(std::to_string(item.downloads) + " downloads", "accent"));
                details.push_back(row_node(std::move(metrics)));
                const json payload{{"id", item.id}};
                json actions = json::array();
                actions.push_back(button_node("detail." + std::to_string(index), "View",
                                              "workshop.plugin.detail", payload, "default", busy));
                actions.push_back(button_node("install." + std::to_string(index), "Install",
                                              "workshop.plugin.install", payload, "primary", busy));
                actions.push_back(button_node("uninstall." + std::to_string(index), "Remove",
                                              "workshop.plugin.uninstall", payload, "danger", busy));
                details.push_back(row_node(std::move(actions)));
                catalog.push_back(card_node(item.name + " · v" + item.version, std::move(details)));
            }
        }
        nodes.push_back(section_node("Catalog", std::move(catalog)));

        json detail_children = json::array();
        if (detail_.has_value()) {
            const PluginDetail& detail = *detail_;
            detail_children.push_back(row_node(json::array({
                badge_node("v" + detail.summary.version, "accent"),
                badge_node(detail.summary.tag.empty() ? "untagged" : detail.summary.tag, "muted"),
                badge_node(detail.summary.author.empty() ? detail.summary.id : detail.summary.author, "muted")})));
            detail_children.push_back(text_node(detail.description.empty() ? "No description supplied."
                                                                            : bounded_text(detail.description, 640U),
                                                "value", 72));
            detail_children.push_back(text_node("Package: " + format_bytes(detail.size_bytes) + " · " +
                                                    detail.signature_algorithm, "muted", 28));
            detail_children.push_back(text_node("Minimum client: " + std::to_string(detail.min_client_version_major) +
                              "." + std::to_string(detail.min_client_version_minor) + "." +
                              std::to_string(detail.min_client_version_patch), "muted", 24));
            detail_children.push_back(text_node("SHA-256 " + detail.sha256_hex, "mono", 30));
            const json payload{{"id", detail.summary.id}};
            json detail_actions = json::array();
            detail_actions.push_back(button_node("detail.install", "Install", "workshop.plugin.install",
                                                 payload, "primary", busy));
            detail_actions.push_back(button_node("detail.remove", "Remove", "workshop.plugin.uninstall",
                                                 payload, "danger", busy));
            detail_children.push_back(row_node(std::move(detail_actions)));
            nodes.push_back(section_node("Detail", json::array({card_node(detail.summary.name,
                                                                            std::move(detail_children))} )));
        } else {
            detail_children.push_back(text_node("Select a plugin to review its package and compatibility details.",
                                                "muted", 52));
            nodes.push_back(section_node("Detail", std::move(detail_children)));
        }

        json task_center = json::array();
        json task_badges = json::array();
        task_badges.push_back(badge_node(busy ? "In progress" : "Ready", busy ? "warn" : "ok"));
        task_badges.push_back(badge_node(std::to_string(completed_operations_) + " completed", "accent"));
        const std::uint64_t remaining = queued_operations_ > completed_operations_
                                            ? queued_operations_ - completed_operations_ : 0;
        task_badges.push_back(badge_node(std::to_string(remaining) + " pending", "gold"));
        task_center.push_back(row_node(std::move(task_badges)));
        task_center.push_back(text_node(status_text_, last_status_ == SAO_STATUS_OK ? "value" : "bad", 30));
        task_center.push_back(json{{"type", "bar"}, {"pct", progress_percent_},
                                   {"caption", progress_text_}, {"height", 24}});
        if (!error_text_.empty())
            task_center.push_back(text_node(error_text_, "bad", 42));
        nodes.push_back(section_node("Task Center", std::move(task_center)));
        std::string spec = json{{"version", 1}, {"title", ""}, {"nodes", std::move(nodes)}}.dump();
        if (spec.size() <= kMaximumPanelSpecBytes)
            return spec;
        return json{{"version", 1}, {"title", ""},
                    {"nodes", json::array({section_node("Spec Limit",
                        json::array({text_node("Workshop content is too large to display.", "bad", 44)}))})}}.dump();
    }
    sao_status_t publish_spec(bool force) {
        sao_ui_panel_body_handle_t body = nullptr;
        std::string spec;
        {
            std::lock_guard lock(mutex_);
            body = body_;
            if (body == nullptr)
                return SAO_STATUS_ERR_NOT_INITIALIZED;
            if (!force && !dirty_)
                return SAO_STATUS_OK;
            spec = build_spec();
            if (!force && spec == last_spec_) {
                dirty_ = false;
                return SAO_STATUS_OK;
            }
        }
        const sao_status_t status = sao_ui_panel_body_set_spec(
            body, reinterpret_cast<const std::uint8_t*>(spec.data()), spec.size());
        if (status != SAO_STATUS_OK)
            return status;
        std::lock_guard lock(mutex_);
        if (body_ != body)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        last_spec_ = std::move(spec);
        dirty_ = false;
        return SAO_STATUS_OK;
    }

    void stop_and_join() noexcept {
        {
            std::lock_guard lock(mutex_);
            accepting_ = false;
            online_ = false;
            tasks_.clear();
        }
        worker_cv_.notify_all();
        if (worker_.joinable()) {
            worker_.request_stop();
            worker_cv_.notify_all();
            worker_.join();
        }
    }

    static bool panel_is_gone(sao_status_t status) noexcept {
        return status == SAO_STATUS_ERR_HANDLE_INVALID || status == SAO_STATUS_ERR_NOT_FOUND;
    }

    void clear_retired_panel(sao_ui_panel_handle_t panel) noexcept {
        VisibilityChangedCallback callback;
        bool notify = false;
        {
            std::lock_guard lock(mutex_);
            if (panel_ != panel)
                return;
            panel_ = nullptr;
            body_ = nullptr;
            panel_ready_ = false;
            action_handler_attached_ = false;
            event_handler_attached_ = false;
            callback_accepting_ = false;
            hide_requested_ = false;
            notify = visibility_known_ && visible_;
            visibility_known_ = true;
            visible_ = false;
            if (notify)
                callback = visibility_callback_;
        }
        if (notify)
            invoke_visibility_callback(callback, false);
    }

    sao_status_t restore_handlers(sao_ui_panel_handle_t panel, bool restore_action,
                                  bool restore_event) noexcept {
        sao_status_t first_status = SAO_STATUS_OK;
        bool action_attached = !restore_action;
        bool event_attached = !restore_event;
        if (restore_action) {
            const sao_status_t status =
                sao_ui_panel_set_action_handler(panel, &panel_action_callback, this);
            if (panel_is_gone(status)) {
                clear_retired_panel(panel);
                return SAO_STATUS_OK;
            }
            action_attached = status == SAO_STATUS_OK;
            if (status != SAO_STATUS_OK)
                first_status = status;
        }
        if (restore_event) {
            const sao_status_t status =
                sao_ui_panel_set_event_handler(panel, &panel_event_callback, this);
            if (panel_is_gone(status)) {
                clear_retired_panel(panel);
                return SAO_STATUS_OK;
            }
            event_attached = status == SAO_STATUS_OK;
            if (status != SAO_STATUS_OK && first_status == SAO_STATUS_OK)
                first_status = status;
        }
        {
            std::lock_guard lock(mutex_);
            if (panel_ == panel) {
                if (restore_action)
                    action_handler_attached_ = action_attached;
                if (restore_event)
                    event_handler_attached_ = event_attached;
                callback_accepting_ = action_handler_attached_ && event_handler_attached_;
            }
        }
        return first_status;
    }

    sao_status_t claim_retirement() noexcept {
        std::lock_guard lock(mutex_);
        if (retiring_ || callbacks_in_flight_ != 0U)
            return SAO_UI_PANEL_STATUS_ERR_BUSY;
        retiring_ = true;
        callback_accepting_ = false;
        return SAO_STATUS_OK;
    }

    void release_retirement_claim(sao_status_t status) noexcept {
        std::lock_guard lock(mutex_);
        retiring_ = false;
        if (status != SAO_STATUS_OK && panel_ != nullptr && action_handler_attached_ &&
            event_handler_attached_) {
            callback_accepting_ = true;
        }
    }

    sao_status_t retire_panel_claimed() noexcept {
        const sao_ui_panel_handle_t panel = panel_handle();
        if (panel == nullptr)
            return SAO_STATUS_OK;

        sao_status_t status = sao_ui_panel_hide(panel);
        if (panel_is_gone(status)) {
            clear_retired_panel(panel);
            return SAO_STATUS_OK;
        }
        if (status != SAO_STATUS_OK)
            return status;
        status = sync_visibility(panel);
        if (panel_is_gone(status)) {
            clear_retired_panel(panel);
            return SAO_STATUS_OK;
        }
        if (status != SAO_STATUS_OK)
            return status;

        bool detach_action = false;
        bool detach_event = false;
        {
            std::lock_guard lock(mutex_);
            if (panel_ != panel)
                return SAO_STATUS_OK;
            detach_action = action_handler_attached_;
            detach_event = event_handler_attached_;
        }

        bool event_detached = false;
        if (detach_event) {
            status = sao_ui_panel_set_event_handler(panel, nullptr, nullptr);
            if (panel_is_gone(status)) {
                clear_retired_panel(panel);
                return SAO_STATUS_OK;
            }
            if (status != SAO_STATUS_OK)
                return status;
            event_detached = true;
            std::lock_guard lock(mutex_);
            if (panel_ == panel)
                event_handler_attached_ = false;
        }

        bool action_detached = false;
        if (detach_action) {
            status = sao_ui_panel_set_action_handler(panel, nullptr, nullptr);
            if (panel_is_gone(status)) {
                clear_retired_panel(panel);
                return SAO_STATUS_OK;
            }
            if (status != SAO_STATUS_OK) {
                const sao_status_t rollback = restore_handlers(panel, false, event_detached);
                if (!owns_panel(panel))
                    return SAO_STATUS_OK;
                return rollback == SAO_STATUS_OK ? status : SAO_UI_PANEL_STATUS_ERR_ROLLBACK_FAILED;
            }
            action_detached = true;
            std::lock_guard lock(mutex_);
            if (panel_ == panel)
                action_handler_attached_ = false;
        }

        sao_status_t injected = SAO_STATUS_OK;
        {
            std::lock_guard lock(mutex_);
            injected = std::exchange(fail_next_unregister_status_, SAO_STATUS_OK);
        }
        const bool unregister_called = injected == SAO_STATUS_OK;
        status = unregister_called ? sao_ui_panel_unregister(panel) : injected;
        if (status == SAO_STATUS_OK || (unregister_called && panel_is_gone(status))) {
            clear_retired_panel(panel);
            return SAO_STATUS_OK;
        }

        const sao_status_t rollback = restore_handlers(panel, action_detached, event_detached);
        if (!owns_panel(panel))
            return SAO_STATUS_OK;
        return rollback == SAO_STATUS_OK ? status : SAO_UI_PANEL_STATUS_ERR_ROLLBACK_FAILED;
    }

    sao_ui_compositor_handle_t compositor_{};
    std::filesystem::path base_dir_;
    std::filesystem::path plugins_dir_;
    std::filesystem::path cache_dir_;
    Operations operations_;
    mutable std::mutex mutex_;
    std::condition_variable_any worker_cv_;
    std::deque<Task> tasks_;
    std::deque<Completion> completions_;
    std::jthread worker_;
    sao_status_t startup_status_{SAO_STATUS_OK};
    sao_ui_panel_handle_t panel_{};
    sao_ui_panel_body_handle_t body_{};
    sao_status_t fail_next_unregister_status_{SAO_STATUS_OK};
    VisibilityChangedCallback visibility_callback_;
    std::vector<PluginSummary> items_;
    std::optional<PluginDetail> detail_;
    std::string status_text_{"Ready to browse the workshop"};
    std::string progress_text_{"Open the panel to load the catalog"};
    std::string error_text_;
    std::string last_spec_;
    std::uint64_t next_sequence_{1};
    std::uint64_t queued_operations_{};
    std::uint64_t completed_operations_{};
    std::uint32_t current_page_{1};
    std::uint32_t page_size_{kPageSize};
    std::uint32_t total_{};
    std::uint32_t progress_percent_{};
    sao_status_t last_status_{SAO_STATUS_OK};
    bool accepting_{true};
    bool online_{};
    bool visible_{};
    bool visibility_known_{};
    bool worker_active_{};
    bool hide_requested_{};
    bool panel_ready_{};
    bool retiring_{};
    bool action_handler_attached_{};
    bool event_handler_attached_{};
    bool callback_accepting_{};
    bool dirty_{true};
    std::size_t callbacks_in_flight_{};
};

std::mutex Owner::Impl::deferred_mutex_;
std::vector<std::unique_ptr<Owner::Impl>> Owner::Impl::deferred_cleanup_;

void Owner::Impl::defer_cleanup(std::unique_ptr<Impl> state) noexcept { if (state != nullptr) { std::lock_guard lock(deferred_mutex_); deferred_cleanup_.push_back(std::move(state)); } }
void Owner::Impl::drain_deferred_cleanup() noexcept { std::vector<std::unique_ptr<Impl>> pending; { std::lock_guard lock(deferred_mutex_); pending.swap(deferred_cleanup_); } std::vector<std::unique_ptr<Impl>> retry; for (auto& state : pending) { const sao_status_t status = state->try_take_offline(); if (status != SAO_STATUS_OK) retry.push_back(std::move(state)); } if (!retry.empty()) { std::lock_guard lock(deferred_mutex_); for (auto& state : retry) deferred_cleanup_.push_back(std::move(state)); } }

void Owner::drain_deferred_cleanup_for_owner() noexcept { Impl::drain_deferred_cleanup(); }

void Owner::drain_deferred_cleanup_for_testing() noexcept { drain_deferred_cleanup_for_owner(); }

Owner::Owner(sao_ui_compositor_handle_t compositor, std::filesystem::path base_dir)
    : Owner(compositor, std::move(base_dir), make_production_operations()) {}

Owner::Owner(sao_ui_compositor_handle_t compositor, std::filesystem::path base_dir,
             Operations operations)
    : impl_(std::make_unique<Impl>(compositor, std::move(base_dir), std::move(operations))) {}

Owner::~Owner() {
    if (impl_ == nullptr)
        return;
    auto state = std::move(impl_);
    if (!state->shutdown_noexcept())
        Impl::defer_cleanup(std::move(state));
}

sao_status_t Owner::open() {
    return impl_ == nullptr ? SAO_STATUS_ERR_NOT_INITIALIZED : impl_->open();
}

sao_status_t Owner::hide() {
    return impl_ == nullptr ? SAO_STATUS_ERR_NOT_INITIALIZED : impl_->hide();
}

sao_status_t Owner::service_ui() {
    return impl_ == nullptr ? SAO_STATUS_ERR_NOT_INITIALIZED : impl_->service_ui();
}

sao_status_t Owner::tick() {
    return service_ui();
}

sao_status_t Owner::try_take_offline() {
    return impl_ == nullptr ? SAO_STATUS_OK : impl_->try_take_offline();
}

void Owner::set_visibility_changed_callback(VisibilityChangedCallback callback) {
    if (impl_ != nullptr)
        impl_->set_visibility_changed_callback(std::move(callback));
}

sao_status_t Owner::dispatch_action_for_testing(std::string_view action,
                                                std::string_view payload_json) {
    if (impl_ == nullptr)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    try {
        return impl_->dispatch_action_for_testing(action, payload_json);
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

sao_status_t Owner::dispatch_panel_event_for_testing(std::int32_t event_kind) {
    return impl_ == nullptr ? SAO_STATUS_ERR_NOT_INITIALIZED
                            : impl_->dispatch_panel_event_for_testing(event_kind);
}

void Owner::fail_next_unregister_for_testing(sao_status_t status) {
    if (impl_ != nullptr)
        impl_->fail_next_unregister_for_testing(status);
}

Snapshot Owner::snapshot() const {
    return impl_ == nullptr ? Snapshot{} : impl_->snapshot();
}

} // namespace sao::launcher::workshop_panel