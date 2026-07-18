#include "tool_result_filter.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>

namespace sao::ai_editor::native {
namespace {

// Threshold constants — extracted so filter selftests can reference them.
constexpr size_t kListFilesCollapseThreshold = 200;
constexpr size_t kListFilesTopLevelKeepBudget = 200;
constexpr size_t kSearchFilesPerFileSamples = 3;
constexpr size_t kSearchFilesPerFileCollapseThreshold = 4;
constexpr size_t kReadFileTruncateThreshold = 32u * 1024u;
constexpr size_t kReadFileTruncateHeadBytes = 4u * 1024u;

// Cheap JSON-parse guard for is_protected_from_compression().  We do not need
// the parsed value — just whether the string was a valid top-level document.
bool looks_like_valid_json(std::string_view trimmed) noexcept {
    if (trimmed.empty()) {
        return false;
    }
    const char first = trimmed.front();
    const char last = trimmed.back();
    if (!((first == '{' && last == '}') || (first == '[' && last == ']'))) {
        return false;
    }
    // nlohmann::json::accept is nothrow + O(n) and matches the compressor
    // heuristic (validate structure, discard value).
    return Json::accept(trimmed);
}

std::string_view ltrim(std::string_view value) noexcept {
    size_t index = 0;
    while (index < value.size() &&
           std::isspace(static_cast<unsigned char>(value[index]))) {
        ++index;
    }
    return value.substr(index);
}

std::string_view rtrim(std::string_view value) noexcept {
    size_t index = value.size();
    while (index > 0 &&
           std::isspace(static_cast<unsigned char>(value[index - 1]))) {
        --index;
    }
    return value.substr(0, index);
}

// Match `^\[[A-Za-z_][A-Za-z0-9_.-]*\]\s*\n` — a leading TOML-style section
// header on its own line.  Implemented by hand to avoid dragging <regex> into
// a hot path.
bool looks_like_toml_header(std::string_view trimmed) noexcept {
    if (trimmed.size() < 4 || trimmed.front() != '[') {
        return false;
    }
    size_t index = 1;
    if (!(std::isalpha(static_cast<unsigned char>(trimmed[index])) ||
          trimmed[index] == '_')) {
        return false;
    }
    ++index;
    while (index < trimmed.size()) {
        const char ch = trimmed[index];
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' ||
            ch == '.' || ch == '-') {
            ++index;
            continue;
        }
        break;
    }
    if (index >= trimmed.size() || trimmed[index] != ']') {
        return false;
    }
    ++index;
    // Trailing whitespace up to a newline.
    while (index < trimmed.size() && trimmed[index] != '\n' &&
           std::isspace(static_cast<unsigned char>(trimmed[index]))) {
        ++index;
    }
    return index < trimmed.size() && trimmed[index] == '\n';
}

// Match `^---\s*\n` — a YAML document opener.
bool looks_like_yaml_header(std::string_view trimmed) noexcept {
    if (trimmed.size() < 4) {
        return false;
    }
    if (trimmed[0] != '-' || trimmed[1] != '-' || trimmed[2] != '-') {
        return false;
    }
    size_t index = 3;
    while (index < trimmed.size() && trimmed[index] != '\n' &&
           std::isspace(static_cast<unsigned char>(trimmed[index]))) {
        ++index;
    }
    return index < trimmed.size() && trimmed[index] == '\n';
}

// Split a path like "src/foo/bar.ts" into top-level segment ("src") + the
// rest.  Used by ListFilesFolder to key collapse groups by first path segment.
// Handles both `/` and `\` because listFiles returns generic_wstring output
// that may include forward slashes.
std::pair<std::string, bool> top_level_segment(const std::string& name) {
    for (size_t index = 0; index < name.size(); ++index) {
        if (name[index] == '/' || name[index] == '\\') {
            return {name.substr(0, index), true};
        }
    }
    return {name, false};
}

// -----------------------------------------------------------------------------
// ListFilesFolder — collapse large recursive listings by top-level directory.
class ListFilesFolderFilter final : public IToolResultFilter {
public:
    ListFilesFolderFilter() : id_("ListFilesFolder") {}

    const std::string& id() const noexcept override { return id_; }

    bool matches(const std::string& tool_name,
                 const Json& /*input*/) const override {
        return tool_name == "listFiles";
    }

