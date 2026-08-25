#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <iterator>
#include <utility>
#include <vector>

#include "sao/ai_editor/ai_editor_native.h"
#include "sao/ai_editor/ai_editor_status.h"
#include "sao/ui/theme.h"

namespace sao::ai_editor::native {

namespace detail {

inline std::string webview_theme_script(SaoUiThemeId theme) {
    const char* name = theme == SAO_UI_THEME_LIGHT ? "light" : "dark";
    return std::string{"(()=>{const apply=()=>{const root=document.documentElement;if(root){"}
        + "root.dataset.theme='" + name + "';root.style.colorScheme='" + name
        + "';}};apply();if(!document.documentElement)document.addEventListener('DOMContentLoaded',"
          "apply,{once:true});})();";
}

inline char ascii_lower_character(char value) noexcept {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
}

inline std::string ascii_lower_copy(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), ascii_lower_character);
    return result;
}

inline bool html_tag_boundary(char value) noexcept {
    return value == '>' || value == '/' ||
           std::isspace(static_cast<unsigned char>(value)) != 0;
}

inline bool find_html_open_tag(std::string_view html, size_t* out_begin,
                               size_t* out_end) noexcept {
    if (out_begin == nullptr || out_end == nullptr)
        return false;
    for (size_t index = 0; index < html.size();) {
        if (index + 4U <= html.size() && html.substr(index, 4U) == "<!--") {
            const size_t comment_end = html.find("-->", index + 4U);
            if (comment_end == std::string_view::npos)
                return false;
            index = comment_end + 3U;
            continue;
        }
        if (html[index] != '<' || index + 5U > html.size() ||
            ascii_lower_character(html[index + 1U]) != 'h' ||
            ascii_lower_character(html[index + 2U]) != 't' ||
            ascii_lower_character(html[index + 3U]) != 'm' ||
            ascii_lower_character(html[index + 4U]) != 'l' ||
            (index + 5U < html.size() && !html_tag_boundary(html[index + 5U]))) {
            ++index;
            continue;
        }
        char quote = '\0';
        for (size_t cursor = index + 5U; cursor < html.size(); ++cursor) {
            const char value = html[cursor];
            if (quote != '\0') {
                if (value == quote)
                    quote = '\0';
                continue;
            }
            if (value == '\'' || value == '"') {
                quote = value;
            } else if (value == '>') {
                *out_begin = index;
                *out_end = cursor;
                return true;
            }
        }
        return false;
    }
    return false;
}

struct HtmlAttribute {
    size_t begin{};
    size_t end{};
    std::string name;
    std::string value;
};

inline bool parse_html_attributes(std::string_view html, size_t tag_begin,
                                  size_t tag_end,
                                  std::vector<HtmlAttribute>* out_attributes,
                                  bool* out_self_closing) {
    if (out_attributes == nullptr || out_self_closing == nullptr ||
        tag_begin + 5U > tag_end || tag_end > html.size()) {
        return false;
    }
    out_attributes->clear();
    *out_self_closing = false;
    size_t cursor = tag_begin + 5U;
    while (cursor < tag_end) {
        while (cursor < tag_end &&
               std::isspace(static_cast<unsigned char>(html[cursor])) != 0) {
            ++cursor;
        }
        if (cursor >= tag_end)
            break;
        if (html[cursor] == '/') {
            *out_self_closing = true;
            ++cursor;
            continue;
        }
        const size_t attribute_begin = cursor;
        while (cursor < tag_end && html[cursor] != '=' && html[cursor] != '/' &&
               std::isspace(static_cast<unsigned char>(html[cursor])) == 0) {
            ++cursor;
        }
        if (cursor == attribute_begin)
            return false;
        const std::string name = ascii_lower_copy(
            html.substr(attribute_begin, cursor - attribute_begin));
        while (cursor < tag_end &&
               std::isspace(static_cast<unsigned char>(html[cursor])) != 0) {
            ++cursor;
        }
        std::string value;
        if (cursor < tag_end && html[cursor] == '=') {
            ++cursor;
            while (cursor < tag_end &&
                   std::isspace(static_cast<unsigned char>(html[cursor])) != 0) {
                ++cursor;
            }
            if (cursor >= tag_end)
                return false;
            if (html[cursor] == '\'' || html[cursor] == '"') {
                const char quote = html[cursor++];
                const size_t value_begin = cursor;
                while (cursor < tag_end && html[cursor] != quote)
                    ++cursor;
                if (cursor >= tag_end)
                    return false;
                value.assign(html.substr(value_begin, cursor - value_begin));
                ++cursor;
            } else {
                const size_t value_begin = cursor;
                while (cursor < tag_end &&
                       std::isspace(static_cast<unsigned char>(html[cursor])) == 0) {
                    ++cursor;
                }
                value.assign(html.substr(value_begin, cursor - value_begin));
            }
        }
        out_attributes->push_back({attribute_begin, cursor, name, std::move(value)});
    }
    return true;
}

