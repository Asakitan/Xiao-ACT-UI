#include "conversation_store.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <map>
#include <string>
#include <vector>

#include "sha256_helper.h"

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

// Normalise a stored `tags` value into a de-duplicated string list.  Preserves
// first-occurrence order so the on-disk array stays stable across reads/writes.
// Non-string entries are dropped rather than surfaced as an error — legacy
// documents that never had the field return an empty vector.
std::vector<std::string> read_tags(const Json& document) {
    std::vector<std::string> tags;
    if (!document.contains("tags") || !document["tags"].is_array()) {
        return tags;
    }
    tags.reserve(document["tags"].size());
    for (const auto& tag : document["tags"]) {
        if (!tag.is_string()) {
            continue;
        }
        std::string value = tag.get<std::string>();
        if (value.empty()) {
            continue;
        }
        if (std::find(tags.begin(), tags.end(), value) != tags.end()) {
            continue;
        }
        tags.push_back(std::move(value));
    }
    return tags;
}

Json tags_to_json(const std::vector<std::string>& tags) {
    Json array = Json::array();
    for (const auto& tag : tags) {
        array.push_back(tag);
    }
    return array;
}

// Reject tags that are empty or exceed 128 bytes.  UTF-8 non-empty strings are
// otherwise accepted verbatim — the docstring on set_tags spells out the
// permissive char rule (CJK + [A-Za-z0-9_-]) but we lean on "any non-empty
// UTF-8 string" to keep the surface simple and let the UI layer enforce
// stricter conventions if it wants to.
bool tag_string_valid(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    if (value.size() > 128) {
        return false;
    }
    return true;
}