    ToolResultFilterOutput apply(Json& result,
                                 const Json& /*input*/) const override {
        ToolResultFilterOutput out;
        if (!result.is_object() || !result.contains("entries") ||
            !result["entries"].is_array()) {
            return out;
        }
        Json& entries = result["entries"];
        if (entries.size() <= kListFilesCollapseThreshold) {
            return out;
        }
        // Group entries by top-level directory segment.  Direct-child files +
        // directories keep their slot; nested entries feed into the parent
        // group's `itemsInside` counter.  Insertion order is preserved by
        // tracking a per-group vector index so collapsed entries appear in the
        // same position as their first child (stable ordering keeps the
        // downstream LLM's "roughly alphabetical" mental model intact).
        std::unordered_map<std::string, size_t> group_index;
        struct Group final {
            std::string name;
            size_t items_inside = 0;
            bool has_direct_hit = false;
            Json direct_entry;  // populated when the top-level entry itself is
                                // listed (e.g. `node_modules` as a directory).
        };
        std::vector<Group> groups;
        std::vector<Json> passthrough;  // files whose top-level segment is
                                        // themselves; kept individually.
        for (const auto& entry : entries) {
            if (!entry.is_object() || !entry.contains("name") ||
                !entry["name"].is_string()) {
                passthrough.push_back(entry);
                continue;
            }
            const std::string name = entry["name"].get<std::string>();
            const auto [head, has_children] = top_level_segment(name);
            if (!has_children) {
                // A top-level file/dir — record it as the group's direct
                // representative (so a folder that's also listed keeps its
                // own type/size metadata) and, if it's a file, keep it as a
                // standalone passthrough so we do not accidentally hide it.
                const std::string type = entry.value("type", "");
                if (type == "directory") {
                    auto found = group_index.find(head);
                    if (found == group_index.end()) {
                        group_index[head] = groups.size();
                        groups.push_back({head, 0, true, entry});
                    } else {
                        Group& group = groups[found->second];
                        group.has_direct_hit = true;
                        group.direct_entry = entry;
                    }
                } else {
                    passthrough.push_back(entry);
                }
                continue;
            }
            // Nested entry — count it against the top-level group.
            auto found = group_index.find(head);
            if (found == group_index.end()) {
                group_index[head] = groups.size();
                groups.push_back({head, 1, false, Json{}});
            } else {
                ++groups[found->second].items_inside;
            }
        }
        // Materialise collapsed entries.  A group with items_inside==0 wasn't
        // really "large" (only the directory itself appeared), so re-emit its
        // direct entry unchanged.  Groups with children collapse into a single
        // stub carrying `collapsed:true` + `itemsInside:N`.
        Json rewritten = Json::array();
        rewritten.get_ptr<Json::array_t*>()->reserve(groups.size() +
                                                     passthrough.size());
        for (const auto& group : groups) {
            if (group.items_inside == 0 && group.has_direct_hit) {
                rewritten.push_back(group.direct_entry);
                continue;
            }
            Json stub{{"name", group.name},
                      {"type", "directory"},
                      {"collapsed", true},
                      {"itemsInside", group.items_inside}};
            rewritten.push_back(std::move(stub));
        }
        for (auto& entry : passthrough) {
            rewritten.push_back(std::move(entry));
        }
        // Cap output at the top-level budget just in case a filesystem has
        // >200 top-level directories (unlikely but the budget is a hard
        // guarantee for downstream token accounting).
        if (rewritten.size() > kListFilesTopLevelKeepBudget) {
            const size_t overflow =
                rewritten.size() - kListFilesTopLevelKeepBudget + 1;
            Json overflow_stub{
                {"name", "…"},
                {"type", "directory"},
                {"collapsed", true},
                {"itemsInside", overflow}};
            rewritten.get_ptr<Json::array_t*>()->resize(
                kListFilesTopLevelKeepBudget - 1);
            rewritten.push_back(std::move(overflow_stub));
        }
        const size_t before = entries.size();
        const size_t after = rewritten.size();
        // Guard: never expand.  If our grouping somehow produced *more*
        // entries than the input (impossible for the current algorithm but
        // cheap to check) leave the payload alone.
        if (after >= before) {
            return out;
        }
        result["entries"] = std::move(rewritten);
        result["total"] = result["entries"].size();
        result["originalTotal"] = before;
        out.compressed = true;
        out.filter_id = id_;
        return out;
    }

private:
    std::string id_;
};

// -----------------------------------------------------------------------------
// SearchFilesCollapse — collapse per-file hits when the same file dominates.
class SearchFilesCollapseFilter final : public IToolResultFilter {
public:
    SearchFilesCollapseFilter() : id_("SearchFilesCollapse") {}

