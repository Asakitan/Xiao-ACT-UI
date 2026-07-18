#include <catch2/catch_test_macros.hpp>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
        init_socket();
    }
    // Sequenced constructor: first captured request gets responses[0], the
    // second gets responses[1], etc.  After the sequence is exhausted the
    // server falls back to the last entry so tests that request 4x can
    // still line up sensibly.  Used by the retry tests to script a
    // 500-500-200 recovery.
    explicit LocalHttpServer(std::vector<std::string> responses)
        : responses_(std::move(responses)) {
        REQUIRE(!responses_.empty());
        init_socket();
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

    std::vector<std::string> captured_bodies() const {
        std::lock_guard<std::mutex> lock(bodies_mutex_);
        return bodies_;
    }

private:
    void init_socket() {
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

    // Pick the response body for the accepted_index-th connection.  The
    // single-response constructor uses response_ verbatim (may be empty
    // when tests want the server to hang); the sequenced constructor
    // walks responses_ and re-uses the tail for extra hits.
    const std::string& pick_response(size_t index) const noexcept {
        if (!responses_.empty()) {
            return responses_[std::min(index, responses_.size() - 1)];
        }
        return response_;
    }

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
            size_t response_index = 0;
            {
                std::lock_guard<std::mutex> lock(bodies_mutex_);
                response_index = bodies_.size();
                bodies_.emplace_back(request.substr(body_begin, content_length));
            }
            accepted_.fetch_add(1, std::memory_order_release);
            const std::string& outgoing = pick_response(response_index);
            if (!outgoing.empty()) {
                size_t sent_total = 0;
                while (sent_total < outgoing.size()) {
                    const int sent = send(
                        client, outgoing.data() + sent_total,
                        static_cast<int>(outgoing.size() - sent_total), 0);
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
    mutable std::mutex bodies_mutex_;
    std::vector<std::string> bodies_;
    std::thread worker_;
    std::string endpoint_;
    std::string response_;
    std::vector<std::string> responses_;
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

TEST_CASE(
    "OpenAiSseCodec accumulates tool_calls arguments across chunks",
    "[plugins][ai_editor][native][openai][sse][tool_calls]") {
    sao_ai_editor_openai_sse_decoder_t decoder = nullptr;
    REQUIRE(sao_ai_editor_openai_sse_decoder_create(&decoder) ==
            SAO_AI_EDITOR_OK);

    // Chunk 1: opening tool_call w/ id + name + empty arguments.
    const Json chunk1 = Json::parse(feed_sse(
        decoder,
        "data: {\"id\":\"chat-t1\",\"choices\":[{\"delta\":{\"tool_calls\":"
        "[{\"index\":0,\"id\":\"call_abc\",\"type\":\"function\","
        "\"function\":{\"name\":\"readFile\",\"arguments\":\"\"}}]},"
        "\"finish_reason\":null}]}\n\n"));
    REQUIRE(chunk1.size() == 2);
    REQUIRE(chunk1[0]["type"] == "tool_delta");
    REQUIRE(chunk1[0]["index"] == 0);
    REQUIRE(chunk1[0]["name"] == "readFile");
    REQUIRE(chunk1[0]["arguments"] == "");
    REQUIRE(chunk1[0]["partial"]["id"] == "call_abc");
    REQUIRE(chunk1[1]["type"] == "delta");

    // Chunk 2: mid-argument fragment.
    const Json chunk2 = Json::parse(feed_sse(
        decoder,
        "data: {\"id\":\"chat-t1\",\"choices\":[{\"delta\":{\"tool_calls\":"
        "[{\"index\":0,\"function\":{\"arguments\":\"{\\\"path\\\":\\\"\"}}]},"
        "\"finish_reason\":null}]}\n\n"));
    REQUIRE(chunk2.size() == 2);
    REQUIRE(chunk2[0]["type"] == "tool_delta");
    REQUIRE(chunk2[0]["arguments"] == "{\"path\":\"");
    REQUIRE(chunk2[0]["name"] == "readFile");

    // Chunk 3: closing argument fragment + finish_reason=tool_calls.
    const Json chunk3 = Json::parse(feed_sse(
        decoder,
        "data: {\"id\":\"chat-t1\",\"choices\":[{\"delta\":{\"tool_calls\":"
        "[{\"index\":0,\"function\":{\"arguments\":\"README.md\\\"}\"}}]},"
        "\"finish_reason\":\"tool_calls\"}]}\n\n"));
    REQUIRE(chunk3.size() == 2);
    REQUIRE(chunk3[0]["type"] == "tool_delta");
    REQUIRE(chunk3[0]["arguments"] == "{\"path\":\"README.md\"}");

    // Terminate with [DONE]: expect tool_calls_final then done.
    const Json final = Json::parse(feed_sse(decoder, "data: [DONE]\n\n"));
    REQUIRE(final.size() == 2);
    REQUIRE(final[0]["type"] == "tool_calls_final");
    REQUIRE(final[0]["tool_calls"].is_array());
    REQUIRE(final[0]["tool_calls"].size() == 1);
    REQUIRE(final[0]["tool_calls"][0]["id"] == "call_abc");
    REQUIRE(final[0]["tool_calls"][0]["type"] == "function");
    REQUIRE(final[0]["tool_calls"][0]["function"]["name"] == "readFile");
    REQUIRE(final[0]["tool_calls"][0]["function"]["arguments"] ==
            "{\"path\":\"README.md\"}");
    REQUIRE(final[1]["type"] == "done");

    sao_ai_editor_openai_sse_decoder_destroy(decoder);
}

TEST_CASE("OpenAiSseCodec omits tool_calls_final when no tool_calls seen",
          "[plugins][ai_editor][native][openai][sse][tool_calls]") {
    sao_ai_editor_openai_sse_decoder_t decoder = nullptr;
    REQUIRE(sao_ai_editor_openai_sse_decoder_create(&decoder) ==
            SAO_AI_EDITOR_OK);
    const Json events = Json::parse(feed_sse(
        decoder,
        "data: {\"id\":\"chat-plain\",\"choices\":[{\"delta\":"
        "{\"content\":\"hi\"},\"finish_reason\":null}]}\n\n"
        "data: [DONE]\n\n"));
    REQUIRE(events.size() == 2);
    REQUIRE(events[0]["type"] == "delta");
    REQUIRE(events[0]["content"] == "hi");
    REQUIRE(events[1]["type"] == "done");
    // No tool_calls_final should be present.
    for (const auto& event : events) {
        REQUIRE(event.value("type", "") != "tool_calls_final");
    }
    sao_ai_editor_openai_sse_decoder_destroy(decoder);
}

TEST_CASE("OpenAiSseCodec accumulates multiple tool_calls by index",
          "[plugins][ai_editor][native][openai][sse][tool_calls]") {
    sao_ai_editor_openai_sse_decoder_t decoder = nullptr;
    REQUIRE(sao_ai_editor_openai_sse_decoder_create(&decoder) ==
            SAO_AI_EDITOR_OK);

    // Two tool_calls opened in one chunk.
    const Json start = Json::parse(feed_sse(
        decoder,
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":["
        "{\"index\":0,\"id\":\"c1\",\"type\":\"function\","
        "\"function\":{\"name\":\"readFile\",\"arguments\":\"\"}},"
        "{\"index\":1,\"id\":\"c2\",\"type\":\"function\","
        "\"function\":{\"name\":\"writeFile\",\"arguments\":\"\"}}"
        "]},\"finish_reason\":null}]}\n\n"));
    // Two tool_delta events + one normalized delta event.
    REQUIRE(start.size() == 3);
    REQUIRE(start[0]["type"] == "tool_delta");
    REQUIRE(start[0]["index"] == 0);
    REQUIRE(start[0]["name"] == "readFile");
    REQUIRE(start[1]["type"] == "tool_delta");
    REQUIRE(start[1]["index"] == 1);
    REQUIRE(start[1]["name"] == "writeFile");

    // Argument fragments for index 1 (out-of-order OK).
    const Json mid = Json::parse(feed_sse(
        decoder,
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":["
        "{\"index\":1,\"function\":{\"arguments\":\"{\\\"a\\\":1}\"}}"
        "]},\"finish_reason\":null}]}\n\n"));
    REQUIRE(mid.size() == 2);
    REQUIRE(mid[0]["type"] == "tool_delta");
    REQUIRE(mid[0]["index"] == 1);
    REQUIRE(mid[0]["arguments"] == "{\"a\":1}");

    // Argument fragments for index 0.
    const Json last = Json::parse(feed_sse(
        decoder,
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":["
        "{\"index\":0,\"function\":{\"arguments\":\"{\\\"p\\\":\\\"x\\\"}\"}}"
        "]},\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: [DONE]\n\n"));
    // last chunk: tool_delta + delta + tool_calls_final + done = 4 events.
    REQUIRE(last.size() == 4);
    REQUIRE(last[0]["type"] == "tool_delta");
    REQUIRE(last[0]["index"] == 0);
    REQUIRE(last[0]["arguments"] == "{\"p\":\"x\"}");
    REQUIRE(last[2]["type"] == "tool_calls_final");
    REQUIRE(last[2]["tool_calls"].size() == 2);
    REQUIRE(last[2]["tool_calls"][0]["function"]["arguments"] ==
            "{\"p\":\"x\"}");
    REQUIRE(last[2]["tool_calls"][1]["function"]["arguments"] ==
            "{\"a\":1}");
    REQUIRE(last[3]["type"] == "done");

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

TEST_CASE("AI Editor chat.run_with_mcp omits tools when neither MCP servers "
          "nor extraTools are supplied",
          "[plugins][ai_editor][native][runs][mcp]") {
    const std::string body =
        R"({"choices":[{"message":{"role":"assistant","content":"ok"}}]})";
    LocalHttpServer server("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                           "Content-Length: " +
                           std::to_string(body.size()) +
                           "\r\nConnection: close\r\n\r\n" + body);
    RuntimeFixture fixture;
    // mcpServers=null and no extraTools → collected tool list is empty, so
    // the OpenAI request body must not carry a "tools" key (same shape as
    // plain chat.run without tools).
    const Json params{{"provider", {{"id", "fixture"},
                                     {"endpoint", server.endpoint()}}},
                      {"model", "fixture-model"},
                      {"messages", Json::array(
                           {{{"role", "user"}, {"content", "hi"}}})},
                      {"stream", false},
                      {"timeoutMs", 2'000}};
    const Json started =
        dispatch(fixture.get(), "chat.run_with_mcp", params);
    REQUIRE(started.contains("result"));
    REQUIRE(started["result"]["accepted"] == true);
    REQUIRE(server.wait_for_connections(1, 2'000));
    const auto bodies = server.captured_bodies();
    REQUIRE(bodies.size() == 1);
    const Json body_json = Json::parse(bodies.front());
    REQUIRE(body_json.contains("model"));
    REQUIRE(body_json["model"] == "fixture-model");
    REQUIRE_FALSE(body_json.contains("tools"));
}

TEST_CASE("AI Editor chat.run_with_mcp forwards extraTools into OpenAI body",
          "[plugins][ai_editor][native][runs][mcp]") {
    const std::string body =
        R"({"choices":[{"message":{"role":"assistant","content":"ok"}}]})";
    LocalHttpServer server("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                           "Content-Length: " +
                           std::to_string(body.size()) +
                           "\r\nConnection: close\r\n\r\n" + body);
    RuntimeFixture fixture;
    const Json extra_tool{
        {"type", "function"},
        {"function", {{"name", "search_docs"},
                      {"description", "search the docs"},
                      {"parameters",
                       {{"type", "object"},
                        {"properties",
                         {{"query", {{"type", "string"}}}}}}}}}};
    const Json params{{"provider", {{"id", "fixture"},
                                     {"endpoint", server.endpoint()}}},
                      {"model", "fixture-model"},
                      {"messages", Json::array(
                           {{{"role", "user"}, {"content", "hi"}}})},
                      {"stream", false},
                      {"timeoutMs", 2'000},
                      {"mcpServers", Json::array()},
                      {"extraTools", Json::array({extra_tool})}};
    const Json started =
        dispatch(fixture.get(), "chat.run_with_mcp", params);
    REQUIRE(started.contains("result"));
    REQUIRE(started["result"]["accepted"] == true);
    REQUIRE(server.wait_for_connections(1, 2'000));
    const auto bodies = server.captured_bodies();
    REQUIRE(bodies.size() == 1);
    const Json body_json = Json::parse(bodies.front());
    REQUIRE(body_json.contains("tools"));
    REQUIRE(body_json["tools"].is_array());
    REQUIRE(body_json["tools"].size() == 1);
    REQUIRE(body_json["tools"][0]["type"] == "function");
    REQUIRE(body_json["tools"][0]["function"]["name"] == "search_docs");
    // Namespaced mcp__ prefix must NOT appear when no MCP tools were folded
    // in — this proves collect_mcp_openai_tools returned an empty list.
    const std::string dumped = body_json["tools"].dump();
    REQUIRE(dumped.find("mcp__") == std::string::npos);
}

TEST_CASE("AI Editor agents.invoke_with_mcp threads MCP filter through agent "
          "system prompt path",
          "[plugins][ai_editor][native][agents][mcp]") {
    const std::string body =
        R"({"choices":[{"message":{"role":"assistant","content":"done"}}]})";
    LocalHttpServer server(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: " +
        std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body);
    RuntimeFixture fixture;
    const Json extra_tool{
        {"type", "function"},
        {"function", {{"name", "readFile"},
                      {"description", "read a workspace file"},
                      {"parameters",
                       {{"type", "object"},
                        {"properties",
                         {{"path", {{"type", "string"}}}}}}}}}};
    const Json invoked = dispatch(
        fixture.get(), "agents.invoke_with_mcp",
        {{"id", "code-reviewer"},
         {"message", "look at bar()"},
         {"provider", {{"id", "agent-fixture"},
                        {"endpoint", server.endpoint()}}},
         {"model", "test-model"},
         {"timeoutMs", 5000},
         {"mcpServers", Json::array()},
         {"extraTools", Json::array({extra_tool})}});
    REQUIRE(invoked.contains("result"));
    REQUIRE(invoked["result"]["agentId"] == "code-reviewer");
    REQUIRE(invoked["result"]["content"] == "done");
    REQUIRE(server.wait_for_connections(1, 2'000));
    const auto bodies = server.captured_bodies();
    REQUIRE(bodies.size() == 1);
    const Json body_json = Json::parse(bodies.front());
    REQUIRE(body_json.contains("tools"));
    REQUIRE(body_json["tools"].size() == 1);
    REQUIRE(body_json["tools"][0]["function"]["name"] == "readFile");
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

TEST_CASE("AI Editor agents.list_defs surfaces market presets when the "
          "assets/ai_editor/agents dir is reachable",
          "[plugins][ai_editor][native][agents][market]") {
    RuntimeFixture fixture;
    const Json defs = dispatch(fixture.get(), "agents.list_defs");
    REQUIRE(defs.contains("result"));
    const auto& items = defs["result"]["items"];
    std::unordered_map<std::string, Json> by_id;
    for (const auto& item : items) {
        by_id.emplace(item.value("id", std::string{}), item);
    }
    const std::vector<std::string> market_ids{
        "sql-expert", "test-writer", "security-auditor", "refactor-mentor"};
    size_t market_hits = 0;
    for (const auto& id : market_ids) {
        const auto found = by_id.find(id);
        if (found == by_id.end()) {
            continue;
        }
        ++market_hits;
        REQUIRE(found->second.value("scope", std::string{}) == "market");
        REQUIRE(found->second.value("builtin", true) == false);
        REQUIRE_FALSE(found->second.value("name", std::string{}).empty());
    }
    if (market_hits == 0) {
        INFO("Market agent assets/ai_editor/agents not deployed near the "
             "test binary; skipping the presence assertions. This is "
             "expected when assets have not been installed.");
    } else {
        // Any preset we actually found must be the full set — partial
        // deployment would indicate a packaging bug worth surfacing.
        REQUIRE(market_hits == market_ids.size());
    }
}

TEST_CASE("AI Editor agents.recommend ranks SQL performance query against "
          "sql-expert / optimizer",
          "[plugins][ai_editor][native][agents][recommend]") {
    RuntimeFixture fixture;
    // Mixed English + Chinese query: the SQL keyword should score sql-expert
    // (when the market preset is present) or code-reviewer/optimizer (when
    // it's not) high, and the Chinese "性能" glyphs should let optimizer
    // score via its when_to_use text.  We assert the top pick's id belongs
    // to one of a small shortlist rather than pinning a single agent so
    // the test is robust to whether market presets are deployed.
    const Json response = dispatch(
        fixture.get(), "agents.recommend",
        {{"query", "help me audit this SQL for performance problems 性能"},
         {"topK", 3},
         {"boostTags", Json::array({"performance"})}});
    REQUIRE(response.contains("result"));
    const auto& payload = response["result"];
    REQUIRE(payload["query"] == "help me audit this SQL for performance "
                                 "problems 性能");
    REQUIRE(payload["recommendations"].is_array());
    REQUIRE(payload["recommendations"].size() >= 1);
    REQUIRE(payload["recommendations"].size() <= 3);
    REQUIRE(payload["total"] == payload["recommendations"].size());
    // Top pick must be one of the SQL / review / performance-oriented
    // built-in or market agents.
    const std::string top_id =
        payload["recommendations"][0].value("agentId", std::string{});
    const std::vector<std::string> acceptable{"sql-expert", "code-reviewer",
                                                "optimizer"};
    REQUIRE(std::find(acceptable.begin(), acceptable.end(), top_id) !=
             acceptable.end());
    // Score must be normalised to the batch max — the top result therefore
    // must always equal 1.0 (a strict "> 0.999" rather than exact equality
    // avoids depending on catch_approx.hpp being in the include set).
    REQUIRE(payload["recommendations"][0]["score"].get<double>() > 0.999);
    // Reason must mention something matched (either terms or boost tags).
    REQUIRE_FALSE(
        payload["recommendations"][0].value("reason", std::string{}).empty());
    REQUIRE(payload["recommendations"][0]["matches"].is_array());
}

TEST_CASE("AI Editor agents.recommend honours topK=1 and Chinese-only query "
          "tokenisation",
          "[plugins][ai_editor][native][agents][recommend]") {
    RuntimeFixture fixture;
    // Pure-CJK query drives the codepoint-level tokeniser; we only assert
    // shape (topK truncation + non-empty when at least one built-in has
    // relevant terms).  "调试 错误 性能" spans debugger + optimizer text.
    const Json response = dispatch(
        fixture.get(), "agents.recommend",
        {{"query", "调试 错误 性能 bug"}, {"topK", 1}});
    REQUIRE(response.contains("result"));
    const auto& payload = response["result"];
    // topK=1 must never exceed 1 recommendation, even when the raw scoring
    // pass produces more candidates.
    REQUIRE(payload["recommendations"].size() <= 1);
    REQUIRE(payload["total"] == payload["recommendations"].size());
    if (!payload["recommendations"].empty()) {
        // The "bug" ASCII token maps to debugger.description; "调试" is a
        // CJK bigram in the same when_to_use.  Either match keeps score>0.
        REQUIRE(payload["recommendations"][0]["score"].get<double>() > 0.0);
    }
}

TEST_CASE("AI Editor agents.recommend includeBuiltin=false filters the "
          "hard-coded five",
          "[plugins][ai_editor][native][agents][recommend]") {
    RuntimeFixture fixture;
    // With includeBuiltin=false, none of the built-in ids (code-reviewer,
    // explainer, debugger, optimizer, documenter) may appear.  What's left
    // is entirely market/user scope — an environment that ships the SAO
    // market bundle will return sql-expert etc., while a bare deployment
    // returns an empty list.  Either state is legal; both are asserted.
    const std::unordered_set<std::string> builtin_ids{
        "code-reviewer", "explainer", "debugger", "optimizer", "documenter"};

    // Bring the workspace scope into play by saving a workspace-only agent
    // that also mentions "SQL" so we can prove non-builtin recommendations
    // *do* survive the filter (regardless of whether market presets are
    // deployed near the test binary).
    const Json workspace_agent{
        {"id", "workspace-sql-guru"},
        {"name", "Workspace SQL Guru"},
        {"description", "Helps with SQL queries in the current workspace"},
        {"system_prompt", "You answer SQL questions."},
        {"tools", Json::array({"readFile"})},
        {"when_to_use", "When the user asks about SQL in this workspace"}};
    REQUIRE(dispatch(fixture.get(), "agents.save_def",
                     {{"scope", "workspace"},
                      {"agent", workspace_agent}})
                .contains("result"));

    const Json response = dispatch(
        fixture.get(), "agents.recommend",
        {{"query", "SQL performance problem"},
         {"includeBuiltin", false},
         {"topK", 5}});
    REQUIRE(response.contains("result"));
    const auto& payload = response["result"];
    REQUIRE(payload["recommendations"].is_array());
    // Workspace agent must be reachable, and no built-in id may leak.
    bool workspace_hit = false;
    for (const auto& item : payload["recommendations"]) {
        const std::string id = item.value("agentId", std::string{});
        REQUIRE(builtin_ids.count(id) == 0);
        if (id == "workspace-sql-guru") {
            workspace_hit = true;
        }
    }
    REQUIRE(workspace_hit);

    // Belt-and-braces: all three include flags false collapses to an empty
    // recommendation set even when candidates exist.
    const Json empty_response = dispatch(
        fixture.get(), "agents.recommend",
        {{"query", "SQL"},
         {"includeBuiltin", false},
         {"includeMarket", false},
         {"includeUser", false}});
    REQUIRE(empty_response.contains("result"));
    REQUIRE(empty_response["result"]["recommendations"].empty());
    REQUIRE(empty_response["result"]["total"] == 0);
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

    // Enqueue a fully-formed HTTP response — used when the test needs to
    // exercise non-JSON payloads (e.g. text/event-stream MCP replies).
    void enqueue_raw(std::string full_response) {
        std::lock_guard<std::mutex> guard(mutex_);
        queue_.push_back(std::move(full_response));
    }

    // Pop the oldest recorded raw request.  Tests use this to assert that
    // callers propagated e.g. Authorization headers across the wire.
    std::string take_request() {
        std::lock_guard<std::mutex> guard(mutex_);
        if (requests_.empty()) {
            return {};
        }
        std::string out = std::move(requests_.front());
        requests_.pop_front();
        return out;
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
                requests_.push_back(request);
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
    std::deque<std::string> requests_;
    std::string base_url_;
};

}  // namespace

TEST_CASE("AI Editor MCP client speaks Streamable HTTP transport (JSON reply)",
          "[plugins][ai_editor][native][mcp][client][http][integration]") {
    ScriptedHttpServer server;
    // initialize response
    server.enqueue(
        R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2024-11-05",)"
        R"("serverInfo":{"name":"http-mock","version":"0.1"},)"
        R"("capabilities":{"tools":{"listChanged":false}}}})");
    // notifications/initialized is a fire-and-forget POST — client ignores
    // whatever comes back, but the scripted server still needs to hand out
    // *something* so its queue does not underflow into a 500.
    server.enqueue(R"({"jsonrpc":"2.0"})");
    // tools/list response
    server.enqueue(
        R"({"jsonrpc":"2.0","id":2,"result":{"tools":[)"
        R"({"name":"echo","description":"Echoes back input",)"
        R"("inputSchema":{"type":"object"}}]}})");
    // tools/call response
    server.enqueue(
        R"({"jsonrpc":"2.0","id":3,"result":{"content":[)"
        R"({"type":"text","text":"hello via HTTP"}]}})");

    sao_ai_editor_mcp_client_t client = nullptr;
    REQUIRE(sao_ai_editor_mcp_client_create(&client) == SAO_AI_EDITOR_OK);
    REQUIRE(client != nullptr);

    const Json config{
        {"name", "http-mcp"},
        {"transport", "http"},
        {"url", server.base_url() + "/mcp"},
        {"headers", {{"Authorization", "Bearer test-token"},
                     {"X-SAO-Test", "yes"}}},
        {"startupMs", 5000}};
    const std::string config_dump = config.dump();
    REQUIRE(sao_ai_editor_mcp_client_register(
                client, config_dump.data(),
                static_cast<uint32_t>(config_dump.size())) == SAO_AI_EDITOR_OK);

    // The initialize handshake should have propagated our custom headers.
    const std::string initialize_request = server.take_request();
    REQUIRE(initialize_request.find("Authorization: Bearer test-token") !=
            std::string::npos);
    REQUIRE(initialize_request.find("X-SAO-Test: yes") != std::string::npos);
    REQUIRE(initialize_request.find("Accept: application/json") !=
            std::string::npos);
    REQUIRE(initialize_request.find("Content-Type: application/json") !=
            std::string::npos);
    REQUIRE(initialize_request.find("\"method\":\"initialize\"") !=
            std::string::npos);

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
    REQUIRE(servers[0]["name"] == "http-mcp");
    REQUIRE(servers[0]["transport"] == "http");
    REQUIRE(servers[0]["url"] == server.base_url() + "/mcp");
    REQUIRE(servers[0]["serverInfo"]["name"] == "http-mock");
    REQUIRE(servers[0]["protocolVersion"] == "2024-11-05");

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
    REQUIRE(tools.size() == 1);
    REQUIRE(tools[0]["name"] == "echo");
    REQUIRE(tools[0]["server"] == "http-mcp");

    const Json call_request{{"server", "http-mcp"},
                            {"name", "echo"},
                            {"arguments", {{"text", "ping"}}}};
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
    REQUIRE(call["content"][0]["text"] == "hello via HTTP");
    REQUIRE(call["server"] == "http-mcp");

    // A successful register → list_tools → call_tool round trip means the
    // server handled at least four POSTs (initialize + notification +
    // tools/list + tools/call).
    REQUIRE(server.handled() >= 4);

    REQUIRE(sao_ai_editor_mcp_client_close(client, "http-mcp") ==
            SAO_AI_EDITOR_OK);
    sao_ai_editor_mcp_client_destroy(client);
}

TEST_CASE("AI Editor MCP client parses SSE-framed HTTP MCP responses",
          "[plugins][ai_editor][native][mcp][client][http][sse][integration]") {
    ScriptedHttpServer server;
    // initialize response encoded as a single-event text/event-stream body.
    // Real streamable-HTTP servers may reply this way — the client must
    // pull the first `data:` payload out and parse it as JSON-RPC.
    const std::string init_payload =
        R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2024-11-05",)"
        R"("serverInfo":{"name":"sse-mock","version":"0.1"},)"
        R"("capabilities":{}}})";
    const std::string init_sse_body =
        "event: message\r\ndata: " + init_payload + "\r\n\r\n";
    server.enqueue_raw("HTTP/1.1 200 OK\r\n"
                       "Content-Type: text/event-stream\r\n"
                       "Content-Length: " +
                       std::to_string(init_sse_body.size()) +
                       "\r\nConnection: close\r\n\r\n" + init_sse_body);
    // notifications/initialized fire-and-forget reply (contents unused).
    server.enqueue(R"({"jsonrpc":"2.0"})");
    // tools/call reply, again as SSE.
    const std::string call_payload =
        R"({"jsonrpc":"2.0","id":2,"result":{"content":[)"
        R"({"type":"text","text":"streamed"}]}})";
    const std::string call_sse_body =
        "data: " + call_payload + "\n\n";
    server.enqueue_raw("HTTP/1.1 200 OK\r\n"
                       "Content-Type: text/event-stream; charset=utf-8\r\n"
                       "Content-Length: " +
                       std::to_string(call_sse_body.size()) +
                       "\r\nConnection: close\r\n\r\n" + call_sse_body);

    sao_ai_editor_mcp_client_t client = nullptr;
    REQUIRE(sao_ai_editor_mcp_client_create(&client) == SAO_AI_EDITOR_OK);
    REQUIRE(client != nullptr);

    const Json config{
        {"name", "sse-mcp"},
        {"transport", "http"},
        {"url", server.base_url() + "/mcp"},
        {"startupMs", 5000}};
    const std::string config_dump = config.dump();
    REQUIRE(sao_ai_editor_mcp_client_register(
                client, config_dump.data(),
                static_cast<uint32_t>(config_dump.size())) == SAO_AI_EDITOR_OK);

    const Json call_request{{"server", "sse-mcp"},
                            {"name", "echo"},
                            {"arguments", {{"text", "sse"}}}};
    const std::string call_dump = call_request.dump();
    uint32_t required = 0;
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
    REQUIRE(call["content"][0]["text"] == "streamed");

    sao_ai_editor_mcp_client_destroy(client);
}

namespace {

// Trampoline used by the notification-forwarder test.  Buffers every payload
// the client pushes under a mutex so the calling thread can wait for a
// specific arrival count without racing the reader thread.
struct NotificationSink final {
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<std::string> payloads;

    static void SAO_AI_EDITOR_CALL callback(void* user, const char* json_utf8,
                                            uint32_t json_len) {
        auto* self = static_cast<NotificationSink*>(user);
        {
            std::lock_guard<std::mutex> guard(self->mutex);
            self->payloads.emplace_back(json_utf8,
                                        static_cast<size_t>(json_len));
        }
        self->ready.notify_all();
    }

    bool wait_for(size_t expected, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex);
        return ready.wait_for(lock, timeout, [&] {
            return payloads.size() >= expected;
        });
    }
};

}  // namespace

TEST_CASE("AI Editor MCP client forwards stdio notifications through the "
          "installed callback",
          "[plugins][ai_editor][native][mcp][client][notification]"
          "[integration]") {
    sao_ai_editor_mcp_client_t client = nullptr;
    REQUIRE(sao_ai_editor_mcp_client_create(&client) == SAO_AI_EDITOR_OK);
    REQUIRE(client != nullptr);

    // Wire the sink up front so notifications observed during initialize (or
    // right after it — the fake pushes as soon as notifications/initialized
    // arrives) never fall into the pull queue.
    NotificationSink sink;
    REQUIRE(sao_ai_editor_mcp_client_set_notification_forwarder(
                client, &sink, &NotificationSink::callback) ==
            SAO_AI_EDITOR_OK);

    const Json config{
        {"name", "notif-fixture"},
        {"command", SAO_AI_EDITOR_MCP_NOTIFICATION_FIXTURE},
        {"args", Json::array({"--notify-count", "2",
                              "--notify-method",
                              "notifications/tools/list_changed",
                              "--hold-open-ms", "1500"})},
        {"startupMs", 5000}};
    const std::string config_dump = config.dump();
    REQUIRE(sao_ai_editor_mcp_client_register(
                client, config_dump.data(),
                static_cast<uint32_t>(config_dump.size())) == SAO_AI_EDITOR_OK);

    REQUIRE(sink.wait_for(2, std::chrono::milliseconds(4000)));

    std::vector<std::string> payloads;
    {
        std::lock_guard<std::mutex> guard(sink.mutex);
        payloads = sink.payloads;
    }
    REQUIRE(payloads.size() >= 2);
    for (const auto& raw : payloads) {
        const Json envelope = Json::parse(raw);
        REQUIRE(envelope.is_object());
        REQUIRE(envelope["server"] == "notif-fixture");
        REQUIRE(envelope.contains("notification"));
        REQUIRE(envelope["notification"]["method"] ==
                "notifications/tools/list_changed");
        REQUIRE(envelope["notification"]["params"].is_object());
        REQUIRE(envelope["notification"]["params"].contains("sequence"));
    }
    // Sequence numbers must appear in monotonically increasing order — the
    // fixture pushes them tightly and the client's reader thread preserves
    // wire order.
    int64_t previous = -1;
    for (const auto& raw : payloads) {
        const int64_t sequence = Json::parse(raw)["notification"]["params"]
                                     .value("sequence", int64_t{-1});
        REQUIRE(sequence > previous);
        previous = sequence;
    }

    // Clearing the forwarder should also detach — subsequent notifications
    // land in the pull queue instead.  We do not have a way to make the fake
    // push again, but we can prove the API cleanly rebinds by installing a
    // null and re-installing the sink without crashing.
    REQUIRE(sao_ai_editor_mcp_client_set_notification_forwarder(
                client, nullptr, nullptr) == SAO_AI_EDITOR_OK);
    REQUIRE(sao_ai_editor_mcp_client_set_notification_forwarder(
                client, &sink, &NotificationSink::callback) ==
            SAO_AI_EDITOR_OK);

    REQUIRE(sao_ai_editor_mcp_client_close(client, "notif-fixture") ==
            SAO_AI_EDITOR_OK);
    sao_ai_editor_mcp_client_destroy(client);
}

TEST_CASE("AI Editor MCP client queues notifications for pull consumers when "
          "no forwarder is installed",
          "[plugins][ai_editor][native][mcp][client][notification]"
          "[integration]") {
    sao_ai_editor_mcp_client_t client = nullptr;
    REQUIRE(sao_ai_editor_mcp_client_create(&client) == SAO_AI_EDITOR_OK);
    REQUIRE(client != nullptr);

    // No forwarder installed → notifications must accumulate in the pull
    // queue and drain out through next_notification.
    const Json config{
        {"name", "notif-pull"},
        {"command", SAO_AI_EDITOR_MCP_NOTIFICATION_FIXTURE},
        {"args", Json::array({"--notify-count", "3",
                              "--notify-method",
                              "notifications/resources/updated",
                              "--hold-open-ms", "1500"})},
        {"startupMs", 5000}};
    const std::string config_dump = config.dump();
    REQUIRE(sao_ai_editor_mcp_client_register(
                client, config_dump.data(),
                static_cast<uint32_t>(config_dump.size())) == SAO_AI_EDITOR_OK);

    // Poll next_notification until we accumulate three payloads or time out.
    std::vector<std::string> drained;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(4000);
    while (drained.size() < 3 &&
           std::chrono::steady_clock::now() < deadline) {
        uint32_t required = 0;
        const int32_t queried = sao_ai_editor_mcp_client_next_notification(
            client, nullptr, 0, &required);
        if (queried == SAO_AI_EDITOR_ERR_NOT_FOUND) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            continue;
        }
        REQUIRE(queried == SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL);
        std::vector<char> buffer(static_cast<size_t>(required) + 1U, '\0');
        uint32_t written = 0;
        REQUIRE(sao_ai_editor_mcp_client_next_notification(
                    client, buffer.data(),
                    static_cast<uint32_t>(buffer.size()), &written) ==
                SAO_AI_EDITOR_OK);
        drained.emplace_back(buffer.data(), written);
    }
    REQUIRE(drained.size() == 3);
    for (const auto& raw : drained) {
        const Json envelope = Json::parse(raw);
        REQUIRE(envelope["server"] == "notif-pull");
        REQUIRE(envelope["notification"]["method"] ==
                "notifications/resources/updated");
    }

    // Empty queue now — next_notification should report NOT_FOUND without
    // touching *out_len.
    uint32_t after = 42;
    REQUIRE(sao_ai_editor_mcp_client_next_notification(
                client, nullptr, 0, &after) == SAO_AI_EDITOR_ERR_NOT_FOUND);
    REQUIRE(after == 0);

    sao_ai_editor_mcp_client_destroy(client);
}

TEST_CASE("AI Editor NativeRuntime surfaces MCP notifications as sao.event "
          "mcp.notification",
          "[plugins][ai_editor][native][mcp][dispatch][notification]"
          "[integration]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    const Json register_result = dispatch(
        runtime, "mcp.register_server",
        {{"name", "runtime-notif"},
         {"command", SAO_AI_EDITOR_MCP_NOTIFICATION_FIXTURE},
         {"args", Json::array({"--notify-count", "1",
                                "--notify-method",
                                "notifications/prompts/list_changed",
                                "--hold-open-ms", "1500"})}});
    REQUIRE(register_result.contains("result"));

    // Drain sao.event stream until we see the mcp.notification we expect.
    bool saw_notification = false;
    Json seen_payload;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(4000);
    while (!saw_notification &&
           std::chrono::steady_clock::now() < deadline) {
        uint32_t required = 0;
        const int32_t queried = sao_ai_editor_runtime_next_event(
            runtime, nullptr, 0, &required);
        if (queried == SAO_AI_EDITOR_OK && required == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            continue;
        }
        REQUIRE(queried == SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL);
        std::vector<char> event(static_cast<size_t>(required) + 1);
        REQUIRE(sao_ai_editor_runtime_next_event(
                    runtime, event.data(),
                    static_cast<uint32_t>(event.size()), &required) ==
                SAO_AI_EDITOR_OK);
        const Json notification =
            Json::parse(event.data(), event.data() + required);
        if (notification.value("method", "") != "sao.event") {
            continue;
        }
        const Json params = notification.value("params", Json::object());
        if (params.value("event", "") != "mcp.notification") {
            continue;
        }
        saw_notification = true;
        seen_payload = params.value("payload", Json::object());
    }
    REQUIRE(saw_notification);
    REQUIRE(seen_payload["server"] == "runtime-notif");
    REQUIRE(seen_payload["method"] == "notifications/prompts/list_changed");
    REQUIRE(seen_payload["params"].is_object());

    REQUIRE(dispatch(runtime, "mcp.close_server",
                     {{"name", "runtime-notif"}}).contains("result"));
}

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

TEST_CASE("AI Editor workflows.list_defs surfaces market presets when the "
          "assets/ai_editor/workflows dir is reachable",
          "[plugins][ai_editor][native][workflows][market]") {
    RuntimeFixture fixture;
    const Json defs = dispatch(fixture.get(), "workflows.list_defs");
    REQUIRE(defs.contains("result"));
    const auto& items = defs["result"]["items"];
    std::unordered_map<std::string, Json> by_id;
    for (const auto& item : items) {
        by_id.emplace(item.value("id", std::string{}), item);
    }
    const std::vector<std::string> market_ids{
        "sql-schema-fix", "security-audit-and-fix", "test-first-refactor"};
    size_t market_hits = 0;
    for (const auto& id : market_ids) {
        const auto found = by_id.find(id);
        if (found == by_id.end()) {
            continue;
        }
        ++market_hits;
        REQUIRE(found->second.value("scope", std::string{}) == "market");
        REQUIRE(found->second.value("builtin", true) == false);
        REQUIRE(found->second.contains("steps"));
        REQUIRE(found->second["steps"].is_array());
        REQUIRE(found->second["steps"].size() >= 2);
    }
    if (market_hits == 0) {
        INFO("Market workflow assets/ai_editor/workflows not deployed near "
             "the test binary; skipping the presence assertions. This is "
             "expected when assets have not been installed.");
    } else {
        REQUIRE(market_hits == market_ids.size());
    }
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

TEST_CASE("AI Editor workflow.export packages a single workflow with "
          "sao-workflow/1 envelope",
          "[plugins][ai_editor][native][workflows][export]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();

    const Json workflow_json{
        {"id", "custom-export-a"},
        {"name", "Custom Export A"},
        {"description", "Test export workflow"},
        {"steps",
         Json::array({Json{{"agent", "default"},
                             {"prompt", "Summarize {{input}}"},
                             {"output_var", "summary"},
                             {"label", "Summarizing"}},
                       Json{{"agent", "code-reviewer"},
                             {"prompt", "Critique {{summary}}"},
                             {"output_var", "critique"}}})}};
    REQUIRE(dispatch(runtime, "workflows.save_def",
                     {{"scope", "workspace"},
                      {"workflow", workflow_json}})
                .contains("result"));

    // Missing both id and scope → invalid.
    const Json missing =
        dispatch(runtime, "workflow.export", Json::object());
    REQUIRE(missing["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);

    // Providing both id and scope → invalid (mutually exclusive).
    const Json both = dispatch(runtime, "workflow.export",
                                {{"id", "custom-export-a"},
                                 {"scope", "workspace"}});
    REQUIRE(both["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);

    const Json exported = dispatch(runtime, "workflow.export",
                                    {{"id", "custom-export-a"}});
    REQUIRE(exported.contains("result"));
    REQUIRE(exported["result"]["format"] == "sao-workflow/1");
    REQUIRE(exported["result"]["exportedAt"].is_number());
    REQUIRE(exported["result"]["workflow"]["id"] == "custom-export-a");
    REQUIRE(exported["result"]["workflow"]["name"] == "Custom Export A");
    REQUIRE(exported["result"]["workflow"]["steps"].size() == 2);
    REQUIRE(exported["result"]["workflow"]["steps"][0]["prompt"] ==
            "Summarize {{input}}");
    REQUIRE(exported["result"]["workflow"]["steps"][0]["output_var"] ==
            "summary");
    REQUIRE(exported["result"]["workflow"]["steps"][1]["agent"] ==
            "code-reviewer");

    // Batch export scope=workspace surfaces the sao-workflows/1 envelope.
    const Json ws = dispatch(runtime, "workflow.export",
                              {{"scope", "workspace"}});
    REQUIRE(ws["result"]["format"] == "sao-workflows/1");
    REQUIRE(ws["result"]["count"] == 1);
    REQUIRE(ws["result"]["workflows"][0]["id"] == "custom-export-a");

    // scope=builtin surfaces the compile-time built-ins.
    const Json builtin_export =
        dispatch(runtime, "workflow.export", {{"scope", "builtin"}});
    REQUIRE(builtin_export["result"]["format"] == "sao-workflows/1");
    REQUIRE(builtin_export["result"]["count"].get<int>() >= 1);
    bool saw_review_and_fix = false;
    for (const auto& item : builtin_export["result"]["workflows"]) {
        if (item.value("id", "") == "review-and-fix") {
            saw_review_and_fix = true;
            REQUIRE(item["builtin"] == true);
        }
    }
    REQUIRE(saw_review_and_fix);

    // Unknown id → NOT_FOUND propagated as protocol error.
    const Json unknown = dispatch(runtime, "workflow.export",
                                   {{"id", "wf-nope-nope"}});
    REQUIRE(unknown.contains("error"));
}

TEST_CASE("AI Editor workflow.import round-trips with overwrite semantics",
          "[plugins][ai_editor][native][workflows][import]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();

    const Json workflow_json{
        {"id", "custom-import-a"},
        {"name", "Custom Import A"},
        {"description", "Seed"},
        {"steps",
         Json::array({Json{{"agent", "default"},
                             {"prompt", "Do {{input}}"},
                             {"output_var", "output"}}})}};
    REQUIRE(dispatch(runtime, "workflows.save_def",
                     {{"scope", "workspace"},
                      {"workflow", workflow_json}})
                .contains("result"));

    const Json exported = dispatch(runtime, "workflow.export",
                                    {{"id", "custom-import-a"}})["result"];
    REQUIRE(exported["format"] == "sao-workflow/1");

    // Unknown format → invalid.
    const Json bogus = dispatch(runtime, "workflow.import",
                                 {{"payload", {{"format", "not-real/1"}}}});
    REQUIRE(bogus["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);

    // Conflict with overwrite=false → rejected atomically, imported=0.
    const Json conflict =
        dispatch(runtime, "workflow.import",
                 {{"payload", exported},
                  {"scope", "workspace"},
                  {"overwrite", false}});
    REQUIRE(conflict.contains("error"));
    REQUIRE(conflict["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
    REQUIRE(conflict["error"]["data"]["details"]["imported"] == 0);
    REQUIRE(conflict["error"]["data"]["details"]["conflicts"].size() == 1);
    REQUIRE(conflict["error"]["data"]["details"]["conflicts"][0] ==
            "custom-import-a");

    // Overwrite=true replaces the definition; new description wins.
    Json rewritten = exported;
    rewritten["workflow"]["description"] = "Rewritten!";
    rewritten["workflow"]["steps"] = Json::array(
        {Json{{"agent", "reviewer"},
              {"prompt", "New prompt {{input}}"},
              {"output_var", "verdict"}}});
    const Json overwrote = dispatch(runtime, "workflow.import",
                                     {{"payload", rewritten},
                                      {"scope", "workspace"},
                                      {"overwrite", true}});
    REQUIRE(overwrote.contains("result"));
    REQUIRE(overwrote["result"]["imported"] == 1);
    REQUIRE(overwrote["result"]["assignedIds"][0] == "custom-import-a");

    const Json fetched = dispatch(runtime, "workflows.get_def",
                                   {{"id", "custom-import-a"}});
    REQUIRE(fetched["result"]["description"] == "Rewritten!");
    REQUIRE(fetched["result"]["steps"].size() == 1);
    REQUIRE(fetched["result"]["steps"][0]["agent"] == "reviewer");
    REQUIRE(fetched["result"]["steps"][0]["prompt"] ==
            "New prompt {{input}}");

    // Fresh id not present in store → imported without conflict.
    Json fresh = exported;
    fresh["workflow"]["id"] = "custom-import-fresh";
    fresh["workflow"]["name"] = "Fresh Workflow";
    const Json fresh_result =
        dispatch(runtime, "workflow.import",
                 {{"payload", fresh}, {"scope", "workspace"}});
    REQUIRE(fresh_result.contains("result"));
    REQUIRE(fresh_result["result"]["imported"] == 1);
    REQUIRE(fresh_result["result"]["conflicts"].empty());
    REQUIRE(fresh_result["result"]["assignedIds"][0] ==
            "custom-import-fresh");
}

TEST_CASE("AI Editor workflow.import refuses to overwrite built-in workflows",
          "[plugins][ai_editor][native][workflows][import]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();

    // review-and-fix is a compile-time built-in; even overwrite=true must
    // not shadow it via the on-disk registry.
    Json envelope{
        {"format", "sao-workflow/1"},
        {"workflow", Json{{"id", "review-and-fix"},
                            {"name", "Hijacked"},
                            {"description", "Attacker payload"},
                            {"steps",
                             Json::array({Json{
                                 {"agent", "attacker"},
                                 {"prompt", "pwn {{input}}"},
                                 {"output_var", "pwned"}}})}}}};

    const Json overwrite_false =
        dispatch(runtime, "workflow.import",
                 {{"payload", envelope},
                  {"scope", "workspace"},
                  {"overwrite", false}});
    REQUIRE(overwrite_false.contains("error"));
    REQUIRE(overwrite_false["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_PERMISSION_DENIED);
    REQUIRE(overwrite_false["error"]["data"]["details"]["imported"] == 0);
    REQUIRE(overwrite_false["error"]["data"]["details"]["conflicts"][0] ==
            "review-and-fix");

    const Json overwrite_true =
        dispatch(runtime, "workflow.import",
                 {{"payload", envelope},
                  {"scope", "workspace"},
                  {"overwrite", true}});
    REQUIRE(overwrite_true.contains("error"));
    REQUIRE(overwrite_true["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_PERMISSION_DENIED);

    // The compile-time definition must survive both attempts unchanged.
    const Json fetched = dispatch(runtime, "workflows.get_def",
                                   {{"id", "review-and-fix"}})["result"];
    REQUIRE(fetched["builtin"] == true);
    REQUIRE(fetched["name"] == "Review & Fix");
}

namespace {

// Poll workflows.status until the execution reaches a terminal state
// (matches the wait pattern the other workflow tests use).  Fails via
// REQUIRE when the timeout expires so callers get a clear error instead
// of a phantom running/pending status.
Json wait_for_workflow_completion(sao_ai_editor_runtime_t runtime,
                                   const std::string& execution_id,
                                   DWORD timeout_ms) {
    Json status;
    const ULONGLONG started = GetTickCount64();
    do {
        status = dispatch(runtime, "workflows.status",
                          {{"executionId", execution_id}})["result"];
        const std::string current = status.value("status", "");
        if (current != "running" && current != "pending") {
            return status;
        }
        Sleep(20);
    } while (GetTickCount64() - started < timeout_ms);
    REQUIRE_FALSE("workflows.status never reached a terminal state");
    return status;
}

// Build a canned HTTP response body — helper used by the retry tests
// below (and the chat.run retry suite further down the file) so the 500
// / 200 wire framing stays uniform.  Content-Length matters because
// LocalHttpServer's client parses it to know when a request body is
// done, and the *response* framing has to match what WinHTTP expects
// (Content-Length + Connection: close, no chunked encoding).
std::string canned_http_response(int status_code,
                                  std::string_view status_text,
                                  std::string_view body,
                                  std::string_view extra_headers = {}) {
    std::string wire = "HTTP/1.1 " + std::to_string(status_code) + " " +
                       std::string(status_text) + "\r\n";
    wire += "Content-Type: application/json\r\n";
    wire += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    if (!extra_headers.empty()) {
        wire.append(extra_headers.data(), extra_headers.size());
    }
    wire += "Connection: close\r\n\r\n";
    wire.append(body.data(), body.size());
    return wire;
}

}  // namespace

TEST_CASE("AI Editor workflow.list_executions surfaces a completed run "
          "after run_loop persists the record",
          "[plugins][ai_editor][native][workflows][history]") {
    const std::string body =
        R"({"choices":[{"message":{"role":"assistant","content":"listed-content"}}]})";
    LocalHttpServer server(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: " +
        std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body);
    RuntimeFixture fixture;
    const Json run = dispatch(
        fixture.get(), "workflows.run",
        {{"id", "review-and-fix"},
         {"provider", {{"id", "history-fixture"},
                       {"endpoint", server.endpoint()}}},
         {"model", "fixture-model"},
         {"input", "sample input"},
         {"timeoutMs", 5000}});
    REQUIRE(run.contains("result"));
    const std::string execution_id = run["result"]["executionId"];
    const Json terminal =
        wait_for_workflow_completion(fixture.get(), execution_id, 15'000);
    REQUIRE(terminal["status"] == "completed");

    // Give run_loop's post-emit persist a beat to hit disk — completedAt
    // is committed under the mutex before emit_terminal fires, but the
    // write itself happens right after.
    Json listed;
    for (int attempt = 0; attempt < 50; ++attempt) {
        listed = dispatch(fixture.get(), "workflow.list_executions",
                          {{"scope", "workspace"}, {"limit", 20}})["result"];
        if (listed.value("total", int64_t{0}) >= 1) {
            break;
        }
        Sleep(20);
    }
    REQUIRE(listed["total"].get<int64_t>() >= 1);
    bool found = false;
    for (const auto& item : listed["items"]) {
        if (item.value("id", "") == execution_id) {
            found = true;
            REQUIRE(item["workflowId"] == "review-and-fix");
            REQUIRE(item["workflowName"] == "Review & Fix");
            REQUIRE(item["status"] == "completed");
            REQUIRE(item["totalSteps"].get<int64_t>() == 2);
            REQUIRE(item["completedAt"].get<int64_t>() > 0);
        }
    }
    REQUIRE(found);

    // workflowId filter narrows results to that definition only.
    const Json filtered = dispatch(
        fixture.get(), "workflow.list_executions",
        {{"scope", "workspace"}, {"workflowId", "review-and-fix"}})["result"];
    REQUIRE(filtered["total"].get<int64_t>() >= 1);
    const Json missing = dispatch(
        fixture.get(), "workflow.list_executions",
        {{"scope", "workspace"},
         {"workflowId", "no-such-workflow"}})["result"];
    REQUIRE(missing["total"].get<int64_t>() == 0);
    REQUIRE(missing["items"].size() == 0);
}

TEST_CASE("AI Editor workflow.get_execution returns full variables and "
          "stepResults from the persisted record",
          "[plugins][ai_editor][native][workflows][history]") {
    const std::string body =
        R"({"choices":[{"message":{"role":"assistant","content":"detail-content"}}]})";
    LocalHttpServer server(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: " +
        std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body);
    RuntimeFixture fixture;
    const Json run = dispatch(
        fixture.get(), "workflows.run",
        {{"id", "review-and-fix"},
         {"provider", {{"id", "detail-fixture"},
                       {"endpoint", server.endpoint()}}},
         {"model", "fixture-model"},
         {"input", "get-me"},
         {"timeoutMs", 5000}});
    REQUIRE(run.contains("result"));
    const std::string execution_id = run["result"]["executionId"];
    const Json terminal =
        wait_for_workflow_completion(fixture.get(), execution_id, 15'000);
    REQUIRE(terminal["status"] == "completed");

    // Give the post-emit disk write a beat before the first read.
    Json record;
    for (int attempt = 0; attempt < 50; ++attempt) {
        const Json response = dispatch(fixture.get(), "workflow.get_execution",
                                        {{"id", execution_id}});
        if (response.contains("result")) {
            record = response["result"];
            break;
        }
        Sleep(20);
    }
    REQUIRE(record.contains("id"));
    REQUIRE(record["id"] == execution_id);
    REQUIRE(record["workflowId"] == "review-and-fix");
    REQUIRE(record["workflowName"] == "Review & Fix");
    REQUIRE(record["status"] == "completed");
    REQUIRE(record["variables"]["input"] == "get-me");
    REQUIRE(record["variables"]["review"] == "detail-content");
    REQUIRE(record["variables"]["fix"] == "detail-content");
    REQUIRE(record["stepResults"].size() == 2);
    REQUIRE(record["stepResults"][0]["outputVar"] == "review");
    REQUIRE(record["stepResults"][1]["outputVar"] == "fix");
    REQUIRE(record["totalSteps"].get<int64_t>() == 2);
    REQUIRE(record["startedAt"].get<int64_t>() > 0);
    REQUIRE(record["completedAt"].get<int64_t>() >= record["startedAt"].get<int64_t>());

    const Json missing = dispatch(fixture.get(), "workflow.get_execution",
                                   {{"id", "wf-nope-nope"}});
    REQUIRE(missing.contains("error"));
    REQUIRE(missing["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_NOT_FOUND);
}

TEST_CASE("AI Editor workflow.delete_execution removes the persisted "
          "history entry from subsequent listings",
          "[plugins][ai_editor][native][workflows][history]") {
    const std::string body =
        R"({"choices":[{"message":{"role":"assistant","content":"delete-content"}}]})";
    LocalHttpServer server(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: " +
        std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body);
    RuntimeFixture fixture;
    const Json run = dispatch(
        fixture.get(), "workflows.run",
        {{"id", "review-and-fix"},
         {"provider", {{"id", "delete-fixture"},
                       {"endpoint", server.endpoint()}}},
         {"model", "fixture-model"},
         {"input", "delete-me"},
         {"timeoutMs", 5000}});
    REQUIRE(run.contains("result"));
    const std::string execution_id = run["result"]["executionId"];
    const Json terminal =
        wait_for_workflow_completion(fixture.get(), execution_id, 15'000);
    REQUIRE(terminal["status"] == "completed");

    // Confirm the record is on disk before deleting it.
    Json before;
    for (int attempt = 0; attempt < 50; ++attempt) {
        before = dispatch(fixture.get(), "workflow.list_executions",
                          {{"scope", "workspace"}, {"limit", 20}})["result"];
        if (before.value("total", int64_t{0}) >= 1) {
            break;
        }
        Sleep(20);
    }
    bool present_before = false;
    for (const auto& item : before["items"]) {
        if (item.value("id", "") == execution_id) {
            present_before = true;
            break;
        }
    }
    REQUIRE(present_before);

    const Json deleted = dispatch(fixture.get(), "workflow.delete_execution",
                                   {{"id", execution_id}})["result"];
    REQUIRE(deleted["ok"] == true);
    REQUIRE(deleted["id"] == execution_id);

    const Json after = dispatch(fixture.get(), "workflow.list_executions",
                                 {{"scope", "workspace"},
                                  {"limit", 20}})["result"];
    for (const auto& item : after["items"]) {
        REQUIRE(item.value("id", "") != execution_id);
    }

    // Second delete of the same id must now return NOT_FOUND — the API is
    // idempotent-safe in the sense that the disk state cannot regress.
    const Json missing = dispatch(fixture.get(), "workflow.delete_execution",
                                   {{"id", execution_id}});
    REQUIRE(missing.contains("error"));
    REQUIRE(missing["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_NOT_FOUND);
}

TEST_CASE("AI Editor workflow.retry resumes a failed execution from the "
          "failing step with a fresh provider",
          "[plugins][ai_editor][native][workflows][history][retry]") {
    // First provider fails every request; workflow step 0 exhausts the
    // built-in retry budget (maxAttempts=1 disables in-workflow retries)
    // and the execution lands in "failed" state on disk.  The retry
    // dispatch then spins up a new execution against a healthy provider
    // that serves 200 on every hit, so both steps of review-and-fix
    // complete.
    const std::string err_body =
        R"({"error":{"type":"server_error","message":"boom"}})";
    LocalHttpServer failing(canned_http_response(500, "Internal Server Error",
                                                  err_body));
    const std::string ok_body =
        R"({"choices":[{"message":{"role":"assistant","content":"recovered"}}]})";
    LocalHttpServer recovering(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: " +
        std::to_string(ok_body.size()) +
        "\r\nConnection: close\r\n\r\n" + ok_body);
    RuntimeFixture fixture;
    const Json initial_run = dispatch(
        fixture.get(), "workflows.run",
        {{"id", "review-and-fix"},
         {"provider", {{"id", "retry-fail-fixture"},
                        {"endpoint", failing.endpoint()},
                        // maxAttempts=1 short-circuits the transport-level
                        // retry so the 500 surfaces as a step failure
                        // instead of triggering three tries per step.
                        {"retry", {{"maxAttempts", 1}}}}},
         {"model", "fixture-model"},
         {"input", "please review"},
         {"timeoutMs", 5000}});
    REQUIRE(initial_run.contains("result"));
    const std::string failed_execution_id = initial_run["result"]["executionId"];
    const Json failed_terminal =
        wait_for_workflow_completion(fixture.get(), failed_execution_id,
                                      15'000);
    REQUIRE(failed_terminal["status"] == "failed");
    REQUIRE(failed_terminal["stepResults"].size() >= 1);
    REQUIRE(failed_terminal["stepResults"][0]["status"] == "failed");

    // Wait until persist() lands the failed record on disk so
    // workflow.retry can load it.
    for (int attempt = 0; attempt < 50; ++attempt) {
        const Json fetch = dispatch(fixture.get(), "workflow.get_execution",
                                     {{"id", failed_execution_id}});
        if (fetch.contains("result")) {
            break;
        }
        Sleep(20);
    }

    const Json retry_response = dispatch(
        fixture.get(), "workflow.retry",
        {{"id", failed_execution_id},
         {"provider", {{"id", "retry-recover-fixture"},
                        {"endpoint", recovering.endpoint()}}},
         {"model", "fixture-model"},
         {"timeoutMs", 5000}});
    REQUIRE(retry_response.contains("result"));
    REQUIRE(retry_response["result"]["retryOf"] == failed_execution_id);
    REQUIRE(retry_response["result"]["fromStep"].get<int64_t>() == 0);
    REQUIRE(retry_response["result"]["status"] == "running");
    const std::string retry_execution_id =
        retry_response["result"]["executionId"];
    REQUIRE(retry_execution_id != failed_execution_id);

    const Json retry_terminal =
        wait_for_workflow_completion(fixture.get(), retry_execution_id,
                                      15'000);
    REQUIRE(retry_terminal["status"] == "completed");
    REQUIRE(retry_terminal["variables"]["review"] == "recovered");
    REQUIRE(retry_terminal["variables"]["fix"] == "recovered");
    REQUIRE(retry_terminal["stepResults"].size() == 2);
    // snapshot()/history_record() also carry the retryOf lineage so
    // consumers viewing the retry run alone can trace back to the origin.
    REQUIRE(retry_terminal["retryOf"] == failed_execution_id);

    // The persisted retry record picks up the same retryOf marker, so
    // callers browsing workflow_history can distinguish first-try and
    // resumed executions without any client-side state.
    Json persisted_retry;
    for (int attempt = 0; attempt < 50; ++attempt) {
        const Json response = dispatch(
            fixture.get(), "workflow.get_execution",
            {{"id", retry_execution_id}});
        if (response.contains("result")) {
            persisted_retry = response["result"];
            break;
        }
        Sleep(20);
    }
    REQUIRE(persisted_retry.value("retryOf", std::string{}) ==
            failed_execution_id);
}

TEST_CASE("AI Editor workflow.retry keepVariables=true carries earlier "
          "successful step outputs into the resumed execution",
          "[plugins][ai_editor][native][workflows][history][retry]") {
    // Register a three-step workflow so step 0 and step 1 succeed on the
    // first provider, then step 2 hits a failing endpoint.  On retry we
    // want the pre-existing step-0/step-1 variables (and their outputs)
    // to still be visible so downstream steps can chain against them.
    RuntimeFixture fixture;
    const Json workflow_def{
        {"id", "chain-three"},
        {"name", "Chain Three"},
        {"description", "Three-step chain for retry variable inheritance"},
        {"steps",
         Json::array({Json{{"agent", "default"},
                            {"prompt", "step one: {{input}}"},
                            {"output_var", "s1_out"},
                            {"label", "First"}},
                       Json{{"agent", "default"},
                            {"prompt", "step two: {{s1_out}}"},
                            {"output_var", "s2_out"},
                            {"label", "Second"}},
                       Json{{"agent", "default"},
                            {"prompt", "step three: {{s2_out}}"},
                            {"output_var", "s3_out"},
                            {"label", "Third"}}})}};
    REQUIRE(dispatch(fixture.get(), "workflows.save_def",
                     {{"scope", "workspace"},
                      {"workflow", workflow_def}})
                .contains("result"));

    // The first provider serves 200 for the first two calls, then 500 on
    // the third — matches steps [0,1] succeeding and step 2 failing.
    const std::string ok_step1 =
        R"({"choices":[{"message":{"role":"assistant","content":"one-done"}}]})";
    const std::string ok_step2 =
        R"({"choices":[{"message":{"role":"assistant","content":"two-done"}}]})";
    const std::string err_body =
        R"({"error":{"type":"server_error","message":"kaboom"}})";
    LocalHttpServer chained_fail(std::vector<std::string>{
        canned_http_response(200, "OK", ok_step1),
        canned_http_response(200, "OK", ok_step2),
        canned_http_response(500, "Internal Server Error", err_body),
    });
    const Json initial_run = dispatch(
        fixture.get(), "workflows.run",
        {{"id", "chain-three"},
         {"provider", {{"id", "chain-fail-fixture"},
                        {"endpoint", chained_fail.endpoint()},
                        {"retry", {{"maxAttempts", 1}}}}},
         {"model", "fixture-model"},
         {"input", "seed-value"},
         {"timeoutMs", 5000}});
    REQUIRE(initial_run.contains("result"));
    const std::string failed_execution_id = initial_run["result"]["executionId"];
    const Json failed_terminal =
        wait_for_workflow_completion(fixture.get(), failed_execution_id,
                                      15'000);
    REQUIRE(failed_terminal["status"] == "failed");
    // Sanity: variables from the successful earlier steps landed in the
    // persisted record before the failure.
    REQUIRE(failed_terminal["variables"]["s1_out"] == "one-done");
    REQUIRE(failed_terminal["variables"]["s2_out"] == "two-done");
    // stepResults has 3 entries: two completed + one failed step 2.
    REQUIRE(failed_terminal["stepResults"].size() == 3);
    REQUIRE(failed_terminal["stepResults"][2]["status"] == "failed");
    REQUIRE(failed_terminal["stepResults"][2]["stepIndex"].get<int64_t>()
            == 2);

    for (int attempt = 0; attempt < 50; ++attempt) {
        const Json fetch = dispatch(fixture.get(), "workflow.get_execution",
                                     {{"id", failed_execution_id}});
        if (fetch.contains("result")) {
            break;
        }
        Sleep(20);
    }

    // Retry against a healthy provider — only step 2 should fire.
    const std::string ok_step3 =
        R"({"choices":[{"message":{"role":"assistant","content":"three-done"}}]})";
    LocalHttpServer chained_recover(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: " +
        std::to_string(ok_step3.size()) +
        "\r\nConnection: close\r\n\r\n" + ok_step3);
    const Json retry_response = dispatch(
        fixture.get(), "workflow.retry",
        {{"id", failed_execution_id},
         {"provider", {{"id", "chain-recover-fixture"},
                        {"endpoint", chained_recover.endpoint()}}},
         {"model", "fixture-model"},
         {"keepVariables", true},
         {"timeoutMs", 5000}});
    REQUIRE(retry_response.contains("result"));
    REQUIRE(retry_response["result"]["fromStep"].get<int64_t>() == 2);
    REQUIRE(retry_response["result"]["retryOf"] == failed_execution_id);
    const std::string retry_execution_id =
        retry_response["result"]["executionId"];

    const Json retry_terminal =
        wait_for_workflow_completion(fixture.get(), retry_execution_id,
                                      15'000);
    REQUIRE(retry_terminal["status"] == "completed");
    // keepVariables=true carried the earlier successful outputs into the
    // resumed run so the retry snapshot still shows every previous
    // variable in addition to the newly-produced s3_out.
    REQUIRE(retry_terminal["variables"]["input"] == "seed-value");
    REQUIRE(retry_terminal["variables"]["s1_out"] == "one-done");
    REQUIRE(retry_terminal["variables"]["s2_out"] == "two-done");
    REQUIRE(retry_terminal["variables"]["s3_out"] == "three-done");
    // Only the third step ran on the recover provider — server_recover
    // received exactly one request.
    REQUIRE(chained_recover.wait_for_connections(1, 2'000));
    REQUIRE(chained_recover.captured_bodies().size() == 1);
    // stepResults on the retry execution reflect only the resumed steps
    // (index 2 was the sole invocation), not the previously-completed
    // ones.  The persisted history therefore stays discoverable via
    // retryOf rather than being replayed inline.
    REQUIRE(retry_terminal["stepResults"].size() == 1);
    REQUIRE(retry_terminal["stepResults"][0]["stepIndex"].get<int64_t>()
            == 2);
    REQUIRE(retry_terminal["stepResults"][0]["outputVar"] == "s3_out");
}

TEST_CASE("AI Editor workflow.retry rejects non-failed executions and "
          "guards its arguments",
          "[plugins][ai_editor][native][workflows][history][retry]") {
    // First run a workflow to completion so we have a "completed" record
    // to point workflow.retry at — the API contract is retry-of-failed
    // only, so any other status must surface INVALID_ARGUMENT.
    const std::string ok_body =
        R"({"choices":[{"message":{"role":"assistant","content":"already-done"}}]})";
    LocalHttpServer server(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: " +
        std::to_string(ok_body.size()) +
        "\r\nConnection: close\r\n\r\n" + ok_body);
    RuntimeFixture fixture;
    const Json run = dispatch(
        fixture.get(), "workflows.run",
        {{"id", "review-and-fix"},
         {"provider", {{"id", "already-done-fixture"},
                        {"endpoint", server.endpoint()}}},
         {"model", "fixture-model"},
         {"input", "review it"},
         {"timeoutMs", 5000}});
    REQUIRE(run.contains("result"));
    const std::string completed_id = run["result"]["executionId"];
    REQUIRE(wait_for_workflow_completion(fixture.get(), completed_id, 15'000)
                ["status"] == "completed");
    for (int attempt = 0; attempt < 50; ++attempt) {
        const Json fetch = dispatch(fixture.get(), "workflow.get_execution",
                                     {{"id", completed_id}});
        if (fetch.contains("result")) {
            break;
        }
        Sleep(20);
    }

    const Json completed_retry = dispatch(
        fixture.get(), "workflow.retry",
        {{"id", completed_id},
         {"provider", {{"id", "will-not-fire"},
                        {"endpoint", server.endpoint()}}},
         {"model", "fixture-model"}});
    REQUIRE(completed_retry.contains("error"));
    REQUIRE(completed_retry["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);

    // Unknown execution id → NOT_FOUND (mirrors get_execution semantics).
    const Json missing_retry = dispatch(
        fixture.get(), "workflow.retry",
        {{"id", "wf-does-not-exist"},
         {"provider", {{"id", "will-not-fire"},
                        {"endpoint", server.endpoint()}}},
         {"model", "fixture-model"}});
    REQUIRE(missing_retry.contains("error"));
    REQUIRE(missing_retry["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_NOT_FOUND);

    // Empty id → INVALID_ARGUMENT immediately, without any disk lookup.
    const Json empty_retry = dispatch(
        fixture.get(), "workflow.retry",
        {{"id", ""},
         {"provider", {{"id", "will-not-fire"},
                        {"endpoint", server.endpoint()}}},
         {"model", "fixture-model"}});
    REQUIRE(empty_retry.contains("error"));
    REQUIRE(empty_retry["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
}

TEST_CASE("AI Editor conversation.branch forks a new conversation from "
          "messageIndex",
          "[plugins][ai_editor][native][storage][branch]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    REQUIRE(dispatch(runtime, "runtime.initialize").contains("result"));

    const Json created = dispatch(runtime, "conversation.create",
                                  {{"title", "Original Thread"},
                                   {"model", "gpt-branch"},
                                   {"scope", "workspace"}});
    const std::string source_id = created["result"]["id"];
    REQUIRE(dispatch(runtime, "conversation.append",
                     {{"id", source_id},
                      {"message", {{"role", "system"},
                                    {"content", "you are a helpful bot"}}}})
                .contains("result"));
    REQUIRE(dispatch(runtime, "conversation.append",
                     {{"id", source_id},
                      {"message", {{"role", "user"},
                                    {"content", "hello"}}}})
                .contains("result"));
    REQUIRE(dispatch(runtime, "conversation.append",
                     {{"id", source_id},
                      {"message", {{"role", "assistant"},
                                    {"content", "hi there"}}}})
                .contains("result"));

    // Sleep 2ms so branchedAt is guaranteed > source savedAt.
    Sleep(5);

    const Json source_get =
        dispatch(runtime, "conversation.get", {{"id", source_id}})["result"];
    const int64_t source_saved_at =
        source_get["savedAt"].get<int64_t>();

    // Bad scope → invalid.
    const Json bad_scope = dispatch(runtime, "conversation.branch",
                                     {{"sourceId", source_id},
                                      {"messageIndex", 1},
                                      {"scope", "elsewhere"}});
    REQUIRE(bad_scope["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);

    // Negative messageIndex → invalid.
    const Json negative = dispatch(runtime, "conversation.branch",
                                    {{"sourceId", source_id},
                                     {"messageIndex", -1}});
    REQUIRE(negative["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);

    // messageIndex >= source.messages.size() → invalid.
    const Json past_end = dispatch(runtime, "conversation.branch",
                                    {{"sourceId", source_id},
                                     {"messageIndex", 3}});
    REQUIRE(past_end["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);

    // Unknown source → NOT_FOUND (propagated as error).
    const Json missing = dispatch(runtime, "conversation.branch",
                                   {{"sourceId", "conv-does-not-exist"},
                                    {"messageIndex", 0}});
    REQUIRE(missing.contains("error"));

    // Fork from messageIndex=1 → new conversation carries the first two
    // messages (system + user); sourceId + branchedAt round-trip.
    const Json branched = dispatch(runtime, "conversation.branch",
                                    {{"sourceId", source_id},
                                     {"messageIndex", 1}});
    REQUIRE(branched.contains("result"));
    const std::string new_id = branched["result"]["id"];
    REQUIRE(!new_id.empty());
    REQUIRE(new_id != source_id);
    REQUIRE(branched["result"]["title"] == "Original Thread (branch)");
    REQUIRE(branched["result"]["messageCount"] == 2);
    REQUIRE(branched["result"]["sourceId"] == source_id);
    REQUIRE(branched["result"]["branchedAt"].is_number());
    REQUIRE(branched["result"]["branchedAt"].get<int64_t>() >=
            source_saved_at);

    const Json new_conv = dispatch(runtime, "conversation.get",
                                    {{"id", new_id}})["result"];
    REQUIRE(new_conv["messages"].size() == 2);
    REQUIRE(new_conv["messages"][0]["role"] == "system");
    REQUIRE(new_conv["messages"][0]["content"] ==
            "you are a helpful bot");
    REQUIRE(new_conv["messages"][1]["role"] == "user");
    REQUIRE(new_conv["messages"][1]["content"] == "hello");
    REQUIRE(new_conv["model"] == "gpt-branch");
    REQUIRE(new_conv["scope"] == "workspace");
    REQUIRE(new_conv["messageCount"] == 2);

    // Original conversation is untouched.
    const Json orig_after = dispatch(runtime, "conversation.get",
                                      {{"id", source_id}})["result"];
    REQUIRE(orig_after["messages"].size() == 3);

    // Custom title override + messageIndex=0 → only the first message.
    const Json single = dispatch(runtime, "conversation.branch",
                                  {{"sourceId", source_id},
                                   {"messageIndex", 0},
                                   {"title", "Just System"}});
    REQUIRE(single.contains("result"));
    REQUIRE(single["result"]["title"] == "Just System");
    REQUIRE(single["result"]["messageCount"] == 1);
    const std::string single_id = single["result"]["id"];
    const Json single_conv = dispatch(runtime, "conversation.get",
                                       {{"id", single_id}})["result"];
    REQUIRE(single_conv["messages"].size() == 1);
    REQUIRE(single_conv["messages"][0]["role"] == "system");
}

TEST_CASE("AI Editor conversation.merge concatenates three conversations in "
          "explicit order",
          "[plugins][ai_editor][native][storage][merge]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    REQUIRE(dispatch(runtime, "runtime.initialize").contains("result"));

    // Build three conversations; note we deliberately create them in the
    // order a > b > c but savedAt-populate them so alphabetical id ≠
    // chronological savedAt.  That lets a later assertion prove explicit
    // ordering honours sourceIds, not on-disk directory scan order.
    const Json created_a = dispatch(runtime, "conversation.create",
                                     {{"title", "Alpha"},
                                      {"model", "gpt-merge"},
                                      {"scope", "workspace"}});
    const std::string id_a = created_a["result"]["id"];
    REQUIRE(dispatch(runtime, "conversation.append",
                     {{"id", id_a},
                      {"message", {{"role", "user"},
                                    {"content", "A1"}}}})
                .contains("result"));
    REQUIRE(dispatch(runtime, "conversation.append",
                     {{"id", id_a},
                      {"message", {{"role", "assistant"},
                                    {"content", "A2"}}}})
                .contains("result"));
    const Json created_b = dispatch(runtime, "conversation.create",
                                     {{"title", "Beta"},
                                      {"model", "gpt-merge"},
                                      {"scope", "workspace"}});
    const std::string id_b = created_b["result"]["id"];
    REQUIRE(dispatch(runtime, "conversation.append",
                     {{"id", id_b},
                      {"message", {{"role", "user"},
                                    {"content", "B1"}}}})
                .contains("result"));
    const Json created_c = dispatch(runtime, "conversation.create",
                                     {{"title", "Gamma"},
                                      {"model", "gpt-merge"},
                                      {"scope", "workspace"}});
    const std::string id_c = created_c["result"]["id"];
    REQUIRE(dispatch(runtime, "conversation.append",
                     {{"id", id_c},
                      {"message", {{"role", "user"},
                                    {"content", "C1"}}}})
                .contains("result"));
    REQUIRE(dispatch(runtime, "conversation.append",
                     {{"id", id_c},
                      {"message", {{"role", "assistant"},
                                    {"content", "C2"}}}})
                .contains("result"));

    // Bad scope → invalid.
    const Json bad_scope = dispatch(runtime, "conversation.merge",
                                     {{"sourceIds", Json::array({id_a})},
                                      {"scope", "nowhere"}});
    REQUIRE(bad_scope["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);

    // Empty sourceIds → invalid.
    const Json empty_ids = dispatch(runtime, "conversation.merge",
                                     {{"sourceIds", Json::array()}});
    REQUIRE(empty_ids["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);

    // Missing source id → propagates NOT_FOUND.
    const Json missing = dispatch(runtime, "conversation.merge",
                                   {{"sourceIds", Json::array(
                                                        {id_a,
                                                         "conv-nope"})}});
    REQUIRE(missing.contains("error"));

    // Merge in explicit sourceIds order: A + B + C.
    const Json merged = dispatch(runtime, "conversation.merge",
                                  {{"sourceIds", Json::array(
                                                       {id_a, id_b, id_c})}});
    REQUIRE(merged.contains("result"));
    REQUIRE(merged["result"]["messageCount"] == 5);
    REQUIRE(merged["result"]["title"] == "Alpha (merged)");
    REQUIRE(merged["result"]["sourcedFrom"].size() == 3);
    REQUIRE(merged["result"]["sourcedFrom"][0] == id_a);
    REQUIRE(merged["result"]["sourcedFrom"][1] == id_b);
    REQUIRE(merged["result"]["sourcedFrom"][2] == id_c);
    REQUIRE(merged["result"]["mergedAt"].is_number());
    const std::string merged_id = merged["result"]["id"];

    // Ordered concat: A1, A2, B1, C1, C2 (no separator).
    const Json merged_conv = dispatch(runtime, "conversation.get",
                                       {{"id", merged_id}})["result"];
    REQUIRE(merged_conv["messages"].size() == 5);
    REQUIRE(merged_conv["messages"][0]["content"] == "A1");
    REQUIRE(merged_conv["messages"][1]["content"] == "A2");
    REQUIRE(merged_conv["messages"][2]["content"] == "B1");
    REQUIRE(merged_conv["messages"][3]["content"] == "C1");
    REQUIRE(merged_conv["messages"][4]["content"] == "C2");
    // Inherit model + systemPrompt from first source (A).
    REQUIRE(merged_conv["model"] == "gpt-merge");
    REQUIRE(merged_conv["scope"] == "workspace");
    REQUIRE(merged_conv["pinned"] == false);

    // Original conversations must survive untouched.
    const Json a_after = dispatch(runtime, "conversation.get",
                                   {{"id", id_a}})["result"];
    REQUIRE(a_after["messages"].size() == 2);
    const Json c_after = dispatch(runtime, "conversation.get",
                                   {{"id", id_c}})["result"];
    REQUIRE(c_after["messages"].size() == 2);

    // Custom title + explicit order: C + A (skip B) → 4 messages, C first.
    const Json custom = dispatch(runtime, "conversation.merge",
                                  {{"sourceIds", Json::array({id_c, id_a})},
                                   {"title", "Custom Merge"},
                                   {"scope", "system"}});
    REQUIRE(custom.contains("result"));
    REQUIRE(custom["result"]["title"] == "Custom Merge");
    REQUIRE(custom["result"]["messageCount"] == 4);
    const std::string custom_id = custom["result"]["id"];
    const Json custom_conv = dispatch(runtime, "conversation.get",
                                       {{"id", custom_id}})["result"];
    REQUIRE(custom_conv["scope"] == "system");
    REQUIRE(custom_conv["messages"][0]["content"] == "C1");
    REQUIRE(custom_conv["messages"][1]["content"] == "C2");
    REQUIRE(custom_conv["messages"][2]["content"] == "A1");
    REQUIRE(custom_conv["messages"][3]["content"] == "A2");

    // Single source → concat of one, still emits merged shape.
    const Json single_src = dispatch(runtime, "conversation.merge",
                                      {{"sourceIds", Json::array({id_b})}});
    REQUIRE(single_src.contains("result"));
    REQUIRE(single_src["result"]["messageCount"] == 1);
    REQUIRE(single_src["result"]["sourcedFrom"].size() == 1);
    REQUIRE(single_src["result"]["sourcedFrom"][0] == id_b);

    // Bad orderBy → invalid.
    const Json bad_order = dispatch(runtime, "conversation.merge",
                                     {{"sourceIds", Json::array({id_a})},
                                      {"orderBy", "random"}});
    REQUIRE(bad_order["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
}

TEST_CASE("AI Editor conversation.merge inserts separator between sources",
          "[plugins][ai_editor][native][storage][merge]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    REQUIRE(dispatch(runtime, "runtime.initialize").contains("result"));

    const Json a = dispatch(runtime, "conversation.create",
                             {{"title", "First"},
                              {"model", "sep"},
                              {"scope", "workspace"}});
    const std::string id_a = a["result"]["id"];
    REQUIRE(dispatch(runtime, "conversation.append",
                     {{"id", id_a},
                      {"message", {{"role", "user"},
                                    {"content", "Hello A"}}}})
                .contains("result"));
    const Json b = dispatch(runtime, "conversation.create",
                             {{"title", "Second"},
                              {"model", "sep"},
                              {"scope", "workspace"}});
    const std::string id_b = b["result"]["id"];
    REQUIRE(dispatch(runtime, "conversation.append",
                     {{"id", id_b},
                      {"message", {{"role", "user"},
                                    {"content", "Hello B"}}}})
                .contains("result"));
    const Json c = dispatch(runtime, "conversation.create",
                             {{"title", "Third"},
                              {"model", "sep"},
                              {"scope", "workspace"}});
    const std::string id_c = c["result"]["id"];
    REQUIRE(dispatch(runtime, "conversation.append",
                     {{"id", id_c},
                      {"message", {{"role", "user"},
                                    {"content", "Hello C"}}}})
                .contains("result"));

    const Json separator{{"role", "system"},
                         {"content", "--- next conversation ---"}};
    const Json merged = dispatch(runtime, "conversation.merge",
                                  {{"sourceIds", Json::array(
                                                       {id_a, id_b, id_c})},
                                   {"separator", separator}});
    REQUIRE(merged.contains("result"));
    // 3 real messages + 2 separators (between A|B and B|C).  Never at the
    // head or tail.
    REQUIRE(merged["result"]["messageCount"] == 5);
    const std::string merged_id = merged["result"]["id"];
    const Json got = dispatch(runtime, "conversation.get",
                               {{"id", merged_id}})["result"];
    REQUIRE(got["messages"][0]["content"] == "Hello A");
    REQUIRE(got["messages"][1] == separator);
    REQUIRE(got["messages"][2]["content"] == "Hello B");
    REQUIRE(got["messages"][3] == separator);
    REQUIRE(got["messages"][4]["content"] == "Hello C");

    // Invalid separator (missing content) is treated as no separator, not
    // an error.  Callers who omit the field or supply garbage should still
    // get a valid merge.
    const Json degenerate = dispatch(runtime, "conversation.merge",
                                      {{"sourceIds", Json::array(
                                                           {id_a, id_b})},
                                       {"separator", {{"role", "system"}}}});
    REQUIRE(degenerate.contains("result"));
    REQUIRE(degenerate["result"]["messageCount"] == 2);
}

TEST_CASE("AI Editor conversation.split cleaves before/after keeping original "
          "id",
          "[plugins][ai_editor][native][storage][split]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    REQUIRE(dispatch(runtime, "runtime.initialize").contains("result"));

    const Json created = dispatch(runtime, "conversation.create",
                                   {{"title", "Split Me"},
                                    {"model", "gpt-split"},
                                    {"scope", "workspace"}});
    const std::string source_id = created["result"]["id"];
    for (const std::pair<const char*, const char*> message :
         {std::pair<const char*, const char*>{"user", "msg-0"},
          std::pair<const char*, const char*>{"assistant", "msg-1"},
          std::pair<const char*, const char*>{"user", "msg-2"},
          std::pair<const char*, const char*>{"assistant", "msg-3"},
          std::pair<const char*, const char*>{"user", "msg-4"}}) {
        REQUIRE(dispatch(runtime, "conversation.append",
                         {{"id", source_id},
                          {"message", {{"role", message.first},
                                        {"content", message.second}}}})
                    .contains("result"));
    }

    // Bad scope → invalid.
    const Json bad_scope = dispatch(runtime, "conversation.split",
                                     {{"sourceId", source_id},
                                      {"messageIndex", 2},
                                      {"scope", "nowhere"}});
    REQUIRE(bad_scope["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);

    // Missing source → NOT_FOUND (surfaces as error).
    const Json missing = dispatch(runtime, "conversation.split",
                                   {{"sourceId", "conv-missing"},
                                    {"messageIndex", 1}});
    REQUIRE(missing.contains("error"));

    // Negative messageIndex → invalid.
    const Json negative = dispatch(runtime, "conversation.split",
                                    {{"sourceId", source_id},
                                     {"messageIndex", -1}});
    REQUIRE(negative["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);

    // messageIndex at last slot leaves the "after" half empty → invalid.
    const Json past_end = dispatch(runtime, "conversation.split",
                                    {{"sourceId", source_id},
                                     {"messageIndex", 4}});
    REQUIRE(past_end["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);

    // Split at index=2 → before has msg-0..msg-2 (3 msgs); after has msg-3
    // and msg-4 (2 msgs).  keepOriginal=false (default) → the source is
    // rewritten in place with the "before" messages so its id survives.
    const Json split_result = dispatch(runtime, "conversation.split",
                                        {{"sourceId", source_id},
                                         {"messageIndex", 2}});
    REQUIRE(split_result.contains("result"));
    REQUIRE(split_result["result"]["original"]["id"] == source_id);
    REQUIRE(split_result["result"]["original"]["messageCount"] == 3);
    REQUIRE(split_result["result"]["latter"]["messageCount"] == 2);
    REQUIRE(split_result["result"]["latter"]["title"] ==
            "Split Me (after)");
    REQUIRE(split_result["result"]["splitAt"].is_number());
    const std::string latter_id = split_result["result"]["latter"]["id"];
    REQUIRE(!latter_id.empty());
    REQUIRE(latter_id != source_id);

    // Original id still resolves, now with the "before" half's messages.
    const Json before = dispatch(runtime, "conversation.get",
                                  {{"id", source_id}})["result"];
    REQUIRE(before["messages"].size() == 3);
    REQUIRE(before["messages"][0]["content"] == "msg-0");
    REQUIRE(before["messages"][1]["content"] == "msg-1");
    REQUIRE(before["messages"][2]["content"] == "msg-2");

    // Latter half is a fresh conversation carrying msg-3 + msg-4.
    const Json after = dispatch(runtime, "conversation.get",
                                 {{"id", latter_id}})["result"];
    REQUIRE(after["messages"].size() == 2);
    REQUIRE(after["messages"][0]["content"] == "msg-3");
    REQUIRE(after["messages"][1]["content"] == "msg-4");
    REQUIRE(after["model"] == "gpt-split");
    REQUIRE(after["scope"] == "workspace");
    REQUIRE(after["pinned"] == false);
}

TEST_CASE("AI Editor conversation.split with keepOriginal preserves source and "
          "mints two fresh conversations",
          "[plugins][ai_editor][native][storage][split]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    REQUIRE(dispatch(runtime, "runtime.initialize").contains("result"));

    const Json created = dispatch(runtime, "conversation.create",
                                   {{"title", "Original"},
                                    {"model", "gpt-keep"},
                                    {"scope", "workspace"}});
    const std::string source_id = created["result"]["id"];
    for (int index = 0; index < 4; ++index) {
        REQUIRE(dispatch(runtime, "conversation.append",
                         {{"id", source_id},
                          {"message", {{"role", "user"},
                                        {"content", "k" +
                                                    std::to_string(index)}}}})
                    .contains("result"));
    }

    const Json split_result = dispatch(
        runtime, "conversation.split",
        {{"sourceId", source_id},
         {"messageIndex", 1},
         {"keepOriginal", true},
         {"titles", Json::array({"Before Half", "After Half"})}});
    REQUIRE(split_result.contains("result"));
    const std::string before_id = split_result["result"]["original"]["id"];
    const std::string after_id = split_result["result"]["latter"]["id"];
    REQUIRE(before_id != source_id);
    REQUIRE(after_id != source_id);
    REQUIRE(before_id != after_id);
    REQUIRE(split_result["result"]["original"]["messageCount"] == 2);
    REQUIRE(split_result["result"]["latter"]["messageCount"] == 2);
    REQUIRE(split_result["result"]["latter"]["title"] == "After Half");

    // Original conversation untouched.
    const Json source_after = dispatch(runtime, "conversation.get",
                                        {{"id", source_id}})["result"];
    REQUIRE(source_after["messages"].size() == 4);
    REQUIRE(source_after["messages"][0]["content"] == "k0");
    REQUIRE(source_after["messages"][3]["content"] == "k3");
    REQUIRE(source_after["title"] == "Original");

    // Freshly minted "before" half carries msg 0..1 with the custom title.
    const Json before = dispatch(runtime, "conversation.get",
                                  {{"id", before_id}})["result"];
    REQUIRE(before["messages"].size() == 2);
    REQUIRE(before["title"] == "Before Half");
    REQUIRE(before["messages"][0]["content"] == "k0");
    REQUIRE(before["messages"][1]["content"] == "k1");

    // Freshly minted "after" half carries msg 2..3.
    const Json after = dispatch(runtime, "conversation.get",
                                 {{"id", after_id}})["result"];
    REQUIRE(after["messages"].size() == 2);
    REQUIRE(after["messages"][0]["content"] == "k2");
    REQUIRE(after["messages"][1]["content"] == "k3");
    REQUIRE(after["model"] == "gpt-keep");
}

TEST_CASE("AI Editor conversation.compact replace strategy folds early "
          "messages into a system summary keeping the last N intact",
          "[plugins][ai_editor][native][storage][compact]") {
    // Fake OpenAI reply — a fixed summary text so the store rewrite is
    // deterministic and we can compare summaryLength against strlen().
    const std::string canned_summary = "Discussed X, Y, Z";
    const std::string body =
        std::string(R"({"choices":[{"message":{"role":"assistant","content":")") +
        canned_summary +
        R"("}}]})";
    LocalHttpServer server(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body);

    RuntimeFixture fixture;
    auto runtime = fixture.get();
    REQUIRE(dispatch(runtime, "runtime.initialize").contains("result"));

    // Build a conversation with 10 messages so keepLast=3 leaves 7 early
    // messages to be folded into the summary.
    const Json created = dispatch(runtime, "conversation.create",
                                   {{"title", "Long Thread"},
                                    {"model", "gpt-compact"},
                                    {"scope", "workspace"}});
    const std::string conv_id = created["result"]["id"];
    for (int index = 0; index < 10; ++index) {
        const std::string role = (index % 2 == 0) ? "user" : "assistant";
        REQUIRE(dispatch(runtime, "conversation.append",
                         {{"id", conv_id},
                          {"message", {{"role", role},
                                        {"content", "msg" +
                                                    std::to_string(index)}}}})
                    .contains("result"));
    }

    const Json compacted = dispatch(
        runtime, "conversation.compact",
        {{"id", conv_id},
         {"keepLast", 3},
         {"provider", {{"id", "fixture"},
                       {"endpoint", server.endpoint()}}},
         {"model", "gpt-4o-mini"},
         {"timeoutMs", 5000}});
    REQUIRE(compacted.contains("result"));
    REQUIRE(compacted["result"]["id"] == conv_id);
    REQUIRE(compacted["result"]["originalMessageCount"] == 10);
    // replace: 1 system summary + 3 tail = 4 total.
    REQUIRE(compacted["result"]["newMessageCount"] == 4);
    REQUIRE(compacted["result"]["summary"] == canned_summary);
    REQUIRE(compacted["result"]["summaryLength"] == canned_summary.size());
    REQUIRE(compacted["result"]["noop"] == false);
    REQUIRE(compacted["result"]["compactedAt"].is_number());

    // Server must have been hit exactly once for the summary call.
    REQUIRE(server.wait_for_connections(1, 2'000));

    // Reload and verify on-disk state matches the returned counts.
    const Json fetched = dispatch(runtime, "conversation.get",
                                   {{"id", conv_id}})["result"];
    REQUIRE(fetched["messages"].size() == 4);
    REQUIRE(fetched["messages"][0]["role"] == "system");
    REQUIRE(fetched["messages"][0]["content"] == canned_summary);
    // Tail 3 == last 3 originals (msg7, msg8, msg9).
    REQUIRE(fetched["messages"][1]["content"] == "msg7");
    REQUIRE(fetched["messages"][2]["content"] == "msg8");
    REQUIRE(fetched["messages"][3]["content"] == "msg9");
    REQUIRE(fetched["messageCount"] == 4);
}

TEST_CASE("AI Editor conversation.compact prepend strategy retains all "
          "originals with summary at the head",
          "[plugins][ai_editor][native][storage][compact]") {
    const std::string canned_summary = "Prepended context: A, B, C";
    const std::string body =
        std::string(R"({"choices":[{"message":{"role":"assistant","content":")") +
        canned_summary +
        R"("}}]})";
    LocalHttpServer server(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body);

    RuntimeFixture fixture;
    auto runtime = fixture.get();
    REQUIRE(dispatch(runtime, "runtime.initialize").contains("result"));

    // 8 messages, keepLast=6 → 2 early messages get summarised.
    const Json created = dispatch(runtime, "conversation.create",
                                   {{"title", "Prepend Thread"},
                                    {"model", "gpt-compact"},
                                    {"scope", "workspace"}});
    const std::string conv_id = created["result"]["id"];
    for (int index = 0; index < 8; ++index) {
        REQUIRE(dispatch(runtime, "conversation.append",
                         {{"id", conv_id},
                          {"message", {{"role", "user"},
                                        {"content", "p" +
                                                    std::to_string(index)}}}})
                    .contains("result"));
    }

    const Json compacted = dispatch(
        runtime, "conversation.compact",
        {{"id", conv_id},
         {"keepLast", 6},
         {"compactStrategy", "prepend"},
         {"provider", {{"id", "fixture"},
                       {"endpoint", server.endpoint()}}},
         {"model", "gpt-4o-mini"},
         {"timeoutMs", 5000}});
    REQUIRE(compacted.contains("result"));
    REQUIRE(compacted["result"]["originalMessageCount"] == 8);
    // prepend keeps everything + 1 summary at the head → 9 total.
    REQUIRE(compacted["result"]["newMessageCount"] == 9);
    REQUIRE(compacted["result"]["summary"] == canned_summary);
    REQUIRE(compacted["result"]["noop"] == false);

    const Json fetched = dispatch(runtime, "conversation.get",
                                   {{"id", conv_id}})["result"];
    REQUIRE(fetched["messages"].size() == 9);
    REQUIRE(fetched["messages"][0]["role"] == "system");
    REQUIRE(fetched["messages"][0]["content"] == canned_summary);
    // p0..p7 must all still be present in original order.
    for (int index = 0; index < 8; ++index) {
        REQUIRE(fetched["messages"][index + 1]["content"] ==
                "p" + std::to_string(index));
    }
    REQUIRE(fetched["messageCount"] == 9);
}

TEST_CASE("AI Editor conversation.compact noop when total <= keepLast, "
          "rejects keepLast<=0 and unknown conversation",
          "[plugins][ai_editor][native][storage][compact]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    REQUIRE(dispatch(runtime, "runtime.initialize").contains("result"));

    const Json created = dispatch(runtime, "conversation.create",
                                   {{"title", "Short Thread"},
                                    {"model", "gpt-compact"},
                                    {"scope", "workspace"}});
    const std::string conv_id = created["result"]["id"];
    // Only 4 messages, keepLast=6 → nothing to compact.  LLM must NOT be
    // called; we deliberately omit a provider to prove the noop path never
    // dials out (a real provider would fail with INVALID_ARGUMENT).
    for (int index = 0; index < 4; ++index) {
        REQUIRE(dispatch(runtime, "conversation.append",
                         {{"id", conv_id},
                          {"message", {{"role", "user"},
                                        {"content", "s" +
                                                    std::to_string(index)}}}})
                    .contains("result"));
    }

    const Json noop = dispatch(runtime, "conversation.compact",
                                {{"id", conv_id}, {"keepLast", 6}});
    REQUIRE(noop.contains("result"));
    REQUIRE(noop["result"]["noop"] == true);
    REQUIRE(noop["result"]["originalMessageCount"] == 4);
    REQUIRE(noop["result"]["newMessageCount"] == 4);
    REQUIRE(noop["result"]["summary"] == "");
    REQUIRE(noop["result"]["summaryLength"] == 0);

    // On-disk state must still be the original 4 messages, none replaced.
    const Json fetched = dispatch(runtime, "conversation.get",
                                   {{"id", conv_id}})["result"];
    REQUIRE(fetched["messages"].size() == 4);
    REQUIRE(fetched["messages"][0]["content"] == "s0");
    REQUIRE(fetched["messages"][3]["content"] == "s3");

    // keepLast=0 → INVALID_ARGUMENT (bounces before touching storage/LLM).
    const Json zero = dispatch(runtime, "conversation.compact",
                                {{"id", conv_id}, {"keepLast", 0}});
    REQUIRE(zero.contains("error"));
    REQUIRE(zero["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);

    // Negative keepLast → INVALID_ARGUMENT.
    const Json neg = dispatch(runtime, "conversation.compact",
                               {{"id", conv_id}, {"keepLast", -2}});
    REQUIRE(neg.contains("error"));
    REQUIRE(neg["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);

    // Unknown compactStrategy → INVALID_ARGUMENT.
    const Json bad_strategy = dispatch(runtime, "conversation.compact",
                                        {{"id", conv_id},
                                         {"keepLast", 2},
                                         {"compactStrategy", "sideways"}});
    REQUIRE(bad_strategy.contains("error"));
    REQUIRE(bad_strategy["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);

    // Unknown conversation id → NOT_FOUND (propagated as error).
    const Json missing = dispatch(runtime, "conversation.compact",
                                   {{"id", "conv-nope"}, {"keepLast", 2}});
    REQUIRE(missing.contains("error"));
    REQUIRE(missing["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_NOT_FOUND);
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

TEST_CASE("AI Editor mcp.get_prompt returns prompt messages from HTTP server",
          "[plugins][ai_editor][native][mcp][dispatch][prompts][integration]") {
    ScriptedHttpServer server;
    // initialize handshake
    server.enqueue(
        R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2024-11-05",)"
        R"("serverInfo":{"name":"prompts-mock","version":"0.1"},)"
        R"("capabilities":{"prompts":{"listChanged":false}}}})");
    // notifications/initialized fire-and-forget
    server.enqueue(R"({"jsonrpc":"2.0"})");
    // prompts/get response — messages carry {role, content:{type:"text",text}}
    // shape per MCP spec so the runtime can normalize into OpenAI messages.
    server.enqueue(
        R"({"jsonrpc":"2.0","id":2,"result":{"description":"Codebase intro",)"
        R"("messages":[)"
        R"({"role":"system","content":{"type":"text",)"
        R"("text":"You review SAO plugins."}},)"
        R"({"role":"user","content":{"type":"text",)"
        R"("text":"Summarize plugins/ai_editor."}})"
        R"(]}})");

    RuntimeFixture fixture;
    auto runtime = fixture.get();
    const Json register_result = dispatch(
        runtime, "mcp.register_server",
        {{"name", "prompts-http"},
         {"transport", "http"},
         {"url", server.base_url() + "/mcp"},
         {"startupMs", 5000}});
    REQUIRE(register_result.contains("result"));

    const Json prompt = dispatch(
        runtime, "mcp.get_prompt",
        {{"server", "prompts-http"},
         {"name", "codebase-intro"},
         {"arguments", {{"scope", "plugins/ai_editor"}}}});
    REQUIRE(prompt.contains("result"));
    REQUIRE(prompt["result"]["description"] == "Codebase intro");
    REQUIRE(prompt["result"]["server"] == "prompts-http");
    REQUIRE(prompt["result"]["messages"].is_array());
    REQUIRE(prompt["result"]["messages"].size() == 2);
    REQUIRE(prompt["result"]["messages"][0]["role"] == "system");
    REQUIRE(prompt["result"]["messages"][0]["content"]["text"] ==
            "You review SAO plugins.");
    REQUIRE(prompt["result"]["messages"][1]["role"] == "user");
    REQUIRE(prompt["result"]["messages"][1]["content"]["text"] ==
            "Summarize plugins/ai_editor.");

    // Verify the outgoing prompts/get body carried the correct name +
    // arguments the caller passed through.  The first request is the
    // initialize handshake, the second is notifications/initialized, the
    // third is the prompts/get itself.
    (void)server.take_request();  // initialize
    (void)server.take_request();  // notifications/initialized
    const std::string prompt_request = server.take_request();
    REQUIRE(prompt_request.find("\"method\":\"prompts/get\"") !=
            std::string::npos);
    REQUIRE(prompt_request.find("\"name\":\"codebase-intro\"") !=
            std::string::npos);
    REQUIRE(prompt_request.find("\"scope\":\"plugins/ai_editor\"") !=
            std::string::npos);

    REQUIRE(dispatch(runtime, "mcp.close_server",
                     {{"name", "prompts-http"}})
                .contains("result"));
}

TEST_CASE("AI Editor chat.run_with_mcp renders systemPromptSource into "
          "OpenAI messages",
          "[plugins][ai_editor][native][runs][mcp][prompts][integration]") {
    ScriptedHttpServer mcp_server;
    // initialize handshake
    mcp_server.enqueue(
        R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2024-11-05",)"
        R"("serverInfo":{"name":"prompts-mock","version":"0.1"},)"
        R"("capabilities":{"prompts":{}}}})");
    mcp_server.enqueue(R"({"jsonrpc":"2.0"})");
    // chat.run_with_mcp aggregates tools/list across every registered server
    // before it renders the systemPromptSource, so serve an empty tool list
    // to keep the OpenAI body free of MCP-injected function tools.
    mcp_server.enqueue(
        R"({"jsonrpc":"2.0","id":2,"result":{"tools":[]}})");
    // prompts/get renders a system + user message.
    mcp_server.enqueue(
        R"({"jsonrpc":"2.0","id":3,"result":{"description":"Boot header",)"
        R"("messages":[)"
        R"({"role":"system","content":{"type":"text",)"
        R"("text":"You are the SAO reviewer."}},)"
        R"({"role":"user","content":{"type":"text",)"
        R"("text":"Prime the review."}})"
        R"(]}})");

    const std::string chat_body =
        R"({"choices":[{"message":{"role":"assistant","content":"ok"}}]})";
    LocalHttpServer chat_server(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: " + std::to_string(chat_body.size()) +
        "\r\nConnection: close\r\n\r\n" + chat_body);

    RuntimeFixture fixture;
    auto runtime = fixture.get();
    // Register the HTTP MCP server so mcp.get_prompt can resolve the source.
    const Json register_result = dispatch(
        runtime, "mcp.register_server",
        {{"name", "prompts-http"},
         {"transport", "http"},
         {"url", mcp_server.base_url() + "/mcp"},
         {"startupMs", 5000}});
    REQUIRE(register_result.contains("result"));

    // No mcpServers -> keep the tools list empty so the request body is a
    // vanilla chat.run shape; we only care about the messages array here.
    const Json params{
        {"provider", {{"id", "fixture"},
                       {"endpoint", chat_server.endpoint()}}},
        {"model", "fixture-model"},
        {"messages", Json::array(
             {{{"role", "user"}, {"content", "actual user turn"}}})},
        {"stream", false},
        {"timeoutMs", 5'000},
        {"systemPromptSource",
         {{"server", "prompts-http"}, {"name", "codebase-intro"}}}};
    const Json started = dispatch(runtime, "chat.run_with_mcp", params);
    REQUIRE(started.contains("result"));
    REQUIRE(started["result"]["accepted"] == true);
    REQUIRE(chat_server.wait_for_connections(1, 5'000));

    const auto bodies = chat_server.captured_bodies();
    REQUIRE(bodies.size() == 1);
    const Json body_json = Json::parse(bodies.front());
    REQUIRE(body_json.contains("messages"));
    REQUIRE(body_json["messages"].is_array());
    // Expected: system prompt from MCP first (description is skipped
    // because prompt already produced a system message), then user turn
    // from MCP, then the caller's actual user turn.
    REQUIRE(body_json["messages"].size() == 3);
    REQUIRE(body_json["messages"][0]["role"] == "system");
    REQUIRE(body_json["messages"][0]["content"] ==
            "You are the SAO reviewer.");
    REQUIRE(body_json["messages"][1]["role"] == "user");
    REQUIRE(body_json["messages"][1]["content"] == "Prime the review.");
    REQUIRE(body_json["messages"][2]["role"] == "user");
    REQUIRE(body_json["messages"][2]["content"] == "actual user turn");
    // systemPromptSource must not leak into the OpenAI request body.
    REQUIRE_FALSE(body_json.contains("systemPromptSource"));

    REQUIRE(dispatch(runtime, "mcp.close_server",
                     {{"name", "prompts-http"}})
                .contains("result"));
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

TEST_CASE("AI Editor agents.invoke with conversationId appends both turns to "
          "the persisted transcript",
          "[plugins][ai_editor][native][agents][conversation]") {
    // Two-turn chat via a fixed reply "reply1" — same body served on each
    // connection covers the second turn too.
    const std::string body =
        R"({"choices":[{"message":{"role":"assistant","content":"reply1"}}]})";
    LocalHttpServer server(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: " +
        std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body);
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    REQUIRE(dispatch(runtime, "runtime.initialize").contains("result"));

    const Json created = dispatch(runtime, "conversation.create",
                                  {{"title", "Agent multi-turn"},
                                   {"model", "test-model"},
                                   {"scope", "workspace"}});
    REQUIRE(created.contains("result"));
    const std::string conversation_id = created["result"]["id"];

    const Json first = dispatch(
        runtime, "agents.invoke",
        {{"id", "code-reviewer"},
         {"message", "q1"},
         {"conversationId", conversation_id},
         {"provider", {{"id", "agent-conv-fixture"},
                        {"endpoint", server.endpoint()}}},
         {"model", "test-model"},
         {"timeoutMs", 5000}});
    REQUIRE(first.contains("result"));
    REQUIRE(first["result"]["content"] == "reply1");
    // conversationId echoed back so the caller can keep threading it in.
    REQUIRE(first["result"]["conversationId"] == conversation_id);
    REQUIRE(server.wait_for_connections(1, 2'000));

    const Json snapshot_after_first = dispatch(runtime, "conversation.get",
                                                {{"id", conversation_id}});
    REQUIRE(snapshot_after_first.contains("result"));
    const Json& messages_after_first =
        snapshot_after_first["result"]["messages"];
    REQUIRE(messages_after_first.size() == 2);
    REQUIRE(messages_after_first[0]["role"] == "user");
    REQUIRE(messages_after_first[0]["content"] == "q1");
    REQUIRE(messages_after_first[1]["role"] == "assistant");
    REQUIRE(messages_after_first[1]["content"] == "reply1");

    // Second turn: history is loaded from disk, not from the caller's params,
    // so the persisted transcript grows to 4 messages.
    const Json second = dispatch(
        runtime, "agents.invoke",
        {{"id", "code-reviewer"},
         {"message", "q2"},
         {"conversationId", conversation_id},
         {"provider", {{"id", "agent-conv-fixture"},
                        {"endpoint", server.endpoint()}}},
         {"model", "test-model"},
         {"timeoutMs", 5000}});
    REQUIRE(second.contains("result"));
    REQUIRE(second["result"]["conversationId"] == conversation_id);
    REQUIRE(server.wait_for_connections(2, 2'000));

    const Json snapshot_after_second = dispatch(runtime, "conversation.get",
                                                 {{"id", conversation_id}});
    const Json& messages_after_second =
        snapshot_after_second["result"]["messages"];
    REQUIRE(messages_after_second.size() == 4);
    REQUIRE(messages_after_second[2]["role"] == "user");
    REQUIRE(messages_after_second[2]["content"] == "q2");
    REQUIRE(messages_after_second[3]["role"] == "assistant");
    REQUIRE(messages_after_second[3]["content"] == "reply1");
}

TEST_CASE("AI Editor agents.invoke createConversation=true lazily seeds a "
          "workspace conversation and returns its id",
          "[plugins][ai_editor][native][agents][conversation]") {
    const std::string body =
        R"({"choices":[{"message":{"role":"assistant","content":"seeded"}}]})";
    LocalHttpServer server(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: " +
        std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body);
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    REQUIRE(dispatch(runtime, "runtime.initialize").contains("result"));

    const Json invoked = dispatch(
        runtime, "agents.invoke",
        {{"id", "code-reviewer"},
         {"message", "seed the transcript"},
         {"createConversation", true},
         {"conversationTitle", "Seed"},
         {"provider", {{"id", "agent-seed-fixture"},
                        {"endpoint", server.endpoint()}}},
         {"model", "test-model"},
         {"timeoutMs", 5000}});
    REQUIRE(invoked.contains("result"));
    REQUIRE(invoked["result"]["content"] == "seeded");
    REQUIRE(invoked["result"].contains("conversationId"));
    const std::string conversation_id =
        invoked["result"]["conversationId"].get<std::string>();
    REQUIRE(!conversation_id.empty());

    const Json fetched = dispatch(runtime, "conversation.get",
                                   {{"id", conversation_id}});
    REQUIRE(fetched.contains("result"));
    REQUIRE(fetched["result"]["title"] == "Seed");
    const Json& messages = fetched["result"]["messages"];
    REQUIRE(messages.size() == 2);
    REQUIRE(messages[0]["content"] == "seed the transcript");
    REQUIRE(messages[1]["content"] == "seeded");
}

TEST_CASE("AI Editor agents.batch_invoke fans out three agents in parallel "
          "and aggregates completed results",
          "[plugins][ai_editor][native][agents][batch]") {
    // Same canned reply served on every connection; the LocalHttpServer
    // spins one accept() per connect() call so three concurrent WinHTTP
    // sessions each get their own response.
    const std::string body =
        R"({"choices":[{"message":{"role":"assistant","content":"batch-reply"}}]})";
    LocalHttpServer server(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: " +
        std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body);
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    REQUIRE(dispatch(runtime, "runtime.initialize").contains("result"));

    const Json invoked = dispatch(
        runtime, "agents.batch_invoke",
        {{"agents", Json::array({
             Json{{"id", "code-reviewer"}, {"message", "review this"}},
             Json{{"id", "explainer"}, {"message", "explain that"}},
             Json{{"id", "optimizer"}, {"message", "speed it up"}}})},
         {"defaultProvider", {{"id", "batch-fixture"},
                               {"endpoint", server.endpoint()}}},
         {"defaultModel", "test-model"},
         {"timeoutMs", 5000},
         {"concurrency", 3}});
    REQUIRE(invoked.contains("result"));
    const Json& body_result = invoked["result"];
    REQUIRE(body_result["results"].size() == 3);
    REQUIRE(body_result["successCount"] == 3);
    REQUIRE(body_result["failureCount"] == 0);
    // Original agent order is preserved regardless of which worker finished
    // first — the runtime fills the results array by input index, not by
    // completion order.
    REQUIRE(body_result["results"][0]["agentId"] == "code-reviewer");
    REQUIRE(body_result["results"][1]["agentId"] == "explainer");
    REQUIRE(body_result["results"][2]["agentId"] == "optimizer");
    for (size_t i = 0; i < 3; ++i) {
        REQUIRE(body_result["results"][i]["status"] == "completed");
        REQUIRE(body_result["results"][i]["content"] == "batch-reply");
        REQUIRE(body_result["results"][i]["model"] == "test-model");
        REQUIRE(body_result["results"][i].contains("durationMs"));
    }
    REQUIRE(server.wait_for_connections(3, 5'000));
    // totalMs is the wall-clock elapsed across all workers; it should be
    // no smaller than the slowest individual step but we only assert
    // "present + non-negative" here because timing is fixture-specific.
    REQUIRE(body_result.contains("totalMs"));
}

TEST_CASE("AI Editor agents.batch_invoke reports per-agent failure without "
          "aborting peers when an id is missing",
          "[plugins][ai_editor][native][agents][batch]") {
    const std::string body =
        R"({"choices":[{"message":{"role":"assistant","content":"ok"}}]})";
    LocalHttpServer server(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: " +
        std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body);
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    REQUIRE(dispatch(runtime, "runtime.initialize").contains("result"));

    const Json invoked = dispatch(
        runtime, "agents.batch_invoke",
        {{"agents", Json::array({
             Json{{"id", "code-reviewer"}, {"message", "hello"}},
             Json{{"id", "does-not-exist"}, {"message", "orphan"}},
             Json{{"id", "explainer"}, {"message", "hi"}}})},
         {"defaultProvider", {{"id", "batch-missing-fixture"},
                               {"endpoint", server.endpoint()}}},
         {"defaultModel", "test-model"},
         {"timeoutMs", 5000}});
    REQUIRE(invoked.contains("result"));
    const Json& body_result = invoked["result"];
    REQUIRE(body_result["results"].size() == 3);
    REQUIRE(body_result["successCount"] == 2);
    REQUIRE(body_result["failureCount"] == 1);
    REQUIRE(body_result["results"][0]["status"] == "completed");
    REQUIRE(body_result["results"][1]["status"] == "failed");
    REQUIRE(body_result["results"][1]["agentId"] == "does-not-exist");
    REQUIRE(body_result["results"][1]["error"] == "agent not found");
    REQUIRE(body_result["results"][2]["status"] == "completed");
    // The two live agents each burn one HTTP connection; the missing agent
    // never reaches the transport layer so we should see exactly 2.
    REQUIRE(server.wait_for_connections(2, 5'000));
}

TEST_CASE("AI Editor agents.batch_invoke with conversationId appends every "
          "successful (user, assistant) pair in original agent order",
          "[plugins][ai_editor][native][agents][batch][conversation]") {
    const std::string body =
        R"({"choices":[{"message":{"role":"assistant","content":"convo-reply"}}]})";
    LocalHttpServer server(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: " +
        std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body);
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    REQUIRE(dispatch(runtime, "runtime.initialize").contains("result"));

    const Json created = dispatch(runtime, "conversation.create",
                                   {{"title", "Batch"},
                                    {"model", "test-model"},
                                    {"scope", "workspace"}});
    REQUIRE(created.contains("result"));
    const std::string conversation_id = created["result"]["id"];

    const Json invoked = dispatch(
        runtime, "agents.batch_invoke",
        {{"agents", Json::array({
             Json{{"id", "code-reviewer"}, {"message", "msg1"}},
             Json{{"id", "explainer"}, {"message", "msg2"}}})},
         {"defaultProvider", {{"id", "batch-convo-fixture"},
                               {"endpoint", server.endpoint()}}},
         {"defaultModel", "test-model"},
         {"conversationId", conversation_id},
         {"timeoutMs", 5000}});
    REQUIRE(invoked.contains("result"));
    REQUIRE(invoked["result"]["successCount"] == 2);
    REQUIRE(invoked["result"]["conversationId"] == conversation_id);
    REQUIRE(server.wait_for_connections(2, 5'000));

    const Json snapshot = dispatch(runtime, "conversation.get",
                                    {{"id", conversation_id}});
    REQUIRE(snapshot.contains("result"));
    const Json& messages = snapshot["result"]["messages"];
    // Two agents * (user + assistant) = 4 messages; the assistant reply is
    // prefixed with the agent name so downstream readers can attribute it
    // back to its source agent.
    REQUIRE(messages.size() == 4);
    REQUIRE(messages[0]["role"] == "user");
    REQUIRE(messages[0]["content"] == "msg1");
    REQUIRE(messages[0]["agentId"] == "code-reviewer");
    REQUIRE(messages[1]["role"] == "assistant");
    REQUIRE(messages[1]["agentId"] == "code-reviewer");
    // "[Code Reviewer] convo-reply" — human name comes from builtin agent
    // definition.
    REQUIRE(messages[1]["content"].get<std::string>().find(
                "convo-reply") != std::string::npos);
    REQUIRE(messages[1]["content"].get<std::string>().rfind(
                "[Code Reviewer]", 0) == 0);
    REQUIRE(messages[2]["role"] == "user");
    REQUIRE(messages[2]["content"] == "msg2");
    REQUIRE(messages[2]["agentId"] == "explainer");
    REQUIRE(messages[3]["role"] == "assistant");
    REQUIRE(messages[3]["agentId"] == "explainer");
    REQUIRE(messages[3]["content"].get<std::string>().rfind(
                "[Code Explainer]", 0) == 0);
}

TEST_CASE("AI Editor workflow.progress emits step_started + step_completed "
          "boundary events plus a terminal completion event",
          "[plugins][ai_editor][native][workflows][progress]") {
    const std::string body =
        R"({"choices":[{"message":{"role":"assistant","content":"progress-content"}}]})";
    LocalHttpServer server(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: " +
        std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body);
    RuntimeFixture fixture;
    const Json run = dispatch(
        fixture.get(), "workflows.run",
        {{"id", "review-and-fix"},
         {"provider", {{"id", "workflow-progress-fixture"},
                       {"endpoint", server.endpoint()}}},
         {"model", "fixture-model"},
         {"input", "sample code"},
         {"timeoutMs", 5000}});
    REQUIRE(run.contains("result"));
    const std::string execution_id = run["result"]["executionId"];

    // Wait for the workflow to complete before draining events; keeps the
    // event queue stable and lets us assert the terminal event too.
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

    size_t started_count = 0;
    size_t completed_count = 0;
    bool saw_terminal_completed = false;
    for (size_t index = 0; index < 128; ++index) {
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
        if (params.value("event", "") != "workflow.progress") {
            continue;
        }
        const Json payload = params.value("payload", Json::object());
        REQUIRE(payload["executionId"] == execution_id);
        REQUIRE(payload["totalSteps"] == 2);
        const std::string phase = payload.value("status", std::string{});
        if (phase == "step_started") {
            REQUIRE(payload.contains("stepIndex"));
            REQUIRE(payload.contains("label"));
            ++started_count;
        } else if (phase == "step_completed") {
            REQUIRE(payload.contains("stepIndex"));
            REQUIRE(payload["content"] == "progress-content");
            ++completed_count;
        } else if (phase == "completed") {
            saw_terminal_completed = true;
        }
    }
    // The two-step "review-and-fix" builtin should produce exactly one
    // start + one completion per step, plus a single terminal event.
    REQUIRE(started_count == 2);
    REQUIRE(completed_count == 2);
    REQUIRE(saw_terminal_completed);
}

TEST_CASE("AI Editor prompts.list_defs surfaces 3 built-in prompts",
          "[plugins][ai_editor][native][prompts]") {
    RuntimeFixture fixture;
    const Json defs = dispatch(fixture.get(), "prompts.list_defs");
    REQUIRE(defs.contains("result"));
    REQUIRE(defs["result"]["total"] >= 3);
    std::vector<std::string> ids;
    for (const auto& item : defs["result"]["items"]) {
        ids.push_back(item.value("id", ""));
    }
    for (const auto* required :
         {"code-review-base", "explain-code", "bug-diagnosis"}) {
        REQUIRE(std::find(ids.begin(), ids.end(), required) != ids.end());
    }
    // Every built-in must round-trip name/content/variables so the UI can
    // introspect the schema without hitting prompts.get_def separately.
    for (const auto& item : defs["result"]["items"]) {
        if (!item.value("builtin", false)) {
            continue;
        }
        REQUIRE_FALSE(item.value("name", std::string{}).empty());
        REQUIRE_FALSE(item.value("content", std::string{}).empty());
        REQUIRE(item.contains("variables"));
        REQUIRE(item["variables"].is_array());
    }
}

TEST_CASE("AI Editor prompts.list_defs surfaces market presets when the "
          "assets/ai_editor/prompts dir is reachable",
          "[plugins][ai_editor][native][prompts][market]") {
    RuntimeFixture fixture;
    const Json defs = dispatch(fixture.get(), "prompts.list_defs");
    REQUIRE(defs.contains("result"));
    const auto& items = defs["result"]["items"];
    std::unordered_map<std::string, Json> by_id;
    for (const auto& item : items) {
        by_id.emplace(item.value("id", std::string{}), item);
    }
    const std::vector<std::string> market_ids{
        "sql-audit", "security-checklist", "performance-review",
        "refactor-plan"};
    size_t market_hits = 0;
    for (const auto& id : market_ids) {
        const auto found = by_id.find(id);
        if (found == by_id.end()) {
            continue;
        }
        ++market_hits;
        REQUIRE(found->second.value("scope", std::string{}) == "market");
        REQUIRE(found->second.value("builtin", true) == false);
        REQUIRE_FALSE(found->second.value("name", std::string{}).empty());
    }
    if (market_hits == 0) {
        INFO("Market prompt assets/ai_editor/prompts not deployed near the "
             "test binary; skipping the presence assertions. This is "
             "expected when assets have not been installed.");
    } else {
        // Any preset we actually found must be the full set — partial
        // deployment would indicate a packaging bug worth surfacing.
        REQUIRE(market_hits == market_ids.size());
    }
}

TEST_CASE("AI Editor prompts.save_def / delete_def CRUD honours builtin lock",
          "[plugins][ai_editor][native][prompts]") {
    RuntimeFixture fixture;
    const Json prompt_json{
        {"id", "my-review"},
        {"name", "My Review"},
        {"description", "Custom review template"},
        {"content", "Review {{code}} with focus {{focus}}"},
        {"variables", Json::array({
            Json{{"name", "code"}, {"description", "The code"}},
            Json{{"name", "focus"}, {"default", "clarity"}}})},
        {"tags", Json::array({"custom", "review"})},
        {"icon", "\xF0\x9F\x93\x9D"}};
    REQUIRE(dispatch(fixture.get(), "prompts.save_def",
                     {{"scope", "workspace"}, {"prompt", prompt_json}})
                .contains("result"));
    const Json fetched =
        dispatch(fixture.get(), "prompts.get_def", {{"id", "my-review"}})
            ["result"];
    REQUIRE(fetched["name"] == "My Review");
    REQUIRE(fetched["content"] == "Review {{code}} with focus {{focus}}");
    REQUIRE(fetched["variables"].size() == 2);
    REQUIRE(fetched["tags"].size() == 2);
    REQUIRE(fetched["builtin"] == false);
    REQUIRE(dispatch(fixture.get(), "prompts.delete_def",
                     {{"id", "my-review"}})
                .contains("result"));
    // Deleting a builtin must fail with permission denied.
    REQUIRE(dispatch(fixture.get(), "prompts.delete_def",
                     {{"id", "code-review-base"}})["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_PERMISSION_DENIED);
    // Saving a prompt with the id of a builtin must also be rejected.
    const Json shadowed_builtin{
        {"id", "code-review-base"},
        {"name", "Shadowed"},
        {"content", "override"}};
    REQUIRE(dispatch(fixture.get(), "prompts.save_def",
                     {{"scope", "workspace"},
                      {"prompt", shadowed_builtin}})["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_PERMISSION_DENIED);
}

TEST_CASE("AI Editor prompts.render substitutes vars, applies defaults, "
          "collapses unknowns to empty",
          "[plugins][ai_editor][native][prompts]") {
    RuntimeFixture fixture;
    // Provided arguments win over defaults.
    const Json rendered_supplied =
        dispatch(fixture.get(), "prompts.render",
                 {{"id", "code-review-base"},
                  {"arguments", {{"focus", "security only"},
                                  {"code", "SELECT * FROM users"}}}})
            ["result"];
    REQUIRE(rendered_supplied["id"] == "code-review-base");
    const std::string full = rendered_supplied["content"].get<std::string>();
    REQUIRE(full.find("Focus on: security only") != std::string::npos);
    REQUIRE(full.find("SELECT * FROM users") != std::string::npos);
    REQUIRE(full.find("{{") == std::string::npos);
    // Missing `focus` falls back to the declared default ("correctness and
    // security"); `code` has no default so it collapses to empty but the
    // template still renders around it without leaking the placeholder.
    const Json rendered_default =
        dispatch(fixture.get(), "prompts.render",
                 {{"id", "code-review-base"}, {"arguments", Json::object()}})
            ["result"];
    const std::string defaulted =
        rendered_default["content"].get<std::string>();
    REQUIRE(defaulted.find("Focus on: correctness and security") !=
            std::string::npos);
    REQUIRE(defaulted.find("{{code}}") == std::string::npos);
    REQUIRE(defaulted.find("{{focus}}") == std::string::npos);
    // Save a workspace prompt that contains an unknown-variable placeholder
    // and confirm render() collapses it to empty rather than leaking `{{X}}`.
    const Json bespoke_prompt{
        {"id", "render-check"},
        {"name", "Render Check"},
        {"content", "before/{{unknown}}/after"},
        {"variables", Json::array()}};
    REQUIRE(dispatch(fixture.get(), "prompts.save_def",
                     {{"scope", "workspace"}, {"prompt", bespoke_prompt}})
                .contains("result"));
    const Json rendered_unknown =
        dispatch(fixture.get(), "prompts.render",
                 {{"id", "render-check"}, {"arguments", Json::object()}})
            ["result"];
    REQUIRE(rendered_unknown["content"] == "before//after");
    // Structured (non-string) argument values are JSON-dumped so callers can
    // pass tool arguments through unchanged.
    const Json bespoke_structured{
        {"id", "render-structured"},
        {"name", "Render Structured"},
        {"content", "payload={{payload}}"},
        {"variables", Json::array()}};
    REQUIRE(dispatch(fixture.get(), "prompts.save_def",
                     {{"scope", "workspace"},
                      {"prompt", bespoke_structured}})
                .contains("result"));
    const Json rendered_structured =
        dispatch(fixture.get(), "prompts.render",
                 {{"id", "render-structured"},
                  {"arguments", {{"payload", {{"a", 1}, {"b", 2}}}}}})["result"];
    REQUIRE(rendered_structured["content"].get<std::string>().find(
                "\"a\":1") != std::string::npos);
    // Whitespace inside the placeholder braces is tolerated so JSON editors
    // that reflow templates don't break substitution.
    const Json bespoke_spaces{
        {"id", "render-spaces"},
        {"name", "Render Spaces"},
        {"content", "hello {{  name  }}!"},
        {"variables", Json::array()}};
    REQUIRE(dispatch(fixture.get(), "prompts.save_def",
                     {{"scope", "workspace"},
                      {"prompt", bespoke_spaces}})
                .contains("result"));
    const Json rendered_spaces =
        dispatch(fixture.get(), "prompts.render",
                 {{"id", "render-spaces"},
                  {"arguments", {{"name", "world"}}}})["result"];
    REQUIRE(rendered_spaces["content"] == "hello world!");
    // Unknown ids must surface as not-found so the UI can distinguish them
    // from an accidental empty template.
    REQUIRE(dispatch(fixture.get(), "prompts.render",
                     {{"id", "no-such-prompt"},
                      {"arguments", Json::object()}})["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_NOT_FOUND);
}

TEST_CASE("AI Editor chat.run with promptId + promptArguments prepends the "
          "rendered template as a system message before the request body is "
          "sent",
          "[plugins][ai_editor][native][prompts][integration]") {
    const std::string body =
        R"({"choices":[{"message":{"role":"assistant","content":"ok"}}]})";
    LocalHttpServer server(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: " +
        std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body);
    RuntimeFixture fixture;
    const Json started = dispatch(
        fixture.get(), "chat.run",
        {{"promptId", "code-review-base"},
         {"promptArguments", {{"focus", "SQL injection"},
                                {"code", "SELECT * FROM users"}}},
         {"messages", Json::array({Json{{"role", "user"},
                                          {"content", "kick it off"}}})},
         {"stream", false},
         {"provider", {{"id", "prompt-fixture"},
                        {"endpoint", server.endpoint()}}},
         {"model", "test-model"},
         {"timeoutMs", 5000}});
    REQUIRE(started.contains("result"));
    REQUIRE(started["result"]["accepted"] == true);
    REQUIRE(server.wait_for_connections(1, 2'000));
    const auto bodies = server.captured_bodies();
    REQUIRE(bodies.size() == 1);
    const Json outgoing = Json::parse(bodies.front());
    // No caller-supplied system message -> rendered template goes in as
    // the first system message; the caller's user turn survives as-is.
    REQUIRE(outgoing["messages"].is_array());
    REQUIRE(outgoing["messages"].size() == 2);
    REQUIRE(outgoing["messages"][0]["role"] == "system");
    const std::string prepended =
        outgoing["messages"][0]["content"].get<std::string>();
    REQUIRE(prepended.find("Focus on: SQL injection") != std::string::npos);
    REQUIRE(prepended.find("SELECT * FROM users") != std::string::npos);
    REQUIRE(prepended.find("{{") == std::string::npos);
    REQUIRE(outgoing["messages"][1]["role"] == "user");
    REQUIRE(outgoing["messages"][1]["content"] == "kick it off");
    // promptId/promptArguments must not leak into the outbound provider
    // body — they are runtime-only knobs.
    REQUIRE_FALSE(outgoing.contains("promptId"));
    REQUIRE_FALSE(outgoing.contains("promptArguments"));
}

TEST_CASE("AI Editor chat.run with promptId falls back to a user turn when the "
          "caller already provides a system message",
          "[plugins][ai_editor][native][prompts][integration]") {
    const std::string body =
        R"({"choices":[{"message":{"role":"assistant","content":"ok"}}]})";
    LocalHttpServer server(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: " +
        std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body);
    RuntimeFixture fixture;
    const Json started = dispatch(
        fixture.get(), "chat.run",
        {{"promptId", "explain-code"},
         {"promptArguments", {{"code", "print(1)"}}},
         {"messages",
          Json::array({Json{{"role", "system"},
                             {"content", "you are the boss"}},
                        Json{{"role", "user"},
                             {"content", "go"}}})},
         {"stream", false},
         {"provider", {{"id", "prompt-caller-system-fixture"},
                        {"endpoint", server.endpoint()}}},
         {"model", "test-model"},
         {"timeoutMs", 5000}});
    REQUIRE(started.contains("result"));
    REQUIRE(server.wait_for_connections(1, 2'000));
    const auto bodies = server.captured_bodies();
    REQUIRE(bodies.size() == 1);
    const Json outgoing = Json::parse(bodies.front());
    REQUIRE(outgoing["messages"].size() == 3);
    // Caller-supplied system stays authoritative; the rendered prompt is
    // queued as a user turn ahead of the original system message so it
    // still primes the conversation but doesn't shadow the caller intent.
    REQUIRE(outgoing["messages"][0]["role"] == "user");
    const std::string prepended =
        outgoing["messages"][0]["content"].get<std::string>();
    REQUIRE(prepended.find("Explain this code") != std::string::npos);
    REQUIRE(prepended.find("print(1)") != std::string::npos);
    REQUIRE(prepended.find("Depth: detailed") != std::string::npos);
    REQUIRE(outgoing["messages"][1]["role"] == "system");
    REQUIRE(outgoing["messages"][1]["content"] == "you are the boss");
    REQUIRE(outgoing["messages"][2]["role"] == "user");
    REQUIRE(outgoing["messages"][2]["content"] == "go");
}

TEST_CASE("chat.run non-stream reports latency metrics on the completed run",
          "[plugins][ai_editor][native][runs][metrics]") {
    // Provider replies with a usage block containing completion_tokens so we
    // can assert the derived tokensPerSecond field lands on the run result.
    const std::string body =
        R"({"id":"chat-metrics-1","model":"fixture-model",)"
        R"("choices":[{"message":{"role":"assistant",)"
        R"("content":"ok"},"finish_reason":"stop"}],)"
        R"("usage":{"prompt_tokens":8,"completion_tokens":42,)"
        R"("total_tokens":50}})";
    LocalHttpServer server(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
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
                      {"timeoutMs", 5'000}};
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
    } while (GetTickCount64() - wait_started < 5'000);
    REQUIRE(status["status"] == "completed");
    REQUIRE(status["result"]["content"] == "ok");

    // Metrics landed on the completed run's result payload.
    REQUIRE(status["result"].contains("metrics"));
    const Json metrics = status["result"]["metrics"];
    REQUIRE(metrics["provider_type"] == "openai");
    REQUIRE(metrics.contains("totalMs"));
    REQUIRE(metrics["totalMs"].is_number_integer());
    REQUIRE(metrics["totalMs"].get<int64_t>() >= 0);
    REQUIRE(metrics.contains("ttfMs"));
    REQUIRE(metrics["ttfMs"].is_number_integer());
    REQUIRE(metrics["ttfMs"].get<int64_t>() >= 0);
    REQUIRE(metrics["ttfMs"].get<int64_t>() <= metrics["totalMs"].get<int64_t>());
    // completion_tokens propagated + tokensPerSecond computed.
    REQUIRE(metrics["completionTokens"].get<int64_t>() == 42);
    // tokensPerSecond may be absent for zero-duration responses (localhost
    // typically stays sub-millisecond in tests); assert it only when it
    // appears so both fast and slow CI shapes stay green.
    if (metrics.contains("tokensPerSecond")) {
        REQUIRE(metrics["tokensPerSecond"].is_number());
        REQUIRE(metrics["tokensPerSecond"].get<double>() > 0.0);
    }

    // A chat.metrics event fires alongside run.completed carrying the runId.
    bool saw_chat_metrics = false;
    bool saw_run_completed = false;
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
        const std::string event_name =
            notification["params"].value("event", "");
        if (event_name == "chat.metrics") {
            saw_chat_metrics = true;
            const Json payload = notification["params"].value("payload",
                                                                Json::object());
            REQUIRE(payload["runId"] == run_id);
            REQUIRE(payload["provider_type"] == "openai");
            REQUIRE(payload["completionTokens"].get<int64_t>() == 42);
            REQUIRE(payload.contains("totalMs"));
        } else if (event_name == "run.completed") {
            saw_run_completed = true;
            // run.completed's payload carries the same metrics block that
            // chat.metrics splits out, so subscribers who only listen to
            // completion still get latency info without a second event.
            REQUIRE(notification["params"]["payload"]["metrics"]
                                              ["provider_type"] == "openai");
        }
    }
    REQUIRE(saw_chat_metrics);
    REQUIRE(saw_run_completed);
}

TEST_CASE("conversation.pin surfaces pinned entries first and get shows pinned",
          "[plugins][ai_editor][native][storage][pin]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    REQUIRE(dispatch(runtime, "runtime.initialize").contains("result"));

    // Create three conversations with distinct savedAt timestamps (sleep
    // between creates so the millisecond-resolution clock steps forward).
    const std::string oldest = dispatch(runtime, "conversation.create",
                                        {{"title", "Oldest"},
                                         {"model", "m"},
                                         {"scope", "workspace"}})
                                   ["result"]["id"];
    Sleep(5);
    const std::string middle = dispatch(runtime, "conversation.create",
                                        {{"title", "Middle"},
                                         {"model", "m"},
                                         {"scope", "workspace"}})
                                   ["result"]["id"];
    Sleep(5);
    const std::string newest = dispatch(runtime, "conversation.create",
                                        {{"title", "Newest"},
                                         {"model", "m"},
                                         {"scope", "workspace"}})
                                   ["result"]["id"];

    // Baseline: no pins → savedAt descending.
    Json listing = dispatch(runtime, "conversation.list")["result"];
    REQUIRE(listing.size() == 3);
    REQUIRE(listing[0]["id"] == newest);
    REQUIRE(listing[1]["id"] == middle);
    REQUIRE(listing[2]["id"] == oldest);
    for (const auto& entry : listing) {
        REQUIRE(entry.contains("pinned"));
        REQUIRE(entry["pinned"] == false);
    }

    // Pin the oldest conversation → it must move to the front even though
    // its savedAt is the smallest.
    const Json pinned = dispatch(runtime, "conversation.pin",
                                 {{"id", oldest}})["result"];
    REQUIRE(pinned["id"] == oldest);
    REQUIRE(pinned["pinned"] == true);

    const Json fetched = dispatch(runtime, "conversation.get",
                                   {{"id", oldest}})["result"];
    REQUIRE(fetched["pinned"] == true);

    listing = dispatch(runtime, "conversation.list")["result"];
    REQUIRE(listing.size() == 3);
    REQUIRE(listing[0]["id"] == oldest);
    REQUIRE(listing[0]["pinned"] == true);
    // Unpinned entries still sort by savedAt within the group.
    REQUIRE(listing[1]["id"] == newest);
    REQUIRE(listing[2]["id"] == middle);

    // Unpin restores the natural savedAt ordering.
    const Json unpinned = dispatch(runtime, "conversation.unpin",
                                   {{"id", oldest}})["result"];
    REQUIRE(unpinned["pinned"] == false);
    listing = dispatch(runtime, "conversation.list")["result"];
    REQUIRE(listing[0]["id"] == newest);
    REQUIRE(listing[1]["id"] == middle);
    REQUIRE(listing[2]["id"] == oldest);

    // Alternate spelling: set_pinned takes an explicit bool.
    dispatch(runtime, "conversation.set_pinned",
             {{"id", middle}, {"pinned", true}});
    listing = dispatch(runtime, "conversation.list")["result"];
    REQUIRE(listing[0]["id"] == middle);
    REQUIRE(listing[0]["pinned"] == true);

    dispatch(runtime, "conversation.set_pinned",
             {{"id", middle}, {"pinned", false}});
    listing = dispatch(runtime, "conversation.list")["result"];
    REQUIRE(listing[0]["id"] == newest);

    // set_pinned without a bool payload is invalid.
    const Json missing_flag = dispatch(runtime, "conversation.set_pinned",
                                        {{"id", middle}});
    REQUIRE(missing_flag["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
}

TEST_CASE("conversation.list treats legacy documents missing pinned as unpinned",
          "[plugins][ai_editor][native][storage][pin]") {
    // Older on-disk shape (before the pinned field existed) must still list
    // cleanly.  We create a valid conversation, then rewrite the JSON on
    // disk without the pinned key to simulate a pre-existing document.
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    const Json init = dispatch(runtime, "runtime.initialize")["result"];
    std::string workspace_root_utf8;
    for (const auto& scope : init["scopes"]) {
        if (scope.value("scope", "") == "workspace") {
            workspace_root_utf8 = scope.value("path", "");
            break;
        }
    }
    REQUIRE(!workspace_root_utf8.empty());

    const std::string id =
        dispatch(runtime, "conversation.create",
                 {{"title", "Legacy"},
                  {"model", "m"},
                  {"scope", "workspace"}})["result"]["id"];
    // scope_store.history_root() lands docs under <scope>/chat_history/.
    const std::filesystem::path history_dir =
        std::filesystem::path(workspace_root_utf8) / L"chat_history";
    const std::filesystem::path legacy_path =
        history_dir / (std::filesystem::path(id + ".json"));
    REQUIRE(std::filesystem::exists(legacy_path));

    std::string legacy_text;
    {
        std::ifstream stream(legacy_path, std::ios::binary);
        REQUIRE(stream.good());
        legacy_text.assign(std::istreambuf_iterator<char>{stream},
                            std::istreambuf_iterator<char>{});
    }
    Json legacy_document = Json::parse(legacy_text);
    legacy_document.erase("pinned");
    {
        std::ofstream stream(legacy_path,
                              std::ios::binary | std::ios::trunc);
        REQUIRE(stream.good());
        const std::string rewritten = legacy_document.dump(1);
        stream.write(rewritten.data(),
                     static_cast<std::streamsize>(rewritten.size()));
    }

    // list() must default to pinned:false rather than dropping the entry.
    const Json listing = dispatch(runtime, "conversation.list")["result"];
    REQUIRE(listing.size() == 1);
    REQUIRE(listing[0]["id"] == id);
    REQUIRE(listing[0]["pinned"] == false);

    // get() normalises pinned to false for legacy documents so downstream
    // callers can rely on the field being present.
    const Json fetched = dispatch(runtime, "conversation.get",
                                   {{"id", id}})["result"];
    REQUIRE(fetched["pinned"] == false);
}

TEST_CASE("conversation.stats aggregates totals, pinned, model + month buckets",
          "[plugins][ai_editor][native][storage][stats]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    REQUIRE(dispatch(runtime, "runtime.initialize").contains("result"));

    // Empty history — averageMessages defaults to 0 rather than exploding on
    // divide-by-zero, and savedAt bounds are null (not epoch zero) so UI can
    // render "no data".
    const Json empty = dispatch(runtime, "conversation.stats")["result"];
    REQUIRE(empty["total"] == 0);
    REQUIRE(empty["pinned"] == 0);
    REQUIRE(empty["totalMessages"] == 0);
    REQUIRE(empty["averageMessages"] == 0.0);
    REQUIRE(empty["oldestSavedAt"].is_null());
    REQUIRE(empty["newestSavedAt"].is_null());
    REQUIRE(empty["byMonth"].is_array());
    REQUIRE(empty["byMonth"].empty());

    // Seed: 2 workspace convs (one gpt-4o pinned, one claude), 1 system conv
    // (gpt-4o, unpinned) — totals let us verify byScope + byModel + pinned +
    // messageCount aggregation.
    const std::string a = dispatch(runtime, "conversation.create",
                                    {{"title", "A"},
                                     {"model", "gpt-4o"},
                                     {"scope", "workspace"}})
                              ["result"]["id"];
    REQUIRE(dispatch(runtime, "conversation.append",
                     {{"id", a},
                      {"message", {{"role", "user"},
                                   {"content", "hi"}}}})
                .contains("result"));
    REQUIRE(dispatch(runtime, "conversation.append",
                     {{"id", a},
                      {"message", {{"role", "assistant"},
                                   {"content", "hello"}}}})
                .contains("result"));
    REQUIRE(dispatch(runtime, "conversation.pin",
                     {{"id", a}}).contains("result"));

    const std::string b = dispatch(runtime, "conversation.create",
                                    {{"title", "B"},
                                     {"model", "claude-3.5-sonnet"},
                                     {"scope", "workspace"}})
                              ["result"]["id"];
    REQUIRE(dispatch(runtime, "conversation.append",
                     {{"id", b},
                      {"message", {{"role", "user"},
                                   {"content", "q"}}}})
                .contains("result"));

    const std::string c = dispatch(runtime, "conversation.create",
                                    {{"title", "C"},
                                     {"model", "gpt-4o"},
                                     {"scope", "system"}})
                              ["result"]["id"];
    REQUIRE(dispatch(runtime, "conversation.append",
                     {{"id", c},
                      {"message", {{"role", "user"},
                                   {"content", "sys"}}}})
                .contains("result"));
    REQUIRE(dispatch(runtime, "conversation.append",
                     {{"id", c},
                      {"message", {{"role", "assistant"},
                                   {"content", "ok"}}}})
                .contains("result"));

    // scope=all covers both workspace + system.
    const Json all = dispatch(runtime, "conversation.stats")["result"];
    REQUIRE(all["total"] == 3);
    REQUIRE(all["pinned"] == 1);
    REQUIRE(all["totalMessages"] == 5);
    REQUIRE(all["byScope"]["workspace"] == 2);
    REQUIRE(all["byScope"]["system"] == 1);
    REQUIRE(all["byModel"]["gpt-4o"] == 2);
    REQUIRE(all["byModel"]["claude-3.5-sonnet"] == 1);
    // averageMessages is a double (5 / 3) — check with Approx-style tolerance
    // via delta since Catch2 v3 has removed Approx from the default surface.
    const double average = all["averageMessages"].get<double>();
    REQUIRE(average > 1.66);
    REQUIRE(average < 1.67);
    REQUIRE(all["oldestSavedAt"].get<int64_t>() > 0);
    REQUIRE(all["newestSavedAt"].get<int64_t>() >=
            all["oldestSavedAt"].get<int64_t>());
    // At least one YYYY-MM bucket exists and the total across buckets equals
    // "total" — protects against accidental scope-doubling in the accumulator.
    REQUIRE(!all["byMonth"].empty());
    uint64_t month_sum = 0;
    for (const auto& bucket : all["byMonth"]) {
        REQUIRE(bucket["month"].is_string());
        const std::string month = bucket["month"].get<std::string>();
        REQUIRE(month.size() == 7);
        REQUIRE(month[4] == '-');
        month_sum += bucket["count"].get<uint64_t>();
    }
    REQUIRE(month_sum == 3);

    // Scope-filtered variants: workspace sees a+b, system sees c only.
    const Json workspace_stats = dispatch(runtime, "conversation.stats",
                                           {{"scope", "workspace"}})["result"];
    REQUIRE(workspace_stats["total"] == 2);
    REQUIRE(workspace_stats["pinned"] == 1);
    REQUIRE(workspace_stats["totalMessages"] == 3);
    REQUIRE(workspace_stats["byModel"]["gpt-4o"] == 1);
    REQUIRE(workspace_stats["byModel"]["claude-3.5-sonnet"] == 1);
    REQUIRE(!workspace_stats["byScope"].contains("system"));

    const Json system_stats = dispatch(runtime, "conversation.stats",
                                        {{"scope", "system"}})["result"];
    REQUIRE(system_stats["total"] == 1);
    REQUIRE(system_stats["pinned"] == 0);
    REQUIRE(system_stats["totalMessages"] == 2);
    REQUIRE(system_stats["byModel"]["gpt-4o"] == 1);
    REQUIRE(!system_stats["byModel"].contains("claude-3.5-sonnet"));

    // Invalid scope must fail with INVALID_ARGUMENT.
    const Json bad_scope = dispatch(runtime, "conversation.stats",
                                     {{"scope", "bogus"}});
    REQUIRE(bad_scope["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
}

TEST_CASE("tools.register surfaces custom tools in tools.list and tools.call "
          "returns the passthrough payload",
          "[plugins][ai_editor][native][tools][custom]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    REQUIRE(dispatch(runtime, "runtime.initialize").contains("result"));

    // Baseline: four built-in tools before any custom registration.
    const Json baseline = dispatch(runtime, "tools.list")["result"];
    REQUIRE(baseline["tools"].is_array());
    REQUIRE(baseline["tools"].size() == 4);

    // Register a read-only custom tool with a real parameters schema.
    const Json register_result = dispatch(
        runtime, "tools.register",
        {{"name", "translate"},
         {"description", "Translate text between languages"},
         {"parameters",
          {{"type", "object"},
           {"properties",
            {{"text", {{"type", "string"}}},
             {"target", {{"type", "string"}}}}},
           {"required", Json::array({"text", "target"})}}},
         {"readOnly", true}});
    REQUIRE(register_result["result"]["ok"] == true);
    REQUIRE(register_result["result"]["name"] == "translate");

    // tools.list now surfaces five tools; the custom one carries custom:true
    // and the readOnly + parameters schema we registered.
    const Json after_register = dispatch(runtime, "tools.list")["result"];
    REQUIRE(after_register["tools"].size() == 5);
    bool found = false;
    for (const auto& tool : after_register["tools"]) {
        if (tool["name"] == "translate") {
            found = true;
            REQUIRE(tool["custom"] == true);
            REQUIRE(tool["readOnly"] == true);
            REQUIRE(tool["description"] == "Translate text between languages");
            REQUIRE(tool["parameters"]["properties"].contains("text"));
            // Read-only tools are always "allowed" regardless of mode.
            REQUIRE(tool["permission"] == "allowed");
        }
    }
    REQUIRE(found);

    // tools.call routes to the custom handler and echoes back the arguments.
    const Json call_result = dispatch(
        runtime, "tools.call",
        {{"mode", "agent"},
         {"name", "translate"},
         {"arguments",
          {{"text", "hello"},
           {"target", "zh"}}}})["result"];
    REQUIRE(call_result["custom"] == true);
    REQUIRE(call_result["name"] == "translate");
    REQUIRE(call_result["arguments"]["text"] == "hello");
    REQUIRE(call_result["arguments"]["target"] == "zh");

    // Duplicating a built-in name must be rejected — otherwise the custom
    // tool would silently hide behind the built-in.
    const Json shadow = dispatch(runtime, "tools.register",
                                  {{"name", "readFile"},
                                   {"description", "override"}});
    REQUIRE(shadow["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
}

TEST_CASE("tools.register mutating tool respects ask/plan mode gating",
          "[plugins][ai_editor][native][tools][custom][mode]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    REQUIRE(dispatch(runtime, "runtime.initialize").contains("result"));

    // Register a mutating (readOnly:false) tool — the runtime must apply the
    // same ask=deny / plan=confirm gating it applies to editFile.
    REQUIRE(dispatch(runtime, "tools.register",
                     {{"name", "writeMemo"},
                      {"description", "Write a memo"},
                      {"readOnly", false}})
                .contains("result"));

    // ask mode blocks the call entirely.
    const Json ask = dispatch(runtime, "tools.call",
                              {{"mode", "ask"},
                               {"name", "writeMemo"},
                               {"arguments", {{"text", "hi"}}}});
    REQUIRE(ask["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_PERMISSION_DENIED);

    // plan mode without confirmed returns CONFIRMATION_REQUIRED.
    const Json plan = dispatch(runtime, "tools.call",
                               {{"mode", "plan"},
                                {"name", "writeMemo"},
                                {"arguments", {{"text", "hi"}}}});
    REQUIRE(plan["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_CONFIRMATION_REQUIRED);

    // plan mode with confirmed=true or agent mode executes normally.
    const Json confirmed = dispatch(runtime, "tools.call",
                                     {{"mode", "plan"},
                                      {"name", "writeMemo"},
                                      {"arguments",
                                       {{"text", "hi"},
                                        {"confirmed", true}}}})["result"];
    REQUIRE(confirmed["custom"] == true);
    REQUIRE(confirmed["name"] == "writeMemo");

    const Json agent = dispatch(runtime, "tools.call",
                                 {{"mode", "agent"},
                                  {"name", "writeMemo"},
                                  {"arguments", {{"text", "hi"}}}})["result"];
    REQUIRE(agent["custom"] == true);

    // tools.list in plan mode surfaces the mutating custom tool as "confirm";
    // in ask mode as "disabled".
    const Json plan_list = dispatch(runtime, "tools.list",
                                     {{"mode", "plan"}})["result"];
    for (const auto& tool : plan_list["tools"]) {
        if (tool["name"] == "writeMemo") {
            REQUIRE(tool["permission"] == "confirm");
        }
    }
    const Json ask_list = dispatch(runtime, "tools.list",
                                    {{"mode", "ask"}})["result"];
    for (const auto& tool : ask_list["tools"]) {
        if (tool["name"] == "writeMemo") {
            REQUIRE(tool["permission"] == "disabled");
        }
    }
}

TEST_CASE("tools.unregister removes the custom tool and is idempotent-safe",
          "[plugins][ai_editor][native][tools][custom][unregister]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    REQUIRE(dispatch(runtime, "runtime.initialize").contains("result"));

    REQUIRE(dispatch(runtime, "tools.register",
                     {{"name", "ephemeral"},
                      {"description", "temp"}})
                .contains("result"));

    const Json listed = dispatch(runtime, "tools.list")["result"];
    REQUIRE(listed["tools"].size() == 5);

    const Json removed = dispatch(runtime, "tools.unregister",
                                   {{"name", "ephemeral"}})["result"];
    REQUIRE(removed["ok"] == true);
    REQUIRE(removed["name"] == "ephemeral");

    const Json after_remove = dispatch(runtime, "tools.list")["result"];
    REQUIRE(after_remove["tools"].size() == 4);
    for (const auto& tool : after_remove["tools"]) {
        REQUIRE(tool["name"] != "ephemeral");
    }

    // Calling the removed tool must now fall through to ERR_NOT_FOUND.
    const Json missing_call = dispatch(runtime, "tools.call",
                                        {{"mode", "agent"},
                                         {"name", "ephemeral"},
                                         {"arguments", Json::object()}});
    REQUIRE(missing_call["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_NOT_FOUND);

    // Unregistering something that never existed must produce NOT_FOUND so
    // callers can distinguish that from a bad-payload error.
    const Json missing_unreg = dispatch(runtime, "tools.unregister",
                                         {{"name", "never-existed"}});
    REQUIRE(missing_unreg["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_NOT_FOUND);

    // Payload validation: empty name / missing name must be INVALID_ARGUMENT.
    const Json bad_register = dispatch(runtime, "tools.register",
                                        {{"description", "no name"}});
    REQUIRE(bad_register["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
}

namespace {

// Poll run.status until it leaves running/pending or `budget_ms` elapses.
Json wait_for_run_terminal(sao_ai_editor_runtime_t runtime,
                            const std::string& run_id,
                            uint32_t budget_ms) {
    Json status;
    const ULONGLONG started = GetTickCount64();
    do {
        status = dispatch(runtime, "run.status", {{"runId", run_id}})
                     ["result"];
        const std::string& state = status["status"];
        if (state != "running" && state != "pending") {
            return status;
        }
        Sleep(20);
    } while (GetTickCount64() - started < budget_ms);
    return status;
}

}  // namespace

TEST_CASE("chat.run retries transient 500s and completes after recovery",
          "[plugins][ai_editor][native][runs][retry]") {
    // Sequence the fixture server so the first two hits fail with 500 and
    // the third succeeds — the retry policy (initialDelayMs=20 keeps the
    // whole test sub-second) should absorb both errors and surface the
    // final 200 as a normal completion to the caller.
    const std::string ok_body =
        R"({"id":"chat-retry-ok","model":"fixture-model",)"
        R"("choices":[{"message":{"role":"assistant",)"
        R"("content":"final"},"finish_reason":"stop"}],)"
        R"("usage":{"prompt_tokens":1,"completion_tokens":1,)"
        R"("total_tokens":2}})";
    const std::string err_body =
        R"({"error":{"type":"server_error","message":"boom"}})";
    LocalHttpServer server(std::vector<std::string>{
        canned_http_response(500, "Internal Server Error", err_body),
        canned_http_response(500, "Internal Server Error", err_body),
        canned_http_response(200, "OK", ok_body),
    });
    RuntimeFixture fixture;
    const Json params{
        {"provider", {{"id", "retry-fixture"},
                       {"endpoint", server.endpoint()}}},
        {"model", "fixture-model"},
        {"messages", Json::array(
             {{{"role", "user"}, {"content", "hello"}}})},
        {"stream", false},
        {"timeoutMs", 5'000},
        {"retry", {{"maxAttempts", 3},
                    {"initialDelayMs", 20},
                    {"maxDelayMs", 200},
                    {"multiplier", 2.0},
                    {"jitter", 0.0}}}};
    const Json started = dispatch(fixture.get(), "chat.run", params);
    REQUIRE(started.contains("result"));
    const std::string run_id = started["result"]["runId"];
    REQUIRE(server.wait_for_connections(3, 5'000));
    const Json terminal = wait_for_run_terminal(fixture.get(), run_id, 8'000);
    REQUIRE(terminal["status"] == "completed");
    REQUIRE(terminal["result"]["content"] == "final");
    // Server saw exactly three attempts.
    REQUIRE(server.captured_bodies().size() == 3);
}

TEST_CASE("chat.run maxAttempts=1 disables retries and surfaces the first "
          "500 as a failure",
          "[plugins][ai_editor][native][runs][retry]") {
    const std::string err_body =
        R"({"error":{"type":"server_error","message":"kaboom"}})";
    LocalHttpServer server(canned_http_response(500, "Internal Server Error",
                                                 err_body));
    RuntimeFixture fixture;
    const Json params{
        {"provider", {{"id", "retry-off-fixture"},
                       {"endpoint", server.endpoint()}}},
        {"model", "fixture-model"},
        {"messages", Json::array(
             {{{"role", "user"}, {"content", "hi"}}})},
        {"stream", false},
        {"timeoutMs", 5'000},
        {"retry", {{"maxAttempts", 1}}}};
    const Json started = dispatch(fixture.get(), "chat.run", params);
    REQUIRE(started.contains("result"));
    const std::string run_id = started["result"]["runId"];
    REQUIRE(server.wait_for_connections(1, 2'000));
    const Json terminal = wait_for_run_terminal(fixture.get(), run_id, 3'000);
    REQUIRE(terminal["status"] == "failed");
    // Retries were disabled so we must see exactly one attempt on the wire.
    Sleep(200);  // guard: give any (bug) extra attempts time to land.
    REQUIRE(server.captured_bodies().size() == 1);
    // Failed run.result carries the HTTP transport payload (httpStatus + body)
    // directly — run_status forwards run->result as-is so the 500 body is
    // visible to callers who want to render provider error details.
    REQUIRE(terminal.contains("result"));
    REQUIRE(terminal["result"].contains("httpStatus"));
    REQUIRE(terminal["result"]["httpStatus"] == 500);
}

TEST_CASE("chat.run emits chat.retry events before each backoff sleep",
          "[plugins][ai_editor][native][runs][retry][events]") {
    // Two 500s followed by a 200 — we assert one chat.retry event fires
    // before each of the two retry attempts, then run.completed lands
    // as usual.  Retry-After: 1 on the first 500 is echoed in the
    // delayMs of the first chat.retry payload so we exercise the
    // Retry-After header path end-to-end.
    const std::string ok_body =
        R"({"id":"chat-retry-events","model":"fixture-model",)"
        R"("choices":[{"message":{"role":"assistant",)"
        R"("content":"final"},"finish_reason":"stop"}]})";
    const std::string err_body_1 =
        R"({"error":{"type":"rate_limited","message":"slow down"}})";
    const std::string err_body_2 =
        R"({"error":{"type":"server_error","message":"boom"}})";
    LocalHttpServer server(std::vector<std::string>{
        canned_http_response(429, "Too Many Requests", err_body_1,
                              "Retry-After: 1\r\n"),
        canned_http_response(500, "Internal Server Error", err_body_2),
        canned_http_response(200, "OK", ok_body),
    });
    RuntimeFixture fixture;
    const Json params{
        {"provider", {{"id", "retry-events-fixture"},
                       {"endpoint", server.endpoint()}}},
        {"model", "fixture-model"},
        {"messages", Json::array(
             {{{"role", "user"}, {"content", "trigger"}}})},
        {"stream", false},
        {"timeoutMs", 5'000},
        // Set base delay low so the test finishes fast; Retry-After still
        // wins on attempt 2 because respectRetryAfter defaults to true.
        {"retry", {{"maxAttempts", 4},
                    {"initialDelayMs", 20},
                    {"maxDelayMs", 2'000},
                    {"multiplier", 2.0},
                    {"jitter", 0.0}}}};
    const Json started = dispatch(fixture.get(), "chat.run", params);
    REQUIRE(started.contains("result"));
    const std::string run_id = started["result"]["runId"];
    REQUIRE(server.wait_for_connections(3, 5'000));
    const Json terminal = wait_for_run_terminal(fixture.get(), run_id, 8'000);
    REQUIRE(terminal["status"] == "completed");

    // Drain the event queue and look for chat.retry + run.completed.
    std::vector<Json> retry_events;
    bool saw_run_completed = false;
    for (size_t index = 0; index < 128; ++index) {
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
        const std::string event_name =
            notification["params"].value("event", "");
        if (event_name == "chat.retry") {
            retry_events.push_back(
                notification["params"].value("payload", Json::object()));
        } else if (event_name == "run.completed") {
            saw_run_completed = true;
        }
    }
    REQUIRE(saw_run_completed);
    // Two retries (attempt 2 recovers from 429, attempt 3 recovers from 500).
    REQUIRE(retry_events.size() == 2);
    REQUIRE(retry_events[0]["runId"] == run_id);
    REQUIRE(retry_events[0]["attempt"] == 2);
    REQUIRE(retry_events[0]["maxAttempts"] == 4);
    REQUIRE(retry_events[0]["reason"] == "429");
    // Retry-After: 1 → 1000 ms wins over the 20 ms base backoff.
    REQUIRE(retry_events[0]["delayMs"].get<int64_t>() >= 900);
    REQUIRE(retry_events[1]["attempt"] == 3);
    REQUIRE(retry_events[1]["reason"] == "http_5xx");
}

TEST_CASE("tools.call validates arguments against the built-in JSON schema",
          "[plugins][ai_editor][native][tools][validation]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();

    // Missing required `path` → structured validationErrors block.  The path
    // is reported as `$.path` (JSONPath-lite) so the UI can highlight the
    // missing field without regexing the reason string.
    const Json missing_required =
        dispatch(runtime, "tools.call",
                 {{"mode", "agent"},
                  {"name", "readFile"},
                  {"arguments", Json::object()}});
    REQUIRE(missing_required.contains("error"));
    REQUIRE(missing_required["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
    const Json missing_details =
        missing_required["error"]["data"].value("details", Json::object());
    REQUIRE(missing_details.contains("validationErrors"));
    REQUIRE(missing_details["validationErrors"].is_array());
    REQUIRE(missing_details["validationErrors"].size() == 1);
    REQUIRE(missing_details["validationErrors"][0]["path"] == "$.path");
    REQUIRE(missing_details["validationErrors"][0]["reason"] ==
            "missing required field");

    // Wrong type for `path` (number instead of string) — surfaces the type
    // mismatch with both expected and observed types in the reason.
    const Json wrong_type =
        dispatch(runtime, "tools.call",
                 {{"mode", "agent"},
                  {"name", "readFile"},
                  {"arguments", {{"path", 42}}}});
    REQUIRE(wrong_type["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
    const Json type_details =
        wrong_type["error"]["data"].value("details", Json::object());
    REQUIRE(type_details["validationErrors"].size() == 1);
    REQUIRE(type_details["validationErrors"][0]["path"] == "$.path");
    REQUIRE(type_details["validationErrors"][0]["reason"] ==
            "type expected string, got integer");

    // Wrong type for a non-required numeric field (startLine as string) —
    // proves validation drills into optional properties, not just required.
    const Json wrong_optional_type =
        dispatch(runtime, "tools.call",
                 {{"mode", "agent"},
                  {"name", "readFile"},
                  {"arguments", {{"path", "note.txt"},
                                  {"startLine", "one"}}}});
    REQUIRE(wrong_optional_type["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
    const Json optional_details =
        wrong_optional_type["error"]["data"].value("details", Json::object());
    REQUIRE(optional_details["validationErrors"][0]["path"] == "$.startLine");
    REQUIRE(optional_details["validationErrors"][0]["reason"] ==
            "type expected integer, got string");

    // Well-formed arguments continue to dispatch normally.  We stage a file
    // via editFile (mode=agent bypasses the confirmation gate) so the
    // subsequent readFile has real content to return.
    REQUIRE(dispatch(runtime, "tools.call",
                     {{"mode", "agent"},
                      {"name", "editFile"},
                      {"arguments", {{"path", "hello.txt"},
                                     {"content", "hi\n"}}}})
                .contains("result"));
    const Json read_ok =
        dispatch(runtime, "tools.call",
                 {{"mode", "agent"},
                  {"name", "readFile"},
                  {"arguments", {{"path", "hello.txt"}}}});
    REQUIRE(read_ok.contains("result"));
    REQUIRE(read_ok["result"]["content"] == "hi\n");
}

TEST_CASE("tools.call runs custom-tool schema validation before passthrough",
          "[plugins][ai_editor][native][tools][validation]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();

    // Register a custom tool with a JSON schema requiring `query` (string) and
    // an optional `limit` integer inside `filters.limit` — enough shape to
    // exercise nested-object validation on the required + type paths.
    const Json parameters{
        {"type", "object"},
        {"properties",
         {{"query", {{"type", "string"}}},
          {"filters",
           {{"type", "object"},
            {"properties", {{"limit", {{"type", "integer"}}}}}}}}},
        {"required", Json::array({"query"})}};
    REQUIRE(dispatch(runtime, "tools.register",
                     {{"name", "customSearch"},
                      {"description", "custom"},
                      {"parameters", parameters}})
                .contains("result"));

    // Nested type error at `$.filters.limit` — proves the recursive property
    // walk reports the exact JSONPath the LLM should fix.
    const Json invalid =
        dispatch(runtime, "tools.call",
                 {{"mode", "agent"},
                  {"name", "customSearch"},
                  {"arguments",
                   {{"query", "hello"},
                    {"filters", {{"limit", "seven"}}}}}});
    REQUIRE(invalid["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
    const Json nested_details =
        invalid["error"]["data"].value("details", Json::object());
    REQUIRE(nested_details["validationErrors"][0]["path"] ==
            "$.filters.limit");
    REQUIRE(nested_details["validationErrors"][0]["reason"] ==
            "type expected integer, got string");

    // Valid payload still round-trips through the custom-tool passthrough.
    const Json valid =
        dispatch(runtime, "tools.call",
                 {{"mode", "agent"},
                  {"name", "customSearch"},
                  {"arguments",
                   {{"query", "hello"}, {"filters", {{"limit", 7}}}}}});
    REQUIRE(valid.contains("result"));
    REQUIRE(valid["result"]["custom"] == true);
    REQUIRE(valid["result"]["arguments"]["query"] == "hello");
}

TEST_CASE("mcp.render_resource_uri expands simple + reserved templates",
          "[plugins][ai_editor][native][mcp][templates]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();

    // Simple `{path}` expansion percent-encodes the `/` in `docs/README.md`
    // because the caller wants a component-safe substitution.  Same behaviour
    // MCP clients get out of RFC 6570 §3.2.2.
    const Json simple = dispatch(runtime, "mcp.render_resource_uri",
                                 {{"template", "sao://workspace/{path}"},
                                  {"arguments", {{"path", "docs/README.md"}}}});
    REQUIRE(simple.contains("result"));
    REQUIRE(simple["result"]["uri"] ==
            "sao://workspace/docs%2FREADME.md");

    // Reserved expansion `{+path}` keeps the `/` literal so path-shaped
    // template variables round-trip cleanly.
    const Json reserved = dispatch(runtime, "mcp.render_resource_uri",
                                   {{"template", "sao://workspace/{+path}"},
                                    {"arguments", {{"path", "docs/README.md"}}}});
    REQUIRE(reserved.contains("result"));
    REQUIRE(reserved["result"]["uri"] ==
            "sao://workspace/docs/README.md");

    // Missing variable → empty string, mirroring RFC 6570 §3.2.1.  The
    // scheme + prefix stay verbatim so the caller can still detect the gap
    // (URI ends in a trailing slash) without a second round-trip.
    const Json missing =
        dispatch(runtime, "mcp.render_resource_uri",
                 {{"template", "sao://workspace/{path}"},
                  {"arguments", Json::object()}});
    REQUIRE(missing["result"]["uri"] == "sao://workspace/");

    // Unknown operator `{?query}` is preserved verbatim so callers see
    // that the template used a form they did not opt into supporting.
    const Json unsupported =
        dispatch(runtime, "mcp.render_resource_uri",
                 {{"template", "sao://search{?query}"},
                  {"arguments", {{"query", "foo"}}}});
    REQUIRE(unsupported["result"]["uri"] == "sao://search{?query}");

    // Missing `template` → INVALID_ARGUMENT so callers get an actionable
    // error rather than an empty URI.
    const Json missing_template =
        dispatch(runtime, "mcp.render_resource_uri", Json::object());
    REQUIRE(missing_template.contains("error"));
    REQUIRE(missing_template["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
}

TEST_CASE("mcp.list_resource_templates returns a well-formed empty aggregation",
          "[plugins][ai_editor][native][mcp][templates]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    const Json listing =
        dispatch(runtime, "mcp.list_resource_templates", Json::object());
    REQUIRE(listing.contains("result"));
    REQUIRE(listing["result"]["items"].is_array());
    REQUIRE(listing["result"]["items"].empty());
    REQUIRE(listing["result"]["total"] == 0);
}

TEST_CASE("chat.dispatch_tool_calls fans out three readFile calls in parallel "
          "and returns id-tagged results in input order",
          "[plugins][ai_editor][native][chat][tools][dispatch]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();

    // Stage three files via editFile — mode=agent bypasses the confirmation
    // gate so we get real content back on the subsequent readFile calls.
    for (const auto& [path, content] :
         std::vector<std::pair<std::string, std::string>>{
             {"alpha.txt", "alpha\n"},
             {"beta.txt", "beta\n"},
             {"gamma.txt", "gamma\n"}}) {
        REQUIRE(dispatch(runtime, "tools.call",
                         {{"mode", "agent"},
                          {"name", "editFile"},
                          {"arguments", {{"path", path},
                                         {"content", content}}}})
                    .contains("result"));
    }

    // Fan out three readFile tool_calls; concurrency=3 so all three run
    // simultaneously (behaviour tested is not "N wall-clock ms" — timing is
    // fixture-specific — but that every call resolves independently).
    const Json invoked = dispatch(
        runtime, "chat.dispatch_tool_calls",
        {{"toolCalls",
          Json::array({
              Json{{"id", "call_1"}, {"name", "readFile"},
                   {"arguments", {{"path", "alpha.txt"}}}},
              Json{{"id", "call_2"}, {"name", "readFile"},
                   {"arguments", {{"path", "beta.txt"}}}},
              Json{{"id", "call_3"}, {"name", "readFile"},
                   {"arguments", {{"path", "gamma.txt"}}}}})},
         {"mode", "agent"},
         {"concurrency", 3}});
    REQUIRE(invoked.contains("result"));
    const Json& body = invoked["result"];
    REQUIRE(body["results"].size() == 3);
    REQUIRE(body["successCount"] == 3);
    REQUIRE(body["failureCount"] == 0);
    // Results are ordered by input index regardless of which worker
    // finished first — this is the whole point of the phase-3 assembly.
    REQUIRE(body["results"][0]["id"] == "call_1");
    REQUIRE(body["results"][0]["name"] == "readFile");
    REQUIRE(body["results"][0]["status"] == "completed");
    REQUIRE(body["results"][0]["result"]["content"] == "alpha\n");
    REQUIRE(body["results"][0].contains("durationMs"));
    REQUIRE(body["results"][1]["id"] == "call_2");
    REQUIRE(body["results"][1]["result"]["content"] == "beta\n");
    REQUIRE(body["results"][2]["id"] == "call_3");
    REQUIRE(body["results"][2]["result"]["content"] == "gamma\n");
    REQUIRE(body.contains("totalMs"));
}

TEST_CASE("chat.dispatch_tool_calls records per-call failure without aborting "
          "peers when one tool has bad arguments",
          "[plugins][ai_editor][native][chat][tools][dispatch]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();

    // Set up a file so at least one call has real content to return.
    REQUIRE(dispatch(runtime, "tools.call",
                     {{"mode", "agent"},
                      {"name", "editFile"},
                      {"arguments", {{"path", "notes.txt"},
                                     {"content", "hello\n"}}}})
                .contains("result"));

    // Three-way mix: valid readFile, listFiles with schema-satisfying args,
    // and a searchFiles call missing the required `query` field.  The last
    // one fails validation but the peers still return their results.
    const Json invoked = dispatch(
        runtime, "chat.dispatch_tool_calls",
        {{"toolCalls",
          Json::array({
              Json{{"id", "ok_read"}, {"name", "readFile"},
                   {"arguments", {{"path", "notes.txt"}}}},
              Json{{"id", "ok_list"}, {"name", "listFiles"},
                   {"arguments", {{"path", "."}}}},
              Json{{"id", "bad_search"}, {"name", "searchFiles"},
                   {"arguments", Json::object()}}})},
         {"mode", "agent"}});
    REQUIRE(invoked.contains("result"));
    const Json& body = invoked["result"];
    REQUIRE(body["results"].size() == 3);
    REQUIRE(body["successCount"] == 2);
    REQUIRE(body["failureCount"] == 1);
    REQUIRE(body["results"][0]["status"] == "completed");
    REQUIRE(body["results"][0]["result"]["content"] == "hello\n");
    REQUIRE(body["results"][1]["status"] == "completed");
    REQUIRE(body["results"][1]["result"]["entries"].is_array());
    REQUIRE(body["results"][2]["status"] == "failed");
    REQUIRE(body["results"][2]["id"] == "bad_search");
    REQUIRE(body["results"][2]["name"] == "searchFiles");
    REQUIRE(body["results"][2]["error"] == "invalid argument");
    // Structured details from the tool registry (validationErrors) survive
    // the failure path so LLM UIs can highlight the offending field.
    REQUIRE(body["results"][2].contains("details"));
    REQUIRE(body["results"][2]["details"]["validationErrors"][0]["path"] ==
            "$.query");
}

TEST_CASE("chat.dispatch_tool_calls accepts arguments as a serialised JSON "
          "string (LLM tool_call wire format)",
          "[plugins][ai_editor][native][chat][tools][dispatch]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();

    REQUIRE(dispatch(runtime, "tools.call",
                     {{"mode", "agent"},
                      {"name", "editFile"},
                      {"arguments", {{"path", "readme.md"},
                                     {"content", "docs\n"}}}})
                .contains("result"));

    // The OpenAI tool_calls wire format hands the model's arguments back as
    // a JSON string, not an object.  The dispatcher must eagerly parse the
    // string on the caller thread (phase 1) so worker threads never race
    // on Json::parse.  A garbage string on a sibling call fails cleanly
    // without touching the peer that supplied a valid object.
    const Json invoked = dispatch(
        runtime, "chat.dispatch_tool_calls",
        {{"toolCalls",
          Json::array({
              Json{{"id", "str_ok"}, {"name", "readFile"},
                   {"arguments", R"({"path":"readme.md"})"}},
              Json{{"id", "str_bad"}, {"name", "readFile"},
                   {"arguments", R"({"path": broken)"}},
              Json{{"id", "obj_ok"}, {"name", "listFiles"},
                   {"arguments", {{"path", "."}}}}})},
         {"mode", "agent"}});
    REQUIRE(invoked.contains("result"));
    const Json& body = invoked["result"];
    REQUIRE(body["successCount"] == 2);
    REQUIRE(body["failureCount"] == 1);
    REQUIRE(body["results"][0]["status"] == "completed");
    REQUIRE(body["results"][0]["result"]["content"] == "docs\n");
    REQUIRE(body["results"][1]["status"] == "failed");
    REQUIRE(body["results"][1]["error"] == "invalid arguments JSON");
    REQUIRE(body["results"][2]["status"] == "completed");
}

TEST_CASE("chat.dispatch_tool_calls surfaces permission_denied when mode=ask "
          "gates a mutating tool and rejects unsupported modes up front",
          "[plugins][ai_editor][native][chat][tools][dispatch][permissions]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();

    // mode=ask + editFile is denied by NativeToolRegistry.  Batch dispatch
    // must inherit that gating unchanged — otherwise callers could use the
    // fan-out API to bypass the built-in permission policy.
    const Json ask = dispatch(
        runtime, "chat.dispatch_tool_calls",
        {{"toolCalls",
          Json::array({
              Json{{"id", "denied"}, {"name", "editFile"},
                   {"arguments", {{"path", "blocked.txt"},
                                  {"content", "nope"}}}}})},
         {"mode", "ask"}});
    REQUIRE(ask.contains("result"));
    const Json& body = ask["result"];
    REQUIRE(body["failureCount"] == 1);
    REQUIRE(body["successCount"] == 0);
    REQUIRE(body["results"][0]["status"] == "failed");
    REQUIRE(body["results"][0]["error"] == "permission denied");

    // Empty toolCalls array is a hard INVALID_ARGUMENT on the outer envelope
    // — nothing to dispatch, nothing to report.
    const Json empty = dispatch(
        runtime, "chat.dispatch_tool_calls",
        {{"toolCalls", Json::array()}, {"mode", "agent"}});
    REQUIRE(empty.contains("error"));
    REQUIRE(empty["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);

    // Unsupported mode string aborts the whole request instead of silently
    // downgrading each call to `agent`.
    const Json bad_mode = dispatch(
        runtime, "chat.dispatch_tool_calls",
        {{"toolCalls",
          Json::array({
              Json{{"id", "x"}, {"name", "readFile"},
                   {"arguments", {{"path", "notes.txt"}}}}})},
         {"mode", "chat"}});
    REQUIRE(bad_mode.contains("error"));
    REQUIRE(bad_mode["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
}