inline std::string trim_ascii_copy(std::string_view value) {
    size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1U])) != 0) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

inline std::string without_color_scheme(std::string_view style) {
    std::string result;
    size_t segment_begin = 0;
    char quote = '\0';
    int32_t parentheses = 0;
    for (size_t index = 0; index <= style.size(); ++index) {
        const bool at_end = index == style.size();
        const char value = at_end ? ';' : style[index];
        if (!at_end && quote != '\0') {
            if (value == '\\' && index + 1U < style.size()) {
                ++index;
            } else if (value == quote) {
                quote = '\0';
            }
            continue;
        }
        if (!at_end && (value == '\'' || value == '"')) {
            quote = value;
            continue;
        }
        if (!at_end && value == '(') {
            ++parentheses;
            continue;
        }
        if (!at_end && value == ')' && parentheses > 0) {
            --parentheses;
            continue;
        }
        if (value != ';' || parentheses != 0)
            continue;
        const std::string declaration = trim_ascii_copy(
            style.substr(segment_begin, index - segment_begin));
        const size_t colon = declaration.find(':');
        const std::string property = colon == std::string::npos
            ? ascii_lower_copy(declaration)
            : ascii_lower_copy(trim_ascii_copy(
                  std::string_view(declaration).substr(0, colon)));
        if (!declaration.empty() && property != "color-scheme") {
            if (!result.empty())
                result.append("; ");
            result.append(declaration);
        }
        segment_begin = index + 1U;
    }
    return result;
}

inline std::string escape_double_quoted_attribute(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (character == '&')
            result.append("&amp;");
        else if (character == '"')
            result.append("&quot;");
        else if (character == '<')
            result.append("&lt;");
        else if (character == '>')
            result.append("&gt;");
        else if (byte < 0x20U || byte == 0x7fU)
            result.append("&#").append(std::to_string(byte)).append(";");
        else
            result.push_back(character);
    }
    return result;
}