    const std::string& id() const noexcept override { return id_; }

    bool matches(const std::string& tool_name,
                 const Json& /*input*/) const override {
        return tool_name == "searchFiles";
    }

    ToolResultFilterOutput apply(Json& result,
                                 const Json& /*input*/) const override {
        ToolResultFilterOutput out;
        if (!result.is_object() || !result.contains("results") ||
            !result["results"].is_array()) {
            return out;
        }
        Json& results = result["results"];
        // Group hits by file, preserving first-seen order so the compressed
        // output mirrors the input ordering.
        std::unordered_map<std::string, size_t> file_index;
        struct Group final {
            std::string file;
            std::vector<Json> hits;
        };
        std::vector<Group> groups;
        for (const auto& hit : results) {
            if (!hit.is_object() || !hit.contains("file") ||
                !hit["file"].is_string()) {
                // Malformed row — leave results alone; a filter that guesses
                // in the face of a broken payload risks losing real hits.
                return out;
            }
            const std::string file = hit["file"].get<std::string>();
            auto found = file_index.find(file);
            if (found == file_index.end()) {
                file_index[file] = groups.size();
                groups.push_back({file, {hit}});
            } else {
                groups[found->second].hits.push_back(hit);
            }
        }
        // If no file has >= threshold hits there's nothing to gain — bail.
        bool any_dense = false;
        for (const auto& group : groups) {
            if (group.hits.size() >= kSearchFilesPerFileCollapseThreshold) {
                any_dense = true;
                break;
            }
        }
        if (!any_dense) {
            return out;
        }
        Json rewritten = Json::array();
        rewritten.get_ptr<Json::array_t*>()->reserve(groups.size());
        for (auto& group : groups) {
            if (group.hits.size() < kSearchFilesPerFileCollapseThreshold) {
                // Emit each individual hit unchanged.
                for (auto& hit : group.hits) {
                    rewritten.push_back(std::move(hit));
                }
                continue;
            }
            Json samples = Json::array();
            const size_t sample_count =
                std::min<size_t>(kSearchFilesPerFileSamples,
                                 group.hits.size());
            for (size_t index = 0; index < sample_count; ++index) {
                samples.push_back(group.hits[index]);
            }
            Json collapsed{{"file", group.file},
                           {"matches", group.hits.size()},
                           {"samples", std::move(samples)}};
            rewritten.push_back(std::move(collapsed));
        }
        const size_t before = results.size();
        const size_t after = rewritten.size();
        if (after >= before) {
            return out;
        }
        result["results"] = std::move(rewritten);
        result["total"] = result["results"].size();
        result["originalTotal"] = before;
        out.compressed = true;
        out.filter_id = id_;
        return out;
    }

private:
    std::string id_;
};

// -----------------------------------------------------------------------------
// ReadFileTruncate — head-truncate huge file reads when the caller did not
// explicitly ask for a line range.
class ReadFileTruncateFilter final : public IToolResultFilter {
public:
    ReadFileTruncateFilter() : id_("ReadFileTruncate") {}

    const std::string& id() const noexcept override { return id_; }

    bool matches(const std::string& tool_name,
                 const Json& input) const override {
        if (tool_name != "readFile") {
            return false;
        }
        // Respect an explicit line range — the caller wanted these lines and
        // truncating a specific window would break their intent.  readFile
        // stores 0 for "no bound", so any non-zero value opts out.
        const int64_t start = input.value("startLine", int64_t{0});
        const int64_t end = input.value("endLine", int64_t{0});
        return start == 0 && end == 0;
    }

