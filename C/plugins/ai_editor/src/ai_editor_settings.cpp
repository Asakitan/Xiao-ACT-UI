#include "ai_editor_settings.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "sha256_helper.h"

namespace sao::ai_editor::native {
namespace {

constexpr std::string_view kSchema = "sao.ai_editor.settings";
constexpr int kSchemaVersion = 1;
constexpr std::string_view kSecretPresent = "__SAO_SECRET_PRESENT__";
constexpr std::string_view kProtectedPlaceholder = "<protected>";
constexpr size_t kMaximumSettingStringBytes = 64U * 1024U;
constexpr size_t kMaximumResetKeys = 1024U;

const std::array<std::string_view, 6> kProviderIds{
    "openai", "anthropic", "deepseek", "ollama", "custom", "gemini"};

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

std::string trim_ascii(std::string value) {
    const auto is_space = [](unsigned char character) {
        return std::isspace(character) != 0;
    };
    value.erase(value.begin(),
                std::find_if_not(value.begin(), value.end(), is_space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(),
                value.end());
    return value;
}

std::string string_value(const Json& value, std::string_view fallback = {}) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "true" : "false";
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<int64_t>());
    }
    if (value.is_number_unsigned()) {
        return std::to_string(value.get<uint64_t>());
    }
    if (value.is_number_float()) {
        return std::to_string(value.get<double>());
    }
    return std::string(fallback);
}

bool bool_value(const Json& value, bool fallback) {
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number()) {
        return value.get<double>() != 0.0;
    }
    if (value.is_string()) {
        const std::string normalized = lower_ascii(trim_ascii(value.get<std::string>()));
        if (normalized == "1" || normalized == "true" || normalized == "yes" ||
            normalized == "on" || normalized == "enabled") {
            return true;
        }
        if (normalized == "0" || normalized == "false" || normalized == "no" ||
            normalized == "off" || normalized == "disabled") {
            return false;
        }
    }
    return fallback;
}

double number_value(const Json& value, double fallback) {
    try {
        double result = fallback;
        if (value.is_number()) {
            result = value.get<double>();
        } else if (value.is_string()) {
            size_t consumed = 0;
            result = std::stod(trim_ascii(value.get<std::string>()), &consumed);
            if (consumed != trim_ascii(value.get<std::string>()).size()) {
                return fallback;
            }
        }
        return std::isfinite(result) ? result : fallback;
    } catch (...) {
        return fallback;
    }
}

int64_t integer_value(const Json& value, int64_t fallback) {
    try {
        if (value.is_number_integer()) {
            return value.get<int64_t>();
        }
        if (value.is_number_unsigned()) {
            const uint64_t raw = value.get<uint64_t>();
            return raw <= static_cast<uint64_t>(INT64_MAX)
                       ? static_cast<int64_t>(raw)
                       : fallback;
        }
        if (value.is_number_float()) {
            const double raw = value.get<double>();
            if (!std::isfinite(raw) || raw < static_cast<double>(INT64_MIN) ||
                raw > static_cast<double>(INT64_MAX)) {
                return fallback;
            }
            return static_cast<int64_t>(raw);
        }
        if (value.is_string()) {
            const std::string raw = trim_ascii(value.get<std::string>());
            size_t consumed = 0;
            const long long parsed = std::stoll(raw, &consumed, 10);
            return consumed == raw.size() ? static_cast<int64_t>(parsed)
                                          : fallback;
        }
    } catch (...) {
    }
    return fallback;
}

Json string_list(const Json& value, bool split_commas) {
    Json result = Json::array();
    if (value.is_array()) {
        for (const auto& item : value) {
            const std::string text = trim_ascii(string_value(item));
            if (!text.empty()) {
                result.push_back(text);
            }
        }
        return result;
    }
    if (!value.is_string()) {
        return result;
    }
    const std::string text = value.get<std::string>();
    std::string current;
    const auto flush = [&] {
        const std::string normalized = trim_ascii(current);
        if (!normalized.empty()) {
            result.push_back(normalized);
        }
        current.clear();
    };
    for (const char character : text) {
        if (character == '\r' || character == '\n' ||
            (split_commas && character == ',')) {
            flush();
        } else {
            current.push_back(character);
        }
    }
    flush();
    return result;
}

Json provider_presets() {
    return Json::object({
        {"openai",
         {{"type", "openai"},
          {"base_url", "https://api.openai.com/v1"},
          {"model", "gpt-4o"}}},
        {"anthropic",
         {{"type", "anthropic"},
          {"base_url", "https://api.anthropic.com/v1"},
          {"model", ""}}},
        {"deepseek",
         {{"type", "openai"},
          {"base_url", "https://api.deepseek.com/v1"},
          {"model", "deepseek-chat"}}},
        {"ollama",
         {{"type", "openai"},
          {"base_url", "http://localhost:11434/v1"},
          {"model", "llama3.1"}}},
        {"custom", {{"type", "openai"}, {"base_url", ""}, {"model", ""}}},
        {"gemini",
         {{"type", "gemini"},
          {"base_url",
           "https://generativelanguage.googleapis.com/v1beta/models"},
          {"model", "gemini-2.0-flash"}}},
    });
}

Json defaults_impl() {
    return Json::object({
        {"provider", "openai"},
        {"api_key", ""},
        {"base_url", ""},
        {"model", ""},
        {"temperature", 0.7},
        {"max_tokens", 4096},
        {"system_prompt", ""},
        {"transport", "chat_completions"},
        {"top_p", 1.0},
        {"frequency_penalty", 0.0},
        {"presence_penalty", 0.0},
        {"stop", Json::array()},
        {"max_input_tokens", 0},
        {"max_output_tokens", 0},
        {"timeout", 180},
        {"extra_headers", Json::object()},
        {"extra_body", Json::object()},
        {"provider_keys", Json::object()},
        {"custom_models", Json::object()},
        {"permissions", Json::object()},
        {"configuration_targets", Json::object()},
        {"mode", "agent"},
        {"language", "zh"},
        {"approval", "default"},
        {"active_chat_provider", "chat"},
        {"theme", "dark"},
        {"color_theme", ""},
        {"file_icon_theme", ""},
                {"layout",
                 {{"sidebarVisible", true},
                    {"editorVisible", false},
                    {"chatVisible", true},
                    {"panelHeight", ""},
                    {"sidebarWidth", ""},
                    {"rightSidebarWidth", ""},
                    {"activeSidebarPanel", "chat"},
                    {"activeBottomTab", "terminal"},
                    {"minimapVisible", false}}},
        {"claude_code",
         {{"cli_path", ""},
          {"cli_args", Json::array()},
          {"prefer_cli", false},
          {"allow_dangerously_skip_permissions", false},
          {"model", ""}}},
        {"codex",
         {{"cli_path", ""},
          {"cli_args", Json::array()},
          {"transport", "chat_completions"},
          {"model", "codex-mini-latest"}}},
        {"mcp",
                 {{"enabled", true},
                    {"access", "prompt"},
          {"autostart", false},
          {"discovery_enabled", true},
          {"workspace_trusted", false},
          {"collision_behavior", "first"},
                    {"server_sampling", false},
                    {"servers", Json::array()}}},
        {"terminal",
         {{"profile", "PowerShell 7 (No Profile)"},
          {"shell_path", ""},
          {"shell_args", Json::array()},
          {"timeout", 30},
          {"output_limit", 8000},
          {"auto_approve", Json::object()},
          {"use_pty", false}}},
        {"workspace",
         {{"root", ""},
          {"roots", Json::array()},
          {"recent_roots", Json::array()},
          {"auto_detect", true},
          {"remember_last", true},
          {"last_root", ""}}},
        {"extensions",
         {{"confirm_install", true},
          {"allowed_publishers", Json::array()},
          {"blocked_publishers", Json::array()},
          {"enabled_contributions",
           Json::array({"chatParticipants", "languageModelTools", "commands",
                        "views", "customEditors", "notebooks", "terminal",
                        "statusBarItems"})},
          {"enabled_contributions_explicit", false},
          {"diagnostics_enabled", false}}},
        {"editor",
         {{"defaultFormatter", ""},
          {"formatOnType", false},
          {"formatOnSave", false},
          {"formatOnPaste", false},
          {"linkedEditing", false},
          {"codeActionsOnSave", Json::object()},
          {"codeActions", {{"triggerOnFocusChange", false}}},
          {"pasteAs", {{"preferences", Json::array()}}},
          {"tabSize", 4},
          {"insertSpaces", true},
          {"wordSeparators", "`~!@#$%^&*()-=+[{]}\\|;:'\",.<>/?"},
          {"quickSuggestions",
           {{"other", "offWhenInlineCompletions"},
            {"comments", "off"},
            {"strings", "off"}}},
          {"quickSuggestionsDelay", 10},
          {"suggestOnTriggerCharacters", true},
          {"acceptSuggestionOnEnter", "on"},
          {"acceptSuggestionOnCommitCharacter", true},
          {"hover",
           {{"enabled", "on"},
            {"delay", 300},
            {"hidingDelay", 300},
            {"sticky", true},
            {"above", true}}},
          {"minimap",
           {{"enabled", true},
            {"size", "proportional"},
            {"side", "right"},
            {"showSlider", "mouseover"},
            {"renderCharacters", true},
            {"maxColumn", 120},
            {"scale", 1}}},
          {"folding", true},
          {"foldingStrategy", "auto"},
          {"showFoldingControls", "mouseover"},
          {"foldingHighlight", true},
          {"unfoldOnClickAfterEndOfLine", false},
          {"stickyScroll",
           {{"enabled", true},
            {"maxLineCount", 5},
            {"defaultModel", "outlineModel"},
            {"scrollWithEditor", true}}},
          {"renderWhitespace", "selection"},
          {"renderControlCharacters", true},
          {"renderLineHighlight", "line"},
          {"rulers", Json::array()},
          {"bracketPairColorization",
           {{"enabled", true},
            {"independentColorPoolPerBracketType", false}}},
          {"guides",
           {{"bracketPairs", false},
            {"bracketPairsHorizontal", "active"},
            {"highlightActiveBracketPair", true},
            {"indentation", true},
            {"highlightActiveIndentation", true}}}}},
        {"files",
         {{"autoSave", "off"},
          {"autoSaveDelay", 1000},
          {"trimTrailingWhitespace", false},
          {"insertFinalNewline", false},
          {"trimFinalNewlines", false}}},
        {"customization",
         {{"instructions_locations",
           Json::array({".sao/instructions.md", ".sao/instructions"})},
          {"agent_locations", Json::array({".sao/agents"})},
          {"workflow_locations", Json::array({".sao/workflows"})},
          {"skill_locations",
           Json::array({".agents/skills/.local", ".claude/skills/.local"})},
          {"use_agent_md", true},
          {"use_claude_md", false}}},
    });
}

void merge_missing(Json& destination, const Json& source) {
    if (!destination.is_object() || !source.is_object()) {
        return;
    }
    for (const auto& [key, value] : source.items()) {
        if (!destination.contains(key)) {
            destination[key] = value;
        } else if (destination[key].is_object() && value.is_object()) {
            merge_missing(destination[key], value);
        }
    }
}

void merge_patch(Json& destination, const Json& patch) {
    if (!destination.is_object() || !patch.is_object()) {
        return;
    }
    for (const auto& [key, value] : patch.items()) {
        if (destination.contains(key) && destination[key].is_object() &&
            value.is_object()) {
            merge_patch(destination[key], value);
        } else {
            destination[key] = value;
        }
    }
}

bool one_of(std::string_view value,
            std::initializer_list<std::string_view> choices) {
    return std::find(choices.begin(), choices.end(), value) != choices.end();
}

void normalize_string(Json& object, std::string_view key,
                      std::string_view fallback = {}) {
    const std::string owned_key(key);
    object[owned_key] = string_value(object[owned_key], fallback);
}

void normalize_bool(Json& object, std::string_view key, bool fallback) {
    const std::string owned_key(key);
    object[owned_key] = bool_value(object[owned_key], fallback);
}

void normalize_integer(Json& object, std::string_view key, int64_t fallback,
                       int64_t minimum = INT64_MIN,
                       int64_t maximum = INT64_MAX) {
    const std::string owned_key(key);
    object[owned_key] = std::clamp(integer_value(object[owned_key], fallback),
                                   minimum, maximum);
}

void normalize_number(Json& object, std::string_view key, double fallback,
                      double minimum, double maximum) {
    const std::string owned_key(key);
    object[owned_key] = std::clamp(number_value(object[owned_key], fallback),
                                   minimum, maximum);
}

void normalize_object(Json& object, std::string_view key) {
    const std::string owned_key(key);
    if (!object[owned_key].is_object()) {
        object[owned_key] = Json::object();
    }
}

void normalize_list(Json& object, std::string_view key, bool split_commas) {
    const std::string owned_key(key);
    object[owned_key] = string_list(object[owned_key], split_commas);
}

Json normalize_settings(const Json& raw) {
    Json settings = raw.is_object() ? raw : Json::object();
    const Json defaults = defaults_impl();
    merge_missing(settings, defaults);

    std::string provider = trim_ascii(string_value(settings["provider"], "openai"));
    if (provider.empty()) {
        provider = "openai";
    }
    settings["provider"] = provider;
    normalize_string(settings, "api_key");
    normalize_string(settings, "base_url");
    normalize_string(settings, "model");
    normalize_number(settings, "temperature", 0.7, 0.0, 2.0);
    normalize_integer(settings, "max_tokens", 4096, 0, INT32_MAX);
    normalize_string(settings, "system_prompt");

    std::string transport = lower_ascii(trim_ascii(string_value(
        settings["transport"], "chat_completions")));
    std::replace(transport.begin(), transport.end(), '-', '_');
    if (transport == "openai_responses" || transport == "response") {
        transport = "responses";
    }
    if (!one_of(transport, {"chat_completions", "responses"})) {
        transport = "chat_completions";
    }
    settings["transport"] = transport;
    normalize_number(settings, "top_p", 1.0, 0.0, 1.0);
    normalize_number(settings, "frequency_penalty", 0.0, -2.0, 2.0);
    normalize_number(settings, "presence_penalty", 0.0, -2.0, 2.0);
    normalize_list(settings, "stop", true);
    normalize_integer(settings, "max_input_tokens", 0, 0, INT32_MAX);
    normalize_integer(settings, "max_output_tokens", 0, 0, INT32_MAX);
    normalize_integer(settings, "timeout", 180, 1, 3600);
    normalize_object(settings, "extra_headers");
    normalize_object(settings, "extra_body");
    normalize_object(settings, "provider_keys");
    normalize_object(settings, "custom_models");
    normalize_object(settings, "permissions");
    normalize_object(settings, "configuration_targets");

    std::string mode = lower_ascii(trim_ascii(string_value(settings["mode"], "agent")));
    if (mode == "chat") {
        mode = "ask";
    } else if (mode == "edit") {
        mode = "plan";
    }
    if (!one_of(mode, {"agent", "ask", "plan"})) {
        mode = "agent";
    }
    settings["mode"] = mode;
    normalize_string(settings, "language", "zh");
    std::string approval = lower_ascii(trim_ascii(
        string_value(settings["approval"], "default")));
    if (!one_of(approval, {"default", "bypass", "autopilot"})) {
        approval = "default";
    }
    settings["approval"] = approval;
    normalize_string(settings, "active_chat_provider", "chat");
    normalize_string(settings, "theme", "dark");
    normalize_string(settings, "color_theme");
    normalize_string(settings, "file_icon_theme");

    auto& layout = settings["layout"];
    normalize_bool(layout, "sidebarVisible", true);
    normalize_bool(layout, "editorVisible", false);
    normalize_bool(layout, "chatVisible", true);
    normalize_string(layout, "panelHeight");
    normalize_string(layout, "sidebarWidth");
    normalize_string(layout, "rightSidebarWidth");
    normalize_string(layout, "activeSidebarPanel", "chat");
    std::string active_bottom_tab = trim_ascii(string_value(
        layout["activeBottomTab"], "terminal"));
    if (!one_of(active_bottom_tab, {"terminal", "output", "problems"})) {
        active_bottom_tab = "terminal";
    }
    layout["activeBottomTab"] = active_bottom_tab;
    normalize_bool(layout, "minimapVisible", false);

    auto& claude = settings["claude_code"];
    normalize_string(claude, "cli_path");
    normalize_list(claude, "cli_args", false);
    normalize_bool(claude, "prefer_cli", false);
    normalize_bool(claude, "allow_dangerously_skip_permissions", false);
    normalize_string(claude, "model");

    auto& codex = settings["codex"];
    normalize_string(codex, "cli_path");
    normalize_list(codex, "cli_args", false);
    normalize_string(codex, "model", "codex-mini-latest");
    std::string codex_transport = lower_ascii(trim_ascii(
        string_value(codex["transport"], "chat_completions")));
    std::replace(codex_transport.begin(), codex_transport.end(), '-', '_');
    if (codex_transport == "openai_responses" || codex_transport == "response") {
        codex_transport = "responses";
    }
    if (!one_of(codex_transport,
                {"chat_completions", "responses", "cli"})) {
        codex_transport = "chat_completions";
    }
    codex["transport"] = codex_transport;

    auto& mcp = settings["mcp"];
    if (settings.contains("mcpServers") && !mcp.contains("mcpServers") &&
        (settings["mcpServers"].is_array() ||
         settings["mcpServers"].is_object())) {
        mcp["mcpServers"] = settings["mcpServers"];
    }
    normalize_bool(mcp, "enabled", true);
    std::string access = lower_ascii(trim_ascii(string_value(mcp["access"], "prompt")));
    if (!one_of(access, {"prompt", "read_only", "allow", "disabled"})) {
        access = "prompt";
    }
    mcp["access"] = access;
    std::string collision = lower_ascii(trim_ascii(
        string_value(mcp["collision_behavior"], "first")));
    if (!one_of(collision, {"first", "last", "error"})) {
        collision = "first";
    }
    mcp["collision_behavior"] = collision;
    normalize_bool(mcp, "autostart", false);
    normalize_bool(mcp, "discovery_enabled", true);
    normalize_bool(mcp, "workspace_trusted", false);
    normalize_bool(mcp, "server_sampling", false);
    if (!mcp["servers"].is_array() && !mcp["servers"].is_object()) {
        mcp["servers"] = Json::array();
    }
    if (mcp.contains("mcpServers") && !mcp["mcpServers"].is_array() &&
        !mcp["mcpServers"].is_object()) {
        mcp["mcpServers"] = Json::object();
    }

    auto& terminal = settings["terminal"];
    normalize_string(terminal, "profile", "PowerShell 7 (No Profile)");
    normalize_string(terminal, "shell_path");
    normalize_list(terminal, "shell_args", false);
    normalize_integer(terminal, "timeout", 30, 1, 3600);
    normalize_integer(terminal, "output_limit", 8000, 1000, INT32_MAX);
    normalize_object(terminal, "auto_approve");
    normalize_bool(terminal, "use_pty", false);

    auto& workspace = settings["workspace"];
    normalize_string(workspace, "root");
    normalize_list(workspace, "roots", false);
    normalize_list(workspace, "recent_roots", false);
    normalize_bool(workspace, "auto_detect", true);
    normalize_bool(workspace, "remember_last", true);
    normalize_string(workspace, "last_root");

    auto& extensions = settings["extensions"];
    normalize_bool(extensions, "confirm_install", true);
    normalize_list(extensions, "allowed_publishers", true);
    normalize_list(extensions, "blocked_publishers", true);
    normalize_list(extensions, "enabled_contributions", true);
    normalize_bool(extensions, "enabled_contributions_explicit", false);
    normalize_bool(extensions, "diagnostics_enabled", false);

    auto& editor = settings["editor"];
    normalize_string(editor, "defaultFormatter");
    normalize_integer(editor, "tabSize", 4, 1, 16);
    normalize_bool(editor, "insertSpaces", true);
    normalize_string(editor, "wordSeparators",
                     "`~!@#$%^&*()-=+[{]}\\|;:'\",.<>/?");
    normalize_integer(editor, "quickSuggestionsDelay", 10, 0, 10000);
    normalize_bool(editor, "suggestOnTriggerCharacters", true);
    normalize_bool(editor, "acceptSuggestionOnCommitCharacter", true);
    normalize_bool(editor, "formatOnSave", false);
    normalize_bool(editor, "formatOnPaste", false);
    normalize_bool(editor, "formatOnType", false);
    normalize_bool(editor, "linkedEditing", false);
    normalize_bool(editor, "folding", true);
    normalize_bool(editor, "foldingHighlight", true);
    normalize_bool(editor, "unfoldOnClickAfterEndOfLine", false);
    normalize_bool(editor, "renderControlCharacters", true);
    normalize_object(editor, "quickSuggestions");
    normalize_object(editor, "hover");
    normalize_object(editor, "minimap");
    normalize_object(editor, "stickyScroll");
    normalize_object(editor, "bracketPairColorization");
    normalize_object(editor, "guides");
    normalize_object(editor, "codeActionsOnSave");
    normalize_object(editor, "codeActions");
    if (!editor["rulers"].is_array()) {
        editor["rulers"] = Json::array();
    }

    auto& files = settings["files"];
    std::string auto_save = string_value(files["autoSave"], "off");
    if (!one_of(auto_save,
                {"off", "afterDelay", "onFocusChange", "onWindowChange"})) {
        auto_save = "off";
    }
    files["autoSave"] = auto_save;
    normalize_integer(files, "autoSaveDelay", 1000, 0, INT32_MAX);
    normalize_bool(files, "trimTrailingWhitespace", false);
    normalize_bool(files, "insertFinalNewline", false);
    normalize_bool(files, "trimFinalNewlines", false);

    auto& customization = settings["customization"];
    normalize_list(customization, "instructions_locations", false);
    normalize_list(customization, "agent_locations", false);
    normalize_list(customization, "workflow_locations", false);
    normalize_list(customization, "skill_locations", false);
    normalize_bool(customization, "use_agent_md", true);
    normalize_bool(customization, "use_claude_md", false);
    return settings;
}

