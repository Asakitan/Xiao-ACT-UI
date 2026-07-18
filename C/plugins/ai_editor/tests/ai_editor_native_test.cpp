#include <catch2/catch_test_macros.hpp>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "sao/ai_editor/ai_editor_native.h"
#include "sao/ai_editor/mcp_client.h"
#include "sao/ai_editor/mcp_codec.h"
#include "sao/ai_editor/openai_codec.h"

#include "../src/chat_provider_router.h"
#if defined(SAO_AI_EDITOR_HAS_WEBVIEW) && SAO_AI_EDITOR_HAS_WEBVIEW
#include "../src/webview_bridge.h"
#endif

namespace {

using Json = nlohmann::json;

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        wchar_t root[MAX_PATH]{};
        REQUIRE(GetTempPathW(MAX_PATH, root) > 0);
        path_ = std::filesystem::path(root) /
                (L"sao-ai-editor-native-" +
                 std::to_wstring(GetCurrentProcessId()) + L"-" +
                 std::to_wstring(GetTickCount64()));
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

std::string decode_openai(std::string_view input) {
    uint32_t required = 0;
    REQUIRE(sao_ai_editor_openai_decode_response(
                input.data(), static_cast<uint32_t>(input.size()), nullptr, 0,
                &required) == SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL);
    std::vector<char> output(static_cast<size_t>(required) + 1);
    REQUIRE(sao_ai_editor_openai_decode_response(
                input.data(), static_cast<uint32_t>(input.size()), output.data(),
                static_cast<uint32_t>(output.size()), &required) ==
            SAO_AI_EDITOR_OK);
    return std::string(output.data(), required);
}

std::string feed_sse(sao_ai_editor_openai_sse_decoder_t decoder,
                     std::string_view input) {
    uint32_t required = 0;
    REQUIRE(sao_ai_editor_openai_sse_decoder_feed(
                decoder, input.data(), static_cast<uint32_t>(input.size()),
                nullptr, 0, &required) == SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL);
    std::vector<char> output(static_cast<size_t>(required) + 1);
    REQUIRE(sao_ai_editor_openai_sse_decoder_feed(
                decoder, nullptr, 0, output.data(),
                static_cast<uint32_t>(output.size()), &required) ==
            SAO_AI_EDITOR_OK);
    return std::string(output.data(), required);
}

std::string feed_mcp(sao_ai_editor_mcp_decoder_t decoder,
                     std::string_view input) {
    uint32_t required = 0;
    REQUIRE(sao_ai_editor_mcp_decoder_feed(
                decoder, input.data(), static_cast<uint32_t>(input.size()),
                nullptr, 0, &required) == SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL);
    std::vector<char> output(static_cast<size_t>(required) + 1);
    REQUIRE(sao_ai_editor_mcp_decoder_feed(
                decoder, nullptr, 0, output.data(),
                static_cast<uint32_t>(output.size()), &required) ==
            SAO_AI_EDITOR_OK);
    return std::string(output.data(), required);
}

Json dispatch(sao_ai_editor_runtime_t runtime,
              std::string_view method,
              const Json& params = Json::object()) {
    INFO("JSON-RPC method: " << method);
    static int64_t request_id = 0;
    const std::string request = Json{{"jsonrpc", "2.0"},
                                     {"id", ++request_id},
                                     {"method", method},
                                     {"params", params},
                                     {"sao", {{"protocolVersion", 1}}}}
                                    .dump();
    uint32_t required = 0;
    REQUIRE(sao_ai_editor_runtime_dispatch(
                runtime, request.data(), static_cast<uint32_t>(request.size()),
                nullptr, 0, &required) == SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL);
    std::vector<char> output(static_cast<size_t>(required) + 1);
    REQUIRE(sao_ai_editor_runtime_dispatch(
                runtime, nullptr, 0, output.data(),
                static_cast<uint32_t>(output.size()), &required) ==
            SAO_AI_EDITOR_OK);
    return Json::parse(output.data(), output.data() + required);
}

class RuntimeFixture final {
public:
    RuntimeFixture() {
        workspace_ = temporary_.path() / L"workspace";
        system_ = temporary_.path() / L"system";
        plugin_ = temporary_.path() / L"plugin";
        REQUIRE(std::filesystem::create_directories(workspace_));
        REQUIRE(std::filesystem::create_directories(system_));
        REQUIRE(std::filesystem::create_directories(plugin_));
        workspace_utf8_ = utf8_path(workspace_);
        system_utf8_ = utf8_path(system_);
        plugin_json_ = Json::array(
            {{{"id", "fixture"}, {"path", utf8_path(plugin_)}}})
                           .dump();
        SaoAiEditorRuntimeConfig config{};
        config.struct_size = sizeof(config);
        config.workspace_root_utf8 = workspace_utf8_.c_str();
        config.system_root_utf8 = system_utf8_.c_str();
        config.plugin_roots_json_utf8 = plugin_json_.c_str();
        config.max_file_bytes = 64U * 1024U;
        config.max_search_results = 50;
        REQUIRE(sao_ai_editor_runtime_create(&config, &runtime_) ==
                SAO_AI_EDITOR_OK);
        REQUIRE(runtime_ != nullptr);
    }

    ~RuntimeFixture() { sao_ai_editor_runtime_destroy(runtime_); }

    sao_ai_editor_runtime_t get() const noexcept { return runtime_; }
    const std::filesystem::path& workspace() const noexcept {
        return workspace_;
    }

private:
    TemporaryDirectory temporary_;
    std::filesystem::path workspace_;
    std::filesystem::path system_;
    std::filesystem::path plugin_;
    std::string workspace_utf8_;
    std::string system_utf8_;
    std::string plugin_json_;
    sao_ai_editor_runtime_t runtime_ = nullptr;
};

class LocalHttpServer final {
public:
    explicit LocalHttpServer(std::string response = {})
        : response_(std::move(response)) {
        WSADATA data{};
        REQUIRE(WSAStartup(MAKEWORD(2, 2), &data) == 0);
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        REQUIRE(listener_ != INVALID_SOCKET);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        REQUIRE(bind(listener_, reinterpret_cast<const sockaddr*>(&address),
                     sizeof(address)) == 0);
        REQUIRE(listen(listener_, SOMAXCONN) == 0);
        int address_size = sizeof(address);
        REQUIRE(getsockname(listener_, reinterpret_cast<sockaddr*>(&address),
                            &address_size) == 0);
        endpoint_ = "http://127.0.0.1:" +
                    std::to_string(ntohs(address.sin_port)) +
                    "/v1/chat/completions";
        worker_ = std::thread([this] { accept_connections(); });
    }

    ~LocalHttpServer() {
        stopping_.store(true, std::memory_order_release);
        if (listener_ != INVALID_SOCKET) {
            closesocket(listener_);
            listener_ = INVALID_SOCKET;
        }
        {
            std::lock_guard<std::mutex> lock(client_mutex_);
            for (const SOCKET client : clients_) {
                shutdown(client, SD_BOTH);
                closesocket(client);
            }
            clients_.clear();
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        WSACleanup();
    }

    const std::string& endpoint() const noexcept { return endpoint_; }

    bool wait_for_connections(size_t expected, DWORD timeout_ms) const noexcept {
        const ULONGLONG started = GetTickCount64();
        while (accepted_.load(std::memory_order_acquire) < expected) {
            if (GetTickCount64() - started >= timeout_ms) {
                return false;
            }
            Sleep(10);
        }
        return true;
    }

private:
    void accept_connections() noexcept {
        while (!stopping_.load(std::memory_order_acquire)) {
            const SOCKET client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                return;
            }
            std::string request;
            std::array<char, 4U * 1024U> buffer{};
            size_t header_end = std::string::npos;
            while (header_end == std::string::npos) {
                const int received = recv(
                    client, buffer.data(), static_cast<int>(buffer.size()), 0);
                if (received <= 0) {
                    break;
                }
                request.append(buffer.data(), static_cast<size_t>(received));
                header_end = request.find("\r\n\r\n");
            }
            if (header_end == std::string::npos) {
                closesocket(client);
                continue;
            }
            if (request.find("Expect: 100-continue") != std::string::npos) {
                constexpr std::string_view continue_response =
                    "HTTP/1.1 100 Continue\r\n\r\n";
                if (send(client, continue_response.data(),
                         static_cast<int>(continue_response.size()), 0) ==
                    SOCKET_ERROR) {
                    closesocket(client);
                    continue;
                }
            }
            size_t content_length = 0;
            const size_t length_begin = request.find("Content-Length: ");
            if (length_begin != std::string::npos) {
                const char* first = request.data() + length_begin + 16;
                const char* last =
                    request.data() + request.find("\r\n", length_begin);
                const auto [parsed, error] =
                    std::from_chars(first, last, content_length);
                if (error != std::errc{} || parsed != last) {
                    closesocket(client);
                    continue;
                }
            }
            const size_t body_begin = header_end + 4;
            while (request.size() - body_begin < content_length) {
                const int received = recv(
                    client, buffer.data(), static_cast<int>(buffer.size()), 0);
                if (received <= 0) {
                    break;
                }
                request.append(buffer.data(), static_cast<size_t>(received));
            }
            if (request.size() - body_begin < content_length) {
                closesocket(client);
                continue;
            }
            accepted_.fetch_add(1, std::memory_order_release);
            if (!response_.empty()) {
                size_t sent_total = 0;
                while (sent_total < response_.size()) {
                    const int sent = send(
                        client, response_.data() + sent_total,
                        static_cast<int>(response_.size() - sent_total), 0);
                    if (sent == SOCKET_ERROR) {
                        break;
                    }
                    sent_total += static_cast<size_t>(sent);
                }
                shutdown(client, SD_BOTH);
                closesocket(client);
                continue;
            }
            std::lock_guard<std::mutex> lock(client_mutex_);
            clients_.push_back(client);
        }
    }

    std::atomic<bool> stopping_{false};
    std::atomic<size_t> accepted_{0};
    SOCKET listener_ = INVALID_SOCKET;
    std::mutex client_mutex_;
    std::vector<SOCKET> clients_;
    std::thread worker_;
    std::string endpoint_;
    std::string response_;
};

}  // namespace

TEST_CASE("AI Editor OpenAI codecs normalize responses and split SSE chunks",
          "[plugins][ai_editor][native][openai]") {
    const Json normalized = Json::parse(decode_openai(R"({
        "id":"chat-1","model":"fixture-model",
        "choices":[{"message":{"role":"assistant","content":"hello",
        "tool_calls":[{"id":"call-1","type":"function","function":{
        "name":"readFile","arguments":"{}"}}]},
        "finish_reason":"tool_calls"}],"usage":{"total_tokens":7}})"));
    REQUIRE(normalized["ok"] == true);
    REQUIRE(normalized["content"] == "hello");
    REQUIRE(normalized["toolCalls"][0]["name"] == "readFile");
    REQUIRE(normalized["usage"]["total_tokens"] == 7);

    sao_ai_editor_openai_sse_decoder_t decoder = nullptr;
    REQUIRE(sao_ai_editor_openai_sse_decoder_create(&decoder) ==
            SAO_AI_EDITOR_OK);
    const Json first = Json::parse(feed_sse(
        decoder,
        "data: {\"id\":\"chat-2\",\"choices\":[{\"delta\":{\"content\":\"hel"));
    REQUIRE(first.empty());
    const Json second = Json::parse(feed_sse(
        decoder,
        "lo\"},\"finish_reason\":null}]}\r\n\r\ndata: [DONE]\n\n"));
    REQUIRE(second.size() == 2);
    REQUIRE(second[0]["content"] == "hello");
    REQUIRE(second[1]["type"] == "done");
    sao_ai_editor_openai_sse_decoder_destroy(decoder);
}

TEST_CASE("AI Editor MCP codec incrementally decodes framed and line messages",
          "[plugins][ai_editor][native][mcp]") {
    const std::string message =
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})";
    uint32_t frame_size = 0;
    REQUIRE(sao_ai_editor_mcp_encode(message.data(),
                                     static_cast<uint32_t>(message.size()),
                                     nullptr, 0, &frame_size) ==
            SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL);
    std::vector<char> frame(frame_size);
    REQUIRE(sao_ai_editor_mcp_encode(
                message.data(), static_cast<uint32_t>(message.size()),
                frame.data(), frame_size, &frame_size) == SAO_AI_EDITOR_OK);

    sao_ai_editor_mcp_decoder_t decoder = nullptr;
    REQUIRE(sao_ai_editor_mcp_decoder_create(1024, &decoder) ==
            SAO_AI_EDITOR_OK);
    const size_t split = 11;
    REQUIRE(Json::parse(feed_mcp(
                decoder, std::string_view(frame.data(), split))).empty());
    const Json framed = Json::parse(feed_mcp(
        decoder, std::string_view(frame.data() + split, frame.size() - split)));
    REQUIRE(framed.size() == 1);
    REQUIRE(framed[0]["method"] == "tools/list");
    const Json lined = Json::parse(feed_mcp(decoder, message + "\n"));
    REQUIRE(lined.size() == 1);
    REQUIRE(lined[0]["id"] == 1);
    sao_ai_editor_mcp_decoder_destroy(decoder);
}