    ToolResultFilterOutput apply(Json& result,
                                 const Json& /*input*/) const override {
        ToolResultFilterOutput out;
        if (!result.is_object() || !result.contains("content") ||
            !result["content"].is_string()) {
            return out;
        }
        const std::string& content = result["content"].get_ref<const std::string&>();
        if (content.size() <= kReadFileTruncateThreshold) {
            return out;
        }
        // Do not touch structured payloads even when huge — the caller may be
        // ingesting a giant JSON/YAML doc and a mid-file truncation would
        // corrupt the parse.
        if (is_protected_from_compression(content)) {
            return out;
        }
        // Count original lines so the banner can advertise how many were cut.
        // We cap the count at the truncation point to avoid walking the whole
        // file: model just needs "many more lines" not the exact tail length.
        size_t total_lines = 0;
        for (char ch : content) {
            if (ch == '\n') {
                ++total_lines;
            }
        }
        // Head-truncate at kReadFileTruncateHeadBytes but do not split mid-line
        // — round back to the previous newline so the trailing line stays
        // intact.  Falls through to a hard cut if there is no newline within
        // the head window (unlikely for text).
        size_t cut = std::min<size_t>(kReadFileTruncateHeadBytes,
                                       content.size());
        while (cut > 0 && content[cut - 1] != '\n') {
            --cut;
        }
        if (cut == 0) {
            cut = std::min<size_t>(kReadFileTruncateHeadBytes,
                                    content.size());
        }
        size_t kept_lines = 0;
        for (size_t index = 0; index < cut; ++index) {
            if (content[index] == '\n') {
                ++kept_lines;
            }
        }
        const size_t remaining_lines =
            total_lines >= kept_lines ? total_lines - kept_lines : 0;
        std::string head = content.substr(0, cut);
        const std::vector<std::string> filter_ids{id_};
        std::string banner = format_compression_banner(filter_ids,
                                                        content.size(),
                                                        head.size());
        std::ostringstream tail;
        tail << "\n\n" << banner << "\n["
             << remaining_lines
             << " more lines truncated, use startLine/endLine to fetch full]";
        std::string compressed = std::move(head);
        compressed += tail.str();
        // Guard: refuse to inflate the payload.  The banner is a few dozen
        // bytes so this is only reachable in pathological "single-line 32 KB
        // file" territory, but returning `compressed=false` keeps the
        // "never worse than input" contract intact.
        if (compressed.size() >= content.size()) {
            return out;
        }
        result["compressionInfo"] = Json{
            {"originalBytes", content.size()},
            {"compressedBytes", compressed.size()},
            {"filterIds", filter_ids}};
        result["content"] = std::move(compressed);
        out.compressed = true;
        out.filter_id = id_;
        return out;
    }

private:
    std::string id_;
};

}  // namespace

bool is_protected_from_compression(std::string_view text) noexcept {
    const std::string_view rtrimmed = rtrim(ltrim(text));
    if (rtrimmed.empty()) {
        return false;
    }
    if (looks_like_valid_json(rtrimmed)) {
        return true;
    }
    if (looks_like_yaml_header(rtrimmed) ||
        looks_like_toml_header(rtrimmed)) {
        return true;
    }
    return false;
}

std::string format_compression_banner(
    const std::vector<std::string>& filter_ids,
    size_t before_bytes,
    size_t after_bytes) {
    std::ostringstream out;
    out << "[SAO output compressed by ";
    if (filter_ids.empty()) {
        out << "unknown";
    } else {
        for (size_t index = 0; index < filter_ids.size(); ++index) {
            if (index != 0) {
                out << ", ";
            }
            out << filter_ids[index];
        }
    }
    out << ", " << before_bytes << " \xe2\x86\x92 " << after_bytes
        << " bytes]";
    return out.str();
}

void ToolResultFilterRegistry::register_filter(
    std::shared_ptr<IToolResultFilter> filter) {
    if (!filter) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    filters_.push_back(std::move(filter));
}

size_t ToolResultFilterRegistry::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return filters_.size();
}

bool ToolResultFilterRegistry::apply_filters(
    const std::string& tool_name,
    const Json& input,
    Json& result,
    std::vector<std::string>& fired_filter_ids) const {
    // Snapshot under lock so a concurrent register_filter cannot mutate the
    // vector while we walk it.  Filters themselves must be thread-safe (all
    // built-ins are stateless), so calling apply() outside the lock is fine.
    std::vector<std::shared_ptr<IToolResultFilter>> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = filters_;
    }
    bool any_fired = false;
    for (const auto& filter : snapshot) {
        if (!filter) {
            continue;
        }
        if (!filter->matches(tool_name, input)) {
            continue;
        }
        ToolResultFilterOutput out = filter->apply(result, input);
        if (out.compressed) {
            any_fired = true;
            fired_filter_ids.push_back(
                out.filter_id.empty() ? filter->id() : out.filter_id);
        }
    }
    return any_fired;
}

std::shared_ptr<IToolResultFilter> make_list_files_folder_filter() {
    return std::make_shared<ListFilesFolderFilter>();
}

std::shared_ptr<IToolResultFilter> make_search_files_collapse_filter() {
    return std::make_shared<SearchFilesCollapseFilter>();
}

std::shared_ptr<IToolResultFilter> make_read_file_truncate_filter() {
    return std::make_shared<ReadFileTruncateFilter>();
}

}  // namespace sao::ai_editor::native
