// dialog_leaderboard.cpp — SAOLeaderboardDialog port (Phase 13 production).
//
// Formats rows into a padded UTF-8 text table and dispatches through the
// existing sao_ui_dialog_show_info path. For repeated modals from the same
// caller, use the handle form which composes the same text under
// sao_ui_dialog_create. Sorting is done in-process before formatting.
//
// This is the "portable" implementation — works on top of the current dialog
// engine without depending on a widget_table subwidget (which requires D3D
// device access from within the dialog surface, not yet plumbed).

#include "sao/ui/dialog.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace {

using json = nlohmann::json;

std::string format_leaderboard(const char* const* column_names_utf8,
                                uint32_t column_count,
                                const std::vector<std::vector<std::string>>& rows) {
    // Compute column widths.
    std::vector<size_t> widths(column_count, 0);
    for (uint32_t c = 0; c < column_count; ++c) {
        widths[c] = column_names_utf8[c] ? std::strlen(column_names_utf8[c]) : 0;
    }
    for (const auto& row : rows) {
        for (uint32_t c = 0; c < column_count && c < row.size(); ++c) {
            widths[c] = std::max(widths[c], row[c].size());
        }
    }
    auto pad = [](const std::string& s, size_t w) {
        std::string r = s;
        while (r.size() < w) r.push_back(' ');
        return r;
    };
    std::string out;
    // Header.
    for (uint32_t c = 0; c < column_count; ++c) {
        out += pad(column_names_utf8[c] ? column_names_utf8[c] : "", widths[c]);
        if (c + 1 < column_count) out += "  ";
    }
    out += "\n";
    for (uint32_t c = 0; c < column_count; ++c) {
        out += std::string(widths[c], '-');
        if (c + 1 < column_count) out += "  ";
    }
    out += "\n";
    for (const auto& row : rows) {
        for (uint32_t c = 0; c < column_count; ++c) {
            const std::string& cell = c < row.size() ? row[c] : std::string{};
            out += pad(cell, widths[c]);
            if (c + 1 < column_count) out += "  ";
        }
        out += "\n";
    }
    return out;
}

} // namespace

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dialog_leaderboard_show(
    sao_ui_compositor_handle_t compositor, sao_ui_theme_handle_t theme,
    const char* title_utf8, const char* const* column_names_utf8,
    uint32_t column_count, const char* rows_json_utf8,
    sao_ui_dialog_result_callback_t callback, void* user_data) {
    if (compositor == nullptr || column_count == 0 ||
        column_names_utf8 == nullptr || rows_json_utf8 == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    // Parse rows JSON: [ ["col1", "col2", ...], ... ]
    json rows_json;
    try {
        rows_json = json::parse(rows_json_utf8);
    } catch (...) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (!rows_json.is_array()) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::vector<std::vector<std::string>> rows;
    rows.reserve(rows_json.size());
    for (const auto& r : rows_json) {
        if (!r.is_array()) continue;
        std::vector<std::string> row;
        row.reserve(r.size());
        for (const auto& cell : r) {
            if (cell.is_string()) row.push_back(cell.get<std::string>());
            else if (cell.is_number()) row.push_back(cell.dump());
            else row.push_back(std::string{});
        }
        rows.push_back(std::move(row));
    }
    // Sort by first column DPS-style (numeric descending) if numeric-looking.
    std::sort(rows.begin(), rows.end(),
              [](const std::vector<std::string>& a, const std::vector<std::string>& b) {
                  if (a.empty() || b.empty()) return false;
                  char* end_a = nullptr;
                  char* end_b = nullptr;
                  double va = std::strtod(a[0].c_str(), &end_a);
                  double vb = std::strtod(b[0].c_str(), &end_b);
                  if (end_a != a[0].c_str() && end_b != b[0].c_str())
                      return va > vb;
                  return a[0] < b[0];
              });
    std::string body = format_leaderboard(column_names_utf8, column_count, rows);
    return sao_ui_dialog_show_info(compositor, theme,
                                    title_utf8 ? title_utf8 : "Leaderboard",
                                    body.c_str(), callback, user_data);
}