Json field(std::string_view key, std::string_view section,
           std::string_view label, std::string_view description,
           std::string_view type, const Json& default_value,
           Json metadata = Json::object()) {
    Json value{{"key", key},
               {"section", section},
               {"label", label},
               {"description", description},
               {"type", type},
               {"default", default_value}};
    if (metadata.is_object()) {
        merge_patch(value, metadata);
    }
    return value;
}

Json fields_schema() {
    const Json defaults = defaults_impl();
    Json provider_options = Json::array();
    for (const auto provider : kProviderIds) {
        provider_options.push_back(provider);
    }
    Json fields = Json::array();
    const auto add = [&](std::string_view key, std::string_view section,
                         std::string_view label, std::string_view description,
                         std::string_view type, const Json& default_value,
                         Json metadata = Json::object()) {
        fields.push_back(field(key, section, label, description, type,
                               default_value, std::move(metadata)));
    };

    add("provider", "assistant", "Provider", "Active chat endpoint provider.",
        "enum", defaults["provider"],
        {{"options", std::move(provider_options)},
         {"allowCustom", true},
         {"optionsSource", "providers"},
         {"registry", "providers"}});
    add("api_key", "assistant", "API Key", "Protected key for the active provider.",
        "secret", "", {{"sensitive", true}});
    for (const auto provider : kProviderIds) {
        add(std::string("provider_keys.") + std::string(provider), "assistant",
            std::string(provider) + " API Key",
            "Protected provider key used by provider-specific tabs.", "secret", "",
            {{"sensitive", true},
             {"advanced", true},
             {"providerId", provider}});
    }
    add("base_url", "assistant", "Base URL",
        "Provider base URL. Empty uses the provider preset.", "string", "");
    add("model", "assistant", "Model", "Default model used by chat and agents.",
        "string", "");
    add("temperature", "assistant", "Temperature", "Sampling temperature.",
        "number", defaults["temperature"], {{"min", 0.0}, {"max", 2.0}});
    add("max_tokens", "assistant", "Max Output Tokens",
        "Maximum output tokens requested from the provider.", "integer",
        defaults["max_tokens"], {{"min", 0}});
    add("system_prompt", "assistant", "System Prompt",
        "Prompt prepended when a request has no explicit system message.",
        "multiline", "");
    add("mode", "assistant", "Mode", "Default assistant interaction mode.",
        "enum", defaults["mode"],
        {{"options", {"ask", "plan", "agent"}}});
    add("approval", "permissions", "Approval Policy",
        "Default approval policy for tool execution.", "enum",
        defaults["approval"],
        {{"options", {"default", "bypass", "autopilot"}}});
    add("permissions", "permissions", "Tool Permissions",
        "Per-tool permission and approval overrides.", "object",
        Json::object(), {{"advanced", true}});

    add("color_theme", "appearance", "Color Theme",
        "Active built-in or extension-contributed color theme.", "string", "");
    add("file_icon_theme", "appearance", "File Icon Theme",
        "Active Explorer and TreeView icon theme.", "string", "");
    add("theme", "appearance", "Theme Fallback",
        "Legacy dark/light theme fallback.", "enum", defaults["theme"],
        {{"options", {"dark", "light"}}, {"advanced", true}});

    const auto layout_default = [&](std::string_view key) -> Json {
        return defaults["layout"][std::string(key)];
    };
    add("layout.sidebarVisible", "appearance", "Sidebar Visible",
        "Show the primary sidebar.", "boolean",
        layout_default("sidebarVisible"), {{"advanced", true}});
    add("layout.editorVisible", "appearance", "Editor Visible",
        "Show the editor area.", "boolean", layout_default("editorVisible"),
        {{"advanced", true}});
    add("layout.chatVisible", "appearance", "Assistant Visible",
        "Show the Assistant sidebar.", "boolean", layout_default("chatVisible"),
        {{"advanced", true}});
    add("layout.panelHeight", "appearance", "Panel Height",
        "Persisted bottom panel height.", "string", layout_default("panelHeight"),
        {{"advanced", true}});
    add("layout.sidebarWidth", "appearance", "Sidebar Width",
        "Persisted primary sidebar width.", "string",
        layout_default("sidebarWidth"), {{"advanced", true}});
    add("layout.rightSidebarWidth", "appearance", "Assistant Width",
        "Persisted Assistant sidebar width.", "string",
        layout_default("rightSidebarWidth"), {{"advanced", true}});
    add("layout.activeSidebarPanel", "appearance", "Active Sidebar Panel",
        "Last active primary sidebar panel.", "string",
        layout_default("activeSidebarPanel"), {{"advanced", true}});
    add("layout.activeBottomTab", "appearance", "Active Bottom Tab",
        "Last active bottom panel tab.", "enum",
        layout_default("activeBottomTab"),
        {{"options", {"terminal", "output", "problems"}},
         {"advanced", true}});
    add("layout.minimapVisible", "appearance", "Minimap Visible",
        "Persisted minimap visibility.", "boolean",
        layout_default("minimapVisible"), {{"advanced", true}});

    const auto editor_default = [&](std::string_view key) -> Json {
        return defaults["editor"][std::string(key)];
    };
    add("editor.defaultFormatter", "editor", "Default Formatter",
        "Default formatter extension identifier.", "string",
        editor_default("defaultFormatter"));
    add("editor.tabSize", "editor", "Tab Size", "Editor indentation width.",
        "integer", editor_default("tabSize"), {{"min", 1}, {"max", 16}});
    add("editor.insertSpaces", "editor", "Insert Spaces",
        "Use spaces instead of tab characters.", "boolean",
        editor_default("insertSpaces"));
    add("editor.wordSeparators", "editor", "Word Separators",
        "Characters used for word selection and navigation.", "string",
        editor_default("wordSeparators"), {{"advanced", true}});
    add("editor.quickSuggestions", "editor", "Quick Suggestions",
        "Suggestion modes for code, comments, and strings.", "object",
        editor_default("quickSuggestions"), {{"advanced", true}});
    add("editor.quickSuggestionsDelay", "editor", "Quick Suggestions Delay",
        "Delay before automatic suggestions.", "integer",
        editor_default("quickSuggestionsDelay"), {{"min", 0}});
    add("editor.suggestOnTriggerCharacters", "editor", "Trigger Suggestions",
        "Open suggestions on provider trigger characters.", "boolean",
        editor_default("suggestOnTriggerCharacters"));
    add("editor.acceptSuggestionOnEnter", "editor", "Accept On Enter",
        "Controls whether Enter accepts suggestions.", "enum",
        editor_default("acceptSuggestionOnEnter"),
        {{"options", {"on", "smart", "off"}}});
    add("editor.acceptSuggestionOnCommitCharacter", "editor",
        "Accept On Commit Character", "Accept suggestions on commit characters.",
        "boolean", editor_default("acceptSuggestionOnCommitCharacter"));
    add("editor.hover", "editor", "Hover", "Hover enablement and timing.",
        "object", editor_default("hover"), {{"advanced", true}});
    add("editor.minimap", "editor", "Minimap", "Minimap rendering options.",
        "object", editor_default("minimap"), {{"advanced", true}});
    add("editor.renderWhitespace", "editor", "Render Whitespace",
        "Whitespace rendering mode.", "enum",
        editor_default("renderWhitespace"),
        {{"options", {"none", "boundary", "selection", "trailing", "all"}},
         {"advanced", true}});
    add("editor.renderControlCharacters", "editor", "Control Characters",
        "Render ASCII control characters.", "boolean",
        editor_default("renderControlCharacters"), {{"advanced", true}});
    add("editor.renderLineHighlight", "editor", "Line Highlight",
        "Current line highlight mode.", "enum",
        editor_default("renderLineHighlight"),
        {{"options", {"none", "gutter", "line", "all"}},
         {"advanced", true}});
    add("editor.rulers", "editor", "Rulers", "Column ruler definitions.",
        "array", editor_default("rulers"), {{"advanced", true}});
    add("editor.bracketPairColorization", "editor",
        "Bracket Pair Colorization", "Bracket colorization options.", "object",
        editor_default("bracketPairColorization"), {{"advanced", true}});
    add("editor.guides", "editor", "Editor Guides",
        "Indentation and bracket pair guide options.", "object",
        editor_default("guides"), {{"advanced", true}});
    add("editor.folding", "editor", "Code Folding", "Enable code folding.",
        "boolean", editor_default("folding"));
    add("editor.foldingStrategy", "editor", "Folding Strategy",
        "Automatic provider or indentation folding.", "enum",
        editor_default("foldingStrategy"),
        {{"options", {"auto", "indentation"}}, {"advanced", true}});
    add("editor.showFoldingControls", "editor", "Folding Controls",
        "When folding controls are visible.", "enum",
        editor_default("showFoldingControls"),
        {{"options", {"mouseover", "always", "never"}}, {"advanced", true}});
    add("editor.foldingHighlight", "editor", "Folding Highlight",
        "Highlight active foldable regions.", "boolean",
        editor_default("foldingHighlight"), {{"advanced", true}});
    add("editor.unfoldOnClickAfterEndOfLine", "editor", "Unfold After Line End",
        "Unfold when clicking after a folded line.", "boolean",
        editor_default("unfoldOnClickAfterEndOfLine"), {{"advanced", true}});
    add("editor.stickyScroll", "editor", "Sticky Scroll",
        "Sticky nested-scope rendering options.", "object",
        editor_default("stickyScroll"), {{"advanced", true}});
    add("editor.formatOnSave", "editor", "Format On Save",
        "Run the active formatter before explicit saves.", "boolean",
        editor_default("formatOnSave"));
    add("editor.formatOnPaste", "editor", "Format On Paste",
        "Run range formatting after paste or drop edits.", "boolean",
        editor_default("formatOnPaste"));
    add("editor.formatOnType", "editor", "Format On Type",
        "Run on-type format providers for trigger characters.", "boolean",
        editor_default("formatOnType"));
    add("editor.linkedEditing", "editor", "Linked Editing",
        "Mirror edits across extension-provided linked ranges.", "boolean",
        editor_default("linkedEditing"));
    add("editor.codeActionsOnSave", "editor", "Code Actions On Save",
        "Extension code actions applied during saves.", "object",
        editor_default("codeActionsOnSave"), {{"advanced", true}});
    add("editor.codeActions.triggerOnFocusChange", "editor",
        "Code Actions On Focus Change",
        "Run always-on source actions when focus changes.", "boolean", false,
        {{"advanced", true}});

    const auto files_default = [&](std::string_view key) -> Json {
        return defaults["files"][std::string(key)];
    };
    add("files.autoSave", "files", "Auto Save", "File auto-save mode.",
        "enum", files_default("autoSave"),
        {{"options", {"off", "afterDelay", "onFocusChange", "onWindowChange"}}});
    add("files.autoSaveDelay", "files", "Auto Save Delay",
        "Delay in milliseconds for afterDelay auto-save.", "integer",
        files_default("autoSaveDelay"),
        {{"min", 0},
         {"dependencies", {{{"key", "files.autoSave"}, {"equals", "afterDelay"}}}}});
    add("files.trimTrailingWhitespace", "files", "Trim Trailing Whitespace",
        "Remove trailing whitespace during saves.", "boolean",
        files_default("trimTrailingWhitespace"));
    add("files.insertFinalNewline", "files", "Insert Final Newline",
        "Ensure saved files end in a newline.", "boolean",
        files_default("insertFinalNewline"));
    add("files.trimFinalNewlines", "files", "Trim Final Newlines",
        "Remove extra final newlines during saves.", "boolean",
        files_default("trimFinalNewlines"));

    add("claude_code.cli_path", "claude_code", "CLI Command",
        "Claude Code executable path; empty enables discovery.", "string", "");
    add("claude_code.model", "claude_code", "Model",
        "Model used by the Claude Code provider surface.", "string", "");
    add("claude_code.cli_args", "claude_code", "CLI Arguments",
        "One non-secret CLI argument per line.", "array", Json::array());
    add("claude_code.prefer_cli", "claude_code", "Prefer CLI",
        "Keep the local Claude Code CLI ready for external launches.", "boolean",
        false);
    add("claude_code.allow_dangerously_skip_permissions", "claude_code",
        "Allow Skip Permissions", "Allow the external CLI skip-permissions flag.",
        "boolean", false, {{"advanced", true}});

    add("codex.cli_path", "codex", "CLI Command",
        "Codex executable path; empty enables discovery.", "string", "");
    add("codex.model", "codex", "Model", "Model used by the Codex surface.",
        "string", defaults["codex"]["model"]);
    add("codex.transport", "codex", "Transport",
        "Codex embedded and external transport preference.", "enum",
        defaults["codex"]["transport"],
        {{"options", {"chat_completions", "responses", "cli"}}});
    add("codex.cli_args", "codex", "CLI Arguments",
        "One non-secret CLI argument per line.", "array", Json::array());

    add("mcp.enabled", "mcp", "Enable MCP", "Enable configured MCP servers.",
        "boolean", defaults["mcp"]["enabled"]);
    add("mcp.access", "mcp", "Tool Access", "MCP tool access policy.",
        "enum", defaults["mcp"]["access"],
        {{"options", {"prompt", "read_only", "allow", "disabled"}}});
    add("mcp.collision_behavior", "mcp", "Name Collisions",
        "How duplicate MCP names are resolved.", "enum",
        defaults["mcp"]["collision_behavior"],
        {{"options", {"first", "last", "error"}}});
    add("mcp.discovery_enabled", "mcp", "Discover MCP Servers",
        "Read configured MCP server entries.", "boolean", true);
    add("mcp.workspace_trusted", "mcp", "Trust Workspace MCP",
        "Allow workspace and plugin manifests to launch MCP commands.", "boolean",
        false, {{"dependencies", {{{"key", "mcp.discovery_enabled"}, {"equals", true}}}}});
    add("mcp.autostart", "mcp", "Autostart MCP Servers",
        "Start configured MCP servers automatically.", "boolean", false,
        {{"dependencies", {{{"key", "mcp.discovery_enabled"}, {"equals", true}}}}});
    add("mcp.server_sampling", "mcp", "MCP Server Sampling",
        "Allow trusted MCP servers to request model sampling.", "boolean", false,
        {{"advanced", true}});
    add("mcp.servers", "mcp", "MCP Servers",
        "Configured stdio or HTTP MCP servers.", "array", Json::array(),
        {{"advanced", true},
         {"sensitivePaths", {"*.env.*", "*.headers.*"}},
         {"aliases", {"mcp.mcpServers", "mcpServers"}}});

    add("terminal.profile", "terminal", "Profile Name",
        "Integrated terminal profile name.", "string",
        defaults["terminal"]["profile"]);
    add("terminal.shell_path", "terminal", "Shell Command",
        "Shell executable; empty uses the platform default.", "string", "");
    add("terminal.shell_args", "terminal", "Shell Arguments",
        "One non-secret shell argument per line.", "array", Json::array());
    add("terminal.timeout", "terminal", "Timeout",
        "Terminal command timeout in seconds.", "integer", 30,
        {{"min", 1}});
    add("terminal.output_limit", "terminal", "Output Limit",
        "Maximum captured terminal output.", "integer", 8000,
        {{"min", 1000}});
    add("terminal.use_pty", "terminal", "Use ConPTY",
        "Use a real Windows pseudo-terminal when available.", "boolean", false,
        {{"advanced", true}});

    add("workspace.root", "workspace", "Workspace Root",
        "Configured workspace root; empty enables discovery.", "string", "");
    add("workspace.roots", "workspace", "Additional Roots",
        "Fallback workspace roots.", "array", Json::array());
    add("workspace.auto_detect", "workspace", "Auto-detect Workspace",
        "Resolve the workspace from active files and repository roots.", "boolean",
        true);
    add("workspace.remember_last", "workspace", "Remember Last Workspace",
        "Keep the last resolved workspace available.", "boolean", true);

    add("extensions.confirm_install", "extensions", "Confirm Installs",
        "Require confirmation before extension installation.", "boolean", true);
    add("extensions.diagnostics_enabled", "extensions", "Diagnostics",
        "Enable extension host diagnostic tracing.", "boolean", false,
        {{"advanced", true}});
    add("extensions.allowed_publishers", "extensions", "Allowed Publishers",
        "Publisher allow-list.", "array", Json::array());
    add("extensions.blocked_publishers", "extensions", "Blocked Publishers",
        "Publisher deny-list.", "array", Json::array());
    add("extensions.enabled_contributions", "extensions",
        "Enabled Contributions", "VS Code contribution types loaded by the host.",
        "array", defaults["extensions"]["enabled_contributions"]);
    add("extensions.enabled_contributions_explicit", "extensions",
        "Contribution Selection Is Explicit",
        "Whether enabled contributions were explicitly selected.", "boolean", false,
        {{"advanced", true}});

    add("customization.use_agent_md", "customization", "Read AGENTS.md",
        "Include workspace AGENTS.md instruction files.", "boolean", true);
    add("customization.use_claude_md", "customization", "Read CLAUDE.md",
        "Include Claude-style workspace instruction files.", "boolean", false);
    add("customization.instructions_locations", "customization",
        "Instructions Locations", "Relative instruction discovery paths.", "array",
        defaults["customization"]["instructions_locations"]);
    add("customization.agent_locations", "customization", "Agent Locations",
        "Relative agent definition discovery paths.", "array",
        defaults["customization"]["agent_locations"]);
    add("customization.workflow_locations", "customization",
        "Workflow Locations", "Relative workflow discovery paths.", "array",
        defaults["customization"]["workflow_locations"]);
    add("customization.skill_locations", "customization", "Skill Locations",
        "Relative local skill discovery paths.", "array",
        defaults["customization"]["skill_locations"]);

    add("top_p", "advanced", "Top P", "Nucleus sampling probability.",
        "number", 1.0, {{"min", 0.0}, {"max", 1.0}, {"advanced", true}});
    add("frequency_penalty", "advanced", "Frequency Penalty",
        "Frequency penalty applied by compatible providers.", "number", 0.0,
        {{"min", -2.0}, {"max", 2.0}, {"advanced", true}});
    add("presence_penalty", "advanced", "Presence Penalty",
        "Presence penalty applied by compatible providers.", "number", 0.0,
        {{"min", -2.0}, {"max", 2.0}, {"advanced", true}});
    add("timeout", "advanced", "Request Timeout",
        "Chat request timeout in seconds.", "integer", 180,
        {{"min", 1}, {"max", 3600}, {"advanced", true}});
    add("stop", "advanced", "Stop Sequences", "Provider stop sequences.",
        "array", Json::array(), {{"advanced", true}});
    add("max_input_tokens", "advanced", "Max Input Tokens",
        "Context window override; zero uses the model default.", "integer", 0,
        {{"min", 0}, {"advanced", true}});
    add("max_output_tokens", "advanced", "Max Output Tokens Override",
        "Model output window override; zero uses the model default.", "integer", 0,
        {{"min", 0}, {"advanced", true}});
    add("extra_headers", "advanced", "Extra Headers",
        "Protected provider-specific HTTP header map.", "object", Json::object(),
        {{"sensitive", true}, {"advanced", true}});
    add("extra_body", "advanced", "Extra Body",
        "Protected provider-specific request body additions.", "object",
        Json::object(), {{"sensitive", true}, {"advanced", true}});
    add("custom_models", "custom_models", "Custom Models",
        "User-defined context windows and model capabilities.", "object",
        Json::object());
    return fields;
}

