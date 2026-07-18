#include "conversation_store.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <vector>

namespace sao::ai_editor::native {
namespace {

int64_t unix_milliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string next_id() {
    static std::atomic<uint64_t> sequence{0};
    const auto ticks = static_cast<uint64_t>(unix_milliseconds());
    const auto current = sequence.fetch_add(1, std::memory_order_relaxed);
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "conv-%llx-%lx-%llx",
                  static_cast<unsigned long long>(ticks),
                  static_cast<unsigned long>(GetCurrentProcessId()),
                  static_cast<unsigned long long>(current));
    return buffer;
}

Json summary_of(const Json& document) {
    return Json{{"id", document.value("id", "")},
                {"title", document.value("title", "Untitled")},
                {"model", document.value("model", "")},
                {"scope", document.value("scope", "workspace")},
                {"savedAt", document.value("savedAt", int64_t{0})},
                {"messageCount", document.value("messageCount", 0U)}};
}

}  // namespace

ConversationStore::ConversationStore(const ScopeStore& scopes) noexcept
    : scopes_(scopes) {}

int32_t ConversationStore::save(const std::filesystem::path& path,
                                const Json& value) const {
    return write_text_atomic(path, value.dump(1));
}

int32_t ConversationStore::create(std::string_view title,
                                  std::string_view model,
                                  std::string_view scope,
                                  Json& result) const {
    if (scope != "workspace" && scope != "system") {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::string id = next_id();
    Json document{{"id", id},
                  {"title", title.empty() ? "Untitled" : std::string(title)},
                  {"systemPrompt", ""},
                  {"model", std::string(model)},
                  {"scope", std::string(scope)},
                  {"savedAt", unix_milliseconds()},
                  {"messageCount", 0},
                  {"messages", Json::array()}};
    const auto path = scopes_.history_root(scope) /
                      (utf8_to_wide(id) + L".json");
    const int32_t status = save(path, document);
    if (status == SAO_AI_EDITOR_OK) {
        result = std::move(document);
    }
    return status;
}

int32_t ConversationStore::locate(std::string_view conversation_id,
                                  std::filesystem::path& path) const {
    if (!valid_simple_id(conversation_id)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const auto filename = utf8_to_wide(conversation_id) + L".json";
    std::error_code error;
    for (const std::string_view scope : {"workspace", "system"}) {
        auto candidate = scopes_.history_root(scope) / filename;
        if (std::filesystem::is_regular_file(candidate, error)) {
            path = std::move(candidate);
            return SAO_AI_EDITOR_OK;
        }
        error.clear();
    }
    return SAO_AI_EDITOR_ERR_NOT_FOUND;
}

int32_t ConversationStore::get(std::string_view conversation_id,
                               Json& result) const {
    std::filesystem::path path;
    int32_t status = locate(conversation_id, path);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    std::string text;
    status = read_text_file(path, kMaximumJsonBytes, text);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    result = Json::parse(text);
    return result.is_object() && result.value("id", "") == conversation_id
        ? SAO_AI_EDITOR_OK
        : SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
}

int32_t ConversationStore::append(std::string_view conversation_id,
                                  const Json& message,
                                  Json& result) const {
    if (!message.is_object() || !message.contains("role") ||
        !message["role"].is_string() || !message.contains("content")) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::filesystem::path path;
    int32_t status = locate(conversation_id, path);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    status = get(conversation_id, result);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    result["messages"].push_back(message);
    result["messageCount"] = result["messages"].size();
    result["savedAt"] = unix_milliseconds();
    return save(path, result);
}

int32_t ConversationStore::list(std::string_view scope,
                                uint32_t limit,
                                Json& result) const {
    if (scope != "all" && scope != "workspace" && scope != "system") {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    limit = std::clamp(limit, 1U, 500U);
    std::vector<Json> entries;
    const std::vector<std::string_view> selected = scope == "all"
        ? std::vector<std::string_view>{"workspace", "system"}
        : std::vector<std::string_view>{scope};
    for (const auto selected_scope : selected) {
        const auto root = scopes_.history_root(selected_scope);
        std::error_code error;
        if (!std::filesystem::is_directory(root, error)) {
            continue;
        }
        for (const auto& item : std::filesystem::directory_iterator(root, error)) {
            if (error) {
                return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
            }
            if (!item.is_regular_file(error) ||
                item.path().extension() != L".json") {
                continue;
            }
            std::string text;
            if (read_text_file(item.path(), kMaximumJsonBytes, text) !=
                SAO_AI_EDITOR_OK) {
                continue;
            }
            Json document = Json::parse(text, nullptr, false);
            if (document.is_object()) {
                entries.push_back(summary_of(document));
            }
        }
    }
    std::sort(entries.begin(), entries.end(), [](const Json& left,
                                                  const Json& right) {
        return left.value("savedAt", int64_t{0}) >
               right.value("savedAt", int64_t{0});
    });
    result = Json::array();
    for (size_t index = 0;
         index < entries.size() && index < static_cast<size_t>(limit);
         ++index) {
        result.push_back(std::move(entries[index]));
    }
    return SAO_AI_EDITOR_OK;
}

int32_t ConversationStore::remove(std::string_view conversation_id,
                                  Json& result) const {
    std::filesystem::path path;
    const int32_t status = locate(conversation_id, path);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    std::error_code error;
    const bool removed = std::filesystem::remove(path, error);
    if (error) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    result = Json{{"ok", removed}, {"id", std::string(conversation_id)}};
    return removed ? SAO_AI_EDITOR_OK : SAO_AI_EDITOR_ERR_NOT_FOUND;
}

}  // namespace sao::ai_editor::native
