#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "../src/ai_editor_settings.h"
#include "../src/native_secret_store.h"
#include "../src/scope_store.h"
#include "../src/sha256_helper.h"

namespace {

using Json = nlohmann::json;
using sao::ai_editor::native::AiEditorSettings;
using sao::ai_editor::native::ScopeStore;
using sao::ai_editor::native::SecretStore;
using sao::ai_editor::native::sha256_hex;

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        static std::atomic<uint64_t> sequence{0};
        wchar_t root[MAX_PATH]{};
        REQUIRE(GetTempPathW(MAX_PATH, root) > 0);
        path_ = std::filesystem::path(root) /
                (L"sao-ai-editor-settings-contract-" +
                 std::to_wstring(GetCurrentProcessId()) + L"-" +
                 std::to_wstring(GetTickCount64()) + L"-" +
                 std::to_wstring(sequence.fetch_add(1)));
        REQUIRE(std::filesystem::create_directories(path_));
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

std::string utf8_path(const std::filesystem::path& path) {
    const std::wstring wide = path.native();
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    REQUIRE(required > 0);
    std::string result(static_cast<size_t>(required), '\0');
    REQUIRE(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                                static_cast<int>(wide.size()), result.data(),
                                required, nullptr, nullptr) == required);
    return result;
}

Json read_json(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    const std::string text{std::istreambuf_iterator<char>{input},
                           std::istreambuf_iterator<char>{}};
    return Json::parse(text);
}

std::string stable_secret_key(std::string_view owner, std::string_view kind,
                              std::string_view prefix) {
    return std::string(prefix) + "/v2-" + sha256_hex(owner) + "/v2-" +
           sha256_hex(kind);
}

const Json* find_field(const Json& described, std::string_view key) {
    if (!described.contains("fields") || !described["fields"].is_array()) {
        return nullptr;
    }
    for (const auto& field : described["fields"]) {
        if (field.is_object() && field.value("key", std::string{}) == key) {
            return &field;
        }
    }
    return nullptr;
}

const Json* find_server(const Json& group, std::string_view id) {
    if (group.is_object()) {
        const std::string key(id);
        return group.contains(key) && group[key].is_object() ? &group[key]
                                                              : nullptr;
    }
    if (group.is_array()) {
        for (const auto& server : group) {
            if (server.is_object() &&
                server.value("id", std::string{}) == id) {
                return &server;
            }
        }
    }
    return nullptr;
}

class SettingsFixture final {
public:
    SettingsFixture() {
        workspace_ = temporary_.path() / L"workspace";
        system_ = temporary_.path() / L"system";
        plugin_ = temporary_.path() / L"plugin";
        REQUIRE(std::filesystem::create_directories(workspace_));
        REQUIRE(std::filesystem::create_directories(system_));
        REQUIRE(std::filesystem::create_directories(plugin_));
        const Json plugins = Json::array(
            {{{"id", "fixture"}, {"path", utf8_path(plugin_)}}});
        REQUIRE(scopes_.initialize(utf8_path(workspace_), utf8_path(system_),
                                   plugins.dump()) == SAO_AI_EDITOR_OK);
        secrets_ = std::make_unique<SecretStore>(scopes_.secret_vault_path());
    }

    ScopeStore& scopes() noexcept { return scopes_; }
    SecretStore& secrets() noexcept { return *secrets_; }

    std::filesystem::path config_path(std::string_view scope) const {
        if (scope == "system") {
            return system_ / L"config.json";
        }
        if (scope == "plugin") {
            return plugin_ / L".sao" / L"config.json";
        }
        return workspace_ / L".sao" / L"config.json";
    }

private:
    TemporaryDirectory temporary_;
    std::filesystem::path workspace_;
    std::filesystem::path system_;
    std::filesystem::path plugin_;
    ScopeStore scopes_;
    std::unique_ptr<SecretStore> secrets_;
};

}  // namespace

