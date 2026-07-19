#include "tool_execution_monitor.h"

#include <algorithm>

namespace sao::ai_editor::native {

ToolExecutionMonitor::ToolExecutionMonitor() = default;

void ToolExecutionMonitor::record_invocation(const std::string& tool_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& row = per_tool_[tool_name];
    ++row.invocations;
}

void ToolExecutionMonitor::record_result(const std::string& tool_name,
                                          const Json& result,
                                          int64_t duration_ms,
                                          bool cache_hit) {
    const uint64_t bytes = serialized_size(result);
    const bool truncated = result_is_truncated(result);
    const bool filter_applied = result_has_filter_applied(result);
    std::lock_guard<std::mutex> lock(mutex_);
    auto& row = per_tool_[tool_name];
    row.total_bytes_returned += bytes;
    if (truncated) {
        ++row.truncated_count;
    }
    if (filter_applied) {
        ++row.filter_applied_count;
    }
    if (cache_hit) {
        ++row.cache_hits;
    } else {
        ++row.cache_misses;
        // Only fold execute() durations into the aggregate; cache hits are
        // conceptually 0ms and would depress the mean.
        if (duration_ms > 0) {
            row.total_duration_ms += static_cast<uint64_t>(duration_ms);
        }
    }
}

void ToolExecutionMonitor::record_error(const std::string& tool_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& row = per_tool_[tool_name];
    ++row.error_count;
}

void ToolExecutionMonitor::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    per_tool_.clear();
}

Json ToolExecutionMonitor::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Json by_tool = Json::object();
    ToolExecutionCounters aggregate{};
    for (const auto& [name, counters] : per_tool_) {
        Json row = Json::object();
        row["invocations"] = static_cast<int64_t>(counters.invocations);
        row["totalBytesReturned"] =
            static_cast<int64_t>(counters.total_bytes_returned);
        row["truncatedCount"] =
            static_cast<int64_t>(counters.truncated_count);
        row["cacheHits"] = static_cast<int64_t>(counters.cache_hits);
        row["cacheMisses"] = static_cast<int64_t>(counters.cache_misses);
        row["filterAppliedCount"] =
            static_cast<int64_t>(counters.filter_applied_count);
        row["errorCount"] = static_cast<int64_t>(counters.error_count);
        // Only cache-miss executions contribute to total_duration_ms; divide
        // by that same denominator to keep the arithmetic consistent (cache
        // hits are excluded from the mean because their duration is defined
        // as zero and would drag the average down as the hit ratio climbs).
        const uint64_t denom = counters.cache_misses;
        const double avg = denom == 0
                                ? 0.0
                                : static_cast<double>(counters.total_duration_ms) /
                                      static_cast<double>(denom);
        row["avgDurationMs"] = avg;
        by_tool[name] = std::move(row);

        aggregate.invocations += counters.invocations;
        aggregate.total_bytes_returned += counters.total_bytes_returned;
        aggregate.truncated_count += counters.truncated_count;
        aggregate.cache_hits += counters.cache_hits;
        aggregate.cache_misses += counters.cache_misses;
        aggregate.filter_applied_count += counters.filter_applied_count;
        aggregate.total_duration_ms += counters.total_duration_ms;
        aggregate.error_count += counters.error_count;
    }

    Json agg = Json::object();
    agg["invocations"] = static_cast<int64_t>(aggregate.invocations);
    agg["totalBytesReturned"] =
        static_cast<int64_t>(aggregate.total_bytes_returned);
    agg["truncatedCount"] =
        static_cast<int64_t>(aggregate.truncated_count);
    agg["cacheHits"] = static_cast<int64_t>(aggregate.cache_hits);
    agg["cacheMisses"] = static_cast<int64_t>(aggregate.cache_misses);
    agg["filterAppliedCount"] =
        static_cast<int64_t>(aggregate.filter_applied_count);
    agg["errorCount"] = static_cast<int64_t>(aggregate.error_count);
    const uint64_t agg_denom = aggregate.cache_misses;
    const double agg_avg = agg_denom == 0
                                ? 0.0
                                : static_cast<double>(aggregate.total_duration_ms) /
                                      static_cast<double>(agg_denom);
    agg["avgDurationMs"] = agg_avg;

    Json out = Json::object();
    out["byTool"] = std::move(by_tool);
    out["aggregate"] = std::move(agg);
    return out;
}

bool ToolExecutionMonitor::result_is_truncated(const Json& result) noexcept {
    if (!result.is_object()) {
        return false;
    }
    // Top-level `truncated: true` covers ReadFileTruncate + LLM-side truncation
    // signals.  Non-boolean or absent → not truncated.
    if (result.contains("truncated") && result["truncated"].is_boolean()) {
        return result["truncated"].get<bool>();
    }
    // ListFilesFolder + SearchFilesCollapse stash their signal under
    // compressionInfo.truncated for consumers who need the more detailed shape.
    if (result.contains("compressionInfo") &&
        result["compressionInfo"].is_object()) {
        const auto& info = result["compressionInfo"];
        if (info.contains("truncated") && info["truncated"].is_boolean()) {
            return info["truncated"].get<bool>();
        }
    }
    return false;
}

bool ToolExecutionMonitor::result_has_filter_applied(
    const Json& result) noexcept {
    if (!result.is_object()) {
        return false;
    }
    if (!result.contains("compressionInfo") ||
        !result["compressionInfo"].is_object()) {
        return false;
    }
    const auto& info = result["compressionInfo"];
    if (!info.contains("filterIds") || !info["filterIds"].is_array()) {
        return false;
    }
    return !info["filterIds"].empty();
}

uint64_t ToolExecutionMonitor::serialized_size(const Json& result) noexcept {
    // dump() with default options avoids the pretty-printer allocation cost;
    // the size numbers matter for relative comparison across tools, not for
    // wire-accuracy.  Errors during serialisation are treated as 0 bytes.
    try {
        return static_cast<uint64_t>(result.dump().size());
    } catch (...) {
        return 0;
    }
}

}  // namespace sao::ai_editor::native