inline std::string materialized_html_for_theme(std::string html,
                                               SaoUiThemeId theme,
                                               std::string_view panel_id = {}) {
    const char* name = theme == SAO_UI_THEME_LIGHT ? "light" : "dark";
    size_t tag_begin = 0;
    size_t tag_end = 0;
    if (!find_html_open_tag(html, &tag_begin, &tag_end)) {
        std::string root = "<!doctype html><html data-theme=\"" + std::string(name) +
                           "\" style=\"color-scheme: " + name + "\"";
        if (!panel_id.empty()) {
            root.append(" data-sao-panel-id=\"")
                .append(escape_double_quoted_attribute(panel_id))
                .append("\"");
        }
        return root + "><body>" + html + "</body></html>";
    }

    std::vector<HtmlAttribute> attributes;
    bool self_closing = false;
    if (!parse_html_attributes(html, tag_begin, tag_end, &attributes, &self_closing))
        return {};

    std::string style;
    for (const HtmlAttribute& attribute : attributes) {
        if (attribute.name != "style")
            continue;
        const std::string retained = without_color_scheme(attribute.value);
        if (!retained.empty()) {
            if (!style.empty())
                style.append("; ");
            style.append(retained);
        }
    }
    if (!style.empty())
        style.append("; ");
    style.append("color-scheme: ").append(name);

    std::string rebuilt = html.substr(tag_begin, 5U);
    bool wrote_theme = false;
    bool wrote_style = false;
    bool wrote_panel_id = false;
    for (const HtmlAttribute& attribute : attributes) {
        if (attribute.name == "data-theme") {
            if (!wrote_theme) {
                rebuilt.append(" data-theme=\"").append(name).append("\"");
                wrote_theme = true;
            }
        } else if (attribute.name == "style") {
            if (!wrote_style) {
                rebuilt.append(" style=\"")
                    .append(escape_double_quoted_attribute(style))
                    .append("\"");
                wrote_style = true;
            }
        } else if (attribute.name == "data-sao-panel-id") {
            if (!wrote_panel_id && !panel_id.empty()) {
                rebuilt.append(" data-sao-panel-id=\"")
                    .append(escape_double_quoted_attribute(panel_id))
                    .append("\"");
                wrote_panel_id = true;
            }
        } else {
            rebuilt.push_back(' ');
            rebuilt.append(html, attribute.begin, attribute.end - attribute.begin);
        }
    }
    if (!wrote_theme)
        rebuilt.append(" data-theme=\"").append(name).append("\"");
    if (!wrote_style) {
        rebuilt.append(" style=\"")
            .append(escape_double_quoted_attribute(style))
            .append("\"");
    }
    if (!wrote_panel_id && !panel_id.empty()) {
        rebuilt.append(" data-sao-panel-id=\"")
            .append(escape_double_quoted_attribute(panel_id))
            .append("\"");
    }
    if (self_closing)
        rebuilt.append(" /");
    rebuilt.push_back('>');
    html.replace(tag_begin, tag_end - tag_begin + 1U, rebuilt);
    return html;
}

template <typename Navigate, typename Execute>
inline bool apply_webview_document_update(bool html_changed,
                                          Navigate&& navigate,
                                          Execute&& execute) {
    return html_changed
        ? std::invoke(std::forward<Navigate>(navigate))
        : std::invoke(std::forward<Execute>(execute));
}

template <typename Execute, typename Commit>
inline bool sync_webview_theme(int32_t applied_theme,
                               SaoUiThemeId desired_theme,
                               Execute&& execute,
                               Commit&& commit) {
    const int32_t desired = static_cast<int32_t>(desired_theme);
    if (applied_theme == desired) {
        return true;
    }
    if (!std::invoke(std::forward<Execute>(execute))) {
        return false;
    }
    std::invoke(std::forward<Commit>(commit), desired);
    return true;
}

}  // namespace detail

// Config for one off-screen WebView2 technical surface. The HWND is never a
// user-visible panel; the SaoAuto compositor is the only visible consumer.
struct WebViewConfig {
    std::string url;              // navigate target (about:blank if empty)
    std::string user_data_folder; // required by WebView2
    std::string window_title;     // Win32 window caption
    int width = 1280;
    int height = 800;
    // When true, incoming WebMessageReceived JSON payloads are routed into
    // the NativeRuntime hidden behind `runtime_handle`.  Requires
    // `runtime_handle` to be non-null.
    bool bridge_native_runtime = true;
    sao_ai_editor_runtime_t runtime_handle = nullptr;
    // Required compositor bridge frame ring. The WebView2 HWND is created
    // off-screen and captured into this SOPF-compatible MMF source.
    std::string sao_mmf_name_utf8;
    // Required compositor input ring. The bridge polls InputEvent records
    // and forwards them to the off-screen WebView2 HWND.
    std::string sao_input_ring_name_utf8;
};

// Blocking helper: creates an off-screen STA surface, boots CoreWebView2,
// spins its message pump, and returns after a bridge close request or setup
// failure. Missing/invalid MMF or input rings fail closed before WebView2 is
// exposed; no visible fallback exists.
SAO_AI_EDITOR_API int32_t run_webview_bridge(const WebViewConfig& config);

// Detection helper — returns true iff WebView2Loader.dll can be loaded
// and CreateCoreWebView2EnvironmentWithOptions is exported.
SAO_AI_EDITOR_API bool webview_runtime_available();

}  // namespace sao::ai_editor::native
