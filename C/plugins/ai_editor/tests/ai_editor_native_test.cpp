#include <catch2/catch_test_macros.hpp>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
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

    REQUIRE(dispatch(runtime, "agents.save",
                     {{"scope", "plugin:fixture"},
                      {"item", {{"id", "reviewer"},
                                {"name", "Reviewer"}}}})
                .contains("result"));
    const Json agents = dispatch(runtime, "agents.list");
    REQUIRE(agents["result"]["total"] == 1);
    REQUIRE(agents["result"]["items"][0]["id"] == "reviewer");
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