std::string secret_part(std::string_view value) {
    const std::string digest = sha256_hex(value);
    return digest.empty() ? std::string() : "v2-" + digest;
}

std::string legacy_secret_part(std::string_view value) {
    std::string result;
    result.reserve(std::min<size_t>(value.size(), 120));
    bool previous_was_replacement = false;
    const std::string trimmed = trim_ascii(std::string(value));
    for (const unsigned char character : trimmed) {
        const bool allowed = std::isalnum(character) != 0 || character == '.' ||
                             character == '_' || character == '-';
        if (allowed) {
            result.push_back(static_cast<char>(character));
            previous_was_replacement = false;
        } else if (!previous_was_replacement) {
            result.push_back('_');
            previous_was_replacement = true;
        }
        if (result.size() == 120) {
            break;
        }
    }
    return result.empty() ? "default" : result;
}

std::string provider_secret_key(std::string_view provider,
                                std::string_view kind = "api-key") {
    const std::string provider_part = secret_part(provider);
    const std::string kind_part = secret_part(kind);
    if (provider_part.empty() || kind_part.empty()) {
        return {};
    }
    return "provider/" + provider_part + "/" + kind_part;
}

std::string legacy_provider_secret_key(std::string_view provider,
                                       std::string_view kind = "api-key") {
    return "provider/" + legacy_secret_part(provider) + "/" +
           legacy_secret_part(kind);
}

std::string mcp_secret_key(std::string_view server_id, std::string_view kind) {
    const std::string server_part = secret_part(server_id);
    const std::string kind_part = secret_part(kind);
    if (server_part.empty() || kind_part.empty()) {
        return {};
    }
    return "mcp/" + server_part + "/" + kind_part;
}

std::string legacy_mcp_secret_key(std::string_view server_id,
                                  std::string_view kind) {
    return "mcp/" + legacy_secret_part(server_id) + "/" +
           legacy_secret_part(kind);
}

std::string migration_block_key(std::string_view legacy_key) {
    const std::string digest = sha256_hex(legacy_key);
    return digest.empty() ? std::string() : "migration-block/" + digest;
}

bool legacy_unambiguous(std::string_view provider,
                        const std::set<std::string, std::less<>>& candidates) {
    const std::string owner(provider);
    const std::string component = legacy_secret_part(provider);
    size_t matches = 0;
    bool owner_present = false;
    for (const auto& candidate : candidates) {
        if (legacy_secret_part(candidate) != component) {
            continue;
        }
        ++matches;
        owner_present = owner_present || candidate == owner;
    }
    return matches == 1 && owner_present;
}

using MutableMcpServerVisitor =
    std::function<int32_t(std::string_view, Json&)>;
using ConstMcpServerVisitor =
    std::function<int32_t(std::string_view, const Json&)>;

int32_t visit_mcp_servers(Json& mcp,
                          const MutableMcpServerVisitor& visitor) {
    if (!mcp.is_object()) {
        return SAO_AI_EDITOR_OK;
    }
    for (const auto group_name : {"servers", "mcpServers"}) {
        if (!mcp.contains(group_name)) {
            continue;
        }
        Json& group = mcp[group_name];
        if (group.is_array()) {
            for (auto& server : group) {
                if (!server.is_object()) {
                    continue;
                }
                const std::string server_id = string_value(
                    server.value("id", Json()));
                if (server_id.empty()) {
                    continue;
                }
                const int32_t status = visitor(server_id, server);
                if (status != SAO_AI_EDITOR_OK) {
                    return status;
                }
            }
        } else if (group.is_object()) {
            for (auto& [server_id, server] : group.items()) {
                if (server_id.empty() || !server.is_object()) {
                    continue;
                }
                const int32_t status = visitor(server_id, server);
                if (status != SAO_AI_EDITOR_OK) {
                    return status;
                }
            }
        }
    }
    return SAO_AI_EDITOR_OK;
}

int32_t visit_mcp_servers(const Json& mcp,
                          const ConstMcpServerVisitor& visitor) {
    if (!mcp.is_object()) {
        return SAO_AI_EDITOR_OK;
    }
    for (const auto group_name : {"servers", "mcpServers"}) {
        if (!mcp.contains(group_name)) {
            continue;
        }
        const Json& group = mcp[group_name];
        if (group.is_array()) {
            for (const auto& server : group) {
                if (!server.is_object()) {
                    continue;
                }
                const std::string server_id = string_value(
                    server.value("id", Json()));
                if (server_id.empty()) {
                    continue;
                }
                const int32_t status = visitor(server_id, server);
                if (status != SAO_AI_EDITOR_OK) {
                    return status;
                }
            }
        } else if (group.is_object()) {
            for (const auto& [server_id, server] : group.items()) {
                if (server_id.empty() || !server.is_object()) {
                    continue;
                }
                const int32_t status = visitor(server_id, server);
                if (status != SAO_AI_EDITOR_OK) {
                    return status;
                }
            }
        }
    }
    return SAO_AI_EDITOR_OK;
}

std::set<std::string, std::less<>> mcp_server_candidates(const Json& mcp) {
    std::set<std::string, std::less<>> candidates;
    (void)visit_mcp_servers(
        mcp, ConstMcpServerVisitor{[&](std::string_view server_id,
                                      const Json&) {
            candidates.emplace(server_id);
            return SAO_AI_EDITOR_OK;
        }});
    return candidates;
}

std::set<std::string, std::less<>> mcp_secret_fields(const Json& server) {
    std::set<std::string, std::less<>> fields;
    if (!server.is_object() || !server.contains("_secret_fields") ||
        !server["_secret_fields"].is_array()) {
        return fields;
    }
    for (const auto& item : server["_secret_fields"]) {
        const std::string field_name = string_value(item);
        if (field_name == "env" || field_name == "headers") {
            fields.insert(field_name);
        }
    }
    return fields;
}

int32_t get_secret(SecretStore& secrets, std::string_view key,
                   std::string& value);

Json mcp_secret_field_array(
    const std::set<std::string, std::less<>>& fields) {
    Json result = Json::array();
    for (const auto& field_name : fields) {
        result.push_back(field_name);
    }
    return result;
}

bool is_secret_marker(const Json& value) {
    return value.is_string() &&
           (value.get<std::string>() == kSecretPresent ||
            value.get<std::string>() == kProtectedPlaceholder);
}

Json plaintext_mcp_mapping(const Json& value) {
    Json result = Json::object();
    if (!value.is_object()) {
        return result;
    }
    for (const auto& [key, item] : value.items()) {
        if (!is_secret_marker(item)) {
            result[key] = item;
        }
    }
    return result;
}