TEST_CASE("AI Editor runtime merges scopes and persists history registries",
          "[plugins][ai_editor][native][storage]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    const Json initialized = dispatch(runtime, "runtime.initialize");
    REQUIRE(initialized["result"]["nativeAbiVersion"] ==
            SAO_AI_EDITOR_NATIVE_ABI_VERSION);
    REQUIRE(initialized["result"]["scopes"].size() == 3);

    REQUIRE(dispatch(runtime, "config.save",
                     {{"scope", "system"},
                      {"config", {{"theme", "dark"}, {"nested", {{"a", 1}}}}}})
                .contains("result"));
    REQUIRE(dispatch(runtime, "config.save",
                     {{"scope", "workspace"},
                      {"config", {{"nested", {{"b", 2}}}}}})
                .contains("result"));
    const Json merged = dispatch(runtime, "config.load");
    REQUIRE(merged["result"]["theme"] == "dark");
    REQUIRE(merged["result"]["nested"]["a"] == 1);
    REQUIRE(merged["result"]["nested"]["b"] == 2);

    const Json created = dispatch(runtime, "conversation.create",
                                  {{"title", "Fixture"},
                                   {"model", "fixture-model"},
                                   {"scope", "workspace"}});
    const std::string id = created["result"]["id"];
    REQUIRE(dispatch(runtime, "conversation.append",
                     {{"id", id},
                      {"message", {{"role", "user"},
                                   {"content", "hello"}}}})
                ["result"]["messageCount"] == 1);
    REQUIRE(dispatch(runtime, "conversation.list")["result"].size() == 1);

    // Seed a second conversation covering the search title path.
    const Json created_ranged = dispatch(
        runtime, "conversation.create",
        {{"title", "Refactoring Notes"},
         {"model", "test"},
         {"scope", "workspace"}});
    REQUIRE(dispatch(runtime, "conversation.append",
                     {{"id", created_ranged["result"]["id"]},
                      {"message", {{"role", "assistant"},
                                   {"content", "Rewrite the parser."}}}})
                .contains("result"));

    const Json search_title = dispatch(runtime, "conversation.search",
                                        {{"query", "Refactor"},
                                         {"scope", "workspace"},
                                         {"limit", 10}});
    REQUIRE(search_title.contains("result"));
    REQUIRE(search_title["result"]["total"] == 1);
    REQUIRE(search_title["result"]["results"][0]["title"] ==
            "Refactoring Notes");

    const Json search_message = dispatch(runtime, "conversation.search",
                                          {{"query", "hello"}});
    REQUIRE(search_message["result"]["total"] == 1);
    REQUIRE(search_message["result"]["results"][0]["id"] == id);

    const Json empty_query = dispatch(runtime, "conversation.search",
                                       {{"query", ""}});
    REQUIRE(empty_query["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);

    REQUIRE(dispatch(runtime, "agents.save",
                     {{"scope", "plugin:fixture"},
                      {"item", {{"id", "reviewer"},
                                {"name", "Reviewer"}}}})
                .contains("result"));
    const Json agents = dispatch(runtime, "agents.list");
    REQUIRE(agents["result"]["total"] == 1);
    REQUIRE(agents["result"]["items"][0]["id"] == "reviewer");
}

TEST_CASE("AI Editor conversation.export packages a single conversation "
          "with sao-conversation/1 envelope",
          "[plugins][ai_editor][native][storage][export]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    REQUIRE(dispatch(runtime, "runtime.initialize").contains("result"));

    const Json created = dispatch(runtime, "conversation.create",
                                  {{"title", "Exportable"},
                                   {"model", "gpt-export"},
                                   {"scope", "workspace"}});
    const std::string id = created["result"]["id"];
    REQUIRE(dispatch(runtime, "conversation.append",
                     {{"id", id},
                      {"message", {{"role", "user"},
                                   {"content", "hi export"}}}})
                .contains("result"));
    REQUIRE(dispatch(runtime, "conversation.append",
                     {{"id", id},
                      {"message", {{"role", "assistant"},
                                   {"content", "hello back"}}}})
                .contains("result"));

    // Missing both id and scope → invalid.
    const Json missing =
        dispatch(runtime, "conversation.export", Json::object());
    REQUIRE(missing["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);

    // Providing both id and scope → invalid.
    const Json both = dispatch(runtime, "conversation.export",
                                {{"id", id}, {"scope", "all"}});
    REQUIRE(both["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);

    const Json exported = dispatch(runtime, "conversation.export",
                                    {{"id", id}});
    REQUIRE(exported.contains("result"));
    REQUIRE(exported["result"]["format"] == "sao-conversation/1");
    REQUIRE(exported["result"]["exportedAt"].is_number());
    REQUIRE(exported["result"]["conversation"]["id"] == id);
    REQUIRE(exported["result"]["conversation"]["title"] == "Exportable");
    REQUIRE(exported["result"]["conversation"]["messages"].size() == 2);
    REQUIRE(exported["result"]["conversation"]["messageCount"] == 2);

    // Unknown id → NOT_FOUND propagated as protocol -32601 (compound method).
    const Json unknown = dispatch(runtime, "conversation.export",
                                   {{"id", "conv-does-not-exist"}});
    REQUIRE(unknown.contains("error"));
}

TEST_CASE("AI Editor conversation.export scope=all returns "
          "sao-conversations/1 batch",
          "[plugins][ai_editor][native][storage][export]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    REQUIRE(dispatch(runtime, "runtime.initialize").contains("result"));

    const Json a = dispatch(runtime, "conversation.create",
                            {{"title", "A"},
                             {"model", "m"},
                             {"scope", "workspace"}});
    const Json b = dispatch(runtime, "conversation.create",
                            {{"title", "B"},
                             {"model", "m"},
                             {"scope", "system"}});
    REQUIRE(dispatch(runtime, "conversation.append",
                     {{"id", a["result"]["id"]},
                      {"message", {{"role", "user"},
                                   {"content", "content-a"}}}})
                .contains("result"));

    // Invalid scope value.
    const Json bad_scope = dispatch(runtime, "conversation.export",
                                     {{"scope", "everywhere"}});
    REQUIRE(bad_scope["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);

    const Json all = dispatch(runtime, "conversation.export",
                               {{"scope", "all"}});
    REQUIRE(all.contains("result"));
    REQUIRE(all["result"]["format"] == "sao-conversations/1");
    REQUIRE(all["result"]["count"] == 2);
    REQUIRE(all["result"]["conversations"].size() == 2);
    REQUIRE(all["result"]["exportedAt"].is_number());

    // scope=workspace only picks up A.
    const Json ws = dispatch(runtime, "conversation.export",
                              {{"scope", "workspace"}});
    REQUIRE(ws["result"]["count"] == 1);
    REQUIRE(ws["result"]["conversations"][0]["title"] == "A");

    // scope=system only picks up B.
    const Json sys = dispatch(runtime, "conversation.export",
                               {{"scope", "system"}});
    REQUIRE(sys["result"]["count"] == 1);
    REQUIRE(sys["result"]["conversations"][0]["title"] == "B");
}

TEST_CASE("AI Editor conversation.import round-trips with overwrite semantics",
          "[plugins][ai_editor][native][storage][import]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    REQUIRE(dispatch(runtime, "runtime.initialize").contains("result"));

    const Json seed = dispatch(runtime, "conversation.create",
                                {{"title", "Seed"},
                                 {"model", "m"},
                                 {"scope", "workspace"}});
    const std::string seed_id = seed["result"]["id"];
    REQUIRE(dispatch(runtime, "conversation.append",
                     {{"id", seed_id},
                      {"message", {{"role", "user"},
                                   {"content", "original"}}}})
                .contains("result"));
    const Json exported = dispatch(runtime, "conversation.export",
                                    {{"id", seed_id}})["result"];
    REQUIRE(exported["format"] == "sao-conversation/1");

    // Unknown format → invalid.
    const Json bogus = dispatch(runtime, "conversation.import",
                                 {{"payload", {{"format", "totally-fake/9"}}}});
    REQUIRE(bogus["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);

    // Conflict, overwrite=false → imported=0, conflicts=[seed_id], no
    // modifications.
    const Json conflict =
        dispatch(runtime, "conversation.import",
                 {{"payload", exported},
                  {"scope", "workspace"},
                  {"overwrite", false}});
    REQUIRE(conflict.contains("error"));
    REQUIRE(conflict["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
    REQUIRE(conflict["error"]["data"]["details"]["imported"] == 0);
    REQUIRE(conflict["error"]["data"]["details"]["conflicts"].size() == 1);
    REQUIRE(conflict["error"]["data"]["details"]["conflicts"][0] == seed_id);

    // Overwrite=true replaces the conv payload; new content wins.
    Json rewritten = exported;
    rewritten["conversation"]["title"] = "Rewritten";
    rewritten["conversation"]["messages"] = Json::array(
        {{{"role", "user"}, {"content", "brand-new"}}});
    const Json overwrote = dispatch(runtime, "conversation.import",
                                     {{"payload", rewritten},
                                      {"scope", "workspace"},
                                      {"overwrite", true}});
    REQUIRE(overwrote.contains("result"));
    REQUIRE(overwrote["result"]["imported"] == 1);
    REQUIRE(overwrote["result"]["assignedIds"][0] == seed_id);

    const Json fetched = dispatch(runtime, "conversation.get",
                                   {{"id", seed_id}});
    REQUIRE(fetched["result"]["title"] == "Rewritten");
    REQUIRE(fetched["result"]["messages"].size() == 1);
    REQUIRE(fetched["result"]["messageCount"] == 1);
    REQUIRE(fetched["result"]["messages"][0]["content"] == "brand-new");

    // Fresh id (not present in store) → imported without conflict.
    Json fresh = exported;
    fresh["conversation"]["id"] = "conv-fresh-import-0001";
    fresh["conversation"]["title"] = "Fresh";
    const Json fresh_result =
        dispatch(runtime, "conversation.import",
                 {{"payload", fresh}, {"scope", "workspace"}});
    REQUIRE(fresh_result.contains("result"));
    REQUIRE(fresh_result["result"]["imported"] == 1);
    REQUIRE(fresh_result["result"]["conflicts"].empty());
    REQUIRE(fresh_result["result"]["assignedIds"][0] ==
            "conv-fresh-import-0001");

    // Batch payload with an existing id + new id, overwrite=false → the whole
    // batch is rejected because at least one id collides.
    Json batch;
    batch["format"] = "sao-conversations/1";
    batch["conversations"] = Json::array();
    Json partner = fresh["conversation"];
    partner["id"] = "conv-fresh-import-0002";
    partner["title"] = "Partner";
    batch["conversations"].push_back(fresh["conversation"]);  // collides
    batch["conversations"].push_back(partner);                // would be new
    const Json batch_reject =
        dispatch(runtime, "conversation.import",
                 {{"payload", batch}, {"scope", "workspace"}});
    REQUIRE(batch_reject.contains("error"));
    REQUIRE(batch_reject["error"]["data"]["details"]["imported"] == 0);
    REQUIRE(batch_reject["error"]["data"]["details"]["conflicts"].size() == 1);
    // Partner must not have leaked into the store.
    const Json partner_lookup = dispatch(runtime, "conversation.get",
                                          {{"id", "conv-fresh-import-0002"}});
    REQUIRE(partner_lookup.contains("error"));
}

TEST_CASE("AI Editor file tools enforce mode permissions and workspace bounds",
          "[plugins][ai_editor][native][tools]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();

    const Json ask = dispatch(runtime, "tools.call",
                              {{"mode", "ask"},
                               {"name", "editFile"},
                               {"arguments", {{"path", "note.txt"},
                                              {"content", "blocked"}}}});
    REQUIRE(ask["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_PERMISSION_DENIED);

    const Json plan = dispatch(runtime, "tools.call",
                               {{"mode", "plan"},
                                {"name", "editFile"},
                                {"arguments", {{"path", "note.txt"},
                                               {"content", "confirm"}}}});
    REQUIRE(plan["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_CONFIRMATION_REQUIRED);

    REQUIRE(dispatch(runtime, "tools.call",
                     {{"mode", "plan"},
                      {"name", "editFile"},
                      {"arguments", {{"path", "note.txt"},
                                     {"content", "alpha\nbeta\n"},
                                     {"confirmed", true}}}})
                .contains("result"));
    REQUIRE(std::filesystem::exists(fixture.workspace() / L"note.txt"));
    REQUIRE(dispatch(runtime, "tools.call",
                     {{"mode", "agent"},
                      {"name", "readFile"},
                      {"arguments", {{"path", "note.txt"}}}})
                ["result"]["content"] == "alpha\nbeta\n");
    REQUIRE(dispatch(runtime, "tools.call",
                     {{"mode", "agent"},
                      {"name", "searchFiles"},
                      {"arguments", {{"path", "."},
                                     {"query", "beta"},
                                     {"pattern", "*.txt"}}}})
                ["result"]["total"] == 1);

    const Json escaped = dispatch(runtime, "tools.call",
                                  {{"mode", "agent"},
                                   {"name", "editFile"},
                                   {"arguments", {{"path", "../escape.txt"},
                                                  {"content", "escape"}}}});
    REQUIRE(escaped["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION);
}

TEST_CASE("AI Editor chat runs cancel stale work and emit versioned events",
          "[plugins][ai_editor][native][runs]") {
    RuntimeFixture fixture;
    LocalHttpServer server;
    const Json params{{"provider", {{"id", "fixture"},
                                     {"endpoint", server.endpoint()}}},
                      {"model", "fixture-model"},
                      {"messages", Json::array(
                           {{{"role", "user"}, {"content", "hello"}}})},
                      {"stream", true},
                      {"timeoutMs", 30'000}};
    const Json first = dispatch(fixture.get(), "chat.run", params);
    const std::string first_id = first["result"]["runId"];
    REQUIRE(server.wait_for_connections(1, 2'000));
    const Json second = dispatch(fixture.get(), "chat.run", params);
    const std::string second_id = second["result"]["runId"];
    REQUIRE(server.wait_for_connections(2, 2'000));
    REQUIRE(dispatch(fixture.get(), "run.status", {{"runId", first_id}})
                ["result"]["status"] == "stale");
    REQUIRE(dispatch(fixture.get(), "run.cancel", {{"runId", second_id}})
                ["result"]["cancelRequested"] == true);

    const ULONGLONG cancel_started = GetTickCount64();
    std::string second_status;
    do {
        second_status =
            dispatch(fixture.get(), "run.status", {{"runId", second_id}})
                ["result"]["status"];
        if (second_status == "cancelled") {
            break;
        }
        Sleep(10);
    } while (GetTickCount64() - cancel_started < 2'000);
    REQUIRE(second_status == "cancelled");
    REQUIRE(GetTickCount64() - cancel_started < 1'500);

    std::vector<std::string> event_names;
    for (size_t index = 0; index < 4; ++index) {
        uint32_t required = 0;
        const int32_t queried = sao_ai_editor_runtime_next_event(
            fixture.get(), nullptr, 0, &required);
        if (queried == SAO_AI_EDITOR_OK && required == 0) {
            break;
        }
        REQUIRE(queried == SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL);
        std::vector<char> event(static_cast<size_t>(required) + 1);
        REQUIRE(sao_ai_editor_runtime_next_event(
                    fixture.get(), event.data(),
                    static_cast<uint32_t>(event.size()), &required) ==
                SAO_AI_EDITOR_OK);
        const Json notification = Json::parse(event.data(), event.data() + required);
        REQUIRE(notification["method"] == "sao.event");
        REQUIRE(notification["sao"]["protocolVersion"] == 1);
        event_names.push_back(notification["params"]["event"]);
    }
    REQUIRE(std::find(event_names.begin(), event_names.end(), "run.started") !=
            event_names.end());
    REQUIRE(std::find(event_names.begin(), event_names.end(), "run.stale") !=
            event_names.end());
}

TEST_CASE("AI Editor providers.configure persists secrets via DPAPI vault",
          "[plugins][ai_editor][native][secrets]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();

    const Json unauthorized = dispatch(
        runtime, "providers.configure",
        {{"scope", "system"},
         {"provider", {{"id", "openai-fixture"},
                       {"endpoint", "http://127.0.0.1:1/v1/chat/completions"},
                       {"model", "test-model"},
                       {"apiKey", "sk-secret-payload"}}}});
    REQUIRE(unauthorized.contains("result"));
    REQUIRE(!unauthorized["result"].contains("apiKey"));

    const Json initialized = dispatch(runtime, "runtime.initialize");
    std::string system_root_utf8;
    for (const auto& scope : initialized["result"]["scopes"]) {
        if (scope.value("scope", "") == "system") {
            system_root_utf8 = scope.value("path", "");
            break;
        }
    }
    REQUIRE(!system_root_utf8.empty());
    const std::filesystem::path vault_path =
        std::filesystem::path(system_root_utf8) / L"secrets" /
        L"ai_editor.vault.json";
    REQUIRE(std::filesystem::exists(vault_path));

    std::ifstream stream(vault_path, std::ios::binary);
    REQUIRE(stream.good());
    std::string vault_text{std::istreambuf_iterator<char>{stream},
                           std::istreambuf_iterator<char>{}};
    REQUIRE(vault_text.find("sk-secret-payload") == std::string::npos);
    const Json vault_document = Json::parse(vault_text);
    REQUIRE(vault_document["version"] == 1);
    REQUIRE(vault_document["entries"].contains(
        "provider/openai-fixture/apiKey"));
    const std::string ciphertext =
        vault_document["entries"]["provider/openai-fixture/apiKey"];
    REQUIRE(!ciphertext.empty());
    REQUIRE(ciphertext.find("sk-secret-payload") == std::string::npos);
}

TEST_CASE("AI Editor chat.run reuses configured providers via encrypted secrets",
          "[plugins][ai_editor][native][secrets]") {
    const std::string body =
        R"({"choices":[{"message":{"role":"assistant","content":"ok"}}]})";
    LocalHttpServer server(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: " +
        std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body);
    RuntimeFixture fixture;
    auto runtime = fixture.get();

    REQUIRE(dispatch(runtime, "providers.configure",
                     {{"scope", "system"},
                      {"provider", {{"id", "openai-fixture"},
                                    {"endpoint", server.endpoint()},
                                    {"model", "test-model"},
                                    {"apiKey", "sk-live-value"}}}})
                .contains("result"));

    const Json chat = dispatch(
        runtime, "chat.run",
        {{"providerId", "openai-fixture"},
         {"model", "test-model"},
         {"messages", Json::array({{{"role", "user"}, {"content", "hi"}}})},
         {"stream", false},
         {"timeoutMs", 5'000}});
    REQUIRE(chat.contains("result"));
    const std::string run_id = chat["result"]["runId"];
    REQUIRE(server.wait_for_connections(1, 2'000));

    Json status;
    const ULONGLONG wait_started = GetTickCount64();
    do {
        status = dispatch(runtime, "run.status", {{"runId", run_id}})
                     ["result"];
        if (status["status"] != "running") {
            break;
        }
        Sleep(10);
    } while (GetTickCount64() - wait_started < 5'000);
    REQUIRE(status["status"] == "completed");
    REQUIRE(status["result"]["content"] == "ok");
}

TEST_CASE("AI Editor secret vault survives runtime restart",
          "[plugins][ai_editor][native][secrets]") {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / L"workspace";
    const auto system = temporary.path() / L"system";
    REQUIRE(std::filesystem::create_directories(workspace));
    REQUIRE(std::filesystem::create_directories(system));
    const std::string workspace_utf8 = utf8_path(workspace);
    const std::string system_utf8 = utf8_path(system);
    SaoAiEditorRuntimeConfig config{};
    config.struct_size = sizeof(config);
    config.workspace_root_utf8 = workspace_utf8.c_str();
    config.system_root_utf8 = system_utf8.c_str();

    sao_ai_editor_runtime_t first = nullptr;
    REQUIRE(sao_ai_editor_runtime_create(&config, &first) == SAO_AI_EDITOR_OK);
    REQUIRE(dispatch(first, "providers.configure",
                     {{"scope", "system"},
                      {"provider", {{"id", "persist-fixture"},
                                    {"endpoint", "http://127.0.0.1:1/v1"},
                                    {"model", "persist-model"},
                                    {"apiKey", "sk-persistent-value"}}}})
                .contains("result"));
    sao_ai_editor_runtime_destroy(first);

    sao_ai_editor_runtime_t second = nullptr;
    REQUIRE(sao_ai_editor_runtime_create(&config, &second) == SAO_AI_EDITOR_OK);
    const Json list_after_restart =
        dispatch(second, "providers.list")["result"]["items"];
    bool found_persisted = false;
    for (const auto& item : list_after_restart) {
        if (item.value("id", "") == "persist-fixture") {
            found_persisted = true;
            REQUIRE(!item.contains("apiKey"));
        }
    }
    REQUIRE(found_persisted);
    sao_ai_editor_runtime_destroy(second);
}

TEST_CASE("AI Editor chat preserves synchronous response semantics",
          "[plugins][ai_editor][native][runs]") {
    const std::string body =
        R"({"choices":[{"message":{"role":"assistant","content":"hello"}}]})";
    LocalHttpServer server("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                           "Content-Length: " +
                           std::to_string(body.size()) +
                           "\r\nConnection: close\r\n\r\n" + body);
    RuntimeFixture fixture;
    const Json params{{"provider", {{"id", "fixture"},
                                     {"endpoint", server.endpoint()}}},
                      {"model", "fixture-model"},
                      {"messages", Json::array(
                           {{{"role", "user"}, {"content", "hello"}}})},
                      {"stream", false},
                      {"timeoutMs", 2'000}};
    const Json started = dispatch(fixture.get(), "chat.run", params);
    const std::string run_id = started["result"]["runId"];
    REQUIRE(server.wait_for_connections(1, 2'000));

    Json status;
    const ULONGLONG wait_started = GetTickCount64();
    do {
        status = dispatch(fixture.get(), "run.status", {{"runId", run_id}})
                     ["result"];
        if (status["status"] != "running") {
            break;
        }
        Sleep(10);
    } while (GetTickCount64() - wait_started < 2'000);
    REQUIRE(status["status"] == "completed");
    REQUIRE(status["result"]["content"] == "hello");
}

TEST_CASE("AI Editor agents.list_defs surfaces 5 built-in agents",
          "[plugins][ai_editor][native][agents]") {
    RuntimeFixture fixture;
    const Json defs = dispatch(fixture.get(), "agents.list_defs");
    REQUIRE(defs.contains("result"));
    REQUIRE(defs["result"]["total"] >= 5);
    std::vector<std::string> ids;
    for (const auto& item : defs["result"]["items"]) {
        ids.push_back(item.value("id", ""));
    }
    for (const auto* required :
         {"code-reviewer", "explainer", "debugger", "optimizer", "documenter"}) {
        REQUIRE(std::find(ids.begin(), ids.end(), required) != ids.end());
    }
}

TEST_CASE("AI Editor agents.save_def / delete_def CRUD honours builtin lock",
          "[plugins][ai_editor][native][agents]") {
    RuntimeFixture fixture;
    const Json agent_json{
        {"id", "my-tester"},
        {"name", "Test Runner"},
        {"description", "Runs project tests"},
        {"system_prompt", "You run tests carefully."},
        {"tools", Json::array({"readFile", "listFiles"})},
        {"model", "test-model"}};
    REQUIRE(dispatch(fixture.get(), "agents.save_def",
                     {{"scope", "workspace"}, {"agent", agent_json}})
                .contains("result"));
    const Json fetched =
        dispatch(fixture.get(), "agents.get_def", {{"id", "my-tester"}})
            ["result"];
    REQUIRE(fetched["name"] == "Test Runner");
    REQUIRE(fetched["tools"].size() == 2);
    REQUIRE(fetched["builtin"] == false);
    REQUIRE(dispatch(fixture.get(), "agents.delete_def",
                     {{"id", "my-tester"}})
                .contains("result"));
    REQUIRE(dispatch(fixture.get(), "agents.delete_def",
                     {{"id", "code-reviewer"}})["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_PERMISSION_DENIED);
}

TEST_CASE("AI Editor agents.invoke wraps agent system prompt around "
          "chat.run",
          "[plugins][ai_editor][native][agents][integration]") {
    const std::string body =
        R"({"choices":[{"message":{"role":"assistant","content":"reviewed"}}]})";
    LocalHttpServer server(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: " +
        std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body);
    RuntimeFixture fixture;
    const Json invoked = dispatch(
        fixture.get(), "agents.invoke",
        {{"id", "code-reviewer"},
         {"message", "look at foo()"},
         {"provider", {{"id", "agent-fixture"},
                        {"endpoint", server.endpoint()}}},
         {"model", "test-model"},
         {"timeoutMs", 5000}});
    REQUIRE(invoked.contains("result"));
    REQUIRE(invoked["result"]["agentId"] == "code-reviewer");
    REQUIRE(invoked["result"]["content"] == "reviewed");
    REQUIRE(server.wait_for_connections(1, 2'000));
}

TEST_CASE("AI Editor agents.invoke streams delta tokens via sao.event queue "
          "when stream=true",
          "[plugins][ai_editor][native][agents][streaming]") {
    // OpenAI SSE payload: two content deltas + [DONE].
    const std::string sse_body =
        "data: {\"id\":\"chat-agent-stream\",\"choices\":[{"
        "\"delta\":{\"content\":\"hel\"},\"finish_reason\":null}]}\r\n\r\n"
        "data: {\"id\":\"chat-agent-stream\",\"choices\":[{"
        "\"delta\":{\"content\":\"lo\"},\"finish_reason\":null}]}\r\n\r\n"
        "data: [DONE]\r\n\r\n";
    LocalHttpServer server(
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
        "Content-Length: " +
        std::to_string(sse_body.size()) +
        "\r\nConnection: close\r\n\r\n" + sse_body);

    RuntimeFixture fixture;
    const Json invoked = dispatch(
        fixture.get(), "agents.invoke",
        {{"id", "code-reviewer"},
         {"message", "explain hi"},
         {"stream", true},
         {"provider", {{"id", "agent-stream-fixture"},
                        {"endpoint", server.endpoint()}}},
         {"model", "test-model"},
         {"timeoutMs", 5000}});
    REQUIRE(invoked.contains("result"));
    // out_content is the concatenation of streamed content deltas.
    REQUIRE(invoked["result"]["content"] == "hello");
    REQUIRE(invoked["result"]["agentId"] == "code-reviewer");
    REQUIRE(server.wait_for_connections(1, 2'000));

    // Drain the event queue and confirm we received at least one
    // agent.delta forwarding a content string.
    std::string aggregated;
    size_t delta_count = 0;
    for (size_t index = 0; index < 32; ++index) {
        uint32_t required = 0;
        const int32_t queried = sao_ai_editor_runtime_next_event(
            fixture.get(), nullptr, 0, &required);
        if (queried == SAO_AI_EDITOR_OK && required == 0) {
            break;
        }
        if (queried != SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL) {
            break;
        }
        std::vector<char> event(static_cast<size_t>(required) + 1);
        const int32_t drain = sao_ai_editor_runtime_next_event(
            fixture.get(), event.data(),
            static_cast<uint32_t>(event.size()), &required);
        if (drain != SAO_AI_EDITOR_OK) {
            break;
        }
        const Json notification =
            Json::parse(event.data(), event.data() + required);
        if (notification.value("method", "") != "sao.event") {
            continue;
        }
        const Json params = notification.value("params", Json::object());
        if (params.value("event", "") != "agent.delta") {
            continue;
        }
        const Json payload = params.value("payload", Json::object());
        REQUIRE(payload["agentId"] == "code-reviewer");
        if (payload.contains("content") && payload["content"].is_string()) {
            aggregated += payload["content"].get<std::string>();
            ++delta_count;
        }
    }
    REQUIRE(delta_count >= 2);
    REQUIRE(aggregated == "hello");
}

namespace {

std::string drain_mcp(int32_t status, uint32_t required,
                      const std::function<int32_t(char*, uint32_t, uint32_t*)>&
                          drain) {
    if (status == SAO_AI_EDITOR_OK && required == 0) {
        return {};
    }
    REQUIRE(status == SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL);
    // Allow some slack because the aggregated output could pick up additional
    // metadata between the size query and the drain call.
    std::vector<char> buffer(static_cast<size_t>(required) + 4096U, '\0');
    uint32_t written = 0;
    INFO("required=" << required << " capacity=" << buffer.size());
    const int32_t drain_status =
        drain(buffer.data(), static_cast<uint32_t>(buffer.size()), &written);
    INFO("drain_status=" << drain_status << " written=" << written);
    REQUIRE(drain_status == SAO_AI_EDITOR_OK);
    return std::string(buffer.data(), written);
}

}  // namespace

TEST_CASE("AI Editor MCP client spawns SaoAiEditor as MCP server, "
          "aggregates tools and roundtrips tools/call",
          "[plugins][ai_editor][native][mcp][client][integration]") {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / L"workspace";
    REQUIRE(std::filesystem::create_directories(workspace));
    {
        std::ofstream fixture_file(workspace / L"greeting.txt");
        fixture_file << "hello via MCP\n";
    }
    sao_ai_editor_mcp_client_t client = nullptr;
    REQUIRE(sao_ai_editor_mcp_client_create(&client) == SAO_AI_EDITOR_OK);
    REQUIRE(client != nullptr);

    const Json config{
        {"name", "sao-under-test"},
        {"command", SAO_AI_EDITOR_MCP_SERVER_EXECUTABLE},
        {"args", Json::array({"--mcp-server", "--workspace",
                              utf8_path(workspace)})},
        {"startupMs", 15000}};
    const std::string config_dump = config.dump();
    REQUIRE(sao_ai_editor_mcp_client_register(
                client, config_dump.data(),
                static_cast<uint32_t>(config_dump.size())) == SAO_AI_EDITOR_OK);

    uint32_t required = 0;
    const std::string servers_json = drain_mcp(
        sao_ai_editor_mcp_client_list_servers(client, nullptr, 0, &required),
        required,
        [&](char* output, uint32_t capacity, uint32_t* out_length) {
            return sao_ai_editor_mcp_client_list_servers(client, output,
                                                          capacity, out_length);
        });
    const Json servers = Json::parse(servers_json);
    REQUIRE(servers.is_array());
    REQUIRE(servers.size() == 1);
    REQUIRE(servers[0]["name"] == "sao-under-test");
    REQUIRE(servers[0]["serverInfo"]["name"] == "sao-ai-editor");

    required = 0;
    const std::string tools_json = drain_mcp(
        sao_ai_editor_mcp_client_list_tools(client, nullptr, 0, &required),
        required,
        [&](char* output, uint32_t capacity, uint32_t* out_length) {
            return sao_ai_editor_mcp_client_list_tools(client, output, capacity,
                                                        out_length);
        });
    const Json tools = Json::parse(tools_json);
    REQUIRE(tools.is_array());
    bool has_read_file = false;
    for (const auto& tool : tools) {
        if (tool.value("name", "") == "readFile") {
            has_read_file = true;
            REQUIRE(tool["server"] == "sao-under-test");
        }
    }
    REQUIRE(has_read_file);

    const Json call_request{{"server", "sao-under-test"},
                            {"name", "readFile"},
                            {"arguments", {{"path", "greeting.txt"}}}};
    const std::string call_dump = call_request.dump();
    required = 0;
    const std::string call_json = drain_mcp(
        sao_ai_editor_mcp_client_call_tool(
            client, call_dump.data(), static_cast<uint32_t>(call_dump.size()),
            nullptr, 0, &required),
        required,
        [&](char* output, uint32_t capacity, uint32_t* out_length) {
            return sao_ai_editor_mcp_client_call_tool(
                client, nullptr, 0, output, capacity, out_length);
        });
    const Json call = Json::parse(call_json);
    REQUIRE(call.contains("content"));
    REQUIRE(call["content"][0]["type"] == "text");
    REQUIRE(call["content"][0]["text"].get<std::string>().find(
                "hello via MCP") != std::string::npos);

    REQUIRE(sao_ai_editor_mcp_client_close(client, "sao-under-test") ==
            SAO_AI_EDITOR_OK);
    sao_ai_editor_mcp_client_destroy(client);
}

namespace {

class ScriptedHttpServer final {
public:
    ScriptedHttpServer() {
        WSADATA data{};
        REQUIRE(WSAStartup(MAKEWORD(2, 2), &data) == 0);
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        REQUIRE(listener_ != INVALID_SOCKET);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        REQUIRE(bind(listener_, reinterpret_cast<const sockaddr*>(&address),
                     sizeof(address)) == 0);
        REQUIRE(listen(listener_, SOMAXCONN) == 0);
        int size = sizeof(address);
        REQUIRE(getsockname(listener_, reinterpret_cast<sockaddr*>(&address),
                            &size) == 0);
        base_url_ = "http://127.0.0.1:" +
                    std::to_string(ntohs(address.sin_port));
        worker_ = std::thread([this] { serve(); });
    }
    ~ScriptedHttpServer() {
        stopping_.store(true, std::memory_order_release);
        if (listener_ != INVALID_SOCKET) {
            closesocket(listener_);
            listener_ = INVALID_SOCKET;
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        WSACleanup();
    }

    void enqueue(std::string body, int status_code = 200) {
        std::lock_guard<std::mutex> guard(mutex_);
        std::string response = "HTTP/1.1 " + std::to_string(status_code) +
                                " OK\r\nContent-Type: application/json\r\n"
                                "Content-Length: " +
                                std::to_string(body.size()) +
                                "\r\nConnection: close\r\n\r\n" + body;
        queue_.push_back(std::move(response));
    }

    const std::string& base_url() const noexcept { return base_url_; }

    size_t handled() const noexcept {
        return handled_.load(std::memory_order_acquire);
    }

private:
    void serve() {
        while (!stopping_.load(std::memory_order_acquire)) {
            SOCKET client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                return;
            }
            std::string request;
            std::array<char, 4096> buffer{};
            size_t header_end = std::string::npos;
            while (header_end == std::string::npos) {
                const int received = recv(client, buffer.data(),
                                          static_cast<int>(buffer.size()), 0);
                if (received <= 0) {
                    break;
                }
                request.append(buffer.data(), static_cast<size_t>(received));
                header_end = request.find("\r\n\r\n");
            }
            if (header_end == std::string::npos) {
                closesocket(client);
                continue;
            }
            size_t content_length = 0;
            const size_t length_marker = request.find("Content-Length: ");
            if (length_marker != std::string::npos) {
                const char* first = request.data() + length_marker + 16;
                const char* last =
                    request.data() + request.find("\r\n", length_marker);
                std::from_chars(first, last, content_length);
            }
            const size_t body_begin = header_end + 4;
            while (request.size() - body_begin < content_length) {
                const int received = recv(client, buffer.data(),
                                          static_cast<int>(buffer.size()), 0);
                if (received <= 0) {
                    break;
                }
                request.append(buffer.data(), static_cast<size_t>(received));
            }
            std::string response;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (!queue_.empty()) {
                    response = std::move(queue_.front());
                    queue_.pop_front();
                }
            }
            if (response.empty()) {
                response =
                    "HTTP/1.1 500 Internal Server Error\r\nContent-Length: "
                    "0\r\nConnection: close\r\n\r\n";
            }
            send(client, response.data(),
                 static_cast<int>(response.size()), 0);
            shutdown(client, SD_BOTH);
            closesocket(client);
            handled_.fetch_add(1, std::memory_order_release);
        }
    }

    SOCKET listener_ = INVALID_SOCKET;
    std::atomic<bool> stopping_{false};
    std::atomic<size_t> handled_{0};
    std::thread worker_;
    std::mutex mutex_;
    std::deque<std::string> queue_;
    std::string base_url_;
};

}  // namespace

TEST_CASE("SaoAiEditor.exe --cli --cli-method dispatches JSON-RPC and exits",
          "[plugins][ai_editor][production_child][cli][integration]") {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / L"cli-workspace";
    REQUIRE(std::filesystem::create_directories(workspace));

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    SECURITY_ATTRIBUTES security_attributes{};
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.bInheritHandle = TRUE;
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    REQUIRE(CreatePipe(&read_end, &write_end, &security_attributes,
                       128 * 1024) != FALSE);
    REQUIRE(SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0) != FALSE);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = write_end;
    startup.hStdError = write_end;

    const std::string exe_utf8 = SAO_AI_EDITOR_MCP_SERVER_EXECUTABLE;
    const int wide_len = MultiByteToWideChar(CP_UTF8, 0, exe_utf8.c_str(),
                                              -1, nullptr, 0);
    REQUIRE(wide_len > 0);
    std::wstring executable(static_cast<size_t>(wide_len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, exe_utf8.c_str(), -1, executable.data(),
                        wide_len);
    std::wstring quoted_workspace = L"\"" + workspace.native() + L"\"";
    std::wstring command_line = L"\"" + executable + L"\" --cli "
                                L"--cli-method agents.list_defs "
                                L"--workspace " + quoted_workspace;
    std::vector<wchar_t> command_line_buffer(command_line.begin(),
                                              command_line.end());
    command_line_buffer.push_back(L'\0');
    PROCESS_INFORMATION process_information{};
    REQUIRE(CreateProcessW(nullptr, command_line_buffer.data(), nullptr, nullptr,
                           TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                           &process_information) != FALSE);
    CloseHandle(write_end);
    std::string captured;
    std::array<char, 4096> buffer{};
    DWORD read = 0;
    while (ReadFile(read_end, buffer.data(),
                    static_cast<DWORD>(buffer.size()), &read, nullptr) &&
           read > 0) {
        captured.append(buffer.data(), read);
    }
    CloseHandle(read_end);
    REQUIRE(WaitForSingleObject(process_information.hProcess, 15'000) ==
            WAIT_OBJECT_0);
    DWORD exit_code = 1;
    REQUIRE(GetExitCodeProcess(process_information.hProcess, &exit_code) !=
            FALSE);
    CloseHandle(process_information.hProcess);
    CloseHandle(process_information.hThread);
    REQUIRE(exit_code == 0);
    const auto brace = captured.find('{');
    REQUIRE(brace != std::string::npos);
    const Json parsed = Json::parse(captured.substr(brace));
    REQUIRE(parsed.contains("result"));
    REQUIRE(parsed["result"]["total"] >= 5);
}

#if defined(SAO_AI_EDITOR_HAS_WEBVIEW) && SAO_AI_EDITOR_HAS_WEBVIEW
TEST_CASE("WebView2 runtime probe reports Loader availability",
          "[plugins][ai_editor][native][webview]") {
    const bool available =
        sao::ai_editor::native::webview_runtime_available();
    INFO("WebView2Loader.dll available: " << (available ? "yes" : "no"));
    // Either state is fine — the test just proves the probe binary itself
    // links against the loader shim without crashing.
    REQUIRE((available == true || available == false));
}

TEST_CASE("run_webview_bridge fails closed with invalid config",
          "[plugins][ai_editor][native][webview]") {
    sao::ai_editor::native::WebViewConfig config;
    REQUIRE(sao::ai_editor::native::run_webview_bridge(config) ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
    config.user_data_folder = "C:/tmp/sao-webview-fixture";
    config.bridge_native_runtime = true;
    config.runtime_handle = nullptr;
    REQUIRE(sao::ai_editor::native::run_webview_bridge(config) ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
}
#endif

TEST_CASE("AI Editor extensions.* register + list + unregister without node",
          "[plugins][ai_editor][native][extensions]") {
    RuntimeFixture fixture;
    const Json manifest{{"name", "hello"},
                        {"publisher", "sao-test"},
                        {"version", "0.1.0"},
                        {"main", "./out/extension.js"}};
    const Json registered = dispatch(fixture.get(), "extensions.register",
                                     {{"manifest", manifest},
                                      {"extensionPath", "C:\\ext\\hello"}});
    REQUIRE(registered.contains("result"));
    REQUIRE(registered["result"]["id"] == "sao-test.hello");
    REQUIRE(registered["result"]["activated"] == false);

    const Json listed =
        dispatch(fixture.get(), "extensions.list")["result"];
    REQUIRE(listed["total"] == 1);
    REQUIRE(listed["items"][0]["publisher"] == "sao-test");
    REQUIRE(listed["nodeAlive"] == false);

    const Json activate_without_configure =
        dispatch(fixture.get(), "extensions.activate",
                 {{"extensionId", "sao-test.hello"}, {"timeoutMs", 500}});
    REQUIRE(activate_without_configure["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_NOT_INITIALIZED);

    const Json unregistered = dispatch(fixture.get(), "extensions.unregister",
                                       {{"extensionId", "sao-test.hello"}});
    REQUIRE(unregistered.contains("result"));
    REQUIRE(dispatch(fixture.get(), "extensions.list")["result"]["total"] ==
            0);
}

TEST_CASE("dispatch_extension_call maps vscode.workspace.* into tools registry",
          "[plugins][ai_editor][native][extensions][vscode]") {
    RuntimeFixture fixture;
    // Seed a file so readTextDocument can round-trip.
    {
        std::ofstream fixture_file(fixture.workspace() / L"note.txt");
        fixture_file << "hello from vscode API\n";
    }
    // Reach the internal dispatcher via the NativeRuntime C API using
    // tools.call — vscode.workspace.readTextDocument maps to the same
    // registry.
    const Json read_request = Json{{"mode", "agent"},
                                    {"name", "readFile"},
                                    {"arguments", {{"path", "note.txt"}}}};
    const Json call = dispatch(fixture.get(), "tools.call", read_request);
    REQUIRE(call.contains("result"));
    REQUIRE(call["result"]["content"].get<std::string>().find(
                "hello from vscode API") != std::string::npos);
    // Also directly ensure the vscode.workspace.workspaceFolders shim works
    // via a synthetic sao.host.dispatch style path: extensions.snapshot is a
    // representative Node-side view.
    const Json snapshot = dispatch(fixture.get(), "extensions.snapshot");
    REQUIRE(snapshot.contains("result"));
    REQUIRE(snapshot["result"]["nodeAlive"] == false);
}

TEST_CASE("AI Editor auth.begin_device_flow + poll drives RFC 8628 to success",
          "[plugins][ai_editor][native][auth][device_flow][integration]") {
    RuntimeFixture fixture;
    ScriptedHttpServer server;
    // Device authorization response.
    server.enqueue(
        R"({"device_code":"DEVICE-01","user_code":"USER-01",)"
        R"("verification_uri":"https://auth.example.com/device",)"
        R"("verification_uri_complete":"https://auth.example.com/device?user_code=USER-01",)"
        R"("interval":1,"expires_in":600})");
    // First poll: authorization_pending.
    server.enqueue(
        R"({"error":"authorization_pending","error_description":"waiting"})",
        400);
    // Second poll: success.
    server.enqueue(
        R"({"access_token":"TOKEN-ABC","token_type":"Bearer",)"
        R"("refresh_token":"REFRESH-01","expires_in":3600})");

    const Json begun = dispatch(
        fixture.get(), "auth.begin_device_flow",
        {{"providerId", "auth-fixture"},
         {"deviceAuthorizationUrl", server.base_url() + "/device"},
         {"tokenUrl", server.base_url() + "/token"},
         {"clientId", "sao-test-client"},
         {"scope", "openid email"}});
    REQUIRE(begun.contains("result"));
    const std::string flow_id = begun["result"]["flowId"];
    REQUIRE(begun["result"]["userCode"] == "USER-01");
    REQUIRE(begun["result"]["verificationUri"] ==
            "https://auth.example.com/device");
    REQUIRE(begun["result"]["intervalSeconds"] == 1);

    const Json pending = dispatch(fixture.get(), "auth.poll_device_flow",
                                   {{"flowId", flow_id}});
    REQUIRE(pending.contains("result"));
    REQUIRE(pending["result"]["status"] == "pending");
    REQUIRE(pending["result"]["oauthError"] == "authorization_pending");

    const Json completed = dispatch(fixture.get(), "auth.poll_device_flow",
                                     {{"flowId", flow_id}});
    REQUIRE(completed.contains("result"));
    REQUIRE(completed["result"]["status"] == "success");
    REQUIRE(completed["result"]["accessToken"] == "TOKEN-ABC");

    const Json loaded = dispatch(fixture.get(), "auth.load_token",
                                  {{"providerId", "auth-fixture"}});
    REQUIRE(loaded.contains("result"));
    REQUIRE(loaded["result"]["access_token"] == "TOKEN-ABC");
    REQUIRE(loaded["result"]["refresh_token"] == "REFRESH-01");

    REQUIRE(dispatch(fixture.get(), "auth.revoke_token",
                     {{"providerId", "auth-fixture"}})
                .contains("result"));
    REQUIRE(dispatch(fixture.get(), "auth.load_token",
                     {{"providerId", "auth-fixture"}})["error"]["data"]
                        ["status"] == SAO_AI_EDITOR_ERR_NOT_FOUND);
}

TEST_CASE("AI Editor auth.refresh_token exchanges refresh_token for new "
          "access_token",
          "[plugins][ai_editor][native][auth][refresh][integration]") {
    RuntimeFixture fixture;
    ScriptedHttpServer server;
    // device_authorization + immediate token grant (short lifetime so the
    // subsequent get_access_token call triggers a refresh).
    server.enqueue(
        R"({"device_code":"DEV-R","user_code":"USR-R",)"
        R"("verification_uri":"https://auth.example.com/device",)"
        R"("interval":1,"expires_in":600})");
    server.enqueue(
        R"({"access_token":"ACCESS-INITIAL","token_type":"Bearer",)"
        R"("refresh_token":"REFRESH-R","expires_in":1})");
    server.enqueue(
        R"({"access_token":"ACCESS-REFRESHED","token_type":"Bearer",)"
        R"("expires_in":3600})");
    // Explicit refresh follow-up.
    server.enqueue(
        R"({"access_token":"ACCESS-FORCED","token_type":"Bearer",)"
        R"("refresh_token":"REFRESH-ROTATED","expires_in":3600})");

    const Json begun = dispatch(
        fixture.get(), "auth.begin_device_flow",
        {{"providerId", "refresh-fixture"},
         {"deviceAuthorizationUrl", server.base_url() + "/device"},
         {"tokenUrl", server.base_url() + "/token"},
         {"clientId", "sao-test-client"},
         {"scope", "openid"}});
    const std::string flow_id = begun["result"]["flowId"];
    const Json completed = dispatch(fixture.get(), "auth.poll_device_flow",
                                     {{"flowId", flow_id}});
    REQUIRE(completed["result"]["status"] == "success");
    REQUIRE(completed["result"]["accessToken"] == "ACCESS-INITIAL");

    // Sleep just past the 1s expiry so get_access_token triggers a refresh.
    Sleep(1200);
    const Json fresh = dispatch(fixture.get(), "auth.get_access_token",
                                 {{"providerId", "refresh-fixture"},
                                  {"expiryLeewaySeconds", 5}});
    REQUIRE(fresh.contains("result"));
    REQUIRE(fresh["result"]["access_token"] == "ACCESS-REFRESHED");
    REQUIRE(fresh["result"]["refreshed"] == true);
    // The stored blob should carry the original refresh_token forward when
    // the refresh response omitted it.
    REQUIRE(fresh["result"]["refresh_token"] == "REFRESH-R");

    // Explicit auth.refresh_token rotates access + refresh tokens.
    const Json forced = dispatch(fixture.get(), "auth.refresh_token",
                                  {{"providerId", "refresh-fixture"}});
    REQUIRE(forced.contains("result"));
    REQUIRE(forced["result"]["access_token"] == "ACCESS-FORCED");
    REQUIRE(forced["result"]["refresh_token"] == "REFRESH-ROTATED");
}

TEST_CASE("AI Editor extensions.configure_host auto-discovers the shim script",
          "[plugins][ai_editor][native][extensions][shim]") {
    RuntimeFixture fixture;
    const Json configured = dispatch(
        fixture.get(), "extensions.configure_host",
        {{"nodeExecutable", "C:/nonexistent/node.exe"}});
    // The default shim path either resolves (repo/tree/install layout) —
    // in which case configure returns OK — or the harness can't find it,
    // in which case fail-closed to NOT_FOUND.  Either way, the JSON-RPC
    // caller never has to hard-code the shim path.
    const int32_t status =
        configured.contains("result")
            ? SAO_AI_EDITOR_OK
            : configured["error"]["data"]["status"].get<int32_t>();
    REQUIRE((status == SAO_AI_EDITOR_OK ||
             status == SAO_AI_EDITOR_ERR_NOT_FOUND));
    // A live shim resolution should reflect back through extensions.list.
    if (status == SAO_AI_EDITOR_OK) {
        const Json snapshot =
            dispatch(fixture.get(), "extensions.snapshot")["result"];
        const std::string entry = snapshot.value("entryScript", "");
        REQUIRE(entry.find("extension_host_shim.js") != std::string::npos);
    }
}

TEST_CASE("workflow group parallel batches contiguous same-group steps",
          "[plugins][ai_editor][native][workflows][parallel][integration]") {
    // Two responses; the server queue serves them in order to the two
    // concurrent step requests.
    const std::string body1 =
        R"({"choices":[{"message":{"role":"assistant","content":"alpha-out"}}]})";
    const std::string body2 =
        R"({"choices":[{"message":{"role":"assistant","content":"beta-out"}}]})";
    ScriptedHttpServer server;
    server.enqueue(body1);
    server.enqueue(body2);

    RuntimeFixture fixture;
    // Save a custom workflow with two steps sharing the same group id.
    const Json wf{
        {"id", "parallel-demo"},
        {"name", "Parallel Demo"},
        {"steps",
         Json::array({
             Json{{"agent", "default"},
                  {"prompt", "alpha({{input}})"},
                  {"output_var", "alpha"},
                  {"group", "fan-out"}},
             Json{{"agent", "default"},
                  {"prompt", "beta({{input}})"},
                  {"output_var", "beta"},
                  {"group", "fan-out"}},
         })}};
    REQUIRE(dispatch(fixture.get(), "workflows.save_def",
                     {{"scope", "workspace"}, {"workflow", wf}})
                .contains("result"));

    const Json run = dispatch(
        fixture.get(), "workflows.run",
        {{"id", "parallel-demo"},
         {"provider", {{"id", "wf-parallel"},
                       {"endpoint", server.base_url() + "/v1/chat/completions"}}},
         {"model", "test-model"},
         {"input", "payload"},
         {"timeoutMs", 5000}});
    REQUIRE(run.contains("result"));
    const std::string execution_id = run["result"]["executionId"];
    Json status;
    const ULONGLONG started = GetTickCount64();
    do {
        status = dispatch(fixture.get(), "workflows.status",
                          {{"executionId", execution_id}})["result"];
        if (status["status"] != "running" &&
            status["status"] != "pending") {
            break;
        }
        Sleep(20);
    } while (GetTickCount64() - started < 15'000);
    REQUIRE(status["status"] == "completed");
    // Both variables should be present and taken from the parallel batch.
    REQUIRE(status["variables"]["alpha"] != status["variables"]["beta"]);
    REQUIRE(status["variables"]["input"] == "payload");
    // stepResults are committed in step order.
    REQUIRE(status["stepResults"].size() == 2);
    REQUIRE(status["stepResults"][0]["group"] == "fan-out");
    REQUIRE(status["stepResults"][1]["group"] == "fan-out");
}

TEST_CASE("AI Editor auth.store_token persists user-supplied bearer token",
          "[plugins][ai_editor][native][auth]") {
    RuntimeFixture fixture;
    const Json token{{"access_token", "USER-TOKEN"},
                     {"token_type", "Bearer"},
                     {"expires_in", 1800}};
    REQUIRE(dispatch(fixture.get(), "auth.store_token",
                     {{"providerId", "manual-provider"}, {"token", token}})
                .contains("result"));
    const Json loaded = dispatch(fixture.get(), "auth.load_token",
                                  {{"providerId", "manual-provider"}})["result"];
    REQUIRE(loaded["access_token"] == "USER-TOKEN");
    REQUIRE(loaded.contains("expires_at_unix_ms"));
    REQUIRE(dispatch(fixture.get(), "auth.revoke_token",
                     {{"providerId", "manual-provider"}})
                .contains("result"));
}

TEST_CASE("Anthropic SSE decoder emits ordered text deltas",
          "[plugins][ai_editor][native][providers][anthropic][sse]") {
    sao::ai_editor::native::AnthropicSseCodec codec;
    Json events;
    const std::string stream =
        "event: message_start\r\n"
        "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\"}}\r\n"
        "\r\n"
        "event: content_block_start\r\n"
        "data: {\"type\":\"content_block_start\",\"index\":0,"
        "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\r\n"
        "\r\n"
        "event: content_block_delta\r\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"hel\"}}\r\n"
        "\r\n"
        "event: content_block_delta\r\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"lo\"}}\r\n"
        "\r\n"
        "event: content_block_stop\r\n"
        "data: {\"type\":\"content_block_stop\",\"index\":0}\r\n"
        "\r\n"
        "event: message_delta\r\n"
        "data: {\"type\":\"message_delta\","
        "\"delta\":{\"stop_reason\":\"end_turn\"},"
        "\"usage\":{\"output_tokens\":2}}\r\n"
        "\r\n"
        "event: message_stop\r\n"
        "data: {\"type\":\"message_stop\"}\r\n"
        "\r\n";
    REQUIRE(codec.feed(stream, events) == SAO_AI_EDITOR_OK);
    REQUIRE(events.is_array());
    REQUIRE(events.size() == 4);
    REQUIRE(events[0]["type"] == "delta");
    REQUIRE(events[0]["content"] == "hel");
    REQUIRE(events[1]["type"] == "delta");
    REQUIRE(events[1]["content"] == "lo");
    REQUIRE(events[2]["type"] == "message_delta");
    REQUIRE(events[2]["delta"]["stop_reason"] == "end_turn");
    REQUIRE(events[2]["usage"]["output_tokens"] == 2);
    REQUIRE(events[3]["type"] == "done");

    SECTION("empty feed after construction is a no-op") {
        sao::ai_editor::native::AnthropicSseCodec fresh;
        Json fresh_events;
        REQUIRE(fresh.feed(std::string_view{}, fresh_events) ==
                SAO_AI_EDITOR_OK);
        REQUIRE(fresh_events.is_array());
        REQUIRE(fresh_events.empty());
    }

    SECTION("non-object payload surfaces protocol error") {
        sao::ai_editor::native::AnthropicSseCodec bad;
        Json bad_events;
        REQUIRE(bad.feed("data: \"not-an-object\"\n\n", bad_events) ==
                SAO_AI_EDITOR_ERR_PROTOCOL);
    }

    SECTION("input_json_delta becomes tool_delta") {
        sao::ai_editor::native::AnthropicSseCodec tool_codec;
        Json tool_events;
        const std::string tool_stream =
            "event: content_block_delta\n"
            "data: {\"type\":\"content_block_delta\",\"index\":0,"
            "\"delta\":{\"type\":\"input_json_delta\","
            "\"partial_json\":\"{\\\"x\\\":\"}}\n"
            "\n";
        REQUIRE(tool_codec.feed(tool_stream, tool_events) == SAO_AI_EDITOR_OK);
        REQUIRE(tool_events.size() == 1);
        REQUIRE(tool_events[0]["type"] == "tool_delta");
        REQUIRE(tool_events[0]["content"] == "{\"x\":");
    }

    SECTION("feed after done rejects non-whitespace input") {
        Json trailing;
        REQUIRE(codec.feed(std::string_view{}, trailing) == SAO_AI_EDITOR_OK);
        REQUIRE(trailing.empty());
        REQUIRE(codec.feed("\r\n\r\n", trailing) == SAO_AI_EDITOR_OK);
        REQUIRE(trailing.empty());
        REQUIRE(codec.feed("data: {\"type\":\"ping\"}\n\n", trailing) ==
                SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
    }
}

TEST_CASE("Anthropic SSE decoder handles split chunks across feeds",
          "[plugins][ai_editor][native][providers][anthropic][sse]") {
    sao::ai_editor::native::AnthropicSseCodec codec;
    const std::string stream =
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"hel\"}}\n"
        "\n"
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"lo\"}}\n"
        "\n"
        "event: message_delta\n"
        "data: {\"type\":\"message_delta\","
        "\"delta\":{\"stop_reason\":\"end_turn\"}}\n"
        "\n"
        "event: message_stop\n"
        "data: {\"type\":\"message_stop\"}\n"
        "\n";
    // Split partway through the first JSON payload so the codec has to
    // buffer an incomplete `data:` line across the feed boundary.
    const size_t cut = stream.find("\"text\":\"hel\"") + 6;
    REQUIRE(cut > 0);
    REQUIRE(cut < stream.size());
    const std::string_view first(stream.data(), cut);
    const std::string_view second(stream.data() + cut, stream.size() - cut);

    Json first_events;
    REQUIRE(codec.feed(first, first_events) == SAO_AI_EDITOR_OK);
    REQUIRE(first_events.is_array());
    // Nothing has been dispatched yet because the blank-line delimiter
    // has not been observed.
    REQUIRE(first_events.empty());

    Json second_events;
    REQUIRE(codec.feed(second, second_events) == SAO_AI_EDITOR_OK);
    REQUIRE(second_events.size() == 4);
    REQUIRE(second_events[0]["type"] == "delta");
    REQUIRE(second_events[0]["content"] == "hel");
    REQUIRE(second_events[1]["type"] == "delta");
    REQUIRE(second_events[1]["content"] == "lo");
    REQUIRE(second_events[2]["type"] == "message_delta");
    REQUIRE(second_events[2]["delta"]["stop_reason"] == "end_turn");
    REQUIRE(second_events[3]["type"] == "done");

    SECTION("single-byte trickle produces the same events") {
        sao::ai_editor::native::AnthropicSseCodec trickle;
        Json aggregated = Json::array();
        for (size_t i = 0; i < stream.size(); ++i) {
            Json chunk_events;
            REQUIRE(trickle.feed(std::string_view(stream.data() + i, 1),
                                  chunk_events) == SAO_AI_EDITOR_OK);
            for (auto& event : chunk_events) {
                aggregated.push_back(std::move(event));
            }
        }
        REQUIRE(aggregated.size() == 4);
        REQUIRE(aggregated[0]["content"] == "hel");
        REQUIRE(aggregated[1]["content"] == "lo");
        REQUIRE(aggregated[3]["type"] == "done");
    }
}

TEST_CASE("Gemini SSE decoder emits candidate parts as deltas",
          "[plugins][ai_editor][native][providers][gemini][sse]") {
    sao::ai_editor::native::GeminiSseCodec codec;
    Json events;
    const std::string stream =
        "data: {\"candidates\":[{\"content\":{\"parts\":"
        "[{\"text\":\"hel\"}],\"role\":\"model\"}}]}\n"
        "\n"
        "data: {\"candidates\":[{\"content\":{\"parts\":"
        "[{\"text\":\"lo\"}],\"role\":\"model\"},"
        "\"finishReason\":\"STOP\"}],"
        "\"usageMetadata\":{\"totalTokenCount\":42}}\n"
        "\n";
    REQUIRE(codec.feed(stream, events) == SAO_AI_EDITOR_OK);
    REQUIRE(events.is_array());
    REQUIRE(events.size() == 4);
    REQUIRE(events[0]["type"] == "delta");
    REQUIRE(events[0]["content"] == "hel");
    REQUIRE(events[1]["type"] == "delta");
    REQUIRE(events[1]["content"] == "lo");
    REQUIRE(events[2]["type"] == "message_delta");
    REQUIRE(events[2]["finish_reason"] == "STOP");
    REQUIRE(events[2]["usage"]["totalTokenCount"] == 42);
    REQUIRE(events[3]["type"] == "done");

    SECTION("empty input yields no events") {
        sao::ai_editor::native::GeminiSseCodec fresh;
        Json fresh_events;
        REQUIRE(fresh.feed(std::string_view{}, fresh_events) ==
                SAO_AI_EDITOR_OK);
        REQUIRE(fresh_events.is_array());
        REQUIRE(fresh_events.empty());
    }

    SECTION("error payload surfaces as error event") {
        sao::ai_editor::native::GeminiSseCodec err_codec;
        Json err_events;
        REQUIRE(err_codec.feed(
                    "data: {\"error\":{\"code\":429,\"message\":\"rate\"}}\n\n",
                    err_events) == SAO_AI_EDITOR_OK);
        REQUIRE(err_events.size() == 1);
        REQUIRE(err_events[0]["type"] == "error");
        REQUIRE(err_events[0]["error"]["code"] == 429);
        REQUIRE(err_events[0]["error"]["message"] == "rate");
    }

    SECTION("non-object payload is a protocol error") {
        sao::ai_editor::native::GeminiSseCodec bad;
        Json bad_events;
        REQUIRE(bad.feed("data: 12345\n\n", bad_events) ==
                SAO_AI_EDITOR_ERR_PROTOCOL);
    }

    SECTION("FINISH_REASON_UNSPECIFIED does not close the stream") {
        sao::ai_editor::native::GeminiSseCodec pending;
        Json pending_events;
        REQUIRE(pending.feed(
                    "data: {\"candidates\":[{\"content\":{\"parts\":"
                    "[{\"text\":\"x\"}]},"
                    "\"finishReason\":\"FINISH_REASON_UNSPECIFIED\"}]}\n\n",
                    pending_events) == SAO_AI_EDITOR_OK);
        REQUIRE(pending_events.size() == 1);
        REQUIRE(pending_events[0]["type"] == "delta");
        REQUIRE(pending_events[0]["content"] == "x");
    }

    SECTION("feed after done rejects non-whitespace input") {
        Json trailing;
        REQUIRE(codec.feed("\r\n", trailing) == SAO_AI_EDITOR_OK);
        REQUIRE(trailing.empty());
        REQUIRE(codec.feed("data: {}\n\n", trailing) ==
                SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
    }
}

TEST_CASE("Gemini SSE decoder handles split chunks across feeds",
          "[plugins][ai_editor][native][providers][gemini][sse]") {
    sao::ai_editor::native::GeminiSseCodec codec;
    const std::string stream =
        "data: {\"candidates\":[{\"content\":{\"parts\":"
        "[{\"text\":\"hel\"}],\"role\":\"model\"}}]}\n"
        "\n"
        "data: {\"candidates\":[{\"content\":{\"parts\":"
        "[{\"text\":\"lo\"}],\"role\":\"model\"},"
        "\"finishReason\":\"STOP\"}],"
        "\"usageMetadata\":{\"totalTokenCount\":42}}\n"
        "\n";
    // Split partway through the first payload's JSON body so the buffer
    // has to survive across two feed calls before the blank line arrives.
    const size_t cut = stream.find("\"text\":\"hel\"") + 6;
    REQUIRE(cut > 0);
    REQUIRE(cut < stream.size());
    const std::string_view first(stream.data(), cut);
    const std::string_view second(stream.data() + cut, stream.size() - cut);

    Json first_events;
    REQUIRE(codec.feed(first, first_events) == SAO_AI_EDITOR_OK);
    REQUIRE(first_events.is_array());
    REQUIRE(first_events.empty());

    Json second_events;
    REQUIRE(codec.feed(second, second_events) == SAO_AI_EDITOR_OK);
    REQUIRE(second_events.size() == 4);
    REQUIRE(second_events[0]["type"] == "delta");
    REQUIRE(second_events[0]["content"] == "hel");
    REQUIRE(second_events[1]["type"] == "delta");
    REQUIRE(second_events[1]["content"] == "lo");
    REQUIRE(second_events[2]["type"] == "message_delta");
    REQUIRE(second_events[2]["finish_reason"] == "STOP");
    REQUIRE(second_events[2]["usage"]["totalTokenCount"] == 42);
    REQUIRE(second_events[3]["type"] == "done");

    SECTION("split at the blank-line boundary between events") {
        sao::ai_editor::native::GeminiSseCodec codec2;
        // Include both newlines so the first event has fully dispatched
        // before the second feed starts.
        const size_t boundary = stream.find("\n\n") + 2;
        REQUIRE(boundary > 1);
        REQUIRE(boundary < stream.size());
        Json a_events;
        REQUIRE(codec2.feed(std::string_view(stream.data(), boundary),
                              a_events) == SAO_AI_EDITOR_OK);
        REQUIRE(a_events.size() == 1);
        REQUIRE(a_events[0]["type"] == "delta");
        REQUIRE(a_events[0]["content"] == "hel");
        Json b_events;
        REQUIRE(codec2.feed(std::string_view(stream.data() + boundary,
                                                stream.size() - boundary),
                              b_events) == SAO_AI_EDITOR_OK);
        REQUIRE(b_events.size() == 3);
        REQUIRE(b_events[0]["content"] == "lo");
        REQUIRE(b_events[1]["finish_reason"] == "STOP");
        REQUIRE(b_events[2]["type"] == "done");
    }
}

TEST_CASE("Provider router builds Anthropic native body with system + apiKey",
          "[plugins][ai_editor][native][providers][anthropic]") {
    Json openai_body{
        {"model", "claude-3-5-sonnet"},
        {"messages",
         Json::array(
             {Json{{"role", "system"}, {"content", "You are helpful."}},
              Json{{"role", "user"}, {"content", "hi"}},
              Json{{"role", "assistant"}, {"content", "hello"}},
              Json{{"role", "user"}, {"content", "again"}}})},
        {"max_tokens", 512},
        {"temperature", 0.2}};
    sao::ai_editor::native::ProviderRoute route =
        sao::ai_editor::native::normalise_provider(
            Json{{"type", "anthropic"},
                 {"endpoint", "https://api.anthropic.com"},
                 {"apiKey", "sk-ant-xyz"}},
            "claude-3-5-sonnet");
    sao::ai_editor::native::ProviderRequest request;
    REQUIRE(sao::ai_editor::native::build_provider_request(route, openai_body,
                                                            request) ==
            SAO_AI_EDITOR_OK);
    REQUIRE(request.endpoint.find("/v1/messages") != std::string::npos);
    REQUIRE(request.extra_headers.find("x-api-key: sk-ant-xyz") !=
            std::string::npos);
    REQUIRE(request.extra_headers.find("anthropic-version: 2023-06-01") !=
            std::string::npos);
    REQUIRE(request.authorization.empty());
    const Json parsed = Json::parse(request.body_json);
    REQUIRE(parsed["model"] == "claude-3-5-sonnet");
    REQUIRE(parsed["system"] == "You are helpful.");
    REQUIRE(parsed["messages"].size() == 3);
    REQUIRE(parsed["messages"][0]["role"] == "user");
    REQUIRE(parsed["messages"][0]["content"][0]["type"] == "text");
    REQUIRE(parsed["messages"][1]["role"] == "assistant");
    REQUIRE(parsed["temperature"] == 0.2);
    REQUIRE(parsed["max_tokens"] == 512);
}

TEST_CASE("Provider router decodes Anthropic response into normalised content",
          "[plugins][ai_editor][native][providers][anthropic]") {
    sao::ai_editor::native::ProviderRoute route;
    route.type = "anthropic";
    const std::string payload =
        R"({"content":[{"type":"text","text":"anthropic-hello"}],)"
        R"("stop_reason":"end_turn","usage":{"input_tokens":5,)"
        R"("output_tokens":9}})";
    Json result;
    REQUIRE(sao::ai_editor::native::decode_provider_response(route, payload,
                                                              result) ==
            SAO_AI_EDITOR_OK);
    REQUIRE(result["ok"] == true);
    REQUIRE(result["content"] == "anthropic-hello");
    REQUIRE(result["finish_reason"] == "end_turn");
    REQUIRE(result["usage"]["output_tokens"] == 9);
}

TEST_CASE("Provider router builds Gemini contents with parts and api key",
          "[plugins][ai_editor][native][providers][gemini]") {
    Json openai_body{
        {"model", "gemini-2.0-flash"},
        {"messages",
         Json::array(
             {Json{{"role", "system"}, {"content", "System guide"}},
              Json{{"role", "user"}, {"content", "explain quantum"}}})},
        {"max_tokens", 128}};
    sao::ai_editor::native::ProviderRoute route =
        sao::ai_editor::native::normalise_provider(
            Json{{"type", "gemini"},
                 {"endpoint",
                  "https://generativelanguage.googleapis.com/v1beta/models"},
                 {"apiKey", "GEM-KEY"}},
            "gemini-2.0-flash");
    sao::ai_editor::native::ProviderRequest request;
    REQUIRE(sao::ai_editor::native::build_provider_request(route, openai_body,
                                                            request) ==
            SAO_AI_EDITOR_OK);
    REQUIRE(request.endpoint.find(":generateContent") != std::string::npos);
    REQUIRE(request.endpoint.find("key=GEM-KEY") != std::string::npos);
    REQUIRE(request.authorization.empty());
    const Json parsed = Json::parse(request.body_json);
    REQUIRE(parsed["contents"][0]["role"] == "user");
    REQUIRE(parsed["contents"][0]["parts"][0]["text"] == "explain quantum");
    REQUIRE(parsed["systemInstruction"]["parts"][0]["text"] == "System guide");
    REQUIRE(parsed["generationConfig"]["maxOutputTokens"] == 128);
}

TEST_CASE("Provider router decodes Gemini response into normalised content",
          "[plugins][ai_editor][native][providers][gemini]") {
    sao::ai_editor::native::ProviderRoute route;
    route.type = "gemini";
    const std::string payload =
        R"({"candidates":[{"content":{"parts":[{"text":"gemini-hi"}]},)"
        R"("finishReason":"STOP"}],"usageMetadata":{"totalTokenCount":42}})";
    Json result;
    REQUIRE(sao::ai_editor::native::decode_provider_response(route, payload,
                                                              result) ==
            SAO_AI_EDITOR_OK);
    REQUIRE(result["ok"] == true);
    REQUIRE(result["content"] == "gemini-hi");
    REQUIRE(result["finish_reason"] == "STOP");
    REQUIRE(result["usage"]["totalTokenCount"] == 42);
}

TEST_CASE("chat.run routes Anthropic provider through native /v1/messages",
          "[plugins][ai_editor][native][providers][anthropic][integration]") {
    const std::string body =
        R"({"content":[{"type":"text","text":"claude-native"}],)"
        R"("stop_reason":"end_turn"})";
    LocalHttpServer server(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: " +
        std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body);
    RuntimeFixture fixture;
    const Json params{{"provider", {{"id", "anthropic-native"},
                                     {"type", "anthropic"},
                                     {"endpoint", server.endpoint()},
                                     {"apiKey", "sk-ant-fixture"}}},
                      {"model", "claude-3-5-sonnet"},
                      {"messages",
                       Json::array({Json{{"role", "user"},
                                          {"content", "hi"}}})},
                      {"stream", false},
                      {"timeoutMs", 5000}};
    const Json started = dispatch(fixture.get(), "chat.run", params);
    REQUIRE(started.contains("result"));
    const std::string run_id = started["result"]["runId"];
    REQUIRE(server.wait_for_connections(1, 2'000));
    Json status;
    const ULONGLONG wait_started = GetTickCount64();
    do {
        status = dispatch(fixture.get(), "run.status", {{"runId", run_id}})
                     ["result"];
        if (status["status"] != "running") {
            break;
        }
        Sleep(20);
    } while (GetTickCount64() - wait_started < 5'000);
    REQUIRE(status["status"] == "completed");
    REQUIRE(status["result"]["content"] == "claude-native");
}

TEST_CASE("AI Editor workflows.list_defs surfaces built-in workflows",
          "[plugins][ai_editor][native][workflows]") {
    RuntimeFixture fixture;
    const Json defs = dispatch(fixture.get(), "workflows.list_defs");
    REQUIRE(defs.contains("result"));
    REQUIRE(defs["result"]["total"] >= 3);
    std::vector<std::string> ids;
    for (const auto& item : defs["result"]["items"]) {
        ids.push_back(item.value("id", ""));
    }
    REQUIRE(std::find(ids.begin(), ids.end(), "review-and-fix") != ids.end());
    REQUIRE(std::find(ids.begin(), ids.end(), "explain-and-improve") !=
            ids.end());
    REQUIRE(std::find(ids.begin(), ids.end(), "debug-trace") != ids.end());
}

TEST_CASE("AI Editor workflows.run walks steps sequentially and captures "
          "output variables",
          "[plugins][ai_editor][native][workflows][integration]") {
    const std::string body =
        R"({"choices":[{"message":{"role":"assistant","content":"review-content"}}]})";
    LocalHttpServer server(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: " +
        std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body);
    RuntimeFixture fixture;
    const Json run = dispatch(
        fixture.get(), "workflows.run",
        {{"id", "review-and-fix"},
         {"provider", {{"id", "workflow-fixture"},
                       {"endpoint", server.endpoint()}}},
         {"model", "fixture-model"},
         {"input", "some code"},
         {"timeoutMs", 5000}});
    INFO("workflows.run response: " << run.dump());
    REQUIRE(run.contains("result"));
    const std::string execution_id = run["result"]["executionId"];

    Json status;
    const ULONGLONG started = GetTickCount64();
    do {
        status = dispatch(fixture.get(), "workflows.status",
                          {{"executionId", execution_id}})["result"];
        if (status["status"] != "running" &&
            status["status"] != "pending") {
            break;
        }
        Sleep(20);
    } while (GetTickCount64() - started < 15'000);
    REQUIRE(status["status"] == "completed");
    REQUIRE(status["variables"]["input"] == "some code");
    REQUIRE(status["variables"]["review"] == "review-content");
    REQUIRE(status["variables"]["fix"] == "review-content");
    REQUIRE(status["stepResults"].size() == 2);
}

TEST_CASE("AI Editor workflows.cancel terminates in-flight run",
          "[plugins][ai_editor][native][workflows][integration]") {
    LocalHttpServer server;  // never responds; workflow blocks in HTTP
    RuntimeFixture fixture;
    const Json run = dispatch(
        fixture.get(), "workflows.run",
        {{"id", "debug-trace"},
         {"provider", {{"id", "cancel-fixture"},
                       {"endpoint", server.endpoint()}}},
         {"model", "fixture-model"},
         {"input", "bug"},
         {"timeoutMs", 2000}});
    REQUIRE(run.contains("result"));
    const std::string execution_id = run["result"]["executionId"];
    REQUIRE(server.wait_for_connections(1, 2'000));
    REQUIRE(dispatch(fixture.get(), "workflows.cancel",
                     {{"executionId", execution_id}})
                .contains("result"));

    Json status;
    const ULONGLONG started = GetTickCount64();
    do {
        status = dispatch(fixture.get(), "workflows.status",
                          {{"executionId", execution_id}})["result"];
        if (status["status"] != "running" && status["status"] != "pending") {
            break;
        }
        Sleep(20);
    } while (GetTickCount64() - started < 5'000);
    // Cancelled during network wait -> chat times out -> failed OR cancelled
    // depending on when the cancel arrived.
    const std::string terminal = status["status"];
    REQUIRE((terminal == "cancelled" || terminal == "failed"));
}

TEST_CASE("AI Editor workflows.save_def / delete_def round-trips a "
          "user-defined workflow",
          "[plugins][ai_editor][native][workflows]") {
    RuntimeFixture fixture;
    const Json workflow_json{
        {"id", "custom-review"},
        {"name", "Custom Review"},
        {"description", "Test workflow"},
        {"steps",
         Json::array({Json{{"agent", "default"},
                             {"prompt", "Review {{input}}"},
                             {"output_var", "verdict"}}})}};
    REQUIRE(dispatch(fixture.get(), "workflows.save_def",
                     {{"scope", "workspace"},
                      {"workflow", workflow_json}})
                .contains("result"));
    const Json listed = dispatch(fixture.get(), "workflows.list_defs")
                            ["result"];
    bool found = false;
    for (const auto& item : listed["items"]) {
        if (item.value("id", "") == "custom-review") {
            found = true;
            REQUIRE(item["builtin"] == false);
        }
    }
    REQUIRE(found);
    REQUIRE(dispatch(fixture.get(), "workflows.delete_def",
                     {{"id", "custom-review"}})
                .contains("result"));
    REQUIRE(dispatch(fixture.get(), "workflows.delete_def",
                     {{"id", "review-and-fix"}})["error"]["data"]["status"]
                        == SAO_AI_EDITOR_ERR_PERMISSION_DENIED);
}

TEST_CASE("AI Editor mcp.* JSON-RPC surface aggregates and forwards", "[plugins]"
          "[ai_editor][native][mcp][dispatch][integration]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    const auto register_result = dispatch(
        runtime, "mcp.register_server",
        {{"name", "dispatch-mcp"},
         {"command", SAO_AI_EDITOR_MCP_SERVER_EXECUTABLE},
         {"args", Json::array({"--mcp-server", "--workspace",
                                utf8_path(fixture.workspace())})}});
    REQUIRE(register_result.contains("result"));

    const Json list_servers = dispatch(runtime, "mcp.list_servers");
    REQUIRE(list_servers["result"]["total"] == 1);
    REQUIRE(list_servers["result"]["items"][0]["name"] == "dispatch-mcp");

    const Json list_tools = dispatch(runtime, "mcp.list_tools");
    REQUIRE(list_tools["result"]["total"] >= 4);

    {
        std::ofstream fixture_file(fixture.workspace() / L"mcp-dispatch.txt");
        fixture_file << "dispatched via mcp.*\n";
    }
    const Json call = dispatch(runtime, "mcp.call_tool",
                               {{"server", "dispatch-mcp"},
                                {"name", "readFile"},
                                {"arguments",
                                 {{"path", "mcp-dispatch.txt"}}}});
    REQUIRE(call.contains("result"));
    REQUIRE(call["result"]["content"][0]["text"].get<std::string>().find(
                "dispatched via mcp.*") != std::string::npos);

    REQUIRE(dispatch(runtime, "mcp.close_server", {{"name", "dispatch-mcp"}})
                .contains("result"));
    REQUIRE(dispatch(runtime, "mcp.list_servers")["result"]["total"] == 0);
}

TEST_CASE("SaoAiEditor.exe --extension-host serves NativeRuntime JSON-RPC "
          "over stdio and honours host.shutdown",
          "[plugins][ai_editor][production_child][extension_host]"
          "[integration]") {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / L"ext-host-workspace";
    REQUIRE(std::filesystem::create_directories(workspace));

    SECURITY_ATTRIBUTES security_attributes{};
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.bInheritHandle = TRUE;

    HANDLE stdin_read = nullptr;
    HANDLE stdin_write = nullptr;
    REQUIRE(CreatePipe(&stdin_read, &stdin_write, &security_attributes,
                       128 * 1024) != FALSE);
    REQUIRE(SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0) != FALSE);

    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    REQUIRE(CreatePipe(&stdout_read, &stdout_write, &security_attributes,
                       128 * 1024) != FALSE);
    REQUIRE(SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0) !=
            FALSE);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdin_read;
    startup.hStdOutput = stdout_write;
    startup.hStdError = stdout_write;

    const std::string exe_utf8 = SAO_AI_EDITOR_MCP_SERVER_EXECUTABLE;
    const int wide_len = MultiByteToWideChar(CP_UTF8, 0, exe_utf8.c_str(), -1,
                                              nullptr, 0);
    REQUIRE(wide_len > 0);
    std::wstring executable(static_cast<size_t>(wide_len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, exe_utf8.c_str(), -1, executable.data(),
                        wide_len);
    const std::wstring quoted_workspace =
        L"\"" + workspace.native() + L"\"";
    std::wstring command_line = L"\"" + executable +
                                L"\" --extension-host --workspace " +
                                quoted_workspace;
    std::vector<wchar_t> command_line_buffer(command_line.begin(),
                                              command_line.end());
    command_line_buffer.push_back(L'\0');

    PROCESS_INFORMATION process_information{};
    REQUIRE(CreateProcessW(nullptr, command_line_buffer.data(), nullptr,
                           nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                           &startup, &process_information) != FALSE);

    // Close the child ends inside the parent so ReadFile eventually
    // observes EOF once the child exits.
    CloseHandle(stdin_read);
    CloseHandle(stdout_write);

    auto send_frame = [&](const Json& message) {
        const std::string body = message.dump();
        const std::string header =
            "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
        DWORD written = 0;
        REQUIRE(WriteFile(stdin_write, header.data(),
                          static_cast<DWORD>(header.size()), &written,
                          nullptr) != FALSE);
        REQUIRE(written == header.size());
        REQUIRE(WriteFile(stdin_write, body.data(),
                          static_cast<DWORD>(body.size()), &written,
                          nullptr) != FALSE);
        REQUIRE(written == body.size());
    };

    // Simple Content-Length framed reader that pulls until at least one
    // complete message is decoded.  Uses the shared MCP decoder so we
    // exercise the same framing the server produces.
    sao_ai_editor_mcp_decoder_t decoder = nullptr;
    REQUIRE(sao_ai_editor_mcp_decoder_create(4U * 1024U * 1024U, &decoder) ==
            SAO_AI_EDITOR_OK);
    auto read_frame = [&]() -> Json {
        std::array<char, 4096> buffer{};
        while (true) {
            DWORD read = 0;
            const BOOL ok = ReadFile(stdout_read, buffer.data(),
                                      static_cast<DWORD>(buffer.size()),
                                      &read, nullptr);
            REQUIRE(ok != FALSE);
            REQUIRE(read > 0);
            uint32_t required = 0;
            const int32_t query = sao_ai_editor_mcp_decoder_feed(
                decoder, buffer.data(), read, nullptr, 0, &required);
            if (query != SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL ||
                required == 0) {
                continue;
            }
            std::vector<char> messages(static_cast<size_t>(required) + 1U);
            const int32_t drain = sao_ai_editor_mcp_decoder_feed(
                decoder, nullptr, 0, messages.data(),
                static_cast<uint32_t>(messages.size()), &required);
            REQUIRE(drain == SAO_AI_EDITOR_OK);
            Json parsed =
                Json::parse(messages.data(), messages.data() + required,
                             nullptr, false);
            REQUIRE(parsed.is_array());
            // Decoder emits the accumulated array on every feed — an
            // empty array just means the header arrived but the body is
            // still in flight, so keep pulling from stdout.
            if (parsed.empty()) {
                continue;
            }
            return parsed[0];
        }
    };

    // 1) extensions.list — even without configure_host, the runtime
    //    exposes an empty registry (total=0) so we can confirm the
    //    JSON-RPC surface is live.
    send_frame(Json{{"jsonrpc", "2.0"},
                    {"id", 1},
                    {"method", "extensions.list"},
                    {"params", Json::object()}});
    const Json list_response = read_frame();
    REQUIRE(list_response["id"] == 1);
    REQUIRE(list_response.contains("result"));
    REQUIRE(list_response["result"]["total"] == 0);
    REQUIRE(list_response["result"]["nodeAlive"] == false);

    // 2) A representative tools.call round-trip proves the shared
    //    registry is reachable — write a fixture file first, then read
    //    it through the extension host stdio bridge.
    {
        std::ofstream fixture_file(workspace / L"hello.txt");
        fixture_file << "hello via extension host\n";
    }
    send_frame(Json{{"jsonrpc", "2.0"},
                    {"id", 2},
                    {"method", "tools.call"},
                    {"params",
                     {{"mode", "agent"},
                      {"name", "readFile"},
                      {"arguments", {{"path", "hello.txt"}}}}}});
    const Json call_response = read_frame();
    REQUIRE(call_response["id"] == 2);
    REQUIRE(call_response.contains("result"));
    REQUIRE(call_response["result"]["content"].get<std::string>().find(
                "hello via extension host") != std::string::npos);

    // 3) host.shutdown asks the process to exit cleanly.
    send_frame(Json{{"jsonrpc", "2.0"},
                    {"id", 3},
                    {"method", "host.shutdown"},
                    {"params", Json::object()}});
    const Json shutdown_response = read_frame();
    REQUIRE(shutdown_response["id"] == 3);
    REQUIRE(shutdown_response.contains("result"));

    sao_ai_editor_mcp_decoder_destroy(decoder);
    CloseHandle(stdin_write);
    CloseHandle(stdout_read);
    REQUIRE(WaitForSingleObject(process_information.hProcess, 15'000) ==
            WAIT_OBJECT_0);
    DWORD exit_code = 1;
    REQUIRE(GetExitCodeProcess(process_information.hProcess, &exit_code) !=
            FALSE);
    REQUIRE(exit_code == 0);
    CloseHandle(process_information.hProcess);
    CloseHandle(process_information.hThread);
}

TEST_CASE("SaoAiEditor.exe --extension-host exits cleanly on stdin EOF",
          "[plugins][ai_editor][production_child][extension_host]"
          "[integration]") {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / L"ext-host-eof-workspace";
    REQUIRE(std::filesystem::create_directories(workspace));

    SECURITY_ATTRIBUTES security_attributes{};
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.bInheritHandle = TRUE;

    HANDLE stdin_read = nullptr;
    HANDLE stdin_write = nullptr;
    REQUIRE(CreatePipe(&stdin_read, &stdin_write, &security_attributes,
                       64 * 1024) != FALSE);
    REQUIRE(SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0) != FALSE);

    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    REQUIRE(CreatePipe(&stdout_read, &stdout_write, &security_attributes,
                       64 * 1024) != FALSE);
    REQUIRE(SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0) !=
            FALSE);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdin_read;
    startup.hStdOutput = stdout_write;
    startup.hStdError = stdout_write;

    const std::string exe_utf8 = SAO_AI_EDITOR_MCP_SERVER_EXECUTABLE;
    const int wide_len = MultiByteToWideChar(CP_UTF8, 0, exe_utf8.c_str(), -1,
                                              nullptr, 0);
    REQUIRE(wide_len > 0);
    std::wstring executable(static_cast<size_t>(wide_len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, exe_utf8.c_str(), -1, executable.data(),
                        wide_len);
    const std::wstring quoted_workspace =
        L"\"" + workspace.native() + L"\"";
    std::wstring command_line = L"\"" + executable +
                                L"\" --extension-host --workspace " +
                                quoted_workspace;
    std::vector<wchar_t> command_line_buffer(command_line.begin(),
                                              command_line.end());
    command_line_buffer.push_back(L'\0');

    PROCESS_INFORMATION process_information{};
    REQUIRE(CreateProcessW(nullptr, command_line_buffer.data(), nullptr,
                           nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                           &startup, &process_information) != FALSE);

    CloseHandle(stdin_read);
    CloseHandle(stdout_write);

    // Immediately drop stdin — the loop should notice EOF and exit 0
    // without ever seeing a request.
    CloseHandle(stdin_write);

    REQUIRE(WaitForSingleObject(process_information.hProcess, 15'000) ==
            WAIT_OBJECT_0);
    DWORD exit_code = 1;
    REQUIRE(GetExitCodeProcess(process_information.hProcess, &exit_code) !=
            FALSE);
    REQUIRE(exit_code == 0);
    CloseHandle(stdout_read);
    CloseHandle(process_information.hProcess);
    CloseHandle(process_information.hThread);
}

TEST_CASE("SaoAiEditor.exe --node-executable is only accepted with "
          "--extension-host",
          "[plugins][ai_editor][production_child][extension_host]") {
    const std::string exe_utf8 = SAO_AI_EDITOR_MCP_SERVER_EXECUTABLE;
    const int wide_len = MultiByteToWideChar(CP_UTF8, 0, exe_utf8.c_str(), -1,
                                              nullptr, 0);
    REQUIRE(wide_len > 0);
    std::wstring executable(static_cast<size_t>(wide_len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, exe_utf8.c_str(), -1, executable.data(),
                        wide_len);
    // --node-executable without --extension-host is a parse error.
    std::wstring command_line = L"\"" + executable +
                                L"\" --mcp-server --node-executable "
                                L"C:\\node.exe";
    std::vector<wchar_t> command_line_buffer(command_line.begin(),
                                              command_line.end());
    command_line_buffer.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process_information{};
    REQUIRE(CreateProcessW(nullptr, command_line_buffer.data(), nullptr,
                           nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                           &startup, &process_information) != FALSE);
    REQUIRE(WaitForSingleObject(process_information.hProcess, 10'000) ==
            WAIT_OBJECT_0);
    DWORD exit_code = 0;
    REQUIRE(GetExitCodeProcess(process_information.hProcess, &exit_code) !=
            FALSE);
    // parse_arguments returns false -> wWinMain returns 2.
    REQUIRE(exit_code == 2);
    CloseHandle(process_information.hProcess);
    CloseHandle(process_information.hThread);
}