Json summary_of(const Json& document) {
    // `pinned` was added after the initial conversation shape shipped, so we
    // treat a missing field as false rather than rejecting the document —
    // otherwise older on-disk conversations would silently disappear from
    // the sidebar.  `tags` is likewise a later addition and defaults to an
    // empty array so callers can rely on the field being present.
    return Json{{"id", document.value("id", "")},
                {"title", document.value("title", "Untitled")},
                {"model", document.value("model", "")},
                {"scope", document.value("scope", "workspace")},
                {"savedAt", document.value("savedAt", int64_t{0})},
                {"messageCount", document.value("messageCount", 0U)},
                {"pinned", document.value("pinned", false)},
                {"tags", tags_to_json(read_tags(document))}};
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
                  {"tags", Json::array()},
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
    // `tags` was added after the initial shape shipped; legacy documents
    // surface as an empty array so callers can always index into result["tags"].
    // We also normalise malformed entries (non-string / duplicate) through
    // read_tags so external editors that hand-poke the JSON cannot smuggle
    // garbage into the persisted set on the next save().
    result["tags"] = tags_to_json(read_tags(result));
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
    // Stamp the envelope with an SHA-256 of its own body so import can
    // detect out-of-band tampering.  The digest is computed over the
    // canonical dump of `result` with the `sha256` field stripped — see
    // sha256_envelope_hex for the invariant.  Missing digests on import
    // are silently tolerated so pre-checksum payloads keep loading.
    result["sha256"] = sha256_envelope_hex(result);
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
    // Normalise incoming tags through the same de-duplication path used by
    // set_tags so hand-crafted export payloads cannot smuggle empty strings
    // or repeats onto disk.  Missing / non-array `tags` becomes an empty
    // list so downstream code can always index into the field.
    document["tags"] = tags_to_json(read_tags(document));
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
    // Branched documents inherit tags from the source — users typically want
    // to filter both halves of a fork under the same tag(s) without having to
    // re-apply them by hand.  read_tags() collapses duplicates + strips any
    // legacy junk so the new record starts from a clean set.
    Json document{{"id", new_id},
                  {"title", branch_title},
                  {"systemPrompt",
                   source.value("systemPrompt", std::string{})},
                  {"model", source.value("model", std::string{})},
                  {"scope", std::string(scope)},
                  {"savedAt", now},
                  {"messageCount", branched_messages.size()},
                  {"pinned", false},
                  {"tags", tags_to_json(read_tags(source))},
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

int32_t ConversationStore::merge(const std::vector<std::string>& source_ids,
                                  std::string_view title,
                                  std::string_view scope,
                                  const Json& separator,
                                  bool order_by_saved_at,
                                  Json& result) const {
    if (scope != "workspace" && scope != "system") {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    if (source_ids.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    // Fetch every source up front so a missing id fails cleanly before we
    // start writing anything.  Any NOT_FOUND propagates so callers see the
    // same error surface as get() does for individual lookups.
    std::vector<Json> sources;
    sources.reserve(source_ids.size());
    for (const auto& id : source_ids) {
        Json document;
        const int32_t status = get(id, document);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        sources.push_back(std::move(document));
    }
    // Optional separator must be a valid message-shaped object (role +
    // content).  We normalise an explicit null / missing key to "no
    // separator" instead of surfacing an error — callers that omit the
    // param probably do not want a divider.
    const bool has_separator = separator.is_object() &&
                                separator.contains("role") &&
                                separator["role"].is_string() &&
                                separator.contains("content");
    if (order_by_saved_at) {
        // Preserve caller order as a stable tie-breaker if two conversations
        // share the same savedAt (which happens in unit tests that mint
        // records quickly).  std::stable_sort keeps the pre-sort ordering
        // for equal keys.
        std::stable_sort(sources.begin(), sources.end(),
                         [](const Json& left, const Json& right) {
                             return left.value("savedAt", int64_t{0}) <
                                    right.value("savedAt", int64_t{0});
                         });
    }
    Json merged_messages = Json::array();
    Json sourced_from = Json::array();
    for (size_t index = 0; index < sources.size(); ++index) {
        if (index > 0 && has_separator) {
            merged_messages.push_back(separator);
        }
        const auto& source = sources[index];
        sourced_from.push_back(source.value("id", std::string{}));
        if (!source.contains("messages") || !source["messages"].is_array()) {
            continue;
        }
        for (const auto& message : source["messages"]) {
            merged_messages.push_back(message);
        }
    }
    const std::string& first_title = sources.front().value(
        "title", std::string{"Untitled"});
    const std::string merged_title = title.empty()
                                          ? (first_title + " (merged)")
                                          : std::string(title);
    // Union tag sets from every source, preserving first-seen order so the
    // merged conversation surfaces under any category any input belonged to.
    // Duplicates across sources collapse into a single entry.
    std::vector<std::string> merged_tags;
    for (const auto& source : sources) {
        for (auto& tag : read_tags(source)) {
            if (std::find(merged_tags.begin(), merged_tags.end(), tag) ==
                merged_tags.end()) {
                merged_tags.push_back(std::move(tag));
            }
        }
    }
    const std::string new_id = next_id();
    const int64_t now = unix_milliseconds();
    Json document{{"id", new_id},
                  {"title", merged_title},
                  {"systemPrompt",
                   sources.front().value("systemPrompt", std::string{})},
                  {"model",
                   sources.front().value("model", std::string{})},
                  {"scope", std::string(scope)},
                  {"savedAt", now},
                  {"messageCount", merged_messages.size()},
                  {"pinned", false},
                  {"tags", tags_to_json(merged_tags)},
                  {"messages", merged_messages}};
    const auto path = scopes_.history_root(scope) /
                      (utf8_to_wide(new_id) + L".json");
    const int32_t status = save(path, document);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    result = Json{{"id", new_id},
                  {"title", merged_title},
                  {"messageCount", merged_messages.size()},
                  {"sourcedFrom", std::move(sourced_from)},
                  {"mergedAt", now}};
    return SAO_AI_EDITOR_OK;
}

int32_t ConversationStore::split(std::string_view source_id,
                                  size_t message_index,
                                  std::string_view title_before,
                                  std::string_view title_after,
                                  std::string_view scope,
                                  bool keep_original,
                                  Json& result) const {
    if (scope != "workspace" && scope != "system") {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::filesystem::path source_path;
    int32_t status = locate(source_id, source_path);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    Json source;
    status = get(source_id, source);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    if (!source.contains("messages") || !source["messages"].is_array()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const Json& messages = source["messages"];
    const size_t total = messages.size();
    // The split slot must sit strictly inside the range so both halves have
    // at least one message.  message_index == total - 1 would leave the
    // "after" half empty; message_index >= total is off-the-end.
    if (total < 2 || message_index >= total - 1) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const size_t before_count = message_index + 1;
    Json before_messages = Json::array();
    for (size_t index = 0; index < before_count; ++index) {
        before_messages.push_back(messages[index]);
    }
    Json after_messages = Json::array();
    for (size_t index = before_count; index < total; ++index) {
        after_messages.push_back(messages[index]);
    }
    const int64_t now = unix_milliseconds();
    const std::string original_title = source.value(
        "title", std::string{"Untitled"});
    const std::string resolved_title_before = title_before.empty()
        ? original_title
        : std::string(title_before);
    const std::string resolved_title_after = title_after.empty()
        ? (original_title + " (after)")
        : std::string(title_after);

    // "After" half is always a fresh conversation regardless of the mode,
    // inheriting model / systemPrompt from the source so downstream chat
    // continues to work.  We build it first so a save failure short-
    // circuits before we touch the source file.
    // Both halves inherit tags from the source so filtered views still cover
    // the entire split — otherwise splitting a "review" conversation would
    // silently drop the second half from the review filter.
    const Json inherited_tags = tags_to_json(read_tags(source));
    const std::string after_id = next_id();
    Json after_document{{"id", after_id},
                        {"title", resolved_title_after},
                        {"systemPrompt",
                         source.value("systemPrompt", std::string{})},
                        {"model", source.value("model", std::string{})},
                        {"scope", std::string(scope)},
                        {"savedAt", now},
                        {"messageCount", after_messages.size()},
                        {"pinned", false},
                        {"tags", inherited_tags},
                        {"messages", after_messages}};
    const auto after_path = scopes_.history_root(scope) /
                             (utf8_to_wide(after_id) + L".json");
    status = save(after_path, after_document);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }

    std::string original_id_out;
    size_t original_message_count = 0;
    if (keep_original) {
        // Both halves become new conversations; the source keeps its
        // messages / id intact.  `title_before` still names the new
        // "before" copy so callers can label the fork.
        const std::string before_id = next_id();
        Json before_document{{"id", before_id},
                             {"title", resolved_title_before},
                             {"systemPrompt",
                              source.value("systemPrompt", std::string{})},
                             {"model",
                              source.value("model", std::string{})},
                             {"scope", std::string(scope)},
                             {"savedAt", now},
                             {"messageCount", before_messages.size()},
                             {"pinned", false},
                             {"tags", inherited_tags},
                             {"messages", before_messages}};
        const auto before_path = scopes_.history_root(scope) /
                                  (utf8_to_wide(before_id) + L".json");
        status = save(before_path, before_document);
        if (status != SAO_AI_EDITOR_OK) {
            // Rollback the "after" half so a half-written split does not
            // leave dangling files behind.
            std::error_code cleanup_error;
            std::filesystem::remove(after_path, cleanup_error);
            return status;
        }
        original_id_out = before_id;
        original_message_count = before_messages.size();
    } else {
        // Default path: source is rewritten in place with the "before"
        // messages so existing references to the source id keep working.
        // savedAt is bumped because the document content changed.
        Json rewritten = source;
        rewritten["messages"] = before_messages;
        rewritten["messageCount"] = before_messages.size();
        rewritten["savedAt"] = now;
        if (!title_before.empty()) {
            rewritten["title"] = std::string(title_before);
        }
        // Honour a scope change even for the in-place half: if the caller
        // asked for "system" but the source lives under "workspace" (or
        // vice versa), move the file across before we overwrite it.
        const std::string original_scope = rewritten.value(
            "scope", std::string{"workspace"});
        rewritten["scope"] = std::string(scope);
        std::filesystem::path target_source_path = source_path;
        if (original_scope != scope) {
            target_source_path = scopes_.history_root(scope) /
                                  (utf8_to_wide(std::string(source_id)) +
                                   L".json");
            std::error_code remove_error;
            std::filesystem::remove(source_path, remove_error);
        }
        status = save(target_source_path, rewritten);
        if (status != SAO_AI_EDITOR_OK) {
            std::error_code cleanup_error;
            std::filesystem::remove(after_path, cleanup_error);
            return status;
        }
        original_id_out = std::string(source_id);
        original_message_count = before_messages.size();
    }

    result = Json{{"original",
                   Json{{"id", original_id_out},
                        {"messageCount", original_message_count}}},
                  {"latter",
                   Json{{"id", after_id},
                        {"title", resolved_title_after},
                        {"messageCount", after_messages.size()}}},
                  {"splitAt", now}};
    return SAO_AI_EDITOR_OK;
}

int32_t ConversationStore::compact(std::string_view conversation_id,
                                   size_t keep_last,
                                   std::string_view summary,
                                   std::string_view strategy,
                                   Json& result) const {
    // Strategy validation lives here (not just at the dispatch layer) so
    // direct C++ callers cannot smuggle an unknown value past the JSON
    // adapter and quietly get "replace" semantics.
    if (strategy != "replace" && strategy != "prepend") {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
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
    if (!document.contains("messages") || !document["messages"].is_array()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const Json& messages = document["messages"];
    const size_t original_count = messages.size();
    const int64_t now = unix_milliseconds();
    // Noop path: not enough backlog to warrant compaction.  We deliberately
    // leave the file untouched (savedAt included) so pinned-first ordering
    // and "last modified" indicators stay accurate for a "nothing changed"
    // outcome.  The `noop` flag lets the runtime skip the summary LLM call
    // entirely on subsequent tries; see dispatch handler for the pre-check.
    if (original_count <= keep_last) {
        result = Json{{"id", std::string(conversation_id)},
                      {"originalMessageCount", original_count},
                      {"newMessageCount", original_count},
                      {"summaryLength", size_t{0}},
                      {"summary", std::string{}},
                      {"compactedAt", now},
                      {"noop", true}};
        return SAO_AI_EDITOR_OK;
    }
    Json summary_message = Json{{"role", "system"},
                                {"content", std::string(summary)}};
    Json new_messages = Json::array();
    if (strategy == "prepend") {
        // Preserve every original message; only prepend the summary so the
        // model still sees the full transcript but with a lead-in that
        // primes it on the earlier context.
        new_messages.push_back(std::move(summary_message));
        for (const auto& message : messages) {
            new_messages.push_back(message);
        }
    } else {
        // "replace": drop everything except the tail keep_last messages
        // and put the summary at the head.  Guaranteed original_count >
        // keep_last (checked above) so this arithmetic is safe.
        new_messages.push_back(std::move(summary_message));
        const size_t drop_count = original_count - keep_last;
        for (size_t index = drop_count; index < original_count; ++index) {
            new_messages.push_back(messages[index]);
        }
    }
    const size_t new_count = new_messages.size();
    document["messages"] = new_messages;
    document["messageCount"] = new_count;
    document["savedAt"] = now;
    status = save(path, document);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    result = Json{{"id", std::string(conversation_id)},
                  {"originalMessageCount", original_count},
                  {"newMessageCount", new_count},
                  {"summaryLength", summary.size()},
                  {"summary", std::string(summary)},
                  {"compactedAt", now},
                  {"noop", false}};
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

int32_t ConversationStore::stats(std::string_view scope, Json& result) const {
    if (scope != "all" && scope != "workspace" && scope != "system") {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::vector<std::string_view> selected = scope == "all"
        ? std::vector<std::string_view>{"workspace", "system"}
        : std::vector<std::string_view>{scope};
    uint64_t total = 0;
    uint64_t pinned = 0;
    uint64_t total_messages = 0;
    // savedAt is stored as an unsigned unix millis; use int64 optional so an
    // empty history returns null rather than 0 (which callers would mistake
    // for 1970-01-01 UTC).
    bool has_saved_at = false;
    int64_t oldest_saved_at = 0;
    int64_t newest_saved_at = 0;
    // Deterministic key ordering: nlohmann::json::object() sorts alphabetically
    // when serialized, so accumulating into a plain object gives stable output
    // for byScope / byModel across runs (helps snapshot tests + UI diffs).
    Json by_scope = Json::object();
    Json by_model = Json::object();
    // byMonth needs ascending order; keep counts in a std::map and copy into
    // an array at the end.  YYYY-MM keys are lexicographically sortable so we
    // do not need a secondary numeric compare.
    std::map<std::string, uint64_t> by_month;
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
            ++total;
            if (document.value("pinned", false)) {
                ++pinned;
            }
            const std::string document_scope =
                document.value("scope", std::string(selected_scope));
            by_scope[document_scope] =
                by_scope.value(document_scope, uint64_t{0}) + 1;
            const std::string model = document.value("model", std::string{});
            // Empty model buckets under "" (kept as an explicit key so UI can
            // decide whether to display "unspecified" — dropping it would hide
            // conversations that never picked a model).
            by_model[model] = by_model.value(model, uint64_t{0}) + 1;
            uint64_t message_count = 0;
            if (document.contains("messageCount") &&
                document["messageCount"].is_number()) {
                const int64_t stored =
                    document["messageCount"].get<int64_t>();
                if (stored > 0) {
                    message_count = static_cast<uint64_t>(stored);
                }
            } else if (document.contains("messages") &&
                       document["messages"].is_array()) {
                message_count = document["messages"].size();
            }
            total_messages += message_count;
            const int64_t saved_at = document.value("savedAt", int64_t{0});
            if (saved_at > 0) {
                if (!has_saved_at) {
                    oldest_saved_at = saved_at;
                    newest_saved_at = saved_at;
                    has_saved_at = true;
                } else {
                    if (saved_at < oldest_saved_at) {
                        oldest_saved_at = saved_at;
                    }
                    if (saved_at > newest_saved_at) {
                        newest_saved_at = saved_at;
                    }
                }
                // Bucket into UTC YYYY-MM.  Using gmtime keeps the buckets
                // consistent regardless of the runtime host's local timezone,
                // which matters because savedAt is unix millis (UTC).
                const std::time_t seconds =
                    static_cast<std::time_t>(saved_at / 1000);
                std::tm utc{};
                if (gmtime_s(&utc, &seconds) == 0) {
                    char month_key[8]{};
                    std::snprintf(month_key, sizeof(month_key), "%04d-%02d",
                                  utc.tm_year + 1900, utc.tm_mon + 1);
                    ++by_month[month_key];
                }
            }
        }
    }
    Json by_month_array = Json::array();
    for (const auto& entry : by_month) {
        by_month_array.push_back(Json{{"month", entry.first},
                                       {"count", entry.second}});
    }
    // Avoid divide-by-zero by clamping the denominator to 1; the ratio is
    // still 0 for empty history because total_messages is 0.
    const double average_messages =
        static_cast<double>(total_messages) /
        static_cast<double>(std::max<uint64_t>(total, 1U));
    result = Json{{"total", total},
                  {"pinned", pinned},
                  {"byScope", std::move(by_scope)},
                  {"byModel", std::move(by_model)},
                  {"byMonth", std::move(by_month_array)},
                  {"totalMessages", total_messages},
                  {"averageMessages", average_messages},
                  {"oldestSavedAt", has_saved_at
                                        ? Json(oldest_saved_at)
                                        : Json(nullptr)},
                  {"newestSavedAt", has_saved_at
                                        ? Json(newest_saved_at)
                                        : Json(nullptr)}};
    return SAO_AI_EDITOR_OK;
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

int32_t ConversationStore::set_tags(
    std::string_view conversation_id,
    const std::vector<std::string>& add_tags,
    const std::vector<std::string>& remove_tags,
    Json& result) const {
    // Validate up front so a bad tag never touches disk.  An empty add+remove
    // is deliberately allowed — the method doubles as a "return current tags"
    // primitive when both vectors are empty (see docstring).
    for (const auto& tag : add_tags) {
        if (!tag_string_valid(tag)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
    }
    for (const auto& tag : remove_tags) {
        if (!tag_string_valid(tag)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
    }
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
    // Start from the normalised in-memory view get() built (already de-dup'd
    // and legacy-safe), then apply removes before adds so a caller who passes
    // the same value in both lists still ends up with the tag present.
    std::vector<std::string> tags = read_tags(document);
    if (!remove_tags.empty()) {
        tags.erase(
            std::remove_if(tags.begin(), tags.end(),
                           [&remove_tags](const std::string& tag) {
                               return std::find(remove_tags.begin(),
                                                remove_tags.end(),
                                                tag) != remove_tags.end();
                           }),
            tags.end());
    }
    for (const auto& tag : add_tags) {
        if (std::find(tags.begin(), tags.end(), tag) == tags.end()) {
            tags.push_back(tag);
        }
    }
    // Only rewrite the file when tags changed relative to what was on disk;
    // otherwise the no-op path (both vectors empty, or add == existing) leaves
    // savedAt / the file mtime untouched to match the set_pinned semantics.
    const std::vector<std::string> previous = read_tags(document);
    if (tags != previous) {
        document["tags"] = tags_to_json(tags);
        status = save(path, document);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
    }
    result = Json{{"id", std::string(conversation_id)},
                  {"tags", tags_to_json(tags)}};
    return SAO_AI_EDITOR_OK;
}

int32_t ConversationStore::find_by_tag(const std::vector<std::string>& tags,
                                       std::string_view scope,
                                       uint32_t limit,
                                       bool match_all,
                                       Json& result) const {
    if (scope != "all" && scope != "workspace" && scope != "system") {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    if (tags.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    for (const auto& tag : tags) {
        if (!tag_string_valid(tag)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
    }
    limit = std::clamp(limit, 1U, 500U);
    // De-duplicate the query tags so match_all doesn't require an
    // impossible "contains X twice" semantic when the caller passes
    // ["work", "work"].
    std::vector<std::string> query_tags;
    query_tags.reserve(tags.size());
    for (const auto& tag : tags) {
        if (std::find(query_tags.begin(), query_tags.end(), tag) ==
            query_tags.end()) {
            query_tags.push_back(tag);
        }
    }
    struct Hit {
        Json summary;
        bool pinned;
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
            const std::vector<std::string> document_tags = read_tags(document);
            bool matched = false;
            if (match_all) {
                matched = std::all_of(
                    query_tags.begin(), query_tags.end(),
                    [&document_tags](const std::string& query) {
                        return std::find(document_tags.begin(),
                                         document_tags.end(),
                                         query) != document_tags.end();
                    });
            } else {
                matched = std::any_of(
                    query_tags.begin(), query_tags.end(),
                    [&document_tags](const std::string& query) {
                        return std::find(document_tags.begin(),
                                         document_tags.end(),
                                         query) != document_tags.end();
                    });
            }
            if (!matched) {
                continue;
            }
            Hit hit;
            hit.pinned = document.value("pinned", false);
            hit.saved_at = document.value("savedAt", int64_t{0});
            hit.summary = Json{
                {"id", document.value("id", "")},
                {"title", document.value("title", "Untitled")},
                {"tags", tags_to_json(document_tags)},
                {"pinned", hit.pinned},
                {"savedAt", hit.saved_at},
                {"messageCount", document.value("messageCount", 0U)}};
            hits.push_back(std::move(hit));
        }
    }
    // Sort identically to list(): pinned first, then savedAt descending so
    // the freshly-edited pinned entries surface at the top.
    std::sort(hits.begin(), hits.end(),
              [](const Hit& left, const Hit& right) {
                  if (left.pinned != right.pinned) {
                      return left.pinned && !right.pinned;
                  }
                  return left.saved_at > right.saved_at;
              });
    Json items = Json::array();
    for (size_t index = 0;
         index < hits.size() && index < static_cast<size_t>(limit); ++index) {
        items.push_back(std::move(hits[index].summary));
    }
    const size_t total = hits.size();
    result = Json{{"items", std::move(items)}, {"total", total}};
    return SAO_AI_EDITOR_OK;
}

int32_t ConversationStore::list_tags_stats(std::string_view scope,
                                            Json& result) const {
    if (scope != "all" && scope != "workspace" && scope != "system") {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::vector<std::string_view> selected = scope == "all"
        ? std::vector<std::string_view>{"workspace", "system"}
        : std::vector<std::string_view>{scope};
    // std::map keeps names sorted ascending which we use as the tie-breaker
    // after count-descending; we snapshot the ids in insertion order so the
    // per-tag `conversationIds` list mirrors the on-disk enumeration order.
    struct Bucket {
        uint64_t count = 0;
        std::vector<std::string> conversation_ids;
    };
    std::map<std::string, Bucket> buckets;
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
            const std::string document_id =
                document.value("id", std::string{});
            for (const auto& tag : read_tags(document)) {
                auto& bucket = buckets[tag];
                ++bucket.count;
                if (!document_id.empty()) {
                    bucket.conversation_ids.push_back(document_id);
                }
            }
        }
    }
    // Sort by count descending, name ascending as tie-breaker.  std::map
    // already gives us ascending name order so we can rely on stable_sort
    // to preserve that for equal counts.
    std::vector<std::pair<std::string, Bucket>> ordered(buckets.begin(),
                                                          buckets.end());
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const auto& left, const auto& right) {
                         return left.second.count > right.second.count;
                     });
    Json tags_array = Json::array();
    for (const auto& entry : ordered) {
        Json ids_array = Json::array();
        for (const auto& id : entry.second.conversation_ids) {
            ids_array.push_back(id);
        }
        tags_array.push_back(Json{{"name", entry.first},
                                    {"count", entry.second.count},
                                    {"conversationIds", std::move(ids_array)}});
    }
    const size_t total = ordered.size();
    result = Json{{"tags", std::move(tags_array)}, {"total", total}};
    return SAO_AI_EDITOR_OK;
}

}  // namespace sao::ai_editor::native