int32_t read_mcp_secret_with_fallback(
    SecretStore& secrets, std::string_view server_id, std::string_view kind,
    const std::set<std::string, std::less<>>& candidates,
    std::string& value) {
    value.clear();
    const std::string primary = mcp_secret_key(server_id, kind);
    int32_t status = get_secret(secrets, primary, value);
    if (status == SAO_AI_EDITOR_OK) {
        return status;
    }
    if (status != SAO_AI_EDITOR_ERR_NOT_FOUND) {
        return status;
    }

    const std::string legacy = legacy_mcp_secret_key(server_id, kind);
    const bool allow_legacy = legacy_unambiguous(server_id, candidates);
    const std::string block = migration_block_key(legacy);
    if (!allow_legacy) {
        if (!block.empty() && !secrets.has(block)) {
            const int32_t block_status = secrets.set(block, "1");
            if (block_status != SAO_AI_EDITOR_OK) {
                return block_status;
            }
        }
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    if (!block.empty() && secrets.has(block)) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    status = get_secret(secrets, legacy, value);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    const int32_t set_status = secrets.set(primary, value);
    if (set_status != SAO_AI_EDITOR_OK) {
        return set_status;
    }
    const int32_t erase_status = secrets.erase(legacy);
    if (erase_status != SAO_AI_EDITOR_OK &&
        erase_status != SAO_AI_EDITOR_ERR_NOT_FOUND) {
        return erase_status;
    }
    return SAO_AI_EDITOR_OK;
}

int32_t hydrate_mcp_secrets(Json& mcp, SecretStore& secrets,
                            Json& flags) {
    flags = Json::object();
    const auto candidates = mcp_server_candidates(mcp);
    return visit_mcp_servers(
        mcp, MutableMcpServerVisitor{[&](std::string_view server_id,
                                        Json& server) -> int32_t {
            auto fields = mcp_secret_fields(server);
            Json server_flags = Json::object();
            for (const auto kind : {"env", "headers"}) {
                const std::string kind_key(kind);
                const Json raw = server.value(kind_key, Json::object());
                Json plaintext = plaintext_mcp_mapping(raw);
                bool configured = fields.contains(kind_key) ||
                                  !plaintext.empty();
                if (configured) {
                    std::string stored_secret;
                    const int32_t status = read_mcp_secret_with_fallback(
                        secrets, server_id, kind, candidates, stored_secret);
                    if (status == SAO_AI_EDITOR_OK) {
                        Json parsed = Json::parse(
                            stored_secret, nullptr, false);
                        if (!parsed.is_object()) {
                            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                        }
                        merge_patch(parsed, plaintext);
                        plaintext = std::move(parsed);
                    } else if (status != SAO_AI_EDITOR_ERR_NOT_FOUND) {
                        return status;
                    }
                    configured = !plaintext.empty() ||
                                 status == SAO_AI_EDITOR_OK;
                    server[kind_key] = configured
                        ? std::move(plaintext)
                        : Json::object();
                } else {
                    server[kind_key] = Json::object();
                }
                if (configured) {
                    fields.insert(kind_key);
                } else {
                    fields.erase(kind_key);
                }
                server_flags[kind_key] = configured;
            }
            server["_secret_fields"] = mcp_secret_field_array(fields);
            flags[std::string(server_id)] = std::move(server_flags);
            return SAO_AI_EDITOR_OK;
        }});
}

void redact_mcp_secrets(Json& mcp, Json* flags = nullptr,
                        Json* key_states = nullptr) {
    if (flags != nullptr) {
        *flags = Json::object();
    }
    if (key_states != nullptr) {
        *key_states = Json::object();
    }
    (void)visit_mcp_servers(
        mcp, MutableMcpServerVisitor{[&](std::string_view server_id,
                                        Json& server) {
            auto fields = mcp_secret_fields(server);
            Json server_flags = Json::object();
            Json server_keys = Json::object();
            for (const auto kind : {"env", "headers"}) {
                const std::string kind_key(kind);
                Json redacted = Json::object();
                Json keys = Json::array();
                const Json raw = server.value(kind_key, Json::object());
                if (raw.is_object()) {
                    for (const auto& [key, _value] : raw.items()) {
                        redacted[key] = kSecretPresent;
                        keys.push_back(key);
                    }
                }
                const bool configured = fields.contains(kind_key) ||
                                        !redacted.empty();
                if (configured) {
                    fields.insert(kind_key);
                }
                server[kind_key] = std::move(redacted);
                server.erase("_clear_" + kind_key);
                server_flags[kind_key] = configured;
                server_keys[kind_key] = std::move(keys);
            }
            server["_secret_fields"] = mcp_secret_field_array(fields);
            if (flags != nullptr) {
                (*flags)[std::string(server_id)] = std::move(server_flags);
            }
            if (key_states != nullptr) {
                (*key_states)[std::string(server_id)] = std::move(server_keys);
            }
            return SAO_AI_EDITOR_OK;
        }});
}

int32_t get_secret(SecretStore& secrets, std::string_view key,
                   std::string& value) {
    if (key.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    return secrets.get(key, value);
}

int32_t read_secret_with_fallback(
    SecretStore& secrets, std::string_view provider, std::string_view kind,
    const std::set<std::string, std::less<>>& candidates,
    std::string& value) {
    value.clear();
    const std::string primary = provider_secret_key(provider, kind);
    int32_t status = get_secret(secrets, primary, value);
    if (status == SAO_AI_EDITOR_OK) {
        return status;
    }
    if (status != SAO_AI_EDITOR_ERR_NOT_FOUND) {
        return status;
    }

    const std::string legacy = legacy_provider_secret_key(provider, kind);
    const bool allow_legacy = legacy_unambiguous(provider, candidates);
    const std::string block = migration_block_key(legacy);
    if (!allow_legacy) {
        if (!block.empty() && !secrets.has(block)) {
            const int32_t block_status = secrets.set(block, "1");
            if (block_status != SAO_AI_EDITOR_OK) {
                return block_status;
            }
        }
    } else if (block.empty() || !secrets.has(block)) {
        status = get_secret(secrets, legacy, value);
        if (status == SAO_AI_EDITOR_OK) {
            const int32_t set_status = secrets.set(primary, value);
            if (set_status != SAO_AI_EDITOR_OK) {
                return set_status;
            }
            const int32_t erase_status = secrets.erase(legacy);
            if (erase_status != SAO_AI_EDITOR_OK &&
                erase_status != SAO_AI_EDITOR_ERR_NOT_FOUND) {
                return erase_status;
            }
            return status;
        }
        if (status != SAO_AI_EDITOR_ERR_NOT_FOUND) {
            return status;
        }
    }

    if (kind == "api-key" && valid_simple_id(provider)) {
        const std::string cpp_legacy =
            "provider/" + std::string(provider) + "/apiKey";
        status = get_secret(secrets, cpp_legacy, value);
        if (status == SAO_AI_EDITOR_OK) {
            const int32_t set_status = secrets.set(primary, value);
            if (set_status != SAO_AI_EDITOR_OK) {
                return set_status;
            }
            const int32_t erase_status = secrets.erase(cpp_legacy);
            if (erase_status != SAO_AI_EDITOR_OK &&
                erase_status != SAO_AI_EDITOR_ERR_NOT_FOUND) {
                return erase_status;
            }
            return status;
        }
        if (status != SAO_AI_EDITOR_ERR_NOT_FOUND) {
            return status;
        }
    }
    value.clear();
    return SAO_AI_EDITOR_ERR_NOT_FOUND;
}

std::set<std::string, std::less<>> state_values(const Json& settings,
                                                 std::string_view state_key) {
    std::set<std::string, std::less<>> values;
    if (!settings.is_object() || !settings.contains("_secret_state") ||
        !settings["_secret_state"].is_object()) {
        return values;
    }
    const auto& state = settings["_secret_state"];
    const std::string key(state_key);
    if (!state.contains(key) || !state[key].is_array()) {
        return values;
    }
    for (const auto& item : state[key]) {
        const std::string value = trim_ascii(string_value(item));
        if (!value.empty()) {
            values.insert(value);
        }
    }
    return values;
}

Json state_array(const std::set<std::string, std::less<>>& values) {
    Json result = Json::array();
    for (const auto& value : values) {
        result.push_back(value);
    }
    return result;
}

std::set<std::string, std::less<>> provider_candidates(const Json& settings) {
    std::set<std::string, std::less<>> candidates;
    for (const auto provider : kProviderIds) {
        candidates.emplace(provider);
    }
    candidates.insert(trim_ascii(string_value(settings.value("provider", "openai"))));
    for (const auto state_key : {"provider_keys", "extra_headers", "extra_body"}) {
        const auto values = state_values(settings, state_key);
        candidates.insert(values.begin(), values.end());
    }
    if (settings.contains("provider_keys") && settings["provider_keys"].is_object()) {
        for (const auto& [provider, _value] : settings["provider_keys"].items()) {
            if (!trim_ascii(provider).empty()) {
                candidates.insert(provider);
            }
        }
    }
    if (settings.contains("custom_models") && settings["custom_models"].is_object()) {
        for (const auto& [_model, entry] : settings["custom_models"].items()) {
            if (!entry.is_object()) {
                continue;
            }
            const std::string provider = trim_ascii(string_value(
                entry.contains("provider") ? entry["provider"]
                                           : entry.value("provider_id", Json())));
            if (!provider.empty()) {
                candidates.insert(provider);
            }
        }
    }
    candidates.erase("");
    return candidates;
}

int32_t hydrate_settings(Json stored, SecretStore& secrets, Json& hydrated,
                         Json& secret_states) {
    hydrated = normalize_settings(stored);
    auto candidates = provider_candidates(hydrated);
    const std::string active_provider = hydrated["provider"].get<std::string>();
    candidates.insert(active_provider);

    auto provider_state = state_values(hydrated, "provider_keys");
    auto header_state = state_values(hydrated, "extra_headers");
    auto body_state = state_values(hydrated, "extra_body");
    Json provider_keys = Json::object();
    Json provider_flags = Json::object();

    if (stored.contains("provider_keys") && stored["provider_keys"].is_object()) {
        for (const auto& [provider, value] : stored["provider_keys"].items()) {
            const std::string plaintext = string_value(value);
            if (!plaintext.empty() && !is_secret_marker(value)) {
                provider_keys[provider] = plaintext;
                provider_state.insert(provider);
                const std::string primary = provider_secret_key(provider);
                if (!primary.empty()) {
                    const int32_t set_status = secrets.set(primary, plaintext);
                    if (set_status != SAO_AI_EDITOR_OK) {
                        return set_status;
                    }
                }
            }
        }
    }

    for (const auto& provider : candidates) {
        std::string secret;
        const int32_t status = read_secret_with_fallback(
            secrets, provider, "api-key", candidates, secret);
        if (status == SAO_AI_EDITOR_OK && !secret.empty()) {
            provider_keys[provider] = secret;
            provider_flags[provider] = true;
            provider_state.insert(provider);
        } else if (status == SAO_AI_EDITOR_ERR_NOT_FOUND) {
            provider_flags[provider] = provider_keys.contains(provider);
        } else {
            return status;
        }
    }

    const std::string stored_api_key = string_value(stored.value("api_key", Json()));
    if (!stored_api_key.empty() &&
        !is_secret_marker(stored.value("api_key", Json()))) {
        hydrated["api_key"] = stored_api_key;
        provider_keys[active_provider] = stored_api_key;
        provider_flags[active_provider] = true;
        provider_state.insert(active_provider);
        const std::string primary = provider_secret_key(active_provider);
        if (!primary.empty()) {
            const int32_t set_status = secrets.set(primary, stored_api_key);
            if (set_status != SAO_AI_EDITOR_OK) {
                return set_status;
            }
        }
    } else if (provider_keys.contains(active_provider) &&
               provider_keys[active_provider].is_string()) {
        hydrated["api_key"] = provider_keys[active_provider];
    } else {
        hydrated["api_key"] = "";
    }
    hydrated["provider_keys"] = std::move(provider_keys);

    const auto hydrate_json_secret = [&](std::string_view kind,
                                         std::set<std::string, std::less<>>& state,
                                         Json& flags, Json& output) -> int32_t {
        flags = Json::object();
        for (const auto& provider : candidates) {
            std::string secret;
            const int32_t status = read_secret_with_fallback(
                secrets, provider, kind, candidates, secret);
            if (status == SAO_AI_EDITOR_OK && !secret.empty()) {
                const Json parsed = Json::parse(secret, nullptr, false);
                if (!parsed.is_object()) {
                    return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                }
                flags[provider] = true;
                state.insert(provider);
                if (provider == active_provider) {
                    output = parsed;
                }
            } else if (status == SAO_AI_EDITOR_ERR_NOT_FOUND) {
                flags[provider] = false;
            } else {
                return status;
            }
        }
        return SAO_AI_EDITOR_OK;
    };

    Json header_flags;
    Json body_flags;
    Json hydrated_headers = hydrated["extra_headers"];
    Json hydrated_body = hydrated["extra_body"];
    int32_t status = hydrate_json_secret("extra_headers", header_state,
                                         header_flags, hydrated_headers);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    status = hydrate_json_secret("extra_body", body_state, body_flags,
                                 hydrated_body);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    hydrated["extra_headers"] = std::move(hydrated_headers);
    hydrated["extra_body"] = std::move(hydrated_body);
    hydrated["_secret_state"] =
        {{"provider_keys", state_array(provider_state)},
         {"extra_headers", state_array(header_state)},
         {"extra_body", state_array(body_state)}};

    Json mcp_flags;
    status = hydrate_mcp_secrets(hydrated["mcp"], secrets, mcp_flags);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }

    secret_states = {{"activeProvider", active_provider},
                     {"apiKey", provider_flags.value(active_provider, false)},
                     {"providerKeys", std::move(provider_flags)},
                     {"extraHeaders", std::move(header_flags)},
                     {"extraBody", std::move(body_flags)},
                     {"mcpServers", mcp_flags},
                     {"mcp_servers", std::move(mcp_flags)}};
    return SAO_AI_EDITOR_OK;
}

void strip_named_secrets(Json& value) {
    if (!value.is_object()) {
        return;
    }
    value.erase("_provider_keys");
    value.erase("secretUpdates");
    value.erase("secret_updates");
}

Json redact_settings(Json settings) {
    settings = normalize_settings(settings);
    settings["api_key"] = "";
    settings["provider_keys"] = Json::object();
    settings["extra_headers"] = Json::object();
    settings["extra_body"] = Json::object();
    settings.erase("_provider_keys");
    settings.erase("secretUpdates");
    settings.erase("secret_updates");
    settings.erase("_clear_api_key");
    settings.erase("_clear_provider_keys");
    settings.erase("_clear_extra_headers");
    settings.erase("_clear_extra_body");
    Json mcp_flags;
    Json mcp_keys;
    redact_mcp_secrets(settings["mcp"], &mcp_flags, &mcp_keys);
    settings.erase("mcpServers");
    return settings;
}

Json redact_scope_overrides(Json settings) {
    if (!settings.is_object()) {
        return Json::object();
    }
    settings.erase("api_key");
    settings.erase("provider_keys");
    settings.erase("extra_headers");
    settings.erase("extra_body");
    settings.erase("_provider_keys");
    settings.erase("_secret_state");
    settings.erase("secretUpdates");
    settings.erase("secret_updates");
    settings.erase("_clear_api_key");
    settings.erase("_clear_provider_keys");
    settings.erase("_clear_extra_headers");
    settings.erase("_clear_extra_body");
    if (settings.contains("mcp") && settings["mcp"].is_object()) {
        redact_mcp_secrets(settings["mcp"]);
    }
    settings.erase("mcpServers");
    return settings;
}

Json section_list() {
    return Json::array({
        {{"id", "overview"}, {"label", "Overview"},
         {"description", "Provider, model, mode, and settings health."}},
        {{"id", "assistant"}, {"label", "AI / Endpoint"},
         {"description", "Provider, endpoint, model, and assistant defaults."}},
        {{"id", "appearance"}, {"label", "Appearance"},
         {"description", "Color and file icon themes."}},
        {{"id", "editor"}, {"label", "Editor"},
         {"description", "Editing, suggestions, formatting, and visual aids."}},
        {{"id", "files"}, {"label", "Files"},
         {"description", "Auto-save and save participant behavior."}},
        {{"id", "claude_code"}, {"label", "Claude Code"},
         {"description", "Claude Code CLI and model preferences."}},
        {{"id", "codex"}, {"label", "Codex"},
         {"description", "Codex CLI, model, and transport preferences."}},
        {{"id", "mcp"}, {"label", "MCP"},
         {"description", "MCP discovery, trust, access, and servers."}},
        {{"id", "terminal"}, {"label", "Terminal"},
         {"description", "Integrated terminal profile and limits."}},
        {{"id", "workspace"}, {"label", "Workspace"},
         {"description", "Workspace discovery and remembered roots."}},
        {{"id", "extensions"}, {"label", "Extensions"},
         {"description", "Extension trust and contribution surfaces."}},
        {{"id", "customization"}, {"label", "Customization"},
         {"description", "Instruction, agent, workflow, and skill discovery."}},
        {{"id", "advanced"}, {"label", "Advanced"},
         {"description", "Sampling, context, connection, and provider payloads."},
         {"advanced", true}},
        {{"id", "custom_models"}, {"label", "Custom Models"},
         {"description", "User-defined context windows and capabilities."}},
        {{"id", "permissions"}, {"label", "Permissions"},
         {"description", "Tool permissions and approval policy."}},
    });
}

int32_t load_selected_config(const ScopeStore& scopes, const Json& params,
                             Json& root, std::string& scope,
                             std::string& plugin_id) {
    scope = trim_ascii(string_value(params.value("scope", Json())));
    plugin_id = trim_ascii(string_value(params.value("pluginId", Json())));
    const bool merged = bool_value(params.value("merged", Json()), false) ||
                        scope.empty() || scope == "merged";
    if (merged) {
        scope = "merged";
        plugin_id.clear();
        return scopes.load_merged_config(root);
    }
    return scopes.load_scope_config(scope, plugin_id, root);
}

struct SettingsScopeLayer {
    std::string scope;
    std::string plugin_id;
    std::string source;
    Json settings{Json::object()};
};

Json settings_from_scope_root(const Json& root) {
    Json settings = root.contains("ai_editor") && root["ai_editor"].is_object()
                        ? root["ai_editor"]
                        : Json::object();
    if (settings.contains("mcpServers") &&
        (settings["mcpServers"].is_array() || settings["mcpServers"].is_object())) {
        if (!settings["mcp"].is_object())
            settings["mcp"] = Json::object();
        if (!settings["mcp"].contains("mcpServers"))
            settings["mcp"]["mcpServers"] = settings["mcpServers"];
        settings.erase("mcpServers");
    }
    return settings;
}

void overlay_source_tree(Json& target, const Json& values,
                         std::string_view source) {
    if (!target.is_object())
        target = Json::object();
    if (!values.is_object())
        return;
    for (const auto& [key, value] : values.items()) {
        if (value.is_object()) {
            Json& child = target[key];
            if (!child.is_object())
                child = Json::object();
            child["$scope"] = source;
            overlay_source_tree(child, value, source);
        } else {
            target[key] = source;
        }
    }
}

int32_t load_settings_scope_layers(const ScopeStore& scopes,
                                   std::vector<SettingsScopeLayer>& layers) {
    layers.clear();
    const Json described = scopes.describe_scopes();
    if (!described.is_array())
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    layers.reserve(described.size());
    for (const auto& entry : described) {
        if (!entry.is_object())
            continue;
        SettingsScopeLayer layer;
        layer.scope = trim_ascii(string_value(entry.value("scope", Json())));
        layer.plugin_id = trim_ascii(string_value(entry.value("pluginId", Json())));
        if (!one_of(layer.scope, {"system", "workspace", "plugin"}))
            continue;
        layer.source = layer.scope == "plugin" ? "plugin:" + layer.plugin_id : layer.scope;
        Json root;
        const int32_t status =
            scopes.load_scope_config(layer.scope, layer.plugin_id, root);
        if (status != SAO_AI_EDITOR_OK)
            return status;
        layer.settings = settings_from_scope_root(root);
        layers.push_back(std::move(layer));
    }
    return SAO_AI_EDITOR_OK;
}

std::vector<std::string> dotted_path(std::string_view path) {
    std::vector<std::string> parts;
    size_t begin = 0;
    while (begin <= path.size()) {
        const size_t end = path.find('.', begin);
        const std::string part = trim_ascii(std::string(path.substr(
            begin, end == std::string_view::npos ? std::string_view::npos
                                                 : end - begin)));
        if (part.empty()) {
            return {};
        }
        parts.push_back(part);
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return parts;
}

std::string canonical_settings_path(std::string path) {
    path = trim_ascii(std::move(path));
    if (path == "mcpServers") {
        return "mcp.mcpServers";
    }
    return path;
}

bool set_dotted_path(Json& target, std::string_view path, const Json& value) {
    const auto parts = dotted_path(path);
    if (parts.empty()) {
        return false;
    }
    Json* current = &target;
    for (size_t index = 0; index + 1 < parts.size(); ++index) {
        if (!current->is_object()) {
            *current = Json::object();
        }
        Json& child = (*current)[parts[index]];
        if (!child.is_object()) {
            child = Json::object();
        }
        current = &child;
    }
    if (!current->is_object()) {
        *current = Json::object();
    }
    (*current)[parts.back()] = value;
    return true;
}

bool erase_dotted_path(Json& target, std::string_view path) {
    const auto parts = dotted_path(path);
    if (parts.empty() || !target.is_object()) {
        return false;
    }
    Json* current = &target;
    std::vector<std::pair<Json*, std::string>> parents;
    for (size_t index = 0; index + 1 < parts.size(); ++index) {
        if (!current->contains(parts[index]) ||
            !(*current)[parts[index]].is_object()) {
            return true;
        }
        parents.emplace_back(current, parts[index]);
        current = &(*current)[parts[index]];
    }
    current->erase(parts.back());
    for (auto iterator = parents.rbegin(); iterator != parents.rend();
         ++iterator) {
        Json& child = (*iterator->first)[iterator->second];
        if (!child.is_object() || !child.empty()) {
            break;
        }
        iterator->first->erase(iterator->second);
    }
    return true;
}

Json validation_error(std::string_view path, std::string_view code,
                      std::string_view message, std::string_view expected = {},
                      const Json& actual = Json()) {
    Json error{{"path", path}, {"code", code}, {"message", message}};
    if (!expected.empty()) {
        error["expected"] = expected;
    }
    if (!actual.is_discarded()) {
        error["actualType"] = actual.type_name();
    }
    return error;
}

int32_t invalid_argument(Json& details, Json errors) {
    details = {{"code", "INVALID_ARGUMENT"},
               {"validationErrors", errors},
               {"errors", std::move(errors)}};
    return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
}

bool valid_json_integer(const Json& value, int64_t minimum, int64_t maximum) {
    if (value.is_number_integer()) {
        const int64_t number = value.get<int64_t>();
        return number >= minimum && number <= maximum;
    }
    if (value.is_number_unsigned()) {
        const uint64_t number = value.get<uint64_t>();
        if (maximum < 0) {
            return false;
        }
        const uint64_t unsigned_minimum = minimum <= 0
            ? 0
            : static_cast<uint64_t>(minimum);
        return number >= unsigned_minimum &&
               number <= static_cast<uint64_t>(maximum);
    }
    return false;
}

bool valid_finite_number(const Json& value, double minimum, double maximum) {
    if (!value.is_number()) {
        return false;
    }
    const double number = value.get<double>();
    return std::isfinite(number) && number >= minimum && number <= maximum;
}

void validate_string_field(const Json& object, std::string_view key,
                           std::string_view path, Json& errors,
                           bool allow_empty = true) {
    const std::string owned_key(key);
    if (!object.contains(owned_key)) {
        return;
    }
    const Json& value = object[owned_key];
    if (!value.is_string() ||
        value.get_ref<const std::string&>().size() >
            kMaximumSettingStringBytes ||
        (!allow_empty && trim_ascii(value.get<std::string>()).empty())) {
        errors.push_back(validation_error(
            path, "TYPE_OR_RANGE", "expected bounded JSON string",
            allow_empty ? "string <= 64 KiB" : "non-empty string <= 64 KiB",
            value));
    }
}

void validate_bool_field(const Json& object, std::string_view key,
                         std::string_view path, Json& errors) {
    const std::string owned_key(key);
    if (object.contains(owned_key) && !object[owned_key].is_boolean()) {
        errors.push_back(validation_error(path, "TYPE", "expected boolean",
                                          "boolean", object[owned_key]));
    }
}

void validate_object_field(const Json& object, std::string_view key,
                           std::string_view path, Json& errors) {
    const std::string owned_key(key);
    if (object.contains(owned_key) && !object[owned_key].is_object()) {
        errors.push_back(validation_error(path, "TYPE", "expected object",
                                          "object", object[owned_key]));
    }
}

void validate_array_field(const Json& object, std::string_view key,
                          std::string_view path, Json& errors) {
    const std::string owned_key(key);
    if (object.contains(owned_key) && !object[owned_key].is_array()) {
        errors.push_back(validation_error(path, "TYPE", "expected array",
                                          "array", object[owned_key]));
    }
}

void validate_enum_field(
    const Json& object, std::string_view key, std::string_view path,
    std::initializer_list<std::string_view> choices, Json& errors) {
    const std::string owned_key(key);
    if (!object.contains(owned_key) || !object[owned_key].is_string()) {
        return;
    }
    const std::string value = lower_ascii(trim_ascii(
        object[owned_key].get<std::string>()));
    if (!one_of(value, choices)) {
        errors.push_back(validation_error(
            path, "VALUE", "string is outside the accepted values",
            "supported enum value", object[owned_key]));
    }
}

void validate_string_array(const Json& value, std::string_view path,
                           Json& errors) {
    if (!value.is_array()) {
        return;
    }
    for (size_t index = 0; index < value.size(); ++index) {
        const Json& item = value[index];
        if (!item.is_string() ||
            item.get_ref<const std::string&>().size() >
                kMaximumSettingStringBytes) {
            errors.push_back(validation_error(
                std::string(path) + "." + std::to_string(index),
                "TYPE_OR_RANGE", "expected bounded string",
                "string <= 64 KiB", item));
        }
    }
}

enum class SecretValueKind { text, string_map, json_object };

bool secret_control_object(const Json& value) {
    return value.is_object() &&
           (value.contains("action") || value.contains("op") ||
            value.contains("value"));
}

void validate_secret_string_map(const Json& value, std::string_view path,
                                Json& errors) {
    if (!value.is_object()) {
        errors.push_back(validation_error(
            path, "TYPE", "secret mapping must be an object", "object",
            value));
        return;
    }
    for (const auto& [key, item] : value.items()) {
        if (key.empty() || key.size() > kMaximumSettingStringBytes ||
            !item.is_string() ||
            item.get_ref<const std::string&>().size() >
                kMaximumSettingStringBytes) {
            errors.push_back(validation_error(
                std::string(path) + "." + key, "TYPE_OR_RANGE",
                "secret mapping entries require bounded string keys and values",
                "non-empty string key and string value <= 64 KiB", item));
        }
    }
}

void validate_secret_update(const Json& update, SecretValueKind kind,
                            std::string_view path, Json& errors) {
    if (update.is_null()) {
        return;
    }
    if (update.is_string()) {
        const std::string& value = update.get_ref<const std::string&>();
        if (value.size() > kMaximumSettingStringBytes) {
            errors.push_back(validation_error(
                path, "RANGE", "secret text exceeds the accepted size",
                "string <= 64 KiB", update));
            return;
        }
        if (kind != SecretValueKind::text) {
            const std::string action = lower_ascii(trim_ascii(value));
            if (!value.empty() && value != kSecretPresent &&
                value != kProtectedPlaceholder && action != "preserve" &&
                action != "clear") {
                errors.push_back(validation_error(
                    path, "TYPE", "mapping secrets require an object value",
                    "object or preserve/clear marker", update));
            }
        }
        return;
    }
    if (!update.is_object()) {
        errors.push_back(validation_error(
            path, "TYPE", "secret update has an unsupported JSON type",
            "string|object|null", update));
        return;
    }

    if (!secret_control_object(update)) {
        if (kind == SecretValueKind::text) {
            errors.push_back(validation_error(
                path, "TYPE", "text secret update requires a control object",
                "{action,value}", update));
        } else if (kind == SecretValueKind::string_map) {
            validate_secret_string_map(update, path, errors);
        }
        return;
    }

    if (update.contains("action") && !update["action"].is_string()) {
        errors.push_back(validation_error(
            std::string(path) + ".action", "TYPE",
            "secret action must be a string", "string", update["action"]));
        return;
    }
    if (update.contains("op") && !update["op"].is_string()) {
        errors.push_back(validation_error(
            std::string(path) + ".op", "TYPE",
            "secret operation must be a string", "string", update["op"]));
        return;
    }
    const std::string action = lower_ascii(trim_ascii(string_value(
        update.contains("action") ? update["action"]
                                  : update.value("op", Json()),
        "preserve")));
    if (!one_of(action,
                {"preserve", "set", "replace", "clear", "delete"})) {
        errors.push_back(validation_error(
            path, "VALUE", "unknown secret update action",
            "preserve|set|replace|clear|delete", update));
        return;
    }
    if (update.contains("action") && update.contains("op")) {
        const std::string operation = lower_ascii(trim_ascii(
            update["op"].get<std::string>()));
        if (operation != action) {
            errors.push_back(validation_error(
                path, "CONFLICT", "secret action and op must match"));
            return;
        }
    }
    if (action != "set" && action != "replace") {
        return;
    }
    if (!update.contains("value")) {
        errors.push_back(validation_error(
            std::string(path) + ".value", "REQUIRED",
            "set secret update requires value"));
        return;
    }
    const Json& value = update["value"];
    if (kind == SecretValueKind::text) {
        if (!value.is_string() ||
            value.get_ref<const std::string&>().size() >
                kMaximumSettingStringBytes) {
            errors.push_back(validation_error(
                std::string(path) + ".value", "TYPE_OR_RANGE",
                "text secret value must be bounded text",
                "string <= 64 KiB", value));
        }
    } else if (kind == SecretValueKind::string_map) {
        validate_secret_string_map(value, std::string(path) + ".value",
                                   errors);
    } else if (!value.is_object()) {
        errors.push_back(validation_error(
            std::string(path) + ".value", "TYPE",
            "JSON secret value must be an object", "object", value));
    }
}

void validate_provider_secret_map(const Json& updates, SecretValueKind kind,
                                  std::string_view path, Json& errors) {
    if (!updates.is_object()) {
        errors.push_back(validation_error(
            path, "TYPE", "provider secret updates must be an object",
            "object", updates));
        return;
    }
    for (const auto& [provider_id, update] : updates.items()) {
        if (trim_ascii(provider_id).empty()) {
            errors.push_back(validation_error(
                path, "REQUIRED", "provider ID must not be empty"));
            continue;
        }
        validate_secret_update(update, kind,
                               std::string(path) + "." + provider_id,
                               errors);
    }
}

void validate_mcp_secret_updates(const Json& secret_updates,
                                 Json& errors) {
    const Json* updates = nullptr;
    std::string selected_key;
    for (const auto key : {"mcpServers", "mcp_servers", "mcp"}) {
        if (!secret_updates.contains(key)) {
            continue;
        }
        if (!secret_updates[key].is_object()) {
            errors.push_back(validation_error(
                key, "TYPE", "MCP secret updates must be an object",
                "object", secret_updates[key]));
            continue;
        }
        if (updates != nullptr && *updates != secret_updates[key]) {
            errors.push_back(validation_error(
                key, "CONFLICT", "MCP secret update aliases must match"));
            continue;
        }
        updates = &secret_updates[key];
        selected_key = key;
    }
    if (updates == nullptr) {
        return;
    }
    for (const auto& [server_id, server_update] : updates->items()) {
        const std::string server_path = selected_key + "." + server_id;
        if (server_id.empty() || !server_update.is_object()) {
            errors.push_back(validation_error(
                server_path, "TYPE_OR_RANGE",
                "MCP server secret update requires a non-empty ID and object",
                "object", server_update));
            continue;
        }
        for (const auto kind : {"env", "headers"}) {
            if (server_update.contains(kind)) {
                validate_secret_update(
                    server_update[kind], SecretValueKind::string_map,
                    server_path + "." + kind, errors);
            }
        }
    }
}

void validate_secret_updates(const Json& params, Json& errors) {
    const Json* secret_updates = nullptr;
    for (const auto key : {"secretUpdates", "secret_updates"}) {
        if (!params.contains(key)) {
            continue;
        }
        if (!params[key].is_object()) {
            errors.push_back(validation_error(
                key, "TYPE", "secret updates must be an object", "object",
                params[key]));
            continue;
        }
        if (secret_updates != nullptr && *secret_updates != params[key]) {
            errors.push_back(validation_error(
                key, "CONFLICT", "secretUpdates and secret_updates must match"));
            continue;
        }
        secret_updates = &params[key];
    }
    if (secret_updates == nullptr) {
        return;
    }

    if (secret_updates->contains("apiKey")) {
        const Json& update = (*secret_updates)["apiKey"];
        if (update.is_object() && !secret_control_object(update)) {
            validate_provider_secret_map(
                update, SecretValueKind::text, "apiKey", errors);
        } else {
            validate_secret_update(update, SecretValueKind::text, "apiKey",
                                   errors);
        }
    }
    for (const auto key : {"providerKeys", "provider_keys"}) {
        if (secret_updates->contains(key)) {
            validate_provider_secret_map(
                (*secret_updates)[key], SecretValueKind::text, key, errors);
        }
    }
    for (const auto& [key, kind] :
         std::array<std::pair<std::string_view, SecretValueKind>, 2>{
             std::pair{"extraHeaders", SecretValueKind::string_map},
             std::pair{"extraBody", SecretValueKind::json_object}}) {
        const std::string owned_key(key);
        if (!secret_updates->contains(owned_key)) {
            continue;
        }
        const Json& update = (*secret_updates)[owned_key];
        const bool provider_map =
            update.is_object() && !secret_control_object(update) &&
            std::any_of(update.begin(), update.end(),
                        [](const Json& value) {
                            return secret_control_object(value);
                        });
        if (provider_map) {
            validate_provider_secret_map(update, kind, key, errors);
        } else {
            validate_secret_update(update, kind, key, errors);
        }
    }
    validate_mcp_secret_updates(*secret_updates, errors);
}

void validate_mcp_server_group(const Json& group, std::string_view path,
                               Json& errors) {
    if (!group.is_array() && !group.is_object()) {
        errors.push_back(validation_error(
            path, "TYPE", "expected MCP server array or object",
            "array|object", group));
        return;
    }
    const auto validate_server = [&](const Json& server,
                                     std::string server_path,
                                     bool requires_id) {
        if (!server.is_object()) {
            errors.push_back(validation_error(server_path, "TYPE",
                                              "expected MCP server object",
                                              "object", server));
            return;
        }
        if (requires_id) {
            validate_string_field(server, "id", server_path + ".id", errors,
                                  false);
            if (!server.contains("id")) {
                errors.push_back(validation_error(
                    server_path + ".id", "REQUIRED",
                    "array-form MCP server requires id", "non-empty string"));
            }
        }
        for (const auto kind : {"env", "headers"}) {
            const std::string kind_key(kind);
            if (!server.contains(kind_key)) {
                continue;
            }
            const Json& mapping = server[kind_key];
            if (!mapping.is_object()) {
                errors.push_back(validation_error(
                    server_path + "." + kind_key, "TYPE",
                    "expected secret mapping object", "object", mapping));
                continue;
            }
            for (const auto& [key, value] : mapping.items()) {
                if (key.empty() || !value.is_string() ||
                    value.get_ref<const std::string&>().size() >
                        kMaximumSettingStringBytes) {
                    errors.push_back(validation_error(
                        server_path + "." + kind_key + "." + key,
                        "TYPE_OR_RANGE", "expected bounded secret string",
                        "string <= 64 KiB", value));
                }
            }
        }
        validate_bool_field(server, "enabled", server_path + ".enabled",
                            errors);
        validate_bool_field(server, "trusted", server_path + ".trusted",
                            errors);
        validate_string_field(server, "transport",
                              server_path + ".transport", errors);
        validate_string_field(server, "command", server_path + ".command",
                              errors);
        validate_string_field(server, "url", server_path + ".url", errors);
        validate_array_field(server, "args", server_path + ".args", errors);
        validate_array_field(server, "inherit_env",
                             server_path + ".inherit_env", errors);
    };
    if (group.is_array()) {
        for (size_t index = 0; index < group.size(); ++index) {
            validate_server(group[index],
                            std::string(path) + "." + std::to_string(index),
                            true);
        }
    } else {
        for (const auto& [server_id, server] : group.items()) {
            if (server_id.empty()) {
                errors.push_back(validation_error(
                    std::string(path), "REQUIRED",
                    "MCP server id must not be empty", "non-empty object key"));
            }
            validate_server(server, std::string(path) + "." + server_id,
                            false);
        }
    }
}

void validate_settings_patch(const Json& patch, Json& errors) {
    if (!patch.is_object()) {
        errors.push_back(validation_error("settings", "TYPE",
                                          "settings patch must be an object",
                                          "object", patch));
        return;
    }
    for (const auto& [key, _value] : patch.items()) {
        const std::string canonical_key = canonical_settings_path(key);
        if (canonical_key.size() > kMaximumSettingStringBytes ||
            (canonical_key.find('.') != std::string::npos &&
             dotted_path(canonical_key).empty())) {
            errors.push_back(validation_error(
                key, "FORMAT", "setting key must be a valid dotted path",
                "non-empty dotted path"));
        }
    }
    for (const auto key : {"provider", "api_key", "base_url", "model",
                           "system_prompt", "transport", "mode", "language",
                           "approval", "active_chat_provider", "theme",
                           "color_theme", "file_icon_theme"}) {
        validate_string_field(patch, key, key, errors,
                              std::string_view(key) != "provider");
    }
    validate_enum_field(
        patch, "transport", "transport",
        {"chat_completions", "chat-completions", "responses",
         "openai_responses", "response"},
        errors);
    validate_enum_field(patch, "mode", "mode",
                        {"agent", "ask", "plan", "chat", "edit"}, errors);
    validate_enum_field(patch, "approval", "approval",
                        {"default", "bypass", "autopilot"}, errors);
    validate_enum_field(patch, "theme", "theme", {"dark", "light"},
                        errors);
    for (const auto [key, minimum, maximum] :
         std::array<std::tuple<std::string_view, double, double>, 3>{
             std::tuple{"temperature", 0.0, 2.0},
             std::tuple{"top_p", 0.0, 1.0},
             std::tuple{"frequency_penalty", -2.0, 2.0}}) {
        const std::string owned_key(key);
        if (patch.contains(owned_key) &&
            !valid_finite_number(patch[owned_key], minimum, maximum)) {
            errors.push_back(validation_error(
                key, "TYPE_OR_RANGE", "number is outside the accepted range",
                "finite number", patch[owned_key]));
        }
    }
    if (patch.contains("presence_penalty") &&
        !valid_finite_number(patch["presence_penalty"], -2.0, 2.0)) {
        errors.push_back(validation_error(
            "presence_penalty", "TYPE_OR_RANGE",
            "number is outside the accepted range", "finite number",
            patch["presence_penalty"]));
    }
    for (const auto [key, minimum, maximum] :
         std::array<std::tuple<std::string_view, int64_t, int64_t>, 4>{
             std::tuple{"max_tokens", 0, INT32_MAX},
             std::tuple{"max_input_tokens", 0, INT32_MAX},
             std::tuple{"max_output_tokens", 0, INT32_MAX},
             std::tuple{"timeout", 1, 3600}}) {
        const std::string owned_key(key);
        if (patch.contains(owned_key) &&
            !valid_json_integer(patch[owned_key], minimum, maximum)) {
            errors.push_back(validation_error(
                key, "TYPE_OR_RANGE", "integer is outside the accepted range",
                "integer", patch[owned_key]));
        }
    }
    validate_array_field(patch, "stop", "stop", errors);
    if (patch.contains("stop")) {
        validate_string_array(patch["stop"], "stop", errors);
    }
    for (const auto key : {"extra_headers", "extra_body", "provider_keys",
                           "custom_models", "permissions",
                           "configuration_targets"}) {
        validate_object_field(patch, key, key, errors);
    }
    for (const auto section : {"claude_code", "codex", "mcp", "terminal",
                               "workspace", "extensions", "editor", "files",
                               "customization", "layout"}) {
        validate_object_field(patch, section, section, errors);
    }
    if (patch.contains("mcp") && patch["mcp"].is_object()) {
        const Json& mcp = patch["mcp"];
        for (const auto key : {"enabled", "autostart", "discovery_enabled",
                               "workspace_trusted", "server_sampling"}) {
            validate_bool_field(mcp, key, std::string("mcp.") + key, errors);
        }
        for (const auto key : {"access", "collision_behavior"}) {
            validate_string_field(mcp, key, std::string("mcp.") + key,
                                  errors, false);
        }
        validate_enum_field(mcp, "access", "mcp.access",
                            {"prompt", "read_only", "allow", "disabled"},
                            errors);
        validate_enum_field(mcp, "collision_behavior",
                            "mcp.collision_behavior",
                            {"first", "last", "error"}, errors);
        for (const auto group_name : {"servers", "mcpServers"}) {
            if (mcp.contains(group_name)) {
                validate_mcp_server_group(
                    mcp[group_name], std::string("mcp.") + group_name,
                    errors);
            }
        }
    }
    if (patch.contains("mcpServers")) {
        validate_mcp_server_group(patch["mcpServers"], "mcpServers", errors);
    }
    if (patch.contains("layout") && patch["layout"].is_object()) {
        const Json& layout = patch["layout"];
        for (const auto key : {"sidebarVisible", "editorVisible",
                               "chatVisible", "minimapVisible"}) {
            validate_bool_field(layout, key, std::string("layout.") + key,
                                errors);
        }
        for (const auto key : {"panelHeight", "sidebarWidth",
                               "rightSidebarWidth", "activeSidebarPanel",
                               "activeBottomTab"}) {
            validate_string_field(layout, key, std::string("layout.") + key,
                                  errors);
        }
    }

    const auto dotted_value = [&](std::string_view path) -> const Json* {
        const std::string owned_path(path);
        if (patch.contains(owned_path)) {
            return &patch[owned_path];
        }
        const Json* current = &patch;
        for (const auto& part : dotted_path(path)) {
            if (!current->is_object() || !current->contains(part)) {
                return nullptr;
            }
            current = &(*current)[part];
        }
        return current;
    };
    const auto check_dotted_bool = [&](std::string_view path) {
        if (const Json* value = dotted_value(path);
            value != nullptr && !value->is_boolean()) {
            errors.push_back(validation_error(
                path, "TYPE", "expected boolean", "boolean", *value));
        }
    };
    const auto check_dotted_string = [&](std::string_view path) {
        if (const Json* value = dotted_value(path);
            value != nullptr &&
            (!value->is_string() ||
             value->get_ref<const std::string&>().size() >
                 kMaximumSettingStringBytes)) {
            errors.push_back(validation_error(
                path, "TYPE_OR_RANGE", "expected bounded string",
                "string <= 64 KiB", *value));
        }
    };
    const auto check_dotted_integer = [&](std::string_view path,
                                          int64_t minimum,
                                          int64_t maximum) {
        if (const Json* value = dotted_value(path);
            value != nullptr &&
            !valid_json_integer(*value, minimum, maximum)) {
            errors.push_back(validation_error(
                path, "TYPE_OR_RANGE", "integer is outside the accepted range",
                "integer", *value));
        }
    };
    const auto check_dotted_enum = [&]<size_t Size>(
                                       std::string_view path,
                                       const std::array<std::string_view,
                                                        Size>& choices) {
        if (const Json* value = dotted_value(path); value != nullptr &&
            value->is_string()) {
            const std::string normalized = lower_ascii(trim_ascii(
                value->get<std::string>()));
            if (std::find(choices.begin(), choices.end(), normalized) ==
                choices.end()) {
                errors.push_back(validation_error(
                    path, "VALUE", "string is outside the accepted values",
                    "supported enum value", *value));
            }
        }
    };
    for (const auto path : {"editor.insertSpaces", "editor.formatOnType",
                            "editor.formatOnSave", "editor.formatOnPaste",
                            "editor.linkedEditing", "editor.folding",
                            "editor.foldingHighlight",
                            "editor.unfoldOnClickAfterEndOfLine",
                            "editor.renderControlCharacters",
                            "files.trimTrailingWhitespace",
                            "files.insertFinalNewline",
                            "files.trimFinalNewlines", "terminal.use_pty",
                            "workspace.auto_detect", "workspace.remember_last",
                            "layout.sidebarVisible", "layout.editorVisible",
                            "layout.chatVisible", "layout.minimapVisible"}) {
        check_dotted_bool(path);
    }
    for (const auto path : {"editor.defaultFormatter", "editor.wordSeparators",
                            "editor.acceptSuggestionOnEnter",
                            "editor.renderWhitespace", "editor.renderLineHighlight",
                            "editor.foldingStrategy", "editor.showFoldingControls",
                            "files.autoSave", "terminal.profile",
                            "terminal.shell_path", "workspace.root",
                            "workspace.last_root", "layout.panelHeight",
                            "layout.sidebarWidth", "layout.rightSidebarWidth",
                            "layout.activeSidebarPanel", "layout.activeBottomTab"}) {
        check_dotted_string(path);
    }
    check_dotted_integer("editor.tabSize", 1, 16);
    check_dotted_integer("editor.quickSuggestionsDelay", 0, 10000);
    check_dotted_integer("files.autoSaveDelay", 0, INT32_MAX);
    check_dotted_integer("terminal.timeout", 1, 3600);
    check_dotted_integer("terminal.output_limit", 1000, INT32_MAX);
    check_dotted_enum(
        "files.autoSave",
        std::array<std::string_view, 4>{"off", "afterdelay",
                                        "onfocuschange", "onwindowchange"});
    check_dotted_enum(
        "layout.activeBottomTab",
        std::array<std::string_view, 3>{"terminal", "output", "problems"});
    if (const Json* value = dotted_value("mcp.servers"); value != nullptr) {
        validate_mcp_server_group(*value, "mcp.servers", errors);
    }
    if (const Json* value = dotted_value("mcp.mcpServers"); value != nullptr) {
        validate_mcp_server_group(*value, "mcp.mcpServers", errors);
    }
    for (const auto& [key, value] : patch.items()) {
        if (key.starts_with("provider_keys.") &&
            (!value.is_string() ||
             value.get_ref<const std::string&>().size() >
                 kMaximumSettingStringBytes)) {
            errors.push_back(validation_error(
                key, "TYPE_OR_RANGE", "provider key must be bounded text",
                "string <= 64 KiB", value));
        }
    }
    for (const auto key : {"provider_keys", "_provider_keys"}) {
        if (!patch.contains(key) || !patch[key].is_object()) {
            continue;
        }
        for (const auto& [provider_id, value] : patch[key].items()) {
            if (provider_id.empty() || !value.is_string() ||
                value.get_ref<const std::string&>().size() >
                    kMaximumSettingStringBytes) {
                errors.push_back(validation_error(
                    std::string(key) + "." + provider_id,
                    "TYPE_OR_RANGE", "provider key must be bounded text",
                    "non-empty provider ID and string <= 64 KiB", value));
            }
        }
    }
    for (const auto key : {"_clear_api_key", "_clear_extra_headers",
                           "_clear_extra_body"}) {
        validate_bool_field(patch, key, key, errors);
    }
    validate_array_field(patch, "_clear_provider_keys",
                         "_clear_provider_keys", errors);
    if (patch.contains("_clear_provider_keys")) {
        validate_string_array(patch["_clear_provider_keys"],
                              "_clear_provider_keys", errors);
    }
}

int32_t parse_reset_keys(const Json& params,
                         std::vector<std::string>& reset_keys,
                         Json& errors) {
    reset_keys.clear();
    const Json* source = nullptr;
    for (const auto key : {"resetKeys", "reset_keys"}) {
        if (!params.contains(key)) {
            continue;
        }
        if (!params[key].is_array()) {
            errors.push_back(validation_error(
                key, "TYPE", "reset keys must be an array", "array",
                params[key]));
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        if (source == nullptr) {
            source = &params[key];
        } else if (*source != params[key]) {
            errors.push_back(validation_error(
                key, "CONFLICT", "resetKeys and reset_keys must match"));
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
    }
    if (source == nullptr) {
        return SAO_AI_EDITOR_OK;
    }
    if (source->size() > kMaximumResetKeys) {
        errors.push_back(validation_error(
            "resetKeys", "RANGE", "too many reset keys", "<= 1024 items"));
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::set<std::string, std::less<>> seen;
    for (size_t index = 0; index < source->size(); ++index) {
        const Json& item = (*source)[index];
        if (!item.is_string()) {
            errors.push_back(validation_error(
                "resetKeys." + std::to_string(index), "TYPE",
                "reset key must be a dotted string", "string", item));
            continue;
        }
        const std::string path = canonical_settings_path(
            item.get<std::string>());
        if (path.empty() || dotted_path(path).empty()) {
            errors.push_back(validation_error(
                "resetKeys." + std::to_string(index), "FORMAT",
                "reset key must be a valid dotted path"));
            continue;
        }
        if (seen.insert(path).second) {
            reset_keys.push_back(path);
        }
    }
    return errors.empty() ? SAO_AI_EDITOR_OK
                          : SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
}

void apply_settings_patch(Json& candidate, const Json& patch) {
    for (const auto& [key, value] : patch.items()) {
        const std::string canonical_key = canonical_settings_path(key);
        if (canonical_key.find('.') != std::string::npos) {
            (void)set_dotted_path(candidate, canonical_key, value);
        } else if (candidate.contains(canonical_key) &&
                   candidate[canonical_key].is_object() &&
                   value.is_object()) {
            merge_patch(candidate[canonical_key], value);
        } else {
            candidate[canonical_key] = value;
        }
    }
}

struct SecretMutation final {
    std::string key;
    bool erase = false;
    std::string value;
    bool had_previous = false;
    std::string previous;
};

enum class SecretAction { preserve, set, clear };

struct ParsedSecretUpdate final {
    SecretAction action = SecretAction::preserve;
    std::string value;
};

ParsedSecretUpdate parse_secret_update(const Json& update, bool json_value) {
    ParsedSecretUpdate parsed;
    if (update.is_null()) {
        parsed.action = SecretAction::clear;
        return parsed;
    }
    if (update.is_string()) {
        const std::string value = update.get<std::string>();
        const std::string action = lower_ascii(trim_ascii(value));
        if (action == "preserve" || value == kSecretPresent ||
            value == kProtectedPlaceholder || value.empty()) {
            return parsed;
        }
        if (action == "clear") {
            parsed.action = SecretAction::clear;
            return parsed;
        }
        parsed.action = SecretAction::set;
        parsed.value = value;
        return parsed;
    }
    if (update.is_object()) {
        const bool has_control = update.contains("action") || update.contains("op") ||
                                 update.contains("value");
        if (!has_control && json_value) {
            parsed.action = SecretAction::set;
            parsed.value = update.dump();
            return parsed;
        }
        const std::string action = lower_ascii(trim_ascii(string_value(
            update.contains("action") ? update["action"]
                                      : update.value("op", Json()),
            "preserve")));
        if (action == "clear" || action == "delete") {
            parsed.action = SecretAction::clear;
            return parsed;
        }
        if (action == "set" || action == "replace") {
            if (!update.contains("value")) {
                return parsed;
            }
            parsed.action = SecretAction::set;
            parsed.value = json_value && !update["value"].is_string()
                               ? update["value"].dump()
                               : string_value(update["value"]);
            return parsed;
        }
        return parsed;
    }
    if (json_value && (update.is_array() || update.is_number() ||
                       update.is_boolean())) {
        parsed.action = SecretAction::set;
        parsed.value = update.dump();
    }
    return parsed;
}

void queue_mutation(std::map<std::string, SecretMutation, std::less<>>& mutations,
                    std::string key, ParsedSecretUpdate update) {
    if (key.empty() || update.action == SecretAction::preserve) {
        return;
    }
    SecretMutation mutation;
    mutation.key = key;
    mutation.erase = update.action == SecretAction::clear;
    mutation.value = std::move(update.value);
    mutations[mutation.key] = std::move(mutation);
}

int32_t read_planned_secret(
    SecretStore& secrets,
    const std::map<std::string, SecretMutation, std::less<>>& mutations,
    std::string_view key, std::string& value) {
    value.clear();
    if (key.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const auto pending = mutations.find(key);
    if (pending != mutations.end()) {
        if (pending->second.erase) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        value = pending->second.value;
        return SAO_AI_EDITOR_OK;
    }
    return get_secret(secrets, key, value);
}

int32_t read_mcp_secret_for_plan(
    SecretStore& secrets,
    std::map<std::string, SecretMutation, std::less<>>& mutations,
    std::string_view server_id, std::string_view kind,
    const std::set<std::string, std::less<>>& candidates,
    std::string& value) {
    const std::string primary = mcp_secret_key(server_id, kind);
    int32_t status = read_planned_secret(secrets, mutations, primary, value);
    if (status == SAO_AI_EDITOR_OK) {
        return status;
    }
    if (status != SAO_AI_EDITOR_ERR_NOT_FOUND) {
        return status;
    }

    const std::string legacy = legacy_mcp_secret_key(server_id, kind);
    const std::string block = migration_block_key(legacy);
    bool blocked = false;
    if (!block.empty()) {
        std::string block_value;
        const int32_t block_status = read_planned_secret(
            secrets, mutations, block, block_value);
        if (block_status == SAO_AI_EDITOR_OK) {
            blocked = true;
        } else if (block_status != SAO_AI_EDITOR_ERR_NOT_FOUND) {
            return block_status;
        }
    }

    if (!legacy_unambiguous(server_id, candidates)) {
        if (!blocked && !block.empty()) {
            ParsedSecretUpdate quarantine;
            quarantine.action = SecretAction::set;
            quarantine.value = "1";
            queue_mutation(mutations, block, std::move(quarantine));
        }
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    if (blocked) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }

    status = read_planned_secret(secrets, mutations, legacy, value);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }

    ParsedSecretUpdate migrate;
    migrate.action = SecretAction::set;
    migrate.value = value;
    queue_mutation(mutations, primary, std::move(migrate));
    ParsedSecretUpdate erase_legacy;
    erase_legacy.action = SecretAction::clear;
    queue_mutation(mutations, legacy, std::move(erase_legacy));
    return SAO_AI_EDITOR_OK;
}

void queue_provider_update(
    std::map<std::string, SecretMutation, std::less<>>& mutations,
    std::string_view provider, std::string_view kind,
    const ParsedSecretUpdate& update,
    const std::set<std::string, std::less<>>& candidates) {
    if (update.action == SecretAction::preserve) {
        return;
    }
    queue_mutation(mutations, provider_secret_key(provider, kind), update);
    ParsedSecretUpdate erase_legacy;
    erase_legacy.action = SecretAction::clear;
    if (legacy_unambiguous(provider, candidates)) {
        queue_mutation(mutations, legacy_provider_secret_key(provider, kind),
                       erase_legacy);
    }
    if (kind == "api-key" && valid_simple_id(provider)) {
        queue_mutation(mutations,
                       "provider/" + std::string(provider) + "/apiKey",
                       erase_legacy);
    }
}

void apply_provider_update(
    std::map<std::string, SecretMutation, std::less<>>& mutations,
    std::set<std::string, std::less<>>& state,
    std::string_view provider, std::string_view kind,
    const ParsedSecretUpdate& update,
    const std::set<std::string, std::less<>>& candidates) {
    if (update.action == SecretAction::preserve) {
        return;
    }
    queue_provider_update(mutations, provider, kind, update, candidates);
    if (update.action == SecretAction::set) {
        state.insert(std::string(provider));
    } else {
        state.erase(std::string(provider));
    }
}

void parse_provider_update_map(
    const Json& updates, std::string_view kind, bool json_value,
    std::map<std::string, SecretMutation, std::less<>>& mutations,
    std::set<std::string, std::less<>>& state,
    const std::set<std::string, std::less<>>& candidates) {
    if (!updates.is_object()) {
        return;
    }
    for (const auto& [provider, update] : updates.items()) {
        if (trim_ascii(provider).empty()) {
            continue;
        }
        apply_provider_update(mutations, state, provider, kind,
                              parse_secret_update(update, json_value), candidates);
    }
}

Json* find_mcp_server(Json& mcp, std::string_view requested_id) {
    Json* found = nullptr;
    (void)visit_mcp_servers(
        mcp, MutableMcpServerVisitor{[&](std::string_view server_id,
                                        Json& server) {
            if (found == nullptr && server_id == requested_id) {
                found = &server;
            }
            return SAO_AI_EDITOR_OK;
        }});
    return found;
}

int32_t apply_mcp_secret_update_controls(Json& mcp,
                                         const Json& secret_updates) {
    const Json* updates = nullptr;
    for (const auto key : {"mcpServers", "mcp_servers", "mcp"}) {
        if (secret_updates.contains(key)) {
            if (!secret_updates[key].is_object()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            updates = &secret_updates[key];
            break;
        }
    }
    if (updates == nullptr) {
        return SAO_AI_EDITOR_OK;
    }
    for (const auto& [server_id, server_update] : updates->items()) {
        if (!server_update.is_object()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        Json* server = find_mcp_server(mcp, server_id);
        if (server == nullptr) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        for (const auto kind : {"env", "headers"}) {
            if (!server_update.contains(kind)) {
                continue;
            }
            const ParsedSecretUpdate update = parse_secret_update(
                server_update[kind], true);
            if (update.action == SecretAction::preserve) {
                continue;
            }
            const std::string clear_key = "_clear_" + std::string(kind);
            const std::string replace_key = "_replace_" + std::string(kind);
            if (update.action == SecretAction::clear) {
                (*server)[clear_key] = true;
                server->erase(replace_key);
                (*server)[kind] = Json::object();
                continue;
            }
            Json value = Json::parse(update.value, nullptr, false);
            if (!value.is_object()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            (*server)[kind] = std::move(value);
            server->erase(clear_key);
            (*server)[replace_key] = true;
        }
    }
    return SAO_AI_EDITOR_OK;
}

void queue_mcp_update(
    std::map<std::string, SecretMutation, std::less<>>& mutations,
    std::string_view server_id, std::string_view kind,
    const ParsedSecretUpdate& update,
    const std::set<std::string, std::less<>>& candidates) {
    if (update.action == SecretAction::preserve) {
        return;
    }
    queue_mutation(mutations, mcp_secret_key(server_id, kind), update);
    if (legacy_unambiguous(server_id, candidates)) {
        ParsedSecretUpdate erase_legacy;
        erase_legacy.action = SecretAction::clear;
        queue_mutation(mutations, legacy_mcp_secret_key(server_id, kind),
                       erase_legacy);
    }
}

int32_t plan_mcp_secret_mutations(
    Json& mcp, SecretStore& secrets,
    std::map<std::string, SecretMutation, std::less<>>& mutations,
    const Json& secret_updates) {
    int32_t status = apply_mcp_secret_update_controls(mcp, secret_updates);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    const auto candidates = mcp_server_candidates(mcp);
    return visit_mcp_servers(
        mcp, MutableMcpServerVisitor{[&](std::string_view server_id,
                                        Json& server) -> int32_t {
            auto fields = mcp_secret_fields(server);
            for (const auto kind : {"env", "headers"}) {
                const std::string kind_key(kind);
                const std::string clear_key = "_clear_" + kind_key;
                const std::string replace_key = "_replace_" + kind_key;
                const bool clear = bool_value(
                    server.value(clear_key, Json(false)), false);
                const bool replace = bool_value(
                    server.value(replace_key, Json(false)), false);
                server.erase(clear_key);
                server.erase(replace_key);
                if (clear) {
                    ParsedSecretUpdate update;
                    update.action = SecretAction::clear;
                    queue_mcp_update(mutations, server_id, kind, update,
                                     candidates);
                    fields.erase(kind_key);
                    server[kind_key] = Json::object();
                    continue;
                }

                const Json raw = server.value(kind_key, Json::object());
                if (!raw.is_object()) {
                    return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                }
                Json merged = Json::object();
                bool had_secret = false;
                const bool explicit_replacement = replace ||
                    (!raw.empty() &&
                     std::none_of(raw.begin(), raw.end(),
                                  [](const Json& value) {
                                      return is_secret_marker(value);
                                  }));
                if (!explicit_replacement &&
                    (fields.contains(kind_key) || !raw.empty())) {
                    std::string stored_secret;
                    const int32_t read_status = read_mcp_secret_for_plan(
                        secrets, mutations, server_id, kind, candidates,
                        stored_secret);
                    if (read_status == SAO_AI_EDITOR_OK) {
                        merged = Json::parse(stored_secret, nullptr, false);
                        if (!merged.is_object()) {
                            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                        }
                        had_secret = true;
                    } else if (read_status != SAO_AI_EDITOR_ERR_NOT_FOUND) {
                        return read_status;
                    }
                }

                bool changed = explicit_replacement;
                for (const auto& [key, value] : raw.items()) {
                    if (is_secret_marker(value)) {
                        continue;
                    }
                    if (!value.is_string()) {
                        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                    }
                    merged[key] = value;
                    changed = true;
                }
                if (changed) {
                    ParsedSecretUpdate update;
                    update.action = SecretAction::set;
                    update.value = merged.dump();
                    queue_mcp_update(mutations, server_id, kind, update,
                                     candidates);
                    had_secret = true;
                }

                Json redacted = Json::object();
                if (had_secret) {
                    for (const auto& [key, _value] : merged.items()) {
                        redacted[key] = kSecretPresent;
                    }
                    fields.insert(kind_key);
                } else {
                    fields.erase(kind_key);
                }
                server[kind_key] = std::move(redacted);
            }
            server["_secret_fields"] = mcp_secret_field_array(fields);
            return SAO_AI_EDITOR_OK;
        }});
}

int32_t plan_secret_mutations(
    Json& candidate, const Json& patch, const Json& params,
    std::map<std::string, SecretMutation, std::less<>>& mutations) {
    const std::string provider = candidate["provider"].get<std::string>();
    auto candidates = provider_candidates(candidate);
    candidates.insert(provider);
    auto provider_state = state_values(candidate, "provider_keys");
    auto header_state = state_values(candidate, "extra_headers");
    auto body_state = state_values(candidate, "extra_body");

    if (candidate.contains("provider_keys") && candidate["provider_keys"].is_object()) {
        for (const auto& [provider_id, value] : candidate["provider_keys"].items()) {
            const std::string plaintext = string_value(value);
            if (!plaintext.empty() && !is_secret_marker(value)) {
                ParsedSecretUpdate update;
                update.action = SecretAction::set;
                update.value = plaintext;
                apply_provider_update(mutations, provider_state, provider_id,
                                      "api-key", update, candidates);
            }
        }
    }
    const std::string active_plaintext = string_value(candidate["api_key"]);
    if (!active_plaintext.empty() && !is_secret_marker(candidate["api_key"])) {
        ParsedSecretUpdate update;
        update.action = SecretAction::set;
        update.value = active_plaintext;
        apply_provider_update(mutations, provider_state, provider, "api-key",
                              update, candidates);
    }
    if (candidate["extra_headers"].is_object() &&
        !candidate["extra_headers"].empty()) {
        ParsedSecretUpdate update;
        update.action = SecretAction::set;
        update.value = candidate["extra_headers"].dump();
        apply_provider_update(mutations, header_state, provider, "extra_headers",
                              update, candidates);
    }
    if (candidate["extra_body"].is_object() && !candidate["extra_body"].empty()) {
        ParsedSecretUpdate update;
        update.action = SecretAction::set;
        update.value = candidate["extra_body"].dump();
        apply_provider_update(mutations, body_state, provider, "extra_body",
                              update, candidates);
    }

    if (patch.value("_clear_api_key", false)) {
        ParsedSecretUpdate update;
        update.action = SecretAction::clear;
        apply_provider_update(mutations, provider_state, provider, "api-key",
                              update, candidates);
    }
    if (patch.contains("_clear_provider_keys") &&
        patch["_clear_provider_keys"].is_array()) {
        for (const auto& item : patch["_clear_provider_keys"]) {
            const std::string provider_id = trim_ascii(string_value(item));
            if (provider_id.empty()) {
                continue;
            }
            ParsedSecretUpdate update;
            update.action = SecretAction::clear;
            apply_provider_update(mutations, provider_state, provider_id,
                                  "api-key", update, candidates);
        }
    }
    for (const auto& [clear_key, kind, state] :
         std::array<std::tuple<std::string_view, std::string_view,
                               std::set<std::string, std::less<>>*>, 2>{
             std::tuple{"_clear_extra_headers", "extra_headers", &header_state},
             std::tuple{"_clear_extra_body", "extra_body", &body_state}}) {
        if (patch.value(std::string(clear_key), false)) {
            ParsedSecretUpdate update;
            update.action = SecretAction::clear;
            apply_provider_update(mutations, *state, provider, kind, update,
                                  candidates);
        }
    }

    Json secret_updates = Json::object();
    if (params.contains("secretUpdates") && params["secretUpdates"].is_object()) {
        secret_updates = params["secretUpdates"];
    } else if (params.contains("secret_updates") &&
               params["secret_updates"].is_object()) {
        secret_updates = params["secret_updates"];
    }
    if (secret_updates.contains("apiKey")) {
        const auto update = parse_secret_update(secret_updates["apiKey"], false);
        if (secret_updates["apiKey"].is_object() &&
            !secret_updates["apiKey"].contains("action") &&
            !secret_updates["apiKey"].contains("op") &&
            !secret_updates["apiKey"].contains("value")) {
            parse_provider_update_map(secret_updates["apiKey"], "api-key", false,
                                      mutations, provider_state, candidates);
        } else {
            apply_provider_update(mutations, provider_state, provider, "api-key",
                                  update, candidates);
        }
    }
    if (secret_updates.contains("providerKeys")) {
        parse_provider_update_map(secret_updates["providerKeys"], "api-key", false,
                                  mutations, provider_state, candidates);
    }
    if (secret_updates.contains("provider_keys")) {
        parse_provider_update_map(secret_updates["provider_keys"], "api-key", false,
                                  mutations, provider_state, candidates);
    }
    if (secret_updates.contains("extraHeaders")) {
        const Json& update = secret_updates["extraHeaders"];
        if (update.is_object() && !update.contains("action") &&
            !update.contains("op") && !update.contains("value") &&
            std::any_of(update.begin(), update.end(), [](const Json& value) {
                return value.is_object() &&
                       (value.contains("action") || value.contains("op") ||
                        value.contains("value"));
            })) {
            parse_provider_update_map(update, "extra_headers", true, mutations,
                                      header_state, candidates);
        } else {
            apply_provider_update(mutations, header_state, provider,
                                  "extra_headers",
                                  parse_secret_update(update, true), candidates);
        }
    }
    if (secret_updates.contains("extraBody")) {
        const Json& update = secret_updates["extraBody"];
        if (update.is_object() && !update.contains("action") &&
            !update.contains("op") && !update.contains("value") &&
            std::any_of(update.begin(), update.end(), [](const Json& value) {
                return value.is_object() &&
                       (value.contains("action") || value.contains("op") ||
                        value.contains("value"));
            })) {
            parse_provider_update_map(update, "extra_body", true, mutations,
                                      body_state, candidates);
        } else {
            apply_provider_update(mutations, body_state, provider, "extra_body",
                                  parse_secret_update(update, true), candidates);
        }
    }

    candidate["api_key"] = "";
    candidate["provider_keys"] = Json::object();
    candidate["extra_headers"] = Json::object();
    candidate["extra_body"] = Json::object();
    candidate.erase("_provider_keys");
    candidate.erase("_clear_api_key");
    candidate.erase("_clear_provider_keys");
    candidate.erase("_clear_extra_headers");
    candidate.erase("_clear_extra_body");
    candidate["_secret_state"] =
        {{"provider_keys", state_array(provider_state)},
         {"extra_headers", state_array(header_state)},
         {"extra_body", state_array(body_state)}};
    return SAO_AI_EDITOR_OK;
}

void persist_secret_state(Json& candidate, const Json& secret_state) {
    candidate.erase("api_key");
    candidate.erase("provider_keys");
    candidate.erase("extra_headers");
    candidate.erase("extra_body");
    candidate.erase("_provider_keys");
    candidate.erase("_clear_api_key");
    candidate.erase("_clear_provider_keys");
    candidate.erase("_clear_extra_headers");
    candidate.erase("_clear_extra_body");

    bool has_state = false;
    if (secret_state.is_object()) {
        has_state = std::any_of(
            secret_state.begin(), secret_state.end(), [](const Json& value) {
                return value.is_array() ? !value.empty() : !value.is_null();
            });
    }
    if (has_state) {
        candidate["_secret_state"] = secret_state;
    } else {
        candidate.erase("_secret_state");
    }
}

int32_t apply_secret_mutations(SecretStore& secrets,
                               std::map<std::string, SecretMutation,
                                        std::less<>>& mutations,
                               std::vector<std::string>& applied) {
    applied.clear();
    for (auto& [key, mutation] : mutations) {
        std::string previous;
        const int32_t get_status = secrets.get(key, previous);
        if (get_status == SAO_AI_EDITOR_OK) {
            mutation.had_previous = true;
            mutation.previous = std::move(previous);
        } else if (get_status != SAO_AI_EDITOR_ERR_NOT_FOUND) {
            return get_status;
        }
        const int32_t status = mutation.erase ? secrets.erase(key)
                                              : secrets.set(key, mutation.value);
        if (status != SAO_AI_EDITOR_OK &&
            !(mutation.erase && status == SAO_AI_EDITOR_ERR_NOT_FOUND)) {
            return status;
        }
        applied.push_back(key);
    }
    return SAO_AI_EDITOR_OK;
}

int32_t rollback_secret_mutations(
    SecretStore& secrets,
    const std::map<std::string, SecretMutation, std::less<>>& mutations,
    const std::vector<std::string>& applied) {
    int32_t rollback_status = SAO_AI_EDITOR_OK;
    for (auto iterator = applied.rbegin(); iterator != applied.rend(); ++iterator) {
        const auto found = mutations.find(*iterator);
        if (found == mutations.end()) {
            continue;
        }
        int32_t status = SAO_AI_EDITOR_OK;
        if (found->second.had_previous) {
            status = secrets.set(found->second.key, found->second.previous);
        } else {
            status = secrets.erase(found->second.key);
            if (status == SAO_AI_EDITOR_ERR_NOT_FOUND) {
                status = SAO_AI_EDITOR_OK;
            }
        }
        if (rollback_status == SAO_AI_EDITOR_OK &&
            status != SAO_AI_EDITOR_OK) {
            rollback_status = status;
        }
    }
    return rollback_status;
}

bool has_plaintext_secret(const Json& settings) {
    const auto contains_plaintext = [](const Json& value) {
        if (value.is_string()) {
            const std::string text = value.get<std::string>();
            return !text.empty() && !is_secret_marker(value);
        }
        if (!value.is_object()) {
            return false;
        }
        return std::any_of(value.begin(), value.end(), [](const Json& item) {
            return item.is_string() && !item.get<std::string>().empty() &&
                   !is_secret_marker(item);
        });
    };
    if (!settings.is_object()) {
        return false;
    }
    if (contains_plaintext(settings.value("api_key", Json())) ||
        contains_plaintext(settings.value("provider_keys", Json())) ||
        contains_plaintext(settings.value("_provider_keys", Json())) ||
        (settings.value("extra_headers", Json::object()).is_object() &&
         !settings.value("extra_headers", Json::object()).empty()) ||
        (settings.value("extra_body", Json::object()).is_object() &&
         !settings.value("extra_body", Json::object()).empty())) {
        return true;
    }
    bool found = false;
    Json mcp = settings.value("mcp", Json::object());
    if (settings.contains("mcpServers") && !mcp.contains("mcpServers")) {
        mcp["mcpServers"] = settings["mcpServers"];
    }
    (void)visit_mcp_servers(
        static_cast<const Json&>(mcp),
        ConstMcpServerVisitor{[&](std::string_view, const Json& server) {
            for (const auto kind : {"env", "headers"}) {
                const Json value = server.value(kind, Json::object());
                if (!value.is_object()) {
                    continue;
                }
                for (const auto& item : value) {
                    if (!is_secret_marker(item)) {
                        found = true;
                        return SAO_AI_EDITOR_OK;
                    }
                }
            }
            return SAO_AI_EDITOR_OK;
        }});
    return found;
}

int32_t migrate_scope_plaintext_secrets(
    const ScopeStore& scopes, SecretStore& secrets, std::string_view scope,
    std::string_view plugin_id) {
    Json root;
    int32_t status = scopes.load_scope_config(scope, plugin_id, root);
    if (status != SAO_AI_EDITOR_OK || !root.contains("ai_editor") ||
        !root["ai_editor"].is_object() ||
        !has_plaintext_secret(root["ai_editor"])) {
        return status;
    }

    const Json stored = root["ai_editor"];
    Json candidate = stored;
    if ((!candidate.contains("provider_keys") ||
         !candidate["provider_keys"].is_object()) &&
        candidate.contains("_provider_keys") &&
        candidate["_provider_keys"].is_object()) {
        candidate["provider_keys"] = candidate["_provider_keys"];
    }
    Json normalized = normalize_settings(candidate);

    std::map<std::string, SecretMutation, std::less<>> mutations;
    status = plan_secret_mutations(normalized, stored, Json::object(),
                                   mutations);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    persist_secret_state(candidate, normalized["_secret_state"]);
    Json mcp = candidate.value("mcp", Json::object());
    if (candidate.contains("mcpServers") && !mcp.contains("mcpServers")) {
        mcp["mcpServers"] = candidate["mcpServers"];
    }
    candidate.erase("mcpServers");
    status = plan_mcp_secret_mutations(
        mcp, secrets, mutations, Json::object());
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    if (candidate.contains("mcp") || !mcp.empty()) {
        candidate["mcp"] = std::move(mcp);
    }
    strip_named_secrets(candidate);

    std::vector<std::string> applied;
    status = apply_secret_mutations(secrets, mutations, applied);
    if (status != SAO_AI_EDITOR_OK) {
        const int32_t rollback_status =
            rollback_secret_mutations(secrets, mutations, applied);
        return rollback_status != SAO_AI_EDITOR_OK ? rollback_status : status;
    }

    Json updated_root = root;
    updated_root["ai_editor"] = std::move(candidate);
    status = scopes.save_scope_config(scope, plugin_id, updated_root);
    if (status != SAO_AI_EDITOR_OK) {
        const int32_t rollback_status =
            rollback_secret_mutations(secrets, mutations, applied);
        if (rollback_status != SAO_AI_EDITOR_OK) {
            return rollback_status;
        }
    }
    return status;
}

int32_t migrate_plaintext_secrets(const ScopeStore& scopes,
                                  SecretStore& secrets) {
    const Json described_scopes = scopes.describe_scopes();
    if (!described_scopes.is_array()) {
        return SAO_AI_EDITOR_ERR_CONFIG_MISSING;
    }
    for (const auto& entry : described_scopes) {
        if (!entry.is_object() || !entry.contains("scope") ||
            !entry["scope"].is_string()) {
            continue;
        }
        const std::string scope = entry["scope"].get<std::string>();
        const std::string plugin_id =
            entry.value("pluginId", std::string{});
        const int32_t status = migrate_scope_plaintext_secrets(
            scopes, secrets, scope, plugin_id);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
    }
    return SAO_AI_EDITOR_OK;
}

Json secret_states_from_stored(const Json& settings) {
    const std::string provider = string_value(
        settings.value("provider", Json("openai")));
    Json provider_flags = Json::object();
    const auto providers = state_values(settings, "provider_keys");
    for (const auto& item : providers) {
        provider_flags[item] = true;
    }
    Json header_flags = Json::object();
    for (const auto& item : state_values(settings, "extra_headers")) {
        header_flags[item] = true;
    }
    Json body_flags = Json::object();
    for (const auto& item : state_values(settings, "extra_body")) {
        body_flags[item] = true;
    }
    Json mcp_flags;
    Json mcp_keys;
    Json mcp = settings.value("mcp", Json::object());
    redact_mcp_secrets(mcp, &mcp_flags, &mcp_keys);
    return {{"activeProvider", provider},
            {"apiKey", provider_flags.value(provider, false)},
            {"providerKeys", std::move(provider_flags)},
            {"extraHeaders", std::move(header_flags)},
            {"extraBody", std::move(body_flags)},
            {"mcpServers", mcp_flags},
            {"mcp_servers", mcp_flags},
            {"mcpServerKeys", mcp_keys},
            {"mcp_server_keys", std::move(mcp_keys)}};
}

Json find_provider(const Json& registry, std::string_view id) {
    if (!registry.is_array()) {
        return Json();
    }
    for (const auto& entry : registry) {
        if (!entry.is_object()) {
            continue;
        }
        const std::string entry_id = string_value(entry.value("id", Json()));
        if (entry_id == id) {
            return entry;
        }
    }
    return Json();
}

bool has_nonempty_string(const Json& object, std::string_view key) {
    const std::string owned_key(key);
    return object.is_object() && object.contains(owned_key) &&
           object[owned_key].is_string() &&
           !trim_ascii(object[owned_key].get<std::string>()).empty();
}

void inject_if_absent(Json& request, std::string_view camel_key,
                      std::string_view snake_key, const Json& value) {
    const std::string camel(camel_key);
    const std::string snake(snake_key);
    if (request.contains(camel) || (!snake.empty() && request.contains(snake))) {
        return;
    }
    request[camel] = value;
}

bool has_system_message(const Json& request) {
    if (!request.contains("messages") || !request["messages"].is_array()) {
        return false;
    }
    return std::any_of(request["messages"].begin(), request["messages"].end(),
                       [](const Json& message) {
                           return message.is_object() &&
                                  lower_ascii(string_value(
                                      message.value("role", Json()))) == "system";
                       });
}

void validate_chat_request(const Json& request, Json& errors) {
    if (!request.is_object()) {
        errors.push_back(validation_error(
            "request", "TYPE", "chat request must be an object", "object",
            request));
        return;
    }
    for (const auto key : {"mode", "approval", "model", "apiKey",
                           "api_key", "providerId", "conversationId"}) {
        validate_string_field(request, key, key, errors,
                              std::string_view(key) != "providerId");
    }
    validate_object_field(request, "permissions", "permissions", errors);
    validate_object_field(request, "retry", "retry", errors);
    validate_array_field(request, "messages", "messages", errors);
    validate_array_field(request, "tools", "tools", errors);
    validate_bool_field(request, "stream", "stream", errors);

    if (request.contains("provider")) {
        const Json& provider = request["provider"];
        if (!provider.is_string() && !provider.is_object()) {
            errors.push_back(validation_error(
                "provider", "TYPE", "provider must be an ID or object",
                "string|object", provider));
        } else if (provider.is_string() &&
                   trim_ascii(provider.get<std::string>()).empty()) {
            errors.push_back(validation_error(
                "provider", "REQUIRED", "provider ID must not be empty",
                "non-empty string", provider));
        } else if (provider.is_object()) {
            for (const auto key : {"id", "type", "provider_type", "endpoint",
                                   "base_url", "model", "apiKey", "api_key",
                                   "apiKeyEnv"}) {
                validate_string_field(provider, key,
                                      std::string("provider.") + key, errors);
            }
            validate_object_field(provider, "extra_headers",
                                  "provider.extra_headers", errors);
            validate_object_field(provider, "extra_body",
                                  "provider.extra_body", errors);
            validate_object_field(provider, "retry", "provider.retry",
                                  errors);
        }
    }

    if (request.contains("messages") && request["messages"].is_array()) {
        for (size_t index = 0; index < request["messages"].size(); ++index) {
            const Json& message = request["messages"][index];
            const std::string path = "messages." + std::to_string(index);
            if (!message.is_object()) {
                errors.push_back(validation_error(
                    path, "TYPE", "message must be an object", "object",
                    message));
                continue;
            }
            validate_string_field(message, "role", path + ".role", errors,
                                  false);
            if (message.contains("content") &&
                !message["content"].is_string() &&
                !message["content"].is_array() &&
                !message["content"].is_null()) {
                errors.push_back(validation_error(
                    path + ".content", "TYPE",
                    "message content must be text, parts, or null",
                    "string|array|null", message["content"]));
            }
        }
    }

    for (const auto [camel, snake, minimum, maximum] :
         std::array<std::tuple<std::string_view, std::string_view, double,
                               double>, 3>{
             std::tuple{"temperature", "temperature", 0.0, 2.0},
             std::tuple{"topP", "top_p", 0.0, 1.0},
             std::tuple{"frequencyPenalty", "frequency_penalty", -2.0,
                        2.0}}) {
        const std::array<std::string_view, 2> keys{camel, snake};
        for (size_t index = 0; index < keys.size(); ++index) {
            const auto key = keys[index];
            if (index != 0 && key == keys[0]) {
                continue;
            }
            const std::string owned_key(key);
            if (request.contains(owned_key) &&
                !valid_finite_number(request[owned_key], minimum, maximum)) {
                errors.push_back(validation_error(
                    key, "TYPE_OR_RANGE",
                    "chat number is outside the accepted range",
                    "finite number", request[owned_key]));
            }
        }
    }
    for (const auto key : {"presencePenalty", "presence_penalty"}) {
        if (request.contains(key) &&
            !valid_finite_number(request[key], -2.0, 2.0)) {
            errors.push_back(validation_error(
                key, "TYPE_OR_RANGE",
                "chat number is outside the accepted range",
                "finite number", request[key]));
        }
    }

    for (const auto [camel, snake, minimum, maximum] :
         std::array<std::tuple<std::string_view, std::string_view, int64_t,
                               int64_t>, 4>{
             std::tuple{"maxTokens", "max_tokens", 0, INT32_MAX},
             std::tuple{"maxInputTokens", "max_input_tokens", 0, INT32_MAX},
             std::tuple{"maxOutputTokens", "max_output_tokens", 0, INT32_MAX},
             std::tuple{"timeoutMs", "timeout_ms", 1, 3'600'000}}) {
        for (const auto key : {camel, snake}) {
            const std::string owned_key(key);
            if (request.contains(owned_key) &&
                !valid_json_integer(request[owned_key], minimum, maximum)) {
                errors.push_back(validation_error(
                    key, "TYPE_OR_RANGE",
                    "chat integer is outside the accepted range", "integer",
                    request[owned_key]));
            }
        }
        const std::string camel_key(camel);
        const std::string snake_key(snake);
        if (camel != snake && request.contains(camel_key) &&
            request.contains(snake_key) &&
            request[camel_key] != request[snake_key]) {
            errors.push_back(validation_error(
                camel, "CONFLICT", "camelCase and snake_case values differ"));
        }
    }

    if (request.contains("stop")) {
        const Json& stop = request["stop"];
        if (!stop.is_string() && !stop.is_array()) {
            errors.push_back(validation_error(
                "stop", "TYPE", "stop must be text or an array of text",
                "string|array", stop));
        } else if (stop.is_array()) {
            for (size_t index = 0; index < stop.size(); ++index) {
                if (!stop[index].is_string()) {
                    errors.push_back(validation_error(
                        "stop." + std::to_string(index), "TYPE",
                        "stop sequence must be text", "string", stop[index]));
                }
            }
        }
    }
}

int32_t validate_runtime_request(const Json& request,
                                 Json& prepared_request) {
    Json errors = Json::array();
    if (!request.is_object()) {
        errors.push_back(validation_error(
            "request", "TYPE", "runtime request must be an object", "object",
            request));
    } else {
        validate_string_field(request, "mode", "mode", errors);
        validate_string_field(request, "approval", "approval", errors);
        validate_object_field(request, "permissions", "permissions", errors);
    }
    if (!errors.empty()) {
        return invalid_argument(prepared_request, std::move(errors));
    }
    return SAO_AI_EDITOR_OK;
}

}  // namespace

Json AiEditorSettings::defaults() { return defaults_impl(); }

Json AiEditorSettings::describe(const ScopeStore& scopes) {
    return {{"schema", kSchema},
            {"version", kSchemaVersion},
            {"schemaVersion", kSchemaVersion},
            {"sections", section_list()},
            {"fields", fields_schema()},
            {"defaults", redact_settings(defaults_impl())},
            {"providerPresets", provider_presets()},
            {"scopes", scopes.describe_scopes()}};
}

int32_t AiEditorSettings::load(const ScopeStore& scopes,
                               SecretStore& secrets,
                               const Json& params,
                               Json& result) {
    if (!params.is_object()) {
        return invalid_argument(
            result, Json::array({validation_error(
                        "params", "TYPE", "params must be an object",
                        "object", params)}));
    }
    Json errors = Json::array();
    validate_string_field(params, "scope", "scope", errors);
    validate_string_field(params, "pluginId", "pluginId", errors);
    validate_bool_field(params, "merged", "merged", errors);
    if (!errors.empty()) {
        return invalid_argument(result, std::move(errors));
    }
    const std::string requested_scope = trim_ascii(string_value(
        params.value("scope", Json())));
    if (!requested_scope.empty() && requested_scope != "merged" &&
        !one_of(requested_scope, {"system", "workspace", "plugin"})) {
        return invalid_argument(
            result, Json::array({validation_error(
                        "scope", "VALUE", "unknown settings scope",
                        "merged|system|workspace|plugin")}));
    }
    if (requested_scope == "plugin" &&
        trim_ascii(string_value(params.value("pluginId", Json()))).empty()) {
        return invalid_argument(
            result, Json::array({validation_error(
                        "pluginId", "REQUIRED",
                        "plugin scope requires pluginId", "non-empty string")}));
    }
    const int32_t migration_status =
        migrate_plaintext_secrets(scopes, secrets);
    if (migration_status != SAO_AI_EDITOR_OK) {
        return migration_status;
    }
    Json root;
    std::string scope;
    std::string plugin_id;
    const int32_t status = load_selected_config(
        scopes, params, root, scope, plugin_id);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    Json hydrated;
    Json secret_states;
    const Json stored = root.contains("ai_editor") && root["ai_editor"].is_object()
                            ? root["ai_editor"]
                            : Json::object();
    const int32_t hydrate_status =
        hydrate_settings(stored, secrets, hydrated, secret_states);
    if (hydrate_status != SAO_AI_EDITOR_OK) {
        return hydrate_status;
    }
    Json redacted = redact_settings(hydrated);
    Json overrides = scope == "merged" ? Json::object()
                                        : redact_scope_overrides(stored);
    std::vector<SettingsScopeLayer> layers;
    const int32_t layers_status = load_settings_scope_layers(scopes, layers);
    if (layers_status != SAO_AI_EDITOR_OK)
        return layers_status;

    Json effective_stored = Json::object();
    Json inherited_stored = Json::object();
    Json sources = Json::object();
    overlay_source_tree(sources, defaults_impl(), "default");
    for (const auto& layer : layers) {
        merge_patch(effective_stored, layer.settings);
        overlay_source_tree(sources, layer.settings, layer.source);
        const bool selected = scope != "merged" && layer.scope == scope &&
                              (scope != "plugin" || layer.plugin_id == plugin_id);
        if (!selected)
            merge_patch(inherited_stored, layer.settings);
    }

    Json effective_hydrated;
    Json effective_secret_states;
    const int32_t effective_status = hydrate_settings(
        effective_stored, secrets, effective_hydrated, effective_secret_states);
    if (effective_status != SAO_AI_EDITOR_OK)
        return effective_status;
    Json effective = redact_settings(effective_hydrated);

    Json inherited = Json::object();
    Json inherited_secret_states = Json::object();
    if (scope != "merged") {
        Json inherited_hydrated;
        const int32_t inherited_status = hydrate_settings(
            inherited_stored, secrets, inherited_hydrated, inherited_secret_states);
        if (inherited_status != SAO_AI_EDITOR_OK)
            return inherited_status;
        inherited = redact_settings(inherited_hydrated);
    }
    result = {{"schema", kSchema},
              {"version", kSchemaVersion},
              {"scope", scope},
              {"pluginId", plugin_id},
              {"overrides", overrides},
              {"scopeOverrides", overrides},
              {"scope_overrides", std::move(overrides)},
              {"settings", redacted},
              {"data", std::move(redacted)},
              {"effective", effective},
              {"effectiveValues", effective},
              {"effective_values", std::move(effective)},
              {"inherited", std::move(inherited)},
              {"sources", std::move(sources)},
              {"secretStates", std::move(secret_states)},
              {"effectiveSecretStates", std::move(effective_secret_states)},
              {"inheritedSecretStates", std::move(inherited_secret_states)}};
    return SAO_AI_EDITOR_OK;
}

int32_t AiEditorSettings::save(const ScopeStore& scopes,
                               SecretStore& secrets,
                               const Json& params,
                               Json& result) {
    if (!params.is_object()) {
        return invalid_argument(
            result, Json::array({validation_error(
                        "params", "TYPE", "params must be an object",
                        "object", params)}));
    }
    Json errors = Json::array();
    validate_string_field(params, "scope", "scope", errors, false);
    validate_string_field(params, "pluginId", "pluginId", errors);
    if (!errors.empty()) {
        return invalid_argument(result, std::move(errors));
    }
    const std::string scope = trim_ascii(string_value(
        params.value("scope", Json("workspace")), "workspace"));
    const std::string plugin_id = trim_ascii(string_value(
        params.value("pluginId", Json())));
    if (scope == "merged" ||
        !one_of(scope, {"system", "workspace", "plugin"}) ||
        (scope == "plugin" && plugin_id.empty())) {
        return invalid_argument(
            result, Json::array({validation_error(
                        "scope", "VALUE",
                        "scope must select a writable system, workspace, or plugin scope",
                        "system|workspace|plugin")}));
    }

    Json patch;
    if (params.contains("changes")) {
        patch = params["changes"];
    } else if (params.contains("patch")) {
        patch = params["patch"];
    } else if (params.contains("settings")) {
        patch = params["settings"];
    } else if (params.contains("data")) {
        patch = params["data"];
    } else {
        patch = Json::object();
    }
    validate_settings_patch(patch, errors);
    validate_secret_updates(params, errors);
    std::vector<std::string> reset_keys;
    (void)parse_reset_keys(params, reset_keys, errors);
    if (!errors.empty()) {
        return invalid_argument(result, std::move(errors));
    }

    Json root;
    int32_t status = scopes.load_scope_config(scope, plugin_id, root);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    Json candidate = root.contains("ai_editor") && root["ai_editor"].is_object()
                         ? root["ai_editor"]
                         : Json::object();
    apply_settings_patch(candidate, patch);
    for (const auto& key : reset_keys) {
        (void)erase_dotted_path(candidate, key);
    }

    Json normalized = normalize_settings(candidate);

    std::map<std::string, SecretMutation, std::less<>> mutations;
    status = plan_secret_mutations(normalized, patch, params, mutations);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    persist_secret_state(candidate, normalized["_secret_state"]);

    Json secret_updates = Json::object();
    if (params.contains("secretUpdates")) {
        secret_updates = params["secretUpdates"];
    } else if (params.contains("secret_updates")) {
        secret_updates = params["secret_updates"];
    }
    Json mcp = candidate.value("mcp", Json::object());
    if (candidate.contains("mcpServers") && !mcp.contains("mcpServers")) {
        mcp["mcpServers"] = candidate["mcpServers"];
    }
    candidate.erase("mcpServers");
    status = plan_mcp_secret_mutations(mcp, secrets, mutations,
                                       secret_updates);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    if (candidate.contains("mcp") || !mcp.empty()) {
        candidate["mcp"] = std::move(mcp);
    }
    strip_named_secrets(candidate);

    std::vector<std::string> applied;
    status = apply_secret_mutations(secrets, mutations, applied);
    if (status != SAO_AI_EDITOR_OK) {
        const int32_t rollback_status =
            rollback_secret_mutations(secrets, mutations, applied);
        return rollback_status != SAO_AI_EDITOR_OK ? rollback_status : status;
    }

    Json updated_root = root;
    updated_root["ai_editor"] = candidate;
    status = scopes.save_scope_config(scope, plugin_id, updated_root);
    if (status != SAO_AI_EDITOR_OK) {
        const int32_t rollback_status =
            rollback_secret_mutations(secrets, mutations, applied);
        return rollback_status != SAO_AI_EDITOR_OK ? rollback_status : status;
    }

    Json redacted = redact_settings(normalize_settings(candidate));
    Json overrides = redact_scope_overrides(candidate);
    result = {{"schema", kSchema},
              {"version", kSchemaVersion},
              {"scope", scope},
              {"pluginId", plugin_id},
              {"saved", true},
              {"resetKeys", reset_keys},
              {"reset_keys", reset_keys},
              {"overrides", overrides},
              {"scopeOverrides", overrides},
              {"scope_overrides", std::move(overrides)},
              {"settings", redacted},
              {"data", std::move(redacted)},
              {"secretStates", secret_states_from_stored(candidate)}};
    return SAO_AI_EDITOR_OK;
}

int32_t AiEditorSettings::prepare_runtime_request(
    const ScopeStore& scopes, const Json& request, Json& prepared_request) {
    const int32_t validation_status =
        validate_runtime_request(request, prepared_request);
    if (validation_status != SAO_AI_EDITOR_OK) {
        return validation_status;
    }
    Json root;
    const int32_t load_status = scopes.load_merged_config(root);
    if (load_status != SAO_AI_EDITOR_OK) {
        return load_status;
    }
    const Json stored = root.contains("ai_editor") &&
                                root["ai_editor"].is_object()
                            ? root["ai_editor"]
                            : Json::object();
    const Json settings = normalize_settings(stored);
    prepared_request = request;
    if (!prepared_request.contains("mode")) {
        prepared_request["mode"] = settings["mode"];
    }
    if (!prepared_request.contains("approval")) {
        prepared_request["approval"] = settings["approval"];
    }
    if (!prepared_request.contains("permissions")) {
        prepared_request["permissions"] = settings["permissions"];
    }
    return SAO_AI_EDITOR_OK;
}

int32_t AiEditorSettings::prepare_chat_request(
    const ScopeStore& scopes, SecretStore& secrets, const Json& request,
    Json& prepared_request, Json& provider, std::string& api_key) {
    Json errors = Json::array();
    validate_chat_request(request, errors);
    if (!errors.empty()) {
        return invalid_argument(prepared_request, std::move(errors));
    }
    const int32_t migration_status =
        migrate_plaintext_secrets(scopes, secrets);
    if (migration_status != SAO_AI_EDITOR_OK) {
        return migration_status;
    }
    Json root;
    const int32_t load_status = scopes.load_merged_config(root);
    if (load_status != SAO_AI_EDITOR_OK) {
        return load_status;
    }
    const Json stored = root.contains("ai_editor") && root["ai_editor"].is_object()
                            ? root["ai_editor"]
                            : Json::object();
    Json settings;
    Json secret_states;
    const int32_t hydrate_status =
        hydrate_settings(stored, secrets, settings, secret_states);
    if (hydrate_status != SAO_AI_EDITOR_OK) {
        return hydrate_status;
    }

    prepared_request = request;
    if (prepared_request.contains("timeout_ms")) {
        prepared_request["timeoutMs"] = prepared_request["timeout_ms"];
        prepared_request.erase("timeout_ms");
    }
    Json registry;
    const int32_t registry_status =
        scopes.load_registry("providers", Json::array(), registry);
    if (registry_status != SAO_AI_EDITOR_OK) {
        return registry_status;
    }
    auto registry_secret_candidates = provider_candidates(settings);
    if (registry.is_array()) {
        for (const auto& entry : registry) {
            if (!entry.is_object()) {
                continue;
            }
            const std::string entry_id = trim_ascii(string_value(
                entry.value("id", Json())));
            if (!entry_id.empty()) {
                registry_secret_candidates.insert(entry_id);
            }
        }
    }
    const bool explicit_provider = request.contains("provider");
    const bool explicit_provider_id = request.contains("providerId");
    const bool explicit_inline_provider =
        explicit_provider && request["provider"].is_object();
    bool provider_from_registry = false;
    if (explicit_provider && request["provider"].is_string() &&
        explicit_provider_id &&
        trim_ascii(request["provider"].get<std::string>()) !=
            trim_ascii(request["providerId"].get<std::string>())) {
        return invalid_argument(
            prepared_request, Json::array({validation_error(
                                  "providerId", "CONFLICT",
                                  "provider and providerId must name the same provider")}));
    }
    if (explicit_inline_provider) {
        provider = request["provider"];
    } else {
        const std::string provider_id = explicit_provider
            ? trim_ascii(string_value(request["provider"]))
            : explicit_provider_id
                ? trim_ascii(string_value(request["providerId"]))
                : settings["provider"].get<std::string>();
        if (provider_id.empty()) {
            return invalid_argument(
                prepared_request, Json::array({validation_error(
                                      "providerId", "REQUIRED",
                                      "provider ID must not be empty")}));
        }
        provider = find_provider(registry, provider_id);
        if (provider.is_object()) {
            provider_from_registry = true;
        } else {
            provider = Json::object({{"id", provider_id}, {"type", provider_id}});
        }
    }
    if (!provider.is_object()) {
        return invalid_argument(
            prepared_request, Json::array({validation_error(
                                  "provider", "TYPE",
                                  "provider must resolve to an object")}));
    }

    const std::string active_provider = settings["provider"].get<std::string>();
    std::string provider_id = trim_ascii(string_value(provider.value("id", Json())));
    if (provider_id.empty()) {
        provider_id = trim_ascii(string_value(provider.value("type", Json())));
    }
    if (provider_id.empty()) {
        if (explicit_provider || explicit_provider_id) {
            return invalid_argument(
                prepared_request, Json::array({validation_error(
                                      "provider.id", "REQUIRED",
                                      "explicit provider object requires id or type")}));
        }
        provider_id = active_provider;
    }
    provider["id"] = provider_id;
    std::string provider_type = lower_ascii(trim_ascii(
        string_value(provider.value("type", Json()), provider_id)));
    if (provider_type.empty()) {
        provider_type = provider_id;
    }
    const Json presets = provider_presets();
    const std::string preset_id = presets.contains(provider_id)
                                      ? provider_id
                                      : (presets.contains(provider_type)
                                             ? provider_type
                                             : std::string{});
    const Json preset = presets.value(preset_id, Json::object());

    const bool explicit_object_has_id =
        explicit_inline_provider &&
        has_nonempty_string(request["provider"], "id");
    const bool provider_identity_known =
        (!explicit_provider && !explicit_provider_id) ||
        explicit_provider_id || request["provider"].is_string() ||
        explicit_object_has_id;
    const bool uses_active_settings =
        (!explicit_provider && !explicit_provider_id) ||
        (provider_identity_known && provider_id == active_provider);
    if (uses_active_settings) {
        if (!has_nonempty_string(provider, "endpoint") &&
            !has_nonempty_string(provider, "base_url")) {
            const std::string base_url = trim_ascii(
                settings["base_url"].get<std::string>());
            provider["base_url"] = !base_url.empty()
                ? base_url
                : string_value(preset.value("base_url", Json()));
        }
        if (!has_nonempty_string(provider, "model")) {
            const std::string model = trim_ascii(settings["model"].get<std::string>());
            provider["model"] = !model.empty()
                ? model
                : string_value(preset.value("model", Json()));
        }
        if (!provider.contains("extra_headers")) {
            provider["extra_headers"] = settings["extra_headers"];
        }
        if (!provider.contains("extra_body")) {
            provider["extra_body"] = settings["extra_body"];
        }
        if (!provider.contains("transport")) {
            provider["transport"] = settings["transport"];
        }
    }
    if (prepared_request.contains("transport")) {
        provider["transport"] = prepared_request["transport"];
    }
    if (provider_type == "deepseek" || provider_type == "ollama" ||
        provider_type == "custom") {
        provider_type = "openai";
    }
    provider["type"] = provider_type;

    if (!prepared_request.contains("model") ||
        !has_nonempty_string(prepared_request, "model")) {
        const std::string model = trim_ascii(string_value(provider.value("model", Json())));
        if (!model.empty()) {
            prepared_request["model"] = model;
        }
    }
    inject_if_absent(prepared_request, "temperature", "temperature",
                     settings["temperature"]);
    inject_if_absent(prepared_request, "maxTokens", "max_tokens",
                     settings["max_tokens"]);
    inject_if_absent(prepared_request, "topP", "top_p", settings["top_p"]);
    inject_if_absent(prepared_request, "frequencyPenalty", "frequency_penalty",
                     settings["frequency_penalty"]);
    inject_if_absent(prepared_request, "presencePenalty", "presence_penalty",
                     settings["presence_penalty"]);
    inject_if_absent(prepared_request, "stop", "stop", settings["stop"]);
    inject_if_absent(prepared_request, "maxInputTokens", "max_input_tokens",
                     settings["max_input_tokens"]);
    inject_if_absent(prepared_request, "maxOutputTokens", "max_output_tokens",
                     settings["max_output_tokens"]);
    if (!prepared_request.contains("timeoutMs") &&
        !prepared_request.contains("timeout_ms")) {
        const int64_t seconds = integer_value(settings["timeout"], 180);
        prepared_request["timeoutMs"] =
            std::clamp<int64_t>(seconds, 1, UINT32_MAX / 1000) * 1000;
    }
    if (!prepared_request.contains("mode")) {
        prepared_request["mode"] = settings["mode"];
    }
    if (!prepared_request.contains("approval")) {
        prepared_request["approval"] = settings["approval"];
    }
    if (!prepared_request.contains("permissions")) {
        prepared_request["permissions"] = settings["permissions"];
    }
    const std::string system_prompt = settings["system_prompt"].get<std::string>();
    if (!system_prompt.empty() && !has_system_message(prepared_request)) {
        if (!prepared_request.contains("messages") ||
            !prepared_request["messages"].is_array()) {
            prepared_request["messages"] = Json::array();
        }
        prepared_request["messages"].insert(
            prepared_request["messages"].begin(),
            Json{{"role", "system"}, {"content", system_prompt}});
    }

    api_key = string_value(request.value("apiKey", Json()));
    if (api_key.empty()) {
        api_key = string_value(request.value("api_key", Json()));
    }
    if (api_key.empty()) {
        api_key = string_value(provider.value("apiKey", Json()));
    }
    if (api_key.empty()) {
        api_key = string_value(provider.value("api_key", Json()));
    }
    if (api_key.empty() && provider_from_registry) {
        std::string protected_key;
        const int32_t protected_status = read_secret_with_fallback(
            secrets, provider_id, "api-key", registry_secret_candidates,
            protected_key);
        if (protected_status == SAO_AI_EDITOR_OK) {
            api_key = std::move(protected_key);
        } else if (protected_status != SAO_AI_EDITOR_ERR_NOT_FOUND) {
            return protected_status;
        }
    }
    const bool allow_inherited_key =
        !explicit_inline_provider && provider_identity_known;
    if (api_key.empty() && allow_inherited_key &&
        settings["provider_keys"].is_object()) {
        if (settings["provider_keys"].contains(provider_id) &&
            settings["provider_keys"][provider_id].is_string()) {
            api_key = settings["provider_keys"][provider_id].get<std::string>();
        }
    }
    if (api_key.empty() && !explicit_inline_provider && uses_active_settings) {
        api_key = settings["api_key"].get<std::string>();
    }
    return SAO_AI_EDITOR_OK;
}

}  // namespace sao::ai_editor::native