TEST_CASE("AI Editor settings describe and custom provider metadata stay in parity",
          "[plugins][ai_editor][settings][contract]") {
    SettingsFixture fixture;
    const Json described = AiEditorSettings::describe(fixture.scopes());

    REQUIRE(described["defaults"]["layout"]["sidebarVisible"] == true);
    REQUIRE(described["defaults"]["layout"]["activeBottomTab"] ==
            "terminal");
    REQUIRE(described["defaults"]["mcp"]["enabled"] == true);
    REQUIRE(described["defaults"]["mcp"]["servers"].is_array());

    const Json* provider = find_field(described, "provider");
    REQUIRE(provider != nullptr);
    REQUIRE((*provider)["allowCustom"] == true);
    REQUIRE((*provider)["optionsSource"] == "providers");
    REQUIRE((*provider)["registry"] == "providers");
    for (const auto id : {"openai", "anthropic", "deepseek", "ollama",
                          "custom", "gemini"}) {
        const Json* key = find_field(
            described, std::string("provider_keys.") + id);
        REQUIRE(key != nullptr);
        REQUIRE((*key)["sensitive"] == true);
        REQUIRE((*key)["providerId"] == id);
    }
    REQUIRE(find_field(described, "layout.chatVisible") != nullptr);
    REQUIRE(find_field(described, "mcp.enabled") != nullptr);

    REQUIRE(fixture.scopes().save_scope_config(
                "workspace", "",
                {{"rootSibling", {{"preserve", true}}},
                 {"ai_editor", {{"unknownSetting", {{"x", 1}}}}}}) ==
            SAO_AI_EDITOR_OK);

    Json saved;
    REQUIRE(AiEditorSettings::save(
                fixture.scopes(), fixture.secrets(),
                {{"scope", "workspace"},
                 {"changes", {{"provider", "Acme.Registry"}}}},
                saved) == SAO_AI_EDITOR_OK);
    REQUIRE(saved["settings"]["provider"] == "Acme.Registry");
    REQUIRE(saved["overrides"]["provider"] == "Acme.Registry");
    REQUIRE(saved["overrides"]["unknownSetting"]["x"] == 1);
    REQUIRE_FALSE(saved["overrides"].contains("temperature"));

    const Json raw = read_json(fixture.config_path("workspace"));
    REQUIRE(raw["rootSibling"]["preserve"] == true);
    REQUIRE(raw["ai_editor"]["provider"] == "Acme.Registry");
    REQUIRE(raw["ai_editor"]["unknownSetting"]["x"] == 1);
    REQUIRE_FALSE(raw["ai_editor"].contains("temperature"));
    REQUIRE_FALSE(raw["ai_editor"].contains("api_key"));
    REQUIRE_FALSE(raw["ai_editor"].contains("provider_keys"));
    REQUIRE_FALSE(raw["ai_editor"].contains("extra_headers"));
    REQUIRE_FALSE(raw["ai_editor"].contains("extra_body"));
    REQUIRE_FALSE(raw["ai_editor"].contains("_secret_state"));

    Json loaded;
    REQUIRE(AiEditorSettings::load(
                fixture.scopes(), fixture.secrets(),
                {{"scope", "workspace"}}, loaded) == SAO_AI_EDITOR_OK);
    REQUIRE(loaded["settings"]["provider"] == "Acme.Registry");
    REQUIRE(loaded["overrides"]["provider"] == "Acme.Registry");
    REQUIRE_FALSE(loaded["overrides"].contains("temperature"));

    Json invalid;
    REQUIRE(AiEditorSettings::save(
                fixture.scopes(), fixture.secrets(),
                {{"scope", "workspace"},
                 {"changes", {{"temperature", "hot"}}}},
                invalid) == SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
    REQUIRE(invalid["code"] == "INVALID_ARGUMENT");
    REQUIRE(invalid["validationErrors"].is_array());
    REQUIRE_FALSE(invalid["validationErrors"].empty());
}

TEST_CASE("AI Editor settings dotted updates and resets preserve inheritance",
          "[plugins][ai_editor][settings][reset]") {
    SettingsFixture fixture;
    REQUIRE(fixture.scopes().save_scope_config(
                "system", "",
                {{"systemRoot", 1},
                 {"ai_editor",
                  {{"editor", {{"tabSize", 8}}},
                   {"layout", {{"chatVisible", false}}},
                   {"systemUnknown", "keep"}}}}) == SAO_AI_EDITOR_OK);
    REQUIRE(fixture.scopes().save_scope_config(
                "workspace", "",
                {{"workspaceRoot", {{"keep", true}}},
                 {"ai_editor",
                  {{"editor", {{"tabSize", 2}}},
                   {"layout", {{"chatVisible", true}}},
                   {"workspaceUnknown", {{"keep", 7}}}}}}) ==
            SAO_AI_EDITOR_OK);

    Json workspace_loaded;
    REQUIRE(AiEditorSettings::load(
                fixture.scopes(), fixture.secrets(),
                {{"scope", "workspace"}}, workspace_loaded) == SAO_AI_EDITOR_OK);
    REQUIRE(workspace_loaded["effective"]["editor"]["tabSize"] == 2);
    REQUIRE(workspace_loaded["effective"]["layout"]["chatVisible"] == true);
    REQUIRE(workspace_loaded["inherited"]["editor"]["tabSize"] == 8);
    REQUIRE(workspace_loaded["inherited"]["layout"]["chatVisible"] == false);
    REQUIRE(workspace_loaded["sources"]["editor"]["tabSize"] == "workspace");
    REQUIRE(workspace_loaded["sources"]["layout"]["chatVisible"] == "workspace");

    Json reset_result;
    REQUIRE(AiEditorSettings::save(
                fixture.scopes(), fixture.secrets(),
                {{"scope", "workspace"},
                 {"changes",
                  {{"editor.tabSize", 6}, {"layout.chatVisible", true}}},
                 {"resetKeys",
                  Json::array({"editor.tabSize", "layout.chatVisible"})},
                 {"reset_keys",
                  Json::array({"editor.tabSize", "layout.chatVisible"})}},
                reset_result) == SAO_AI_EDITOR_OK);

    const Json raw_after_reset = read_json(fixture.config_path("workspace"));
    REQUIRE(raw_after_reset["workspaceRoot"]["keep"] == true);
    REQUIRE(raw_after_reset["ai_editor"]["workspaceUnknown"]["keep"] == 7);
    REQUIRE_FALSE(raw_after_reset["ai_editor"].contains("editor"));
    REQUIRE_FALSE(raw_after_reset["ai_editor"].contains("layout"));
    REQUIRE_FALSE(reset_result["overrides"].contains("editor"));
    REQUIRE_FALSE(reset_result["overrides"].contains("layout"));

    Json merged;
    REQUIRE(AiEditorSettings::load(fixture.scopes(), fixture.secrets(),
                                   {{"scope", "merged"}}, merged) ==
            SAO_AI_EDITOR_OK);
    REQUIRE(merged["settings"]["editor"]["tabSize"] == 8);
    REQUIRE(merged["settings"]["layout"]["chatVisible"] == false);
    REQUIRE(merged["settings"]["systemUnknown"] == "keep");
    REQUIRE(merged["settings"]["workspaceUnknown"]["keep"] == 7);
    REQUIRE(merged["effective"]["editor"]["tabSize"] == 8);
    REQUIRE(merged["sources"]["editor"]["tabSize"] == "system");

    Json set_result;
    REQUIRE(AiEditorSettings::save(
                fixture.scopes(), fixture.secrets(),
                {{"scope", "workspace"},
                 {"changes", {{"editor.tabSize", 3}}}},
                set_result) == SAO_AI_EDITOR_OK);
    const Json raw_after_set = read_json(fixture.config_path("workspace"));
    REQUIRE(raw_after_set["ai_editor"]["editor"]["tabSize"] == 3);

    Json invalid;
    REQUIRE(AiEditorSettings::save(
                fixture.scopes(), fixture.secrets(),
                {{"scope", "workspace"}, {"resetKeys", "editor.tabSize"}},
                invalid) == SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
    REQUIRE(invalid["code"] == "INVALID_ARGUMENT");
    REQUIRE(invalid["validationErrors"][0]["path"] == "resetKeys");
}

TEST_CASE("AI Editor settings protect MCP env and headers with stable updates",
          "[plugins][ai_editor][settings][mcp][secrets]") {
    SettingsFixture fixture;
    const Json remote = {
        {"id", "remote"},
        {"transport", "streamable_http"},
        {"url", "https://example.invalid/mcp"},
        {"env", {{"MCP_TOKEN", "env-one"}, {"OLD_ONLY", "old"}}},
        {"headers", {{"Authorization", "Bearer header-one"}}},
    };
    const Json named = {
        {"transport", "stdio"},
        {"command", "named-fixture.exe"},
        {"env", {{"NAMED_TOKEN", "named-env"}}},
        {"headers", {{"X-Named", "named-header"}}},
    };

    Json saved;
    REQUIRE(AiEditorSettings::save(
                fixture.scopes(), fixture.secrets(),
                {{"scope", "workspace"},
                 {"changes",
                  {{"mcp.servers", Json::array({remote})},
                   {"mcpServers", {{"named", named}}}}}},
                saved) == SAO_AI_EDITOR_OK);

    const Json& settings = saved["settings"];
    const Json* remote_redacted = find_server(settings["mcp"]["servers"],
                                              "remote");
    const Json* named_redacted = find_server(settings["mcp"]["mcpServers"],
                                             "named");
    REQUIRE(remote_redacted != nullptr);
    REQUIRE(named_redacted != nullptr);
    REQUIRE((*remote_redacted)["env"]["MCP_TOKEN"] ==
            "__SAO_SECRET_PRESENT__");
        REQUIRE((*remote_redacted)["env"]["OLD_ONLY"] ==
            "__SAO_SECRET_PRESENT__");
    REQUIRE((*remote_redacted)["headers"]["Authorization"] ==
            "__SAO_SECRET_PRESENT__");
    REQUIRE((*named_redacted)["env"]["NAMED_TOKEN"] ==
            "__SAO_SECRET_PRESENT__");
    REQUIRE(saved["secretStates"]["mcpServers"]["remote"]["env"] == true);
    REQUIRE(saved["secretStates"]["mcpServers"]["remote"]["headers"] ==
            true);
    REQUIRE(saved["secretStates"]["mcpServers"]["named"]["env"] == true);

    const Json raw = read_json(fixture.config_path("workspace"));
    const std::string raw_text = raw.dump();
    REQUIRE(raw_text.find("env-one") == std::string::npos);
    REQUIRE(raw_text.find("header-one") == std::string::npos);
    REQUIRE(raw_text.find("named-env") == std::string::npos);
    REQUIRE(raw_text.find("named-header") == std::string::npos);
    REQUIRE_FALSE(raw["ai_editor"].contains("mcpServers"));

    const std::string remote_env_key = stable_secret_key("remote", "env", "mcp");
    const std::string remote_headers_key =
        stable_secret_key("remote", "headers", "mcp");
    const std::string named_env_key = stable_secret_key("named", "env", "mcp");
    std::string value;
    REQUIRE(fixture.secrets().get(remote_env_key, value) == SAO_AI_EDITOR_OK);
    REQUIRE(Json::parse(value)["MCP_TOKEN"] == "env-one");
    REQUIRE(fixture.secrets().get(remote_headers_key, value) ==
            SAO_AI_EDITOR_OK);
    REQUIRE(Json::parse(value)["Authorization"] == "Bearer header-one");
    REQUIRE(fixture.secrets().get(named_env_key, value) == SAO_AI_EDITOR_OK);
    REQUIRE(Json::parse(value)["NAMED_TOKEN"] == "named-env");

    Json preserve;
    REQUIRE(AiEditorSettings::save(
                fixture.scopes(), fixture.secrets(),
                {{"scope", "workspace"},
                 {"changes", {{"mcp.servers", settings["mcp"]["servers"]}}}},
                preserve) == SAO_AI_EDITOR_OK);
    REQUIRE(fixture.secrets().get(remote_env_key, value) == SAO_AI_EDITOR_OK);
    REQUIRE(Json::parse(value)["MCP_TOKEN"] == "env-one");

    Json updated;
    REQUIRE(AiEditorSettings::save(
                fixture.scopes(), fixture.secrets(),
                {{"scope", "workspace"},
                 {"secretUpdates",
                  {{"mcpServers",
                    {{"remote",
                      {{"env",
                        {{"action", "set"},
                         {"value",
                          {{"MCP_TOKEN", "env-two"},
                           {"EXTRA_TOKEN", "extra"}}}}}}}}}}}},
                updated) == SAO_AI_EDITOR_OK);
    REQUIRE(fixture.secrets().get(remote_env_key, value) == SAO_AI_EDITOR_OK);
    const Json updated_env = Json::parse(value);
    REQUIRE(updated_env["MCP_TOKEN"] == "env-two");
    REQUIRE(updated_env["EXTRA_TOKEN"] == "extra");
    REQUIRE_FALSE(updated_env.contains("OLD_ONLY"));

    Json cleared;
    REQUIRE(AiEditorSettings::save(
                fixture.scopes(), fixture.secrets(),
                {{"scope", "workspace"},
                 {"secret_updates",
                  {{"mcp_servers",
                    {{"remote", {{"headers", {{"action", "clear"}}}}}}}}}},
                cleared) == SAO_AI_EDITOR_OK);
    REQUIRE(fixture.secrets().get(remote_headers_key, value) ==
            SAO_AI_EDITOR_ERR_NOT_FOUND);
    REQUIRE(fixture.secrets().get(remote_env_key, value) == SAO_AI_EDITOR_OK);
    REQUIRE(cleared["secretStates"]["mcpServers"]["remote"]["headers"] ==
            false);
    REQUIRE(cleared["secretStates"]["mcpServers"]["remote"]["env"] == true);
}

TEST_CASE("AI Editor settings migrate unique MCP legacy refs and isolate collisions",
          "[plugins][ai_editor][settings][mcp][migration]") {
    SECTION("unique legacy ref migrates once") {
        SettingsFixture fixture;
        const std::string legacy = "mcp/a_b/env";
        REQUIRE(fixture.secrets().set(
                    legacy, Json{{"TOKEN", "legacy-value"}}.dump()) ==
                SAO_AI_EDITOR_OK);
        REQUIRE(fixture.scopes().save_scope_config(
                    "workspace", "",
                    {{"ai_editor",
                      {{"mcp",
                        {{"servers",
                          Json::array({{{"id", "a/b"},
                                        {"env",
                                         {{"TOKEN",
                                           "__SAO_SECRET_PRESENT__"}}},
                                        {"_secret_fields",
                                         Json::array({"env"})}}})}}}}}}) ==
                SAO_AI_EDITOR_OK);

        Json loaded;
        REQUIRE(AiEditorSettings::load(
                    fixture.scopes(), fixture.secrets(),
                    {{"scope", "workspace"}}, loaded) == SAO_AI_EDITOR_OK);
        REQUIRE(loaded["secretStates"]["mcpServers"]["a/b"]["env"] == true);
        REQUIRE_FALSE(fixture.secrets().has(legacy));
        const std::string stable = stable_secret_key("a/b", "env", "mcp");
        std::string value;
        REQUIRE(fixture.secrets().get(stable, value) == SAO_AI_EDITOR_OK);
        REQUIRE(Json::parse(value)["TOKEN"] == "legacy-value");
    }

    SECTION("ambiguous legacy ref stays quarantined") {
        SettingsFixture fixture;
        const std::string legacy = "mcp/a_b/env";
        REQUIRE(fixture.secrets().set(
                    legacy, Json{{"TOKEN", "ambiguous-value"}}.dump()) ==
                SAO_AI_EDITOR_OK);
        const Json servers = Json::array({
            {{"id", "a/b"},
             {"env", {{"TOKEN", "__SAO_SECRET_PRESENT__"}}},
             {"_secret_fields", Json::array({"env"})}},
            {{"id", "a?b"},
             {"env", {{"TOKEN", "__SAO_SECRET_PRESENT__"}}},
             {"_secret_fields", Json::array({"env"})}},
        });
        REQUIRE(fixture.scopes().save_scope_config(
                    "workspace", "",
                    {{"ai_editor", {{"mcp", {{"servers", servers}}}}}}) ==
                SAO_AI_EDITOR_OK);

        Json loaded;
        REQUIRE(AiEditorSettings::load(
                    fixture.scopes(), fixture.secrets(),
                    {{"scope", "workspace"}}, loaded) == SAO_AI_EDITOR_OK);
        REQUIRE(loaded["secretStates"]["mcpServers"]["a/b"]["env"] ==
                false);
        REQUIRE(loaded["secretStates"]["mcpServers"]["a?b"]["env"] ==
                false);
        REQUIRE(fixture.secrets().has(legacy));
        REQUIRE_FALSE(fixture.secrets().has(
            stable_secret_key("a/b", "env", "mcp")));
        REQUIRE_FALSE(fixture.secrets().has(
            stable_secret_key("a?b", "env", "mcp")));
    }
}

TEST_CASE("AI Editor chat settings require exact provider ID and canonical timeout",
          "[plugins][ai_editor][settings][chat]") {
    SettingsFixture fixture;
    Json saved;
    REQUIRE(AiEditorSettings::save(
                fixture.scopes(), fixture.secrets(),
                {{"scope", "system"},
                 {"changes",
                  {{"provider", "openai"},
                   {"base_url", "http://127.0.0.1:1/v1"},
                   {"model", "active-model"}}},
                 {"secretUpdates",
                  {{"apiKey", {{"action", "set"},
                                {"value", "active-secret"}}}}}},
                saved) == SAO_AI_EDITOR_OK);
    REQUIRE(fixture.scopes().save_registry_item(
                "providers", "system", "", "other-provider",
                {{"type", "openai"},
                 {"endpoint", "http://127.0.0.1:2/v1"},
                 {"model", "other-model"}}) == SAO_AI_EDITOR_OK);

    const Json messages =
        Json::array({{{"role", "user"}, {"content", "hello"}}});
    Json prepared;
    Json provider;
    std::string api_key;
    REQUIRE(AiEditorSettings::prepare_chat_request(
                fixture.scopes(), fixture.secrets(),
                {{"providerId", "other-provider"},
                 {"messages", messages},
                 {"timeout_ms", 1234}},
                prepared, provider, api_key) == SAO_AI_EDITOR_OK);
    REQUIRE(api_key.empty());
    REQUIRE(provider["id"] == "other-provider");
    REQUIRE(provider["type"] == "openai");
    REQUIRE(prepared["timeoutMs"] == 1234);
    REQUIRE_FALSE(prepared.contains("timeout_ms"));

    REQUIRE(AiEditorSettings::prepare_chat_request(
                fixture.scopes(), fixture.secrets(),
                {{"provider", {{"id", "other-object"},
                                {"type", "openai"},
                                {"endpoint", "http://127.0.0.1:3/v1"},
                                {"model", "object-model"}}},
                 {"messages", messages}},
                prepared, provider, api_key) == SAO_AI_EDITOR_OK);
    REQUIRE(api_key.empty());

    REQUIRE(AiEditorSettings::prepare_chat_request(
                fixture.scopes(), fixture.secrets(),
                {{"provider", {{"type", "openai"},
                                {"endpoint", "http://127.0.0.1:4/v1"},
                                {"model", "type-only-model"}}},
                 {"messages", messages}},
                prepared, provider, api_key) == SAO_AI_EDITOR_OK);
    REQUIRE(api_key.empty());

    REQUIRE(AiEditorSettings::prepare_chat_request(
                fixture.scopes(), fixture.secrets(),
                {{"providerId", "openai"}, {"messages", messages}},
                prepared, provider, api_key) == SAO_AI_EDITOR_OK);
    REQUIRE(api_key == "active-secret");
    REQUIRE(provider["transport"] == "chat_completions");

    Json transport_saved;
    REQUIRE(AiEditorSettings::save(
                fixture.scopes(), fixture.secrets(),
                {{"scope", "system"},
                 {"changes", {{"transport", "responses"}}}},
                transport_saved) == SAO_AI_EDITOR_OK);
    REQUIRE(AiEditorSettings::prepare_chat_request(
                fixture.scopes(), fixture.secrets(),
                {{"providerId", "openai"}, {"messages", messages}},
                prepared, provider, api_key) == SAO_AI_EDITOR_OK);
    REQUIRE(provider["transport"] == "responses");

    REQUIRE(AiEditorSettings::prepare_chat_request(
                fixture.scopes(), fixture.secrets(),
                {{"providerId", "openai"},
                 {"transport", "chat_completions"},
                 {"messages", messages}},
                prepared, provider, api_key) == SAO_AI_EDITOR_OK);
    REQUIRE(provider["transport"] == "chat_completions");

    Json invalid_prepared;
    REQUIRE(AiEditorSettings::prepare_chat_request(
                fixture.scopes(), fixture.secrets(),
                {{"providerId", "openai"},
                 {"messages", messages},
                 {"temperature", "hot"}},
                invalid_prepared, provider, api_key) ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
    REQUIRE(invalid_prepared["code"] == "INVALID_ARGUMENT");
    REQUIRE_FALSE(invalid_prepared["validationErrors"].empty());

    REQUIRE(AiEditorSettings::prepare_chat_request(
                fixture.scopes(), fixture.secrets(),
                {{"providerId", "openai"},
                 {"messages", messages},
                 {"timeout_ms", 0U}},
                invalid_prepared, provider, api_key) ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
    REQUIRE(invalid_prepared["code"] == "INVALID_ARGUMENT");

    Json invalid_secret;
    REQUIRE(AiEditorSettings::save(
                fixture.scopes(), fixture.secrets(),
                {{"scope", "system"},
                 {"secretUpdates",
                  {{"apiKey", {{"action", "rotate"},
                                {"value", "must-not-write"}}}}}},
                invalid_secret) == SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
    REQUIRE(invalid_secret["code"] == "INVALID_ARGUMENT");
    const std::string active_key =
        stable_secret_key("openai", "api-key", "provider");
    std::string stored_secret;
    REQUIRE(fixture.secrets().get(active_key, stored_secret) ==
            SAO_AI_EDITOR_OK);
    REQUIRE(stored_secret == "active-secret");
}

TEST_CASE("AI Editor settings roll back applied secrets when scope save fails",
          "[plugins][ai_editor][settings][rollback]") {
    SettingsFixture fixture;
    const std::filesystem::path config = fixture.config_path("workspace");
    REQUIRE(std::filesystem::create_directories(config));

    Json result;
    REQUIRE(AiEditorSettings::save(
                fixture.scopes(), fixture.secrets(),
                {{"scope", "workspace"},
                 {"changes", {{"provider", "rollback-provider"}}},
                 {"secretUpdates",
                  {{"apiKey", {{"action", "set"},
                                {"value", "rollback-secret"}}}}}},
                result) == SAO_AI_EDITOR_ERR_OS_CALL_FAILED);

    const std::string key =
        stable_secret_key("rollback-provider", "api-key", "provider");
    std::string value;
    REQUIRE(fixture.secrets().get(key, value) == SAO_AI_EDITOR_ERR_NOT_FOUND);
}
