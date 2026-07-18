#include "tool_result_cache.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace sao::ai_editor::native {

namespace {

// Recursively serialise a JSON value with object keys sorted lexicographically,
// so `{"a":1,"b":2}` and `{"b":2,"a":1}` collapse to the same cache key.  This
// keeps the key stable under nlohmann::json's insertion order (which can vary
// across calls that construct the same logical arguments in different orders).
void canonical_dump(const Json& value, std::string& out) {
    switch (value.type()) {
        case Json::value_t::object: {
            std::vector<std::string> keys;
            keys.reserve(value.size());
            for (auto it = value.begin(); it != value.end(); ++it) {
                keys.push_back(it.key());
            }
            std::sort(keys.begin(), keys.end());
            out.push_back('{');
            bool first = true;
            for (const auto& key : keys) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                // Reuse nlohmann's string escaping for the key.  Json(key).dump()
                // produces `"key"` with quotes and JSON escapes already applied,
                // so we can append it verbatim.
                out.append(Json(key).dump());
                out.push_back(':');
                canonical_dump(value.at(key), out);
            }
            out.push_back('}');
            break;
        }
        case Json::value_t::array: {
            out.push_back('[');
            bool first = true;
            for (const auto& element : value) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                canonical_dump(element, out);
            }
            out.push_back(']');
            break;
        }
        default:
            out.append(value.dump());
            break;
    }
}

bool is_builtin_read_only(const std::string& name) noexcept {
    return name == "readFile" || name == "listFiles" || name == "searchFiles";
}

}  // namespace

ToolResultCache::ToolResultCache() = default;

CacheClassification ToolResultCache::classify(const std::string& tool_name,
                                              const Json& input) {
    CacheClassification cls{};
    if (tool_name == "readFile") {
        cls.cls = CacheClass::Fast;
        return cls;
    }
    if (tool_name == "listFiles") {
        // Recursive listings are much heavier + churn less often, so bucket
        // them in the Slow (5min) TTL.  Anything else is Fast (30s).
        const bool recursive = input.is_object() &&
                               input.contains("recursive") &&
                               input["recursive"].is_boolean() &&
                               input["recursive"].get<bool>();
        cls.cls = recursive ? CacheClass::Slow : CacheClass::Fast;
        return cls;
    }
    if (tool_name == "searchFiles") {
        cls.cls = CacheClass::Medium;
        return cls;
    }
    if (tool_name == "editFile") {
        // Mutation: never cached, invalidates readFile / listFiles /
        // searchFiles.  Path-scoped invalidation for readFile only makes sense
        // when we know the argument; listFiles / searchFiles get flushed
        // wholesale because their prior results may have referenced the
        // just-edited path indirectly (search hits, directory listings).
        cls.cls = CacheClass::None;
        cls.invalidates_tools = {"readFile", "listFiles", "searchFiles"};
        cls.invalidates_path = extract_path(input);
        return cls;
    }
    // Every other tool (custom registrations, alias targets that failed to
    // resolve, unknown names) stays uncached — safer default than a wrong
    // classification silently returning stale output.
    cls.cls = CacheClass::None;
    return cls;
}

void ToolResultCache::observe(const std::string& tool_name,
                              const Json& input) {
    const CacheClassification cls = classify(tool_name, input);
    if (cls.invalidates_tools.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t evicted = invalidate_locked(cls);
    invalidation_count_ += evicted;
}

std::optional<ToolResultCacheHit> ToolResultCache::lookup(
    const std::string& tool_name, const Json& input) {
    const CacheClassification cls = classify(tool_name, input);
    if (cls.cls == CacheClass::None) {
        // Not cached at all — don't burn a miss for tools we never plan to
        // hit for (editFile, custom tools).  The counters only track calls
        // that entered the read-only cache path.
        return std::nullopt;
    }
    const std::string key = make_key(tool_name, input);
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t now = now_ms();
    evict_expired_locked(now);
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
        ++miss_count_;
        return std::nullopt;
    }
    ++hit_count_;
    ToolResultCacheHit hit{};
    hit.result = it->second.result;
    hit.timestamp_ms = it->second.timestamp_ms;
    hit.cls = it->second.cls;
    return hit;
}

void ToolResultCache::record(const std::string& tool_name, const Json& input,
                             const Json& result) {
    const CacheClassification cls = classify(tool_name, input);
    if (cls.cls == CacheClass::None) {
        return;
    }
    const std::string key = make_key(tool_name, input);
    std::lock_guard<std::mutex> lock(mutex_);
    Entry entry{};
    entry.result = result;
    entry.timestamp_ms = now_ms();
    entry.cls = cls.cls;
    entry.tool_name = tool_name;
    entry.path_hint = extract_path(input);
    // LRU bump: if the key already lived here, drop it from the order vector
    // so the fresh push_back lands at the tail.
    if (entries_.find(key) != entries_.end()) {
        auto order_it = std::find(insertion_order_.begin(),
                                  insertion_order_.end(), key);
        if (order_it != insertion_order_.end()) {
            insertion_order_.erase(order_it);
        }
    }
    entries_[key] = std::move(entry);
    insertion_order_.push_back(key);
    while (insertion_order_.size() > kMaxEntries) {
        const std::string oldest = insertion_order_.front();
        insertion_order_.erase(insertion_order_.begin());
        entries_.erase(oldest);
    }
}

void ToolResultCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    insertion_order_.clear();
    hit_count_ = 0;
    miss_count_ = 0;
    invalidation_count_ = 0;
}

Json ToolResultCache::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Json by_class = {{"Fast", 0}, {"Medium", 0}, {"Slow", 0}};
    for (const auto& [key, entry] : entries_) {
        (void)key;
        switch (entry.cls) {
            case CacheClass::Fast:
                by_class["Fast"] = by_class["Fast"].get<int64_t>() + 1;
                break;
            case CacheClass::Medium:
                by_class["Medium"] = by_class["Medium"].get<int64_t>() + 1;
                break;
            case CacheClass::Slow:
                by_class["Slow"] = by_class["Slow"].get<int64_t>() + 1;
                break;
            case CacheClass::None:
                break;
        }
    }
    Json out;
    out["totalEntries"] = static_cast<int64_t>(entries_.size());
    out["hitCount"] = static_cast<int64_t>(hit_count_);
    out["missCount"] = static_cast<int64_t>(miss_count_);
    out["invalidationCount"] = static_cast<int64_t>(invalidation_count_);
    out["byClass"] = std::move(by_class);
    return out;
}

int64_t ToolResultCache::ttl_ms(CacheClass cls) noexcept {
    switch (cls) {
        case CacheClass::Fast:
            return 30'000;
        case CacheClass::Medium:
            return 120'000;
        case CacheClass::Slow:
            return 300'000;
        case CacheClass::None:
        default:
            return 0;
    }
}

std::string ToolResultCache::canonical_json(const Json& value) {
    std::string out;
    canonical_dump(value, out);
    return out;
}

std::string ToolResultCache::make_key(const std::string& tool_name,
                                      const Json& input) {
    std::string out;
    out.reserve(tool_name.size() + 64);
    out.append(tool_name);
    out.push_back('|');
    out.append(canonical_json(input));
    return out;
}

std::string ToolResultCache::extract_path(const Json& input) {
    if (!input.is_object() || !input.contains("path")) {
        return {};
    }
    const auto& value = input["path"];
    if (!value.is_string()) {
        return {};
    }
    return value.get<std::string>();
}

int64_t ToolResultCache::now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool ToolResultCache::path_matches_prefix(const std::string& stored,
                                          const std::string& prefix) noexcept {
    if (prefix.empty()) {
        return true;
    }
    if (stored.size() < prefix.size()) {
        return false;
    }
    return std::equal(prefix.begin(), prefix.end(), stored.begin());
}

size_t ToolResultCache::invalidate_locked(const CacheClassification& cls) {
    size_t removed = 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
        const auto& entry = it->second;
        const bool matches_tool =
            std::find(cls.invalidates_tools.begin(),
                      cls.invalidates_tools.end(),
                      entry.tool_name) != cls.invalidates_tools.end();
        if (!matches_tool) {
            ++it;
            continue;
        }
        // Path-scoped invalidation only applies to tools that stored a path
        // hint (readFile); for listFiles / searchFiles we deliberately flush
        // every entry because a directory listing / search hit set can no
        // longer be trusted after any edit under the workspace.
        const bool tool_uses_path_hint = entry.tool_name == "readFile";
        bool should_evict = true;
        if (tool_uses_path_hint && !cls.invalidates_path.empty()) {
            // Only evict when the stored path shares a prefix with the
            // mutation target.  Empty stored path (readFile with missing
            // "path") is a schema error we never reached, but treat it as a
            // miss to be safe.
            should_evict = !entry.path_hint.empty() &&
                           path_matches_prefix(entry.path_hint,
                                               cls.invalidates_path);
        }
        if (!should_evict) {
            ++it;
            continue;
        }
        const std::string key = it->first;
        it = entries_.erase(it);
        auto order_it = std::find(insertion_order_.begin(),
                                  insertion_order_.end(), key);
        if (order_it != insertion_order_.end()) {
            insertion_order_.erase(order_it);
        }
        ++removed;
    }
    return removed;
}

void ToolResultCache::evict_expired_locked(int64_t now) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        const int64_t ttl = ttl_ms(it->second.cls);
        if (ttl > 0 && (now - it->second.timestamp_ms) > ttl) {
            const std::string key = it->first;
            it = entries_.erase(it);
            auto order_it = std::find(insertion_order_.begin(),
                                      insertion_order_.end(), key);
            if (order_it != insertion_order_.end()) {
                insertion_order_.erase(order_it);
            }
        } else {
            ++it;
        }
    }
}

}  // namespace sao::ai_editor::native
