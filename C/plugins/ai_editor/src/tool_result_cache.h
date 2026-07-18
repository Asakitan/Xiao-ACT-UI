#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "native_utils.h"

namespace sao::ai_editor::native {

// TTL-bucketed classification for a tool call.  Modelled on the VSCode
// terminalOutputCache (see toolResultCompressor.ts):
//   - Fast   -> 30s   (readFile, listFiles !recursive)
//   - Medium -> 2min  (searchFiles)
//   - Slow   -> 5min  (listFiles recursive)
//   - None   -> not cached (mutations, custom tools, unknown / non-deterministic)
enum class CacheClass {
    None = 0,
    Fast = 1,
    Medium = 2,
    Slow = 3,
};

struct CacheClassification final {
    CacheClass cls = CacheClass::None;
    // If non-empty, treat this call as a mutation and invalidate every prior
    // cache entry whose tool name appears in this list.
    std::vector<std::string> invalidates_tools;
    // If non-empty AND invalidates_tools is non-empty, only invalidate entries
    // whose recorded path argument (best-effort JSON pointer) shares this
    // prefix.  Empty => invalidate every entry for the listed tools.
    std::string invalidates_path;
};

// A cache hit surfaced to the dispatch layer.  `result` is the exact JSON
// payload the previous execute() call produced; the runtime layer re-emits it
// verbatim with a couple of debug annotations added.
struct ToolResultCacheHit final {
    Json result;
    int64_t timestamp_ms = 0;
    CacheClass cls = CacheClass::None;
};

class ToolResultCache final {
public:
    ToolResultCache();

    // Classify a single canonical tool call.  Runs without touching any cache
    // state so callers can pre-decide whether to bother with observe()/lookup()
    // at all (also handy for unit tests).
    static CacheClassification classify(const std::string& tool_name,
                                        const Json& input);

    // Fire the invalidation side-effects for a call *before* execute().  Safe
    // to invoke for read-only tools too — the classify() result decides.
    void observe(const std::string& tool_name, const Json& input);

    // Return the cached result if `(tool_name, input)` was recently recorded
    // under a still-live TTL.  Returns nullopt on miss / expired / not-cached.
    // Increments the internal hit / miss counters.
    std::optional<ToolResultCacheHit> lookup(const std::string& tool_name,
                                             const Json& input);

    // Store the outcome of a successful execute().  No-op for tools classified
    // as CacheClass::None.  When the entry limit (kMaxEntries) is reached the
    // oldest insertion is evicted (LRU-ish: re-recording bumps recency).
    void record(const std::string& tool_name, const Json& input,
                const Json& result);

    // Wipe every entry + counters.  Used by the `tools.cache_clear` dispatch
    // method and by tests.
    void clear();

    // JSON payload for the `tools.cache_stats` dispatch method:
    //   {totalEntries, hitCount, missCount, invalidationCount,
    //    byClass:{Fast:N, Medium:N, Slow:N}}
    Json stats() const;

private:
    struct Entry final {
        Json result;
        int64_t timestamp_ms = 0;
        CacheClass cls = CacheClass::None;
        std::string tool_name;
        std::string path_hint;  // best-effort arguments.path, for path-scoped
                                // invalidation.  Empty when the tool does not
                                // take a path (or the caller omitted it).
    };

    static constexpr size_t kMaxEntries = 256;

    static int64_t ttl_ms(CacheClass cls) noexcept;
    static std::string make_key(const std::string& tool_name, const Json& input);
    static std::string canonical_json(const Json& value);
    static std::string extract_path(const Json& input);
    static int64_t now_ms() noexcept;
    static bool path_matches_prefix(const std::string& stored,
                                    const std::string& prefix) noexcept;

    // Remove any entry whose (tool, path) matches the invalidation directive.
    // Caller must hold mutex_.  Returns the number of evictions so the counter
    // can be incremented.
    size_t invalidate_locked(const CacheClassification& cls);

    // Evict entries whose age exceeds their TTL.  Caller must hold mutex_.
    void evict_expired_locked(int64_t now);

    mutable std::mutex mutex_;
    // Insertion order preserved by keeping a parallel list of keys (kept in
    // sync with entries_ under mutex_).  Prepending is O(1) via a doubly-linked
    // list; std::list is fine here because the map stays small (<= kMaxEntries).
    std::unordered_map<std::string, Entry> entries_;
    // Track insertion order for LRU-style eviction.  Front = oldest.
    std::vector<std::string> insertion_order_;

    uint64_t hit_count_ = 0;
    uint64_t miss_count_ = 0;
    uint64_t invalidation_count_ = 0;
};

}  // namespace sao::ai_editor::native
