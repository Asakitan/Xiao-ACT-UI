// Frozen Legacy WebView compatibility stub.
//
// Provides three probe entrypoints so the launcher, operator dialogs
// and CTest fixtures can prove the surface is intentionally frozen.
// The manifest at `docs/legacy_webview_manifest.md` lists every
// Python-side API/event that would need to be implemented if the
// surface were ever unfrozen — this cpp only records the freeze,
// it never touches CoreWebView2 or CDOM.

#include "sao/ui/legacy_webview.h"

#include "sao/core/status.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Sentinel Python-side reference points (matches
// docs/legacy_webview_manifest.md §1 top-level snapshot).  If a caller
// hands us a valid python_side_path, we look for these files and
// report which are still present.  We do NOT parse the files or
// mutate anything.
constexpr const char* kFrozenReferences[] = {
    "sao_webview.py",
    "web/menu.html",
    "web/act_aggregate.html",
    "web/piano.html",
    "web/status.html",
    "web/viz.html",
    "web/theme.css",
    "web/act_panel_util.js",
    "web/plugin_layer.js",
};

// Append `s` into `out` capped by `cap`.  Returns true on success,
// false when the buffer would overflow.  Always keeps `out` NUL
// terminated.
bool append(char* out, size_t cap, size_t& used, const char* s) {
    if (out == nullptr || cap == 0) return false;
    const size_t n = std::strlen(s);
    if (used + n + 1 >= cap) {
        // Best-effort truncated string + NUL terminator.
        if (used < cap) {
            const size_t take = cap - used - 1;
            std::memcpy(out + used, s, take);
            used += take;
            out[used] = '\0';
        }
        return false;
    }
    std::memcpy(out + used, s, n);
    used += n;
    out[used] = '\0';
    return true;
}

bool file_exists(const std::string& path) {
    // Portable existence check without pulling in <filesystem> to keep
    // the DLL surface minimal.  Uses stdio fopen.
    FILE* f = nullptr;
#if defined(_WIN32)
    if (fopen_s(&f, path.c_str(), "rb") != 0) return false;
#else
    f = std::fopen(path.c_str(), "rb");
#endif
    if (f == nullptr) return false;
    std::fclose(f);
    return true;
}

}  // namespace

extern "C" SAO_UI_API bool SAO_UI_CALL sao_ui_legacy_webview_available(void) {
    // Frozen surface — see docs/legacy_webview_manifest.md.
    // Flip to true only when the surface is unfrozen and every entry
    // in the manifest §1 has a native implementation.
    return false;
}

extern "C" SAO_UI_API int32_t SAO_UI_CALL sao_ui_legacy_webview_probe(
    const char* python_side_path_utf8,
    char*       report_out,
    size_t      report_capacity) {
    if (report_out == nullptr || report_capacity == 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    report_out[0] = '\0';

    size_t used = 0;

    // Header first — even without a valid python path we tell the
    // caller the surface is frozen.
    if (!append(report_out, report_capacity, used,
                "legacy_webview: FROZEN (see docs/legacy_webview_manifest.md)\n")) {
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    }

    // No python path — just report freeze status and exit.
    if (python_side_path_utf8 == nullptr || python_side_path_utf8[0] == '\0') {
        if (!append(report_out, report_capacity, used,
                    "python_side_path: <not provided>\n")) {
            return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        }
        return SAO_STATUS_OK;
    }

    // With a python path we probe the sentinel references.
    if (!append(report_out, report_capacity, used, "python_side_path: ")) {
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    }
    if (!append(report_out, report_capacity, used, python_side_path_utf8)) {
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    }
    if (!append(report_out, report_capacity, used, "\n")) {
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    }

    // Normalize trailing slash on the python path.
    std::string base = python_side_path_utf8;
    if (!base.empty() && base.back() != '/' && base.back() != '\\') {
        base.push_back('/');
    }

    size_t present = 0;
    size_t missing = 0;
    for (const char* ref : kFrozenReferences) {
        const std::string full = base + ref;
        const bool exists = file_exists(full);
        if (exists) {
            ++present;
            if (!append(report_out, report_capacity, used, "  [present] ") ||
                !append(report_out, report_capacity, used, ref) ||
                !append(report_out, report_capacity, used, "\n")) {
                return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
            }
        } else {
            ++missing;
            if (!append(report_out, report_capacity, used, "  [missing] ") ||
                !append(report_out, report_capacity, used, ref) ||
                !append(report_out, report_capacity, used, "\n")) {
                return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
            }
        }
    }

    // Summary tail.
    char summary[128];
    std::snprintf(summary, sizeof(summary),
                  "summary: %zu present, %zu missing (of %zu tracked)\n",
                  present, missing,
                  sizeof(kFrozenReferences) / sizeof(kFrozenReferences[0]));
    if (!append(report_out, report_capacity, used, summary)) {
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    }

    return SAO_STATUS_OK;
}

extern "C" SAO_UI_API int32_t SAO_UI_CALL sao_ui_legacy_webview_manifest_path(
    char*  path_out,
    size_t path_capacity) {
    if (path_out == nullptr || path_capacity == 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    // Relative path from repo root (sao_auto/C/).  Callers combine
    // with their own base directory.  Kept as a stable constant so
    // tests can string-compare deterministically.
    constexpr const char* kManifest = "docs/legacy_webview_manifest.md";
    const size_t n = std::strlen(kManifest);
    if (n + 1 > path_capacity) {
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    }
    std::memcpy(path_out, kManifest, n + 1);
    return SAO_STATUS_OK;
}
