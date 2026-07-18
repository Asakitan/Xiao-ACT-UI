#include "conversation_store.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <string>
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
    // `pinned` was added after the initial conversation shape shipped, so we
    // treat a missing field as false rather than rejecting the document —
    // otherwise older on-disk conversations would silently disappear from
    // the sidebar.
    return Json{{"id", document.value("id", "")},
                {"title", document.value("title", "Untitled")},
                {"model", document.value("model", "")},
                {"scope", document.value("scope", "workspace")},
                {"savedAt", document.value("savedAt", int64_t{0})},
                {"messageCount", document.value("messageCount", 0U)},
                {"pinned", document.value("pinned", false)}};
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
                  {"pinned", false},
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
    if (!result.is_object() || result.value("id", "") != conversation_id) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    // Older on-disk conversations predate the `pinned` field.  Surface a
    // stable false so downstream callers do not have to guard against
    // missing keys.  Rewriting the file lazily is intentional — reads
    // stay non-mutating; a subsequent set_pinned or append is what
    // persists the field.
    if (!result.contains("pinned") || !result["pinned"].is_boolean()) {
        result["pinned"] = false;
    }
    return SAO_AI_EDITOR_OK;
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
        // Pinned conversations always precede unpinned ones; within a group
        // savedAt (descending) is the tie-breaker so freshly-updated pinned
        // entries surface first.
        const bool left_pinned = left.value("pinned", false);
        const bool right_pinned = right.value("pinned", false);
        if (left_pinned != right_pinned) {
            return left_pinned && !right_pinned;
        }
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

namespace {

std::string ascii_lower(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (const char character : value) {
        lowered.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(character))));
    }
    return lowered;
}

size_t count_occurrences(const std::string& haystack_lower,
                         const std::string& needle_lower) {
    if (needle_lower.empty() || haystack_lower.size() < needle_lower.size()) {
        return 0;
    }
    size_t count = 0;
    size_t position = 0;
    while ((position = haystack_lower.find(needle_lower, position)) !=
           std::string::npos) {
        ++count;
        position += needle_lower.size();
    }
    return count;
}

}  // namespace

