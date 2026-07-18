#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "native_utils.h"

namespace sao::ai_editor::native {

// Outcome of a single filter's apply().  `compressed` is `true` iff the filter
// materially changed the JSON payload it was handed; `filter_id` echoes the
// filter that ran so the dispatch layer can build the fired-filter list without
// re-walking the registry.  When the filter declines to touch a payload it must
// return `compressed=false` and leave `result` unchanged (matches VSCode's
// "never make output worse than input" contract).
struct ToolResultFilterOutput final {
    bool compressed = false;
    std::string filter_id;
};

// Interface for a single tool-result compressor.  Compared to VSCode's
// IToolResultFilter this one operates on the whole JSON result object rather
// than a single text part — the native tools return structured payloads
// (listFiles.entries, searchFiles.results, readFile.content) and per-field
// rewriting is safer than serialising the object back to text.
class IToolResultFilter {
public:
    virtual ~IToolResultFilter() = default;

    // Stable id used in compressionInfo.filterIds + banner formatting.  Kept as
    // a reference-returning noexcept accessor so registry snapshots do not
    // require string copies.
    virtual const std::string& id() const noexcept = 0;

    // Cheap yes/no gate: called once per registered filter per tool.call so
    // implementations should avoid heavy work.  `input` is the arguments Json
    // the caller supplied — a filter can key on both tool name AND arguments
    // (e.g. skip readFile compression when the caller explicitly requested a
    // line range).  Never mutates its inputs.
    virtual bool matches(const std::string& tool_name,
                         const Json& input) const = 0;

    // Rewrite `result` in place.  The filter MUST return
    // `compressed=false` (and leave `result` unchanged) when it decides not to
    // compress after all — dispatch uses that signal to skip banner + info
    // bookkeeping.  `input` is the caller's arguments, echoed here for filters
    // that need to correlate result-size heuristics with caller intent.
    virtual ToolResultFilterOutput apply(Json& result,
                                         const Json& input) const = 0;
};

// Chain-of-responsibility registry.  Filters are appended in registration
// order; apply_filters() walks the whole chain and each filter that matches +
// actually compresses feeds its id into `fired_filter_ids`.  The mutex only
// guards the vector — every filter itself must be internally thread-safe (all
// three built-ins are stateless).
class ToolResultFilterRegistry final {
public:
    void register_filter(std::shared_ptr<IToolResultFilter> filter);

    // Snapshot filter count under the mutex.  Only used by tests +
    // introspection code — production dispatch never needs it.
    size_t size() const;

    // Run the chain over `result` in place.  Returns `true` iff at least one
    // filter reported compression; on `false` the caller should treat the
    // payload as unchanged (no compressionInfo, no banner).  On `true` the
    // caller can inspect `fired_filter_ids` to populate compressionInfo.
    bool apply_filters(const std::string& tool_name,
                       const Json& input,
                       Json& result,
                       std::vector<std::string>& fired_filter_ids) const;

private:
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<IToolResultFilter>> filters_;
};

// --------------------------------------------------------------------------
// Helpers reused by every filter + the dispatch layer.

// Outputs below this many bytes (UTF-8 code units) are not worth compressing;
// banner overhead outweighs savings and the model may reissue reads to recover
// what would have fit uncompressed.  Matches VSCode's MIN_COMPRESSIBLE_LENGTH
// scaled for our larger cap on file bytes.
constexpr size_t kMinCompressibleLength = 4096;

// Detect payloads the model is likely to parse (JSON/YAML/TOML).  Returning
// `true` means the caller should skip compression entirely — false positives
// would corrupt structured output the LLM is about to feed into a parser.
// Cheap heuristics only: full JSON parse for `{..}`/`[..]`, prefix checks for
// YAML `---\n` and TOML `[section]\n`.  false negatives are fine — a filter
// simply declines further compression.
bool is_protected_from_compression(std::string_view text) noexcept;

// Format the "[SAO output compressed by X, N→M bytes]" banner filters prepend
// to their rewritten text.  Kept out of the filter class so the truncate
// filter (readFile) and any user-registered filter can share the same shape.
std::string format_compression_banner(const std::vector<std::string>& filter_ids,
                                      size_t before_bytes,
                                      size_t after_bytes);

// --------------------------------------------------------------------------
// Built-in filters.  Factory functions so NativeRuntime can register them
// without exposing concrete types (keeps the vtable in one translation unit).

std::shared_ptr<IToolResultFilter> make_list_files_folder_filter();
std::shared_ptr<IToolResultFilter> make_search_files_collapse_filter();
std::shared_ptr<IToolResultFilter> make_read_file_truncate_filter();

}  // namespace sao::ai_editor::native
