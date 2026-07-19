#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "native_utils.h"

namespace sao::ai_editor::native {

// Per-tool execution telemetry.  Modelled on VSCode's outputMonitor.ts
// (IOutputMonitorTelemetryCounters) but adapted to the tool-execution side of
// the pipeline rather than the terminal-input side: we track how many times
// each tool was invoked, how large its results were, how many hit the session
// cache, and how many actually ran through execute() (misses).  These counters
// live outside the ToolResultCache so a `tools.cache_clear` call zeroes cache
// state without wiping the telemetry ledger — the two systems answer different
// questions.
struct ToolExecutionCounters final {
    // Total number of tools.call invocations for this tool, including cache
    // hits.  Matches the LLM's perception of "how many times did I ask for
    // this".
    uint64_t invocations = 0;
    // Sum of `result.dump().size()` for every non-error execution (cache hit
    // OR fresh execute()).  Serves as a proxy for how many bytes the tool has
    // returned to the LLM this session; useful for cost accounting alongside
    // chat.cost_stats.
    uint64_t total_bytes_returned = 0;
    // Number of executions whose result payload contains a truthy `truncated`
    // flag or a non-empty `compressionInfo.filterIds` array.  Both are signals
    // that the tool-result compression pipeline (or the tool itself) shrank
    // the payload before returning it.
    uint64_t truncated_count = 0;
    // Cache hits routed through ToolResultCache::lookup + short-circuited
    // before execute().  Distinct from ToolResultCache::stats().hitCount so
    // clear() cannot desync the two.
    uint64_t cache_hits = 0;
    // Cache misses that fell through to execute().  A tool classified as
    // CacheClass::None is neither counted here nor as a hit — it still bumps
    // invocations + total_bytes_returned so the aggregate `invocations` field
    // stays consistent with the request log.
    uint64_t cache_misses = 0;
    // Number of executions whose result had at least one compression filter
    // fire (compressionInfo.filterIds non-empty).  Overlaps with
    // truncated_count when the payload is both truncated and filtered.
    uint64_t filter_applied_count = 0;
    // Sum of per-call durations in milliseconds (fresh execute() path only —
    // cache hits are treated as duration 0 by the runtime).  Divide by
    // (invocations - cache_hits) at read time for the average.
    uint64_t total_duration_ms = 0;
    // Number of executions that returned a non-OK status.  Kept separate so
    // avg_duration_ms is computed only over successful runs; error paths tend
    // to be near-zero-duration validation failures and would skew the mean.
    uint64_t error_count = 0;
};

class ToolExecutionMonitor final {
public:
    ToolExecutionMonitor();

    // Called once per tools.call, before cache lookup + execute().  Bumps
    // `invocations`.  `tool_name` is the alias-resolved canonical name.
    void record_invocation(const std::string& tool_name);

    // Called after a successful execute() or a cache hit.  `duration_ms`
    // should be 0 for cache hits.  `cache_hit=true` flips the cache_hits
    // counter; `cache_hit=false` for the miss path (fresh execute()).
    // `result` is inspected in-place for truncated / compressionInfo signals.
    void record_result(const std::string& tool_name,
                       const Json& result,
                       int64_t duration_ms,
                       bool cache_hit);

    // Called when execute() returned non-OK.  Bumps error_count only —
    // durations + bytes are intentionally excluded so avg_duration_ms and
    // total_bytes_returned reflect user-facing successful output.
    void record_error(const std::string& tool_name);

    // Wipe every counter for every tool.  Intended for tests / debug console;
    // NOT wired to tools.cache_clear (that path deliberately preserves
    // telemetry so operators can watch cache turnover independently of usage
    // trend data).
    void clear();

    // JSON payload for the `tools.telemetry_stats` dispatch method:
    //   {
    //     "byTool": {
    //       "readFile": {
    //         "invocations": N, "totalBytesReturned": N, "truncatedCount": N,
    //         "cacheHits": N, "cacheMisses": N, "filterAppliedCount": N,
    //         "avgDurationMs": N.NN, "errorCount": N
    //       },
    //       ...
    //     },
    //     "aggregate": {"invocations": N, "totalBytesReturned": N, ...}
    //   }
    Json stats() const;

private:
    static bool result_is_truncated(const Json& result) noexcept;
    static bool result_has_filter_applied(const Json& result) noexcept;
    static uint64_t serialized_size(const Json& result) noexcept;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, ToolExecutionCounters> per_tool_;
};

}  // namespace sao::ai_editor::native
