#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "scope_store.h"

namespace sao::ai_editor::native {

class ConversationStore final {
public:
    explicit ConversationStore(const ScopeStore& scopes) noexcept;

    int32_t create(std::string_view title,
                   std::string_view model,
                   std::string_view scope,
                   Json& result) const;
    int32_t append(std::string_view conversation_id,
                   const Json& message,
                   Json& result) const;
    int32_t get(std::string_view conversation_id, Json& result) const;
    int32_t list(std::string_view scope,
                 uint32_t limit,
                 Json& result) const;
    int32_t search(std::string_view query,
                   std::string_view scope,
                   uint32_t limit,
                   Json& result) const;
    int32_t remove(std::string_view conversation_id, Json& result) const;

    // Toggle the `pinned` flag on a conversation.  Pinned conversations are
    // sorted ahead of unpinned entries in `list()` (savedAt still tie-breaks
    // within each group).  Documents missing a `pinned` field are treated as
    // unpinned to keep older stored conversations backward-compatible.
    int32_t set_pinned(std::string_view conversation_id,
                       bool pinned,
                       Json& result) const;

    int32_t export_all(std::string_view scope, Json& result) const;

    // Aggregate history counters for the requested `scope` (workspace / system
    // / all).  Walks every conversation JSON under history_root(scope) and
    // returns: total, pinned, byScope, byModel, byMonth (UTC YYYY-MM buckets
    // sorted ascending), totalMessages, averageMessages, oldestSavedAt,
    // newestSavedAt.  Empty history produces zeroed counters + null timestamps
    // so callers can render "no data" instead of guessing.
    int32_t stats(std::string_view scope, Json& result) const;
    int32_t import_conversation(const Json& conversation,
                                std::string_view scope,
                                bool overwrite,
                                std::string& out_id) const;

    // Fork a new conversation whose messages are the first
    // `message_index + 1` entries of the source (i.e. up to and including
    // the entry at `message_index`).  The new conversation inherits the
    // source's model + systemPrompt, gets a fresh id, savedAt = now, and
    // title defaults to source.title + " (branch)".  Emits the same shape
    // dispatch relays to callers (id / title / messageCount / sourceId /
    // branchedAt).
    int32_t branch(std::string_view source_id,
                   int64_t message_index,
                   std::string_view title,
                   std::string_view scope,
                   Json& result) const;

    // Concatenate N source conversations into a single new conversation.
    // Source conversations are preserved on disk; a new id + savedAt = now
    // are assigned to the merged copy.  systemPrompt / model inherit from
    // the first source after ordering.  If `separator` is a valid message
    // object (role + content) it is inserted between adjacent source
    // message blocks (never before the first, never after the last).
    // `order_by_saved_at` = true sorts source ids by their savedAt (asc)
    // before concat; false honours the caller-supplied order.  Emits
    // {id, title, messageCount, sourcedFrom, mergedAt}.
    int32_t merge(const std::vector<std::string>& source_ids,
                  std::string_view title,
                  std::string_view scope,
                  const Json& separator,
                  bool order_by_saved_at,
                  Json& result) const;

    // Compact a long conversation by folding early messages into a single
    // summary produced by an external LLM (dispatched at the runtime layer)
    // and keeping the tail `keep_last` messages intact.  The store itself
    // is agnostic to how the summary was generated — the caller feeds in
    // the finished summary text so this method can stay purely file-shaped
    // and does not need to reach into the HTTP/provider layer.  Behaviour:
    //   * strategy == "replace": early messages (up to size - keep_last)
    //     are dropped and a single {role:"system", content:summary} entry
    //     is prepended in their place.  Final message count == keep_last + 1.
    //   * strategy == "prepend": every original message is kept, and one
    //     {role:"system", content:summary} entry is prepended at index 0.
    //     Final message count == original + 1.
    // If size <= keep_last there is nothing worth compacting, so the
    // conversation is left untouched and `result.noop == true` is emitted
    // so callers can distinguish "nothing to do" from "compacted".  Missing
    // conversations propagate NOT_FOUND; keep_last <= 0 is rejected by the
    // dispatch layer before we get here.  Emits {id, originalMessageCount,
    // newMessageCount, summaryLength, summary, compactedAt, noop?}.
    int32_t compact(std::string_view conversation_id,
                    size_t keep_last,
                    std::string_view summary,
                    std::string_view strategy,
                    Json& result) const;

    // Cleave a single conversation into two halves at `message_index`.
    // Messages [0..message_index] (inclusive) form the "before" half;
    // [message_index + 1..end] form the "after" half.  Both halves need
    // at least one message, so message_index must sit strictly inside
    // (0..size - 1).  When `keep_original` is false (default) the source
    // is rewritten in-place with the "before" messages (id preserved so
    // existing references stay live) and a fresh conversation is created
    // for the "after" half.  When `keep_original` is true the source is
    // left untouched and both halves are created as new conversations.
    // Emits {original: {id, messageCount}, latter: {id, title,
    // messageCount}, splitAt}.
    int32_t split(std::string_view source_id,
                  size_t message_index,
                  std::string_view title_before,
                  std::string_view title_after,
                  std::string_view scope,
                  bool keep_original,
                  Json& result) const;

private:
    int32_t locate(std::string_view conversation_id,
                   std::filesystem::path& path) const;
    int32_t save(const std::filesystem::path& path, const Json& value) const;

    const ScopeStore& scopes_;
};

}  // namespace sao::ai_editor::native