int32_t ConversationStore::search(std::string_view query,
                                  std::string_view scope,
                                  uint32_t limit,
                                  Json& result) const {
    if (query.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    if (scope != "all" && scope != "workspace" && scope != "system") {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    limit = std::clamp<uint32_t>(limit, 1U, 500U);
    const std::string needle_lower = ascii_lower(query);
    struct Hit {
        Json summary;
        int64_t saved_at;
    };
    std::vector<Hit> hits;
    const std::vector<std::string_view> selected = scope == "all"
        ? std::vector<std::string_view>{"workspace", "system"}
        : std::vector<std::string_view>{scope};
    for (const auto selected_scope : selected) {
        const auto root = scopes_.history_root(selected_scope);
        std::error_code error;
        if (!std::filesystem::is_directory(root, error)) {
            continue;
        }
        for (const auto& item :
             std::filesystem::directory_iterator(root, error)) {
            if (error) {
                error.clear();
                continue;
            }
            std::error_code file_error;
            if (!item.is_regular_file(file_error) ||
                item.path().extension() != L".json") {
                continue;
            }
            std::string text;
            if (read_text_file(item.path(), kMaximumJsonBytes, text) !=
                SAO_AI_EDITOR_OK) {
                continue;
            }
            Json document = Json::parse(text, nullptr, false);
            if (!document.is_object()) {
                continue;
            }
            size_t match_count = 0;
            Json matched_fields = Json::array();
            const std::string title_lower = ascii_lower(
                document.value("title", std::string{}));
            const size_t title_hits = count_occurrences(title_lower,
                                                         needle_lower);
            if (title_hits > 0) {
                match_count += title_hits;
                matched_fields.push_back("title");
            }
            bool message_field_added = false;
            if (document.contains("messages") &&
                document["messages"].is_array()) {
                for (const auto& message : document["messages"]) {
                    if (!message.is_object() || !message.contains("content")) {
                        continue;
                    }
                    const auto& content = message["content"];
                    if (!content.is_string()) {
                        continue;
                    }
                    const std::string content_lower = ascii_lower(
                        content.get<std::string>());
                    const size_t content_hits = count_occurrences(
                        content_lower, needle_lower);
                    if (content_hits > 0) {
                        match_count += content_hits;
                        if (!message_field_added) {
                            matched_fields.push_back("message");
                            message_field_added = true;
                        }
                    }
                }
            }
            if (match_count == 0) {
                continue;
            }
            const int64_t saved_at = document.value("savedAt", int64_t{0});
            Hit hit;
            hit.summary = Json{
                {"id", document.value("id", "")},
                {"title", document.value("title", "Untitled")},
                {"scope", document.value("scope",
                                        std::string(selected_scope))},
                {"savedAt", saved_at},
                {"matchedFields", std::move(matched_fields)},
                {"matchCount", match_count}};
            hit.saved_at = saved_at;
            hits.push_back(std::move(hit));
        }
    }
    std::sort(hits.begin(), hits.end(),
              [](const Hit& left, const Hit& right) {
                  return left.saved_at > right.saved_at;
              });
    Json results_array = Json::array();
    for (size_t index = 0;
         index < hits.size() && index < static_cast<size_t>(limit); ++index) {
        results_array.push_back(std::move(hits[index].summary));
    }
    result = Json{{"query", std::string(query)},
                  {"results", std::move(results_array)},
                  {"total", hits.size()}};
    return SAO_AI_EDITOR_OK;
}

int32_t ConversationStore::export_all(std::string_view scope,
                                      Json& result) const {
    if (scope != "all" && scope != "workspace" && scope != "system") {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::vector<std::string_view> selected = scope == "all"
        ? std::vector<std::string_view>{"workspace", "system"}
        : std::vector<std::string_view>{scope};
    Json conversations = Json::array();
    for (const auto selected_scope : selected) {
        const auto root = scopes_.history_root(selected_scope);
        std::error_code error;
        if (!std::filesystem::is_directory(root, error)) {
            continue;
        }
        for (const auto& item :
             std::filesystem::directory_iterator(root, error)) {
            if (error) {
                return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
            }
            std::error_code file_error;
            if (!item.is_regular_file(file_error) ||
                item.path().extension() != L".json") {
                continue;
            }
            std::string text;
            if (read_text_file(item.path(), kMaximumJsonBytes, text) !=
                SAO_AI_EDITOR_OK) {
                continue;
            }
            Json document = Json::parse(text, nullptr, false);
            if (!document.is_object()) {
                continue;
            }
            conversations.push_back(std::move(document));
        }
    }
    const size_t count = conversations.size();
    result = Json{{"format", "sao-conversations/1"},
                  {"conversations", std::move(conversations)},
                  {"count", count},
                  {"exportedAt", unix_milliseconds()}};
    return SAO_AI_EDITOR_OK;
}

int32_t ConversationStore::import_conversation(const Json& conversation,
                                               std::string_view scope,
                                               bool overwrite,
                                               std::string& out_id) const {
    if (scope != "workspace" && scope != "system") {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    if (!conversation.is_object()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    Json document = conversation;
    std::string id = document.value("id", std::string{});
    if (id.empty()) {
        id = next_id();
        document["id"] = id;
    } else if (!valid_simple_id(id)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    document["scope"] = std::string(scope);
    if (!document.contains("title") || !document["title"].is_string()) {
        document["title"] = "Untitled";
    }
    if (!document.contains("systemPrompt") ||
        !document["systemPrompt"].is_string()) {
        document["systemPrompt"] = "";
    }
    if (!document.contains("model") || !document["model"].is_string()) {
        document["model"] = "";
    }
    if (!document.contains("messages") || !document["messages"].is_array()) {
        document["messages"] = Json::array();
    }
    document["messageCount"] = document["messages"].size();
    if (!document.contains("savedAt") || !document["savedAt"].is_number()) {
        document["savedAt"] = unix_milliseconds();
    }
    if (!document.contains("pinned") || !document["pinned"].is_boolean()) {
        document["pinned"] = false;
    }
    // Determine existing location (workspace or system).
    std::filesystem::path existing_path;
    const int32_t locate_status = locate(id, existing_path);
    if (locate_status == SAO_AI_EDITOR_OK && !overwrite) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    if (locate_status != SAO_AI_EDITOR_OK &&
        locate_status != SAO_AI_EDITOR_ERR_NOT_FOUND) {
        return locate_status;
    }
    const auto target_path = scopes_.history_root(scope) /
                             (utf8_to_wide(id) + L".json");
    // If overwriting and the existing file lives in a different scope,
    // remove the stale copy so scope reassignment is honoured.
    if (locate_status == SAO_AI_EDITOR_OK && overwrite &&
        existing_path != target_path) {
        std::error_code remove_error;
        std::filesystem::remove(existing_path, remove_error);
    }
    const int32_t status = save(target_path, document);
    if (status == SAO_AI_EDITOR_OK) {
        out_id = std::move(id);
    }
    return status;
}

int32_t ConversationStore::branch(std::string_view source_id,
                                  int64_t message_index,
                                  std::string_view title,
                                  std::string_view scope,
                                  Json& result) const {
    if (scope != "workspace" && scope != "system") {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    if (message_index < 0) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    Json source;
    const int32_t get_status = get(source_id, source);
    if (get_status != SAO_AI_EDITOR_OK) {
        return get_status;
    }
    if (!source.contains("messages") || !source["messages"].is_array()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const Json& messages = source["messages"];
    const int64_t total = static_cast<int64_t>(messages.size());
    // `messageIndex` is the index of the last message we want to include,
    // so it must be strictly less than the message count; otherwise there's
    // nothing at that slot.
    if (message_index >= total) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const size_t branch_count = static_cast<size_t>(message_index) + 1;
    Json branched_messages = Json::array();
    for (size_t index = 0; index < branch_count; ++index) {
        branched_messages.push_back(messages[index]);
    }
    const std::string new_id = next_id();
    std::string branch_title(title);
    if (branch_title.empty()) {
        branch_title = source.value("title", std::string{"Untitled"}) +
                       " (branch)";
    }
    const int64_t now = unix_milliseconds();
    Json document{{"id", new_id},
                  {"title", branch_title},
                  {"systemPrompt",
                   source.value("systemPrompt", std::string{})},
                  {"model", source.value("model", std::string{})},
                  {"scope", std::string(scope)},
                  {"savedAt", now},
                  {"messageCount", branched_messages.size()},
                  {"pinned", false},
                  {"messages", branched_messages}};
    const auto path = scopes_.history_root(scope) /
                      (utf8_to_wide(new_id) + L".json");
    const int32_t status = save(path, document);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    result = Json{{"id", new_id},
                  {"title", branch_title},
                  {"messageCount", branched_messages.size()},
                  {"sourceId", std::string(source_id)},
                  {"branchedAt", now}};
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

int32_t ConversationStore::set_pinned(std::string_view conversation_id,
                                      bool pinned,
                                      Json& result) const {
    std::filesystem::path path;
    int32_t status = locate(conversation_id, path);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    Json document;
    status = get(conversation_id, document);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    document["pinned"] = pinned;
    // Intentionally leave savedAt untouched here: pinning is metadata and
    // must not shuffle the "recently edited" ordering used elsewhere in the
    // UI.  The pinned-first grouping still surfaces the entry at the top of
    // list() even without bumping savedAt.
    status = save(path, document);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    result = Json{{"id", std::string(conversation_id)},
                  {"pinned", pinned}};
    return SAO_AI_EDITOR_OK;
}

}  // namespace sao::ai_editor::native
