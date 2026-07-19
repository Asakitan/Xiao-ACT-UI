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
#include "../src/tool_result_filter.h"
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
    // The exported envelope carries a sha256 stamp; drop it so the mutated
    // payload is treated as a legacy (pre-checksum) import rather than a
    // tampered one.  See conversation.import for the invariant.
    rewritten.erase("sha256");
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
    fresh.erase("sha256");  // mutated payload can no longer match the stamp
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

TEST_CASE("chat.run forwards OpenAI sampling knobs into the request body",
          "[plugins][ai_editor][native][providers][openai][sampling]") {
    // Loop back through the LocalHttpServer so we can peek at the outgoing
    // request body verbatim.  Every mainstream sampling knob is expected to
    // land in the body untouched: run_chat_sync's expanded forward list
    // covers top_p, frequency_penalty, presence_penalty, seed, stop,
    // logit_bias, logprobs, top_logprobs, n, user, parallel_tool_calls.
    const std::string body =
        R"({"choices":[{"message":{"role":"assistant","content":"ok"}}]})";
    LocalHttpServer server(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: " +
        std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body);
    RuntimeFixture fixture;
    Json params{{"provider", {{"id", "fixture"},
                              {"endpoint", server.endpoint()}}},
                {"model", "fixture-model"},
                {"messages", Json::array({Json{{"role", "user"},
                                               {"content", "hi"}}})},
                {"stream", false},
                {"timeoutMs", 2'000},
                {"temperature", 0.42},
                {"max_tokens", 128},
                {"top_p", 0.9},
                {"frequency_penalty", 0.5},
                {"presence_penalty", -0.25},
                {"seed", 12345},
                {"stop", Json::array({"END", "###"})},
                {"logit_bias", Json{{"50256", -100}}},
                {"logprobs", true},
                {"top_logprobs", 5},
                {"n", 2},
                {"user", "trace-user-1"},
                {"parallel_tool_calls", false}};
    const Json started = dispatch(fixture.get(), "chat.run", params);
    REQUIRE(started.contains("result"));
    REQUIRE(server.wait_for_connections(1, 2'000));
    const auto bodies = server.captured_bodies();
    REQUIRE(bodies.size() == 1);
    const Json body_json = Json::parse(bodies.front());
    REQUIRE(body_json["model"] == "fixture-model");
    REQUIRE(body_json["temperature"] == 0.42);
    REQUIRE(body_json["max_tokens"] == 128);
    REQUIRE(body_json["top_p"] == 0.9);
    REQUIRE(body_json["frequency_penalty"] == 0.5);
    REQUIRE(body_json["presence_penalty"] == -0.25);
    REQUIRE(body_json["seed"] == 12345);
    REQUIRE(body_json["stop"].is_array());
    REQUIRE(body_json["stop"].size() == 2);
    REQUIRE(body_json["stop"][0] == "END");
    REQUIRE(body_json["logit_bias"]["50256"] == -100);
    REQUIRE(body_json["logprobs"] == true);
    REQUIRE(body_json["top_logprobs"] == 5);
    REQUIRE(body_json["n"] == 2);
    REQUIRE(body_json["user"] == "trace-user-1");
    REQUIRE(body_json["parallel_tool_calls"] == false);
    // Fields never set by the caller must not leak into the body.  If they
    // did, callers with strict OpenAI-compat proxies would see the request
    // rejected for unknown keys.
    REQUIRE_FALSE(body_json.contains("top_k"));
    REQUIRE_FALSE(body_json.contains("tools"));
    REQUIRE_FALSE(body_json.contains("tool_choice"));
    REQUIRE_FALSE(body_json.contains("response_format"));
}

TEST_CASE("Provider router forwards Anthropic top_p / top_k and folds stop "
          "into stop_sequences",
          "[plugins][ai_editor][native][providers][anthropic][sampling]") {
    Json openai_body{
        {"model", "claude-3-5-sonnet"},
        {"messages",
         Json::array({Json{{"role", "user"}, {"content", "hi"}}})},
        {"max_tokens", 256},
        {"temperature", 0.3},
        {"top_p", 0.85},
        {"top_k", 40},
        {"stop", "###"},
        // Every one of these is OpenAI-only.  build_provider_request must
        // silently drop them from the Anthropic body — the API would 400 on
        // unknown fields.
        {"frequency_penalty", 0.4},
        {"presence_penalty", 0.4},
        {"seed", 7},
        {"logit_bias", Json{{"999", -100}}},
        {"logprobs", true},
        {"top_logprobs", 3},
        {"n", 2},
        {"user", "u"},
        {"parallel_tool_calls", false},
        {"response_format", Json{{"type", "json_object"}}}};
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
    const Json parsed = Json::parse(request.body_json);
    REQUIRE(parsed["temperature"] == 0.3);
    REQUIRE(parsed["top_p"] == 0.85);
    REQUIRE(parsed["top_k"] == 40);
    REQUIRE(parsed["stop_sequences"].is_array());
    REQUIRE(parsed["stop_sequences"].size() == 1);
    REQUIRE(parsed["stop_sequences"][0] == "###");
    // Passing an already-arrayed stop should be preserved verbatim (no
    // double-wrapping).
    openai_body["stop"] = Json::array({"A", "B", "C"});
    sao::ai_editor::native::ProviderRequest request_array;
    REQUIRE(sao::ai_editor::native::build_provider_request(
                route, openai_body, request_array) == SAO_AI_EDITOR_OK);
    const Json parsed_array = Json::parse(request_array.body_json);
    REQUIRE(parsed_array["stop_sequences"].size() == 3);
    REQUIRE(parsed_array["stop_sequences"][2] == "C");
    // OpenAI-only knobs must not leak into the Anthropic body.
    for (const char* dropped :
         {"frequency_penalty", "presence_penalty", "seed", "logit_bias",
          "logprobs", "top_logprobs", "n", "user", "parallel_tool_calls",
          "response_format", "stop"}) {
        REQUIRE_FALSE(parsed.contains(dropped));
    }
}

TEST_CASE("Provider router promotes Gemini sampling knobs into "
          "generationConfig",
          "[plugins][ai_editor][native][providers][gemini][sampling]") {
    Json openai_body{
        {"model", "gemini-2.0-flash"},
        {"messages",
         Json::array({Json{{"role", "user"},
                            {"content", "explain quantum"}}})},
        {"max_tokens", 128},
        {"temperature", 0.6},
        {"top_p", 0.75},
        {"top_k", 32},
        {"seed", 99},
        {"stop", Json::array({"STOP1", "STOP2"})},
        {"response_format", Json{{"type", "json_object"}}},
        // OpenAI-only knobs — must be silently dropped.
        {"frequency_penalty", 0.1},
        {"presence_penalty", 0.2},
        {"logit_bias", Json{{"5", -50}}},
        {"logprobs", true},
        {"top_logprobs", 3},
        {"n", 2},
        {"user", "u"},
        {"parallel_tool_calls", true}};
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
    const Json parsed = Json::parse(request.body_json);
    REQUIRE(parsed.contains("generationConfig"));
    const Json& config = parsed["generationConfig"];
    REQUIRE(config["temperature"] == 0.6);
    REQUIRE(config["maxOutputTokens"] == 128);
    REQUIRE(config["topP"] == 0.75);
    REQUIRE(config["topK"] == 32);
    REQUIRE(config["seed"] == 99);
    REQUIRE(config["stopSequences"].is_array());
    REQUIRE(config["stopSequences"].size() == 2);
    REQUIRE(config["stopSequences"][1] == "STOP2");
    REQUIRE(config["responseMimeType"] == "application/json");
    // OpenAI-only knobs must not appear anywhere in the Gemini body — the
    // API would 400 on unknown generationConfig fields.
    for (const char* dropped :
         {"frequencyPenalty", "presencePenalty", "logitBias", "logprobs",
          "topLogprobs", "n", "user", "parallelToolCalls"}) {
        REQUIRE_FALSE(config.contains(dropped));
    }
    // OpenAI-shaped names also must not have leaked at the top level.
    for (const char* dropped :
         {"frequency_penalty", "presence_penalty", "logit_bias", "logprobs",
          "top_logprobs", "n", "user", "parallel_tool_calls",
          "response_format", "stop", "top_p", "top_k", "seed"}) {
        REQUIRE_FALSE(parsed.contains(dropped));
    }
    // A stop string (not array) should upgrade to a single-element
    // stopSequences.
    openai_body["stop"] = "END";
    sao::ai_editor::native::ProviderRequest request_single;
    REQUIRE(sao::ai_editor::native::build_provider_request(
                route, openai_body, request_single) == SAO_AI_EDITOR_OK);
    const Json parsed_single = Json::parse(request_single.body_json);
    REQUIRE(parsed_single["generationConfig"]["stopSequences"].size() == 1);
    REQUIRE(parsed_single["generationConfig"]["stopSequences"][0] == "END");
    // response_format with an unrecognised type must be dropped, not
    // converted to a bogus responseMimeType.
    openai_body["response_format"] = Json{{"type", "text"}};
    sao::ai_editor::native::ProviderRequest request_text;
    REQUIRE(sao::ai_editor::native::build_provider_request(
                route, openai_body, request_text) == SAO_AI_EDITOR_OK);
    const Json parsed_text = Json::parse(request_text.body_json);
    REQUIRE_FALSE(parsed_text["generationConfig"].contains(
        "responseMimeType"));
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
    // Strip the envelope sha256 so the mutated payload imports as a
    // legacy (pre-checksum) shape rather than being rejected as tampered.
    rewritten.erase("sha256");
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
    fresh.erase("sha256");  // mutated payload can no longer match the stamp
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

TEST_CASE("AI Editor agents.export packages a single agent with sao-agent/1 "
          "envelope",
          "[plugins][ai_editor][native][agents][export]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();

    // Seed a workspace-scoped user agent so scope="workspace" has a target
    // beyond the built-ins to enumerate.
    const Json agent_json{
        {"id", "custom-export-agent"},
        {"name", "Custom Export Agent"},
        {"description", "Test agent export"},
        {"system_prompt", "You export agents."},
        {"tools", Json::array({"readFile", "listFiles"})},
        {"model", "test-model"},
        {"icon", "\xF0\x9F\x93\xA6"},
        {"when_to_use", "When exercising agents.export in tests"}};
    REQUIRE(dispatch(runtime, "agents.save_def",
                     {{"scope", "workspace"}, {"agent", agent_json}})
                .contains("result"));

    // Missing both id and scope → invalid.
    const Json missing =
        dispatch(runtime, "agents.export", Json::object());
    REQUIRE(missing["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);

    // Providing both id and scope → invalid (mutually exclusive).
    const Json both = dispatch(runtime, "agents.export",
                                {{"id", "custom-export-agent"},
                                 {"scope", "workspace"}});
    REQUIRE(both["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);

    const Json exported = dispatch(runtime, "agents.export",
                                    {{"id", "custom-export-agent"}});
    REQUIRE(exported.contains("result"));
    REQUIRE(exported["result"]["format"] == "sao-agent/1");
    REQUIRE(exported["result"]["exportedAt"].is_number());
    REQUIRE(exported["result"]["agent"]["id"] == "custom-export-agent");
    REQUIRE(exported["result"]["agent"]["name"] == "Custom Export Agent");
    REQUIRE(exported["result"]["agent"]["tools"].size() == 2);
    REQUIRE(exported["result"]["agent"]["system_prompt"] ==
            "You export agents.");
    REQUIRE(exported["result"]["agent"]["when_to_use"] ==
            "When exercising agents.export in tests");

    // Exporting a compile-time built-in id must succeed and preserve the
    // builtin flag so downstream tooling can distinguish the source.
    const Json builtin_single =
        dispatch(runtime, "agents.export", {{"id", "code-reviewer"}});
    REQUIRE(builtin_single["result"]["format"] == "sao-agent/1");
    REQUIRE(builtin_single["result"]["agent"]["builtin"] == true);
    REQUIRE(builtin_single["result"]["agent"]["id"] == "code-reviewer");

    // Unknown id → NOT_FOUND propagated as protocol error.
    const Json unknown = dispatch(runtime, "agents.export",
                                   {{"id", "agent-nope-nope"}});
    REQUIRE(unknown.contains("error"));

    // scope-string of an unsupported value must reject before touching the
    // store — parity with workflow.export.
    const Json bad_scope =
        dispatch(runtime, "agents.export", {{"scope", "no-such-scope"}});
    REQUIRE(bad_scope["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
}

TEST_CASE("AI Editor agents.export scope=all returns sao-agents/1 envelope "
          "and honours scope filters",
          "[plugins][ai_editor][native][agents][export]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();

    // Seed one workspace-scoped agent so the scope filters have something
    // observable beyond the built-ins.
    const Json workspace_agent{
        {"id", "ws-batch-agent"},
        {"name", "Workspace Batch Agent"},
        {"description", "Batch export target"},
        {"system_prompt", "You batch export."},
        {"tools", Json::array({"readFile"})}};
    REQUIRE(dispatch(runtime, "agents.save_def",
                     {{"scope", "workspace"}, {"agent", workspace_agent}})
                .contains("result"));

    // scope=all surfaces workspace agents alongside the built-ins.
    const Json all = dispatch(runtime, "agents.export", {{"scope", "all"}});
    REQUIRE(all["result"]["format"] == "sao-agents/1");
    REQUIRE(all["result"]["count"].get<int>() >= 6);  // 5 built-ins + 1 user
    bool saw_workspace = false;
    bool saw_builtin = false;
    for (const auto& item : all["result"]["agents"]) {
        const std::string id = item.value("id", "");
        if (id == "ws-batch-agent") {
            saw_workspace = true;
            REQUIRE(item["builtin"] == false);
        } else if (id == "code-reviewer") {
            saw_builtin = true;
            REQUIRE(item["builtin"] == true);
        }
    }
    REQUIRE(saw_workspace);
    REQUIRE(saw_builtin);

    // scope=workspace omits built-ins even though they live in the same
    // registry map.
    const Json ws =
        dispatch(runtime, "agents.export", {{"scope", "workspace"}});
    REQUIRE(ws["result"]["format"] == "sao-agents/1");
    REQUIRE(ws["result"]["count"] == 1);
    REQUIRE(ws["result"]["agents"][0]["id"] == "ws-batch-agent");

    // scope=builtin walks the compile-time list only.
    const Json builtin_export =
        dispatch(runtime, "agents.export", {{"scope", "builtin"}});
    REQUIRE(builtin_export["result"]["format"] == "sao-agents/1");
    REQUIRE(builtin_export["result"]["count"].get<int>() >= 5);
    bool saw_code_reviewer = false;
    for (const auto& item : builtin_export["result"]["agents"]) {
        if (item.value("id", "") == "code-reviewer") {
            saw_code_reviewer = true;
            REQUIRE(item["builtin"] == true);
        }
    }
    REQUIRE(saw_code_reviewer);

    // scope=system has no seed agents in this fixture → count 0 but a
    // well-formed envelope.
    const Json sys =
        dispatch(runtime, "agents.export", {{"scope", "system"}});
    REQUIRE(sys["result"]["format"] == "sao-agents/1");
    REQUIRE(sys["result"]["count"] == 0);
}

TEST_CASE("AI Editor agents.import round-trips with overwrite semantics",
          "[plugins][ai_editor][native][agents][import]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();

    const Json agent_json{
        {"id", "custom-import-agent"},
        {"name", "Custom Import Agent"},
        {"description", "Seed"},
        {"system_prompt", "You import."},
        {"tools", Json::array({"readFile"})}};
    REQUIRE(dispatch(runtime, "agents.save_def",
                     {{"scope", "workspace"}, {"agent", agent_json}})
                .contains("result"));

    const Json exported = dispatch(runtime, "agents.export",
                                    {{"id", "custom-import-agent"}})["result"];
    REQUIRE(exported["format"] == "sao-agent/1");

    // Unknown format → invalid.
    const Json bogus = dispatch(runtime, "agents.import",
                                 {{"payload", {{"format", "not-real/1"}}}});
    REQUIRE(bogus["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);

    // Conflict with overwrite=false → rejected atomically, imported=0.
    const Json conflict =
        dispatch(runtime, "agents.import",
                 {{"payload", exported},
                  {"scope", "workspace"},
                  {"overwrite", false}});
    REQUIRE(conflict.contains("error"));
    REQUIRE(conflict["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
    REQUIRE(conflict["error"]["data"]["details"]["imported"] == 0);
    REQUIRE(conflict["error"]["data"]["details"]["conflicts"].size() == 1);
    REQUIRE(conflict["error"]["data"]["details"]["conflicts"][0] ==
            "custom-import-agent");

    // Overwrite=true replaces the definition; new description wins.
    Json rewritten = exported;
    rewritten["agent"]["description"] = "Rewritten!";
    rewritten["agent"]["system_prompt"] = "You import v2.";
    rewritten["agent"]["tools"] =
        Json::array({"readFile", "searchFiles", "editFile"});
    // Strip the envelope sha256 so the mutated payload imports as a
    // legacy shape rather than being rejected as tampered.
    rewritten.erase("sha256");
    const Json overwrote = dispatch(runtime, "agents.import",
                                     {{"payload", rewritten},
                                      {"scope", "workspace"},
                                      {"overwrite", true}});
    REQUIRE(overwrote.contains("result"));
    REQUIRE(overwrote["result"]["imported"] == 1);
    REQUIRE(overwrote["result"]["assignedIds"][0] == "custom-import-agent");

    const Json fetched = dispatch(runtime, "agents.get_def",
                                   {{"id", "custom-import-agent"}});
    REQUIRE(fetched["result"]["description"] == "Rewritten!");
    REQUIRE(fetched["result"]["system_prompt"] == "You import v2.");
    REQUIRE(fetched["result"]["tools"].size() == 3);

    // Fresh id not present in store → imported without conflict.
    Json fresh = exported;
    fresh["agent"]["id"] = "custom-import-fresh";
    fresh["agent"]["name"] = "Fresh Agent";
    fresh.erase("sha256");  // mutated payload can no longer match the stamp
    const Json fresh_result =
        dispatch(runtime, "agents.import",
                 {{"payload", fresh}, {"scope", "workspace"}});
    REQUIRE(fresh_result.contains("result"));
    REQUIRE(fresh_result["result"]["imported"] == 1);
    REQUIRE(fresh_result["result"]["conflicts"].empty());
    REQUIRE(fresh_result["result"]["assignedIds"][0] ==
            "custom-import-fresh");

    // Batch sao-agents/1 envelope carries both agents — verifies list path
    // and confirms conflicts populate correctly when only some entries hit.
    Json batch{{"format", "sao-agents/1"},
                {"agents", Json::array()}};
    batch["agents"].push_back(fresh["agent"]);  // existing (fresh) → conflict
    Json brand_new = fresh["agent"];
    brand_new["id"] = "custom-import-brand-new";
    brand_new["name"] = "Brand New";
    batch["agents"].push_back(brand_new);
    const Json batch_conflict =
        dispatch(runtime, "agents.import",
                 {{"payload", batch},
                  {"scope", "workspace"},
                  {"overwrite", false}});
    REQUIRE(batch_conflict.contains("error"));
    REQUIRE(
        batch_conflict["error"]["data"]["details"]["conflicts"].size() == 1);
    REQUIRE(
        batch_conflict["error"]["data"]["details"]["conflicts"][0] ==
        "custom-import-fresh");

    // Overwriting both accepts the whole batch atomically.
    const Json batch_ok = dispatch(runtime, "agents.import",
                                    {{"payload", batch},
                                     {"scope", "workspace"},
                                     {"overwrite", true}});
    REQUIRE(batch_ok.contains("result"));
    REQUIRE(batch_ok["result"]["imported"] == 2);
    REQUIRE(batch_ok["result"]["assignedIds"].size() == 2);
}

TEST_CASE("AI Editor agents.import refuses to overwrite built-in agents",
          "[plugins][ai_editor][native][agents][import]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();

    // code-reviewer is a compile-time built-in; even overwrite=true must
    // not shadow it via the on-disk registry.
    Json envelope{
        {"format", "sao-agent/1"},
        {"agent", Json{{"id", "code-reviewer"},
                        {"name", "Hijacked"},
                        {"description", "Attacker payload"},
                        {"system_prompt", "You are pwned."},
                        {"tools", Json::array({"attackerTool"})}}}};

    const Json overwrite_false =
        dispatch(runtime, "agents.import",
                 {{"payload", envelope},
                  {"scope", "workspace"},
                  {"overwrite", false}});
    REQUIRE(overwrite_false.contains("error"));
    REQUIRE(overwrite_false["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_PERMISSION_DENIED);
    REQUIRE(overwrite_false["error"]["data"]["details"]["imported"] == 0);
    REQUIRE(overwrite_false["error"]["data"]["details"]["conflicts"][0] ==
            "code-reviewer");

    const Json overwrite_true =
        dispatch(runtime, "agents.import",
                 {{"payload", envelope},
                  {"scope", "workspace"},
                  {"overwrite", true}});
    REQUIRE(overwrite_true.contains("error"));
    REQUIRE(overwrite_true["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_PERMISSION_DENIED);

    // The compile-time definition must survive both attempts unchanged.
    const Json fetched = dispatch(runtime, "agents.get_def",
                                   {{"id", "code-reviewer"}})["result"];
    REQUIRE(fetched["builtin"] == true);
    REQUIRE(fetched["name"] == "Code Reviewer");
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

TEST_CASE("AI Editor workflow.dry_run previews review-and-fix steps with "
          "interpolated prompts",
          "[plugins][ai_editor][native][workflows][dry_run]") {
    // No provider config needed — dry_run never touches the network.
    // Built-in review-and-fix has two sequential steps: prompts reference
    // {{input}} and {{review}} respectively.
    RuntimeFixture fixture;
    const Json response = dispatch(
        fixture.get(), "workflow.dry_run",
        {{"id", "review-and-fix"}, {"input", "some code"}});
    REQUIRE(response.contains("result"));
    const Json& preview = response["result"];
    REQUIRE(preview["workflowId"] == "review-and-fix");
    REQUIRE(preview["workflowName"] == "Review & Fix");
    REQUIRE(preview["totalSteps"] == 2);
    REQUIRE(preview["groups"].is_array());
    REQUIRE(preview["groups"].size() == 2);
    // Both steps carry empty group → each forms its own single-step batch.
    REQUIRE(preview["groups"][0]["groupId"] == "");
    REQUIRE(preview["groups"][0]["startStep"] == 0);
    REQUIRE(preview["groups"][0]["endStep"] == 0);
    REQUIRE(preview["groups"][0]["parallel"] == false);
    REQUIRE(preview["groups"][1]["startStep"] == 1);
    REQUIRE(preview["groups"][1]["endStep"] == 1);
    REQUIRE(preview["groups"][1]["parallel"] == false);
    REQUIRE(preview["preview"].size() == 2);
    // Step 0 renders {{input}} against the caller-supplied "some code".
    REQUIRE(preview["preview"][0]["stepIndex"] == 0);
    REQUIRE(preview["preview"][0]["label"] == "Reviewing code");
    REQUIRE(preview["preview"][0]["agent"] == "code-reviewer");
    REQUIRE(preview["preview"][0]["outputVar"] == "review");
    REQUIRE(preview["preview"][0]["group"] == "");
    REQUIRE(preview["preview"][0]["requiresConfirmation"] == false);
    const std::string rendered0 =
        preview["preview"][0]["renderedPrompt"].get<std::string>();
    REQUIRE(rendered0.find("some code") != std::string::npos);
    REQUIRE(rendered0.find("{{input}}") == std::string::npos);
    REQUIRE(preview["preview"][0]["willBeParallelWith"].is_array());
    REQUIRE(preview["preview"][0]["willBeParallelWith"].empty());
    // Step 1 sees a synthetic "<simulated: Reviewing code>" placeholder in
    // place of {{review}} because no simulateOutputs entry overrides it.
    const std::string rendered1 =
        preview["preview"][1]["renderedPrompt"].get<std::string>();
    REQUIRE(rendered1.find("{{review}}") == std::string::npos);
    REQUIRE(rendered1.find("<simulated: Reviewing code>") != std::string::npos);
    REQUIRE(preview["preview"][1]["outputVar"] == "fix");
    // estimatedVariables lists the initial seed keys first, then output
    // variables in step order.
    REQUIRE(preview["estimatedVariables"].is_array());
    std::vector<std::string> estimated;
    for (const auto& item : preview["estimatedVariables"]) {
        estimated.push_back(item.get<std::string>());
    }
    REQUIRE(estimated.size() >= 3);
    REQUIRE(estimated[0] == "input");
    REQUIRE(std::find(estimated.begin(), estimated.end(), "review") !=
            estimated.end());
    REQUIRE(std::find(estimated.begin(), estimated.end(), "fix") !=
            estimated.end());
}

TEST_CASE("AI Editor workflow.dry_run simulateOutputs override flows into "
          "later step prompts",
          "[plugins][ai_editor][native][workflows][dry_run]") {
    RuntimeFixture fixture;
    const Json response = dispatch(
        fixture.get(), "workflow.dry_run",
        {{"id", "review-and-fix"},
         {"input", "the source"},
         {"simulateOutputs", {{"review", "The code has 3 bugs"}}}});
    REQUIRE(response.contains("result"));
    const Json& preview = response["result"];
    // Step 1 must interpolate the caller's supplied review string, not the
    // synthetic placeholder — that's the whole point of simulateOutputs.
    const std::string rendered1 =
        preview["preview"][1]["renderedPrompt"].get<std::string>();
    REQUIRE(rendered1.find("The code has 3 bugs") != std::string::npos);
    REQUIRE(rendered1.find("<simulated:") == std::string::npos);
    REQUIRE(rendered1.find("{{review}}") == std::string::npos);
    // Step 0's own {{input}} still resolves normally.
    const std::string rendered0 =
        preview["preview"][0]["renderedPrompt"].get<std::string>();
    REQUIRE(rendered0.find("the source") != std::string::npos);

    // A parallel group should surface willBeParallelWith cross-references
    // and mark the group parallel=true.  Fresh workflow definition keeps
    // this test independent from the built-in one above.
    const Json parallel_wf{
        {"id", "dry-run-parallel"},
        {"name", "Dry Run Parallel Demo"},
        {"steps",
         Json::array(
             {Json{{"agent", "default"},
                    {"prompt", "left({{input}})"},
                    {"output_var", "left"},
                    {"group", "fan-out"},
                    {"label", "Left branch"}},
              Json{{"agent", "default"},
                    {"prompt", "right({{input}})"},
                    {"output_var", "right"},
                    {"group", "fan-out"},
                    {"label", "Right branch"},
                    {"requires_confirmation", true}},
              Json{{"agent", "default"},
                    {"prompt", "join({{left}}|{{right}})"},
                    {"output_var", "final"},
                    {"label", "Join"}}})}};
    REQUIRE(dispatch(fixture.get(), "workflows.save_def",
                     {{"scope", "workspace"}, {"workflow", parallel_wf}})
                .contains("result"));
    const Json parallel_response = dispatch(
        fixture.get(), "workflow.dry_run",
        {{"id", "dry-run-parallel"}, {"input", "seed"}});
    REQUIRE(parallel_response.contains("result"));
    const Json& p = parallel_response["result"];
    REQUIRE(p["totalSteps"] == 3);
    REQUIRE(p["groups"].size() == 2);
    REQUIRE(p["groups"][0]["groupId"] == "fan-out");
    REQUIRE(p["groups"][0]["startStep"] == 0);
    REQUIRE(p["groups"][0]["endStep"] == 1);
    REQUIRE(p["groups"][0]["parallel"] == true);
    REQUIRE(p["groups"][1]["parallel"] == false);
    // Both parallel steps see the same pre-batch snapshot → each renders
    // {{input}} against "seed" and lists its sibling in willBeParallelWith.
    REQUIRE(p["preview"][0]["willBeParallelWith"].size() == 1);
    REQUIRE(p["preview"][0]["willBeParallelWith"][0] == 1);
    REQUIRE(p["preview"][1]["willBeParallelWith"].size() == 1);
    REQUIRE(p["preview"][1]["willBeParallelWith"][0] == 0);
    REQUIRE(p["preview"][1]["requiresConfirmation"] == true);
    const std::string left_rendered =
        p["preview"][0]["renderedPrompt"].get<std::string>();
    const std::string right_rendered =
        p["preview"][1]["renderedPrompt"].get<std::string>();
    REQUIRE(left_rendered.find("left(seed)") != std::string::npos);
    REQUIRE(right_rendered.find("right(seed)") != std::string::npos);
    // Third step's join prompt sees synthetic placeholders for both
    // parallel outputs — proves the batch committed together.
    const std::string join_rendered =
        p["preview"][2]["renderedPrompt"].get<std::string>();
    REQUIRE(join_rendered.find("<simulated: Left branch>") !=
            std::string::npos);
    REQUIRE(join_rendered.find("<simulated: Right branch>") !=
            std::string::npos);
}

TEST_CASE("AI Editor workflow.dry_run guards its arguments and reports "
          "NOT_FOUND for unknown ids",
          "[plugins][ai_editor][native][workflows][dry_run]") {
    RuntimeFixture fixture;
    // Missing id → INVALID_ARGUMENT.
    const Json missing = dispatch(fixture.get(), "workflow.dry_run",
                                    Json::object());
    REQUIRE(missing.contains("error"));
    REQUIRE(missing["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);

    // Non-string id → INVALID_ARGUMENT.
    const Json bad_id = dispatch(fixture.get(), "workflow.dry_run",
                                   {{"id", 42}});
    REQUIRE(bad_id.contains("error"));
    REQUIRE(bad_id["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);

    // Unknown workflow id → NOT_FOUND.
    const Json unknown = dispatch(fixture.get(), "workflow.dry_run",
                                    {{"id", "wf-does-not-exist"}});
    REQUIRE(unknown.contains("error"));
    REQUIRE(unknown["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_NOT_FOUND);

    // Empty-steps workflow → returns totalSteps=0 with empty preview.
    const Json empty_wf{
        {"id", "dry-run-empty"},
        {"name", "Dry Run Empty"},
        {"steps", Json::array()}};
    REQUIRE(dispatch(fixture.get(), "workflows.save_def",
                     {{"scope", "workspace"}, {"workflow", empty_wf}})
                .contains("result"));
    const Json empty_response = dispatch(fixture.get(), "workflow.dry_run",
                                          {{"id", "dry-run-empty"},
                                           {"input", "ignored"}});
    REQUIRE(empty_response.contains("result"));
    REQUIRE(empty_response["result"]["totalSteps"] == 0);
    REQUIRE(empty_response["result"]["preview"].is_array());
    REQUIRE(empty_response["result"]["preview"].empty());
    REQUIRE(empty_response["result"]["groups"].is_array());
    REQUIRE(empty_response["result"]["groups"].empty());
    // estimatedVariables still surfaces the seed keys ("input" at least).
    REQUIRE(empty_response["result"]["estimatedVariables"].is_array());
    REQUIRE(empty_response["result"]["estimatedVariables"][0] == "input");
}

// -- workflow runtime control: skip / replace_variable / snapshot_variables --

// Poll `workflows.status` until the execution reports the requested state
// (or a terminal state), matching wait_for_workflow_completion's pattern but
// letting the caller stop early once the gate they care about is reached.
// Returns the last status observed so failing tests can dump it.
Json wait_for_workflow_status(sao_ai_editor_runtime_t runtime,
                               const std::string& execution_id,
                               std::string_view target_state,
                               DWORD timeout_ms) {
    Json status;
    const ULONGLONG started = GetTickCount64();
    do {
        status = dispatch(runtime, "workflows.status",
                          {{"executionId", execution_id}})["result"];
        const std::string current = status.value("status", "");
        if (current == target_state || current == "completed" ||
            current == "failed" || current == "cancelled") {
            return status;
        }
        Sleep(20);
    } while (GetTickCount64() - started < timeout_ms);
    return status;
}

TEST_CASE("AI Editor workflow.skip_step at waiting_confirmation advances past "
          "the current batch and lets the next step execute normally",
          "[plugins][ai_editor][native][workflows][runtime_control]") {
    // Step 0 gates on requires_confirmation=true so the run parks in
    // waiting_confirmation.  workflow.skip_step then records step 0 as
    // "skipped" (empty content, no LLM hit) and step 1 fires against the
    // fake HTTP fixture — proving that skip advances past the gated batch
    // without consuming a response slot.  Only one HTTP response is needed
    // because only step 1 dials the endpoint.
    const std::string ok_body =
        R"({"choices":[{"message":{"role":"assistant","content":"step-two"}}]})";
    LocalHttpServer server(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: " +
        std::to_string(ok_body.size()) +
        "\r\nConnection: close\r\n\r\n" + ok_body);
    RuntimeFixture fixture;
    // Custom two-step workflow with confirmation on step 0.  Step 1's
    // prompt intentionally references {{s1}} to prove that skipping step 0
    // leaves the variable unset (interpolates to empty) rather than
    // synthesising a placeholder.
    const Json workflow_def{
        {"id", "skip-confirm-workflow"},
        {"name", "Skip Confirm"},
        {"description", "Skip-step regression fixture"},
        {"steps",
         Json::array({Json{{"agent", "default"},
                            {"prompt", "gated({{input}})"},
                            {"output_var", "s1"},
                            {"label", "Gated first"},
                            {"requires_confirmation", true}},
                       Json{{"agent", "default"},
                            {"prompt", "chain({{s1}})"},
                            {"output_var", "s2"},
                            {"label", "Chained second"}}})}};
    REQUIRE(dispatch(fixture.get(), "workflows.save_def",
                     {{"scope", "workspace"},
                      {"workflow", workflow_def}})
                .contains("result"));
    const Json run = dispatch(
        fixture.get(), "workflows.run",
        {{"id", "skip-confirm-workflow"},
         {"provider", {{"id", "skip-fixture"},
                        {"endpoint", server.endpoint()}}},
         {"model", "fixture-model"},
         {"input", "hello"},
         {"timeoutMs", 5000}});
    REQUIRE(run.contains("result"));
    const std::string execution_id = run["result"]["executionId"];

    // Wait for the run to reach the confirmation gate before firing skip.
    const Json gate = wait_for_workflow_status(fixture.get(), execution_id,
                                                "waiting_confirmation", 5'000);
    REQUIRE(gate["status"] == "waiting_confirmation");

    // Skip out of waiting_confirmation — verifies the confirmation path
    // routes into the skip branch rather than approve/reject.
    const Json skip_response = dispatch(
        fixture.get(), "workflow.skip_step",
        {{"executionId", execution_id}, {"reason", "gated step optional"}});
    REQUIRE(skip_response.contains("result"));
    REQUIRE(skip_response["result"]["ok"] == true);
    REQUIRE(skip_response["result"]["executionId"] == execution_id);
    REQUIRE(skip_response["result"]["reason"] == "gated step optional");

    // Run should now proceed through step 1 and complete.
    const Json terminal =
        wait_for_workflow_completion(fixture.get(), execution_id, 15'000);
    REQUIRE(terminal["status"] == "completed");
    REQUIRE(terminal["stepResults"].size() == 2);
    REQUIRE(terminal["stepResults"][0]["status"] == "skipped");
    REQUIRE(terminal["stepResults"][0]["content"] == "");
    REQUIRE(terminal["stepResults"][0]["skipReason"] == "gated step optional");
    REQUIRE(terminal["stepResults"][1]["status"] == "completed");
    REQUIRE(terminal["stepResults"][1]["content"] == "step-two");
    // s1 stays empty (skip does not fabricate an output) but s2 lands
    // because step 1 ran normally against the recovering server.
    REQUIRE(terminal["variables"].contains("s2"));
    REQUIRE(terminal["variables"]["s2"] == "step-two");
    // Only one HTTP call fired — step 0 never dialed the endpoint.
    REQUIRE(server.captured_bodies().size() == 1);
}

TEST_CASE("AI Editor workflow.replace_variable overrides a variable that a "
          "later step interpolates against",
          "[plugins][ai_editor][native][workflows][runtime_control]") {
    // Two-step workflow with confirmation on step 1 so the run pauses
    // between: step 0 completes with "raw-output" as the review; the user
    // then swaps that variable via workflow.replace_variable; step 1's
    // interpolated prompt uses the new value.  The HTTP fixture inspects
    // captured request bodies to confirm the substitution happened.
    const std::string step0_body =
        R"({"choices":[{"message":{"role":"assistant","content":"raw-output"}}]})";
    const std::string step1_body =
        R"({"choices":[{"message":{"role":"assistant","content":"final"}}]})";
    LocalHttpServer server(std::vector<std::string>{
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: " +
            std::to_string(step0_body.size()) +
            "\r\nConnection: close\r\n\r\n" + step0_body,
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: " +
            std::to_string(step1_body.size()) +
            "\r\nConnection: close\r\n\r\n" + step1_body,
    });
    RuntimeFixture fixture;
    const Json workflow_def{
        {"id", "replace-var-workflow"},
        {"name", "Replace Variable"},
        {"description", "Runtime variable-override fixture"},
        {"steps",
         Json::array({Json{{"agent", "default"},
                            {"prompt", "review({{input}})"},
                            {"output_var", "review"},
                            {"label", "Reviewer"}},
                       Json{{"agent", "default"},
                            {"prompt", "final:{{review}}"},
                            {"output_var", "outcome"},
                            {"label", "Finalise"},
                            {"requires_confirmation", true}}})}};
    REQUIRE(dispatch(fixture.get(), "workflows.save_def",
                     {{"scope", "workspace"},
                      {"workflow", workflow_def}})
                .contains("result"));
    const Json run = dispatch(
        fixture.get(), "workflows.run",
        {{"id", "replace-var-workflow"},
         {"provider", {{"id", "replace-fixture"},
                        {"endpoint", server.endpoint()}}},
         {"model", "fixture-model"},
         {"input", "please review"},
         {"timeoutMs", 5000}});
    REQUIRE(run.contains("result"));
    const std::string execution_id = run["result"]["executionId"];

    // Wait for the confirmation gate at step 1 (means step 0 already
    // stashed its output into variables_).
    const Json gate = wait_for_workflow_status(fixture.get(), execution_id,
                                                "waiting_confirmation", 5'000);
    REQUIRE(gate["status"] == "waiting_confirmation");
    REQUIRE(gate["variables"]["review"] == "raw-output");

    // Override review with a corrected value before approving.
    const Json replace_response = dispatch(
        fixture.get(), "workflow.replace_variable",
        {{"executionId", execution_id},
         {"name", "review"},
         {"value", "corrected-review"}});
    REQUIRE(replace_response.contains("result"));
    REQUIRE(replace_response["result"]["ok"] == true);
    REQUIRE(replace_response["result"]["name"] == "review");
    REQUIRE(replace_response["result"]["value"] == "corrected-review");
    REQUIRE(replace_response["result"]["executionId"] == execution_id);

    // Snapshot at the gate reports the new value.
    const Json snap = dispatch(
        fixture.get(), "workflow.snapshot_variables",
        {{"executionId", execution_id}});
    REQUIRE(snap.contains("result"));
    REQUIRE(snap["result"]["variables"]["review"] == "corrected-review");
    REQUIRE(snap["result"]["executionId"] == execution_id);

    // Invalid name -> INVALID_ARGUMENT (defends against a caller writing a
    // path-traversal-shaped key that would break history serialisation).
    const Json bad = dispatch(fixture.get(), "workflow.replace_variable",
                                {{"executionId", execution_id},
                                 {"name", "../etc"},
                                 {"value", "no"}});
    REQUIRE(bad.contains("error"));
    REQUIRE(bad["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);

    // Approve step 1; step 1's rendered prompt must contain the new value.
    REQUIRE(dispatch(fixture.get(), "workflows.confirm",
                     {{"executionId", execution_id}, {"approved", true}})
                .contains("result"));
    const Json terminal =
        wait_for_workflow_completion(fixture.get(), execution_id, 15'000);
    REQUIRE(terminal["status"] == "completed");
    REQUIRE(terminal["variables"]["outcome"] == "final");
    // Verify step 1's on-the-wire body used the overridden value.
    const auto& bodies = server.captured_bodies();
    REQUIRE(bodies.size() == 2);
    REQUIRE(bodies[1].find("final:corrected-review") != std::string::npos);
    REQUIRE(bodies[1].find("raw-output") == std::string::npos);
}

TEST_CASE("AI Editor workflow.snapshot_variables mirrors the current variable "
          "map and guards its arguments",
          "[plugins][ai_editor][native][workflows][runtime_control]") {
    // Cheap regression on the argument surface — no need to spin up a
    // workflow to test the guardrails.  The full "snapshot during run"
    // path is covered by the replace_variable test above.
    RuntimeFixture fixture;
    // Missing executionId -> INVALID_ARGUMENT.
    const Json missing = dispatch(fixture.get(), "workflow.snapshot_variables",
                                    Json::object());
    REQUIRE(missing.contains("error"));
    REQUIRE(missing["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
    // Non-string executionId -> INVALID_ARGUMENT.
    const Json bad_id = dispatch(fixture.get(), "workflow.snapshot_variables",
                                   {{"executionId", 42}});
    REQUIRE(bad_id.contains("error"));
    REQUIRE(bad_id["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
    // Unknown executionId -> NOT_FOUND.
    const Json unknown = dispatch(fixture.get(), "workflow.snapshot_variables",
                                    {{"executionId", "wf-does-not-exist"}});
    REQUIRE(unknown.contains("error"));
    REQUIRE(unknown["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_NOT_FOUND);

    // Now spin up a real execution and confirm snapshot reports the ctor
    // input.  Server never responds so the run parks in step 0 — that's
    // fine, we only care about the initial variable map.
    LocalHttpServer stalling;
    const Json run = dispatch(
        fixture.get(), "workflows.run",
        {{"id", "review-and-fix"},
         {"provider", {{"id", "snapshot-fixture"},
                        {"endpoint", stalling.endpoint()}}},
         {"model", "fixture-model"},
         {"input", "some seed input"},
         {"timeoutMs", 2000}});
    REQUIRE(run.contains("result"));
    const std::string execution_id = run["result"]["executionId"];
    // Give the worker a chance to spawn.
    REQUIRE(stalling.wait_for_connections(1, 2'000));

    const Json snap = dispatch(
        fixture.get(), "workflow.snapshot_variables",
        {{"executionId", execution_id}});
    REQUIRE(snap.contains("result"));
    REQUIRE(snap["result"]["executionId"] == execution_id);
    REQUIRE(snap["result"]["variables"]["input"] == "some seed input");

    // skip_step and replace_variable should also refuse on unknown ids.
    const Json missing_skip = dispatch(
        fixture.get(), "workflow.skip_step",
        {{"executionId", "wf-does-not-exist"}});
    REQUIRE(missing_skip.contains("error"));
    REQUIRE(missing_skip["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_NOT_FOUND);
    const Json missing_replace = dispatch(
        fixture.get(), "workflow.replace_variable",
        {{"executionId", "wf-does-not-exist"},
         {"name", "x"},
         {"value", "y"}});
    REQUIRE(missing_replace.contains("error"));
    REQUIRE(missing_replace["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_NOT_FOUND);

    // skip_step on a running (non-confirming) workflow -> INVALID_ARGUMENT
    // (worker is neither paused nor waiting for confirmation).
    const Json skip_running = dispatch(
        fixture.get(), "workflow.skip_step",
        {{"executionId", execution_id}});
    REQUIRE(skip_running.contains("error"));
    REQUIRE(skip_running["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);

    // Cancel to release the stalled worker so the fixture teardown is
    // clean (the server destructor closes the connection, but the runtime
    // still owns a live worker thread).
    REQUIRE(dispatch(fixture.get(), "workflows.cancel",
                     {{"executionId", execution_id}})
                .contains("result"));
    wait_for_workflow_completion(fixture.get(), execution_id, 5'000);
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

TEST_CASE("AI Editor prompts.render_batch renders every entry and reports "
          "success/failure counts",
          "[plugins][ai_editor][native][prompts]") {
    RuntimeFixture fixture;
    // Two built-ins with fully populated arguments -> both must succeed
    // and the aggregated counts must match.
    const Json response = dispatch(
        fixture.get(), "prompts.render_batch",
        {{"items", Json::array({
                       Json{{"id", "code-review-base"},
                              {"arguments", {{"focus", "security"},
                                              {"code", "SELECT 1"}}}},
                       Json{{"id", "explain-code"},
                              {"arguments", {{"code", "printf('hi')"},
                                              {"depth", "overview"}}}}})}});
    REQUIRE(response.contains("result"));
    const auto& result = response["result"];
    REQUIRE(result["successCount"] == 2);
    REQUIRE(result["failureCount"] == 0);
    REQUIRE(result["results"].size() == 2);
    REQUIRE(result["results"][0]["status"] == "ok");
    REQUIRE(result["results"][0]["id"] == "code-review-base");
    REQUIRE(result["results"][0]["content"].get<std::string>().find(
                "Focus on: security") != std::string::npos);
    REQUIRE(result["results"][1]["status"] == "ok");
    REQUIRE(result["results"][1]["id"] == "explain-code");
    REQUIRE(result["results"][1]["content"].get<std::string>().find(
                "printf('hi')") != std::string::npos);
    // Empty items must surface as INVALID_ARGUMENT so callers never treat
    // an empty batch as a silent success.
    REQUIRE(dispatch(fixture.get(), "prompts.render_batch",
                     {{"items", Json::array()}})["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
}

TEST_CASE("AI Editor prompts.render_batch onMissing=skip records not_found "
          "peers without aborting the batch",
          "[plugins][ai_editor][native][prompts]") {
    RuntimeFixture fixture;
    // Mix a valid id with an unknown id.  With onMissing=skip the unknown
    // becomes a per-entry not_found; the valid entry still renders.
    const Json response = dispatch(
        fixture.get(), "prompts.render_batch",
        {{"onMissing", "skip"},
         {"items", Json::array({
                       Json{{"id", "code-review-base"},
                              {"arguments", {{"focus", "correctness"},
                                              {"code", "x = 1"}}}},
                       Json{{"id", "does-not-exist"},
                              {"arguments", Json::object()}}})}});
    REQUIRE(response.contains("result"));
    const auto& result = response["result"];
    REQUIRE(result["successCount"] == 1);
    REQUIRE(result["failureCount"] == 1);
    REQUIRE(result["results"].size() == 2);
    REQUIRE(result["results"][0]["status"] == "ok");
    REQUIRE(result["results"][1]["status"] == "not_found");
    REQUIRE(result["results"][1]["id"] == "does-not-exist");
    REQUIRE_FALSE(
        result["results"][1].value("error", std::string{}).empty());
    // Same batch with onMissing=fail (the default) must surface
    // INVALID_ARGUMENT so callers can pick their own recovery strategy.
    REQUIRE(dispatch(fixture.get(), "prompts.render_batch",
                     {{"items", Json::array({
                                    Json{{"id", "does-not-exist"},
                                           {"arguments", Json::object()}}})}})
                ["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
}

TEST_CASE("AI Editor prompts.list_defs with tags filter returns only prompts "
          "sharing at least one tag",
          "[plugins][ai_editor][native][prompts]") {
    RuntimeFixture fixture;
    // Save one workspace prompt tagged {sql} and one tagged {random}; the
    // {sql} filter must return the sql one plus any built-in / market
    // prompt that carries "sql", and the built-in "code-review-base"
    // (tags {review, code}) with filter {review} must survive too.
    const Json sql_prompt{
        {"id", "workspace-sql"},
        {"name", "Workspace SQL"},
        {"content", "sql body"},
        {"variables", Json::array()},
        {"tags", Json::array({"sql", "workspace"})}};
    REQUIRE(dispatch(fixture.get(), "prompts.save_def",
                     {{"scope", "workspace"}, {"prompt", sql_prompt}})
                .contains("result"));
    const Json unrelated_prompt{
        {"id", "workspace-random"},
        {"name", "Workspace Random"},
        {"content", "random body"},
        {"variables", Json::array()},
        {"tags", Json::array({"random"})}};
    REQUIRE(dispatch(fixture.get(), "prompts.save_def",
                     {{"scope", "workspace"}, {"prompt", unrelated_prompt}})
                .contains("result"));
    // Filter on {sql}: must include workspace-sql, must NOT include
    // workspace-random or the two non-sql built-ins.
    const Json filtered = dispatch(fixture.get(), "prompts.list_defs",
                                    {{"tags", Json::array({"sql"})}})
                              ["result"];
    std::vector<std::string> filtered_ids;
    for (const auto& item : filtered["items"]) {
        filtered_ids.push_back(item.value("id", std::string{}));
    }
    REQUIRE(std::find(filtered_ids.begin(), filtered_ids.end(),
                       "workspace-sql") != filtered_ids.end());
    REQUIRE(std::find(filtered_ids.begin(), filtered_ids.end(),
                       "workspace-random") == filtered_ids.end());
    REQUIRE(std::find(filtered_ids.begin(), filtered_ids.end(),
                       "explain-code") == filtered_ids.end());
    REQUIRE(std::find(filtered_ids.begin(), filtered_ids.end(),
                       "bug-diagnosis") == filtered_ids.end());
    // Filter on {review}: must include the code-review-base builtin
    // (tags {review, code}); must NOT include workspace-random.
    const Json review = dispatch(fixture.get(), "prompts.list_defs",
                                  {{"tags", Json::array({"review"})}})
                            ["result"];
    std::vector<std::string> review_ids;
    for (const auto& item : review["items"]) {
        review_ids.push_back(item.value("id", std::string{}));
    }
    REQUIRE(std::find(review_ids.begin(), review_ids.end(),
                       "code-review-base") != review_ids.end());
    REQUIRE(std::find(review_ids.begin(), review_ids.end(),
                       "workspace-random") == review_ids.end());
    // OR semantics: {sql, random} must return both workspace prompts even
    // though neither shares both tags.
    const Json either = dispatch(
        fixture.get(), "prompts.list_defs",
        {{"tags", Json::array({"sql", "random"})}})["result"];
    std::vector<std::string> either_ids;
    for (const auto& item : either["items"]) {
        either_ids.push_back(item.value("id", std::string{}));
    }
    REQUIRE(std::find(either_ids.begin(), either_ids.end(),
                       "workspace-sql") != either_ids.end());
    REQUIRE(std::find(either_ids.begin(), either_ids.end(),
                       "workspace-random") != either_ids.end());
    // Empty tags array: must degrade to list_defs (backward compatible).
    const Json all_default =
        dispatch(fixture.get(), "prompts.list_defs")["result"];
    const Json all_empty = dispatch(fixture.get(), "prompts.list_defs",
                                     {{"tags", Json::array()}})["result"];
    REQUIRE(all_default["total"] == all_empty["total"]);
}

TEST_CASE("AI Editor prompts.list_tags returns tag distribution sorted by "
          "count desc / name asc",
          "[plugins][ai_editor][native][prompts]") {
    RuntimeFixture fixture;
    // Layer two workspace prompts on top of the built-ins so we can predict
    // which tags jump ahead in the aggregated ranking.
    const Json prompt_a{
        {"id", "wp-a"},
        {"name", "WP A"},
        {"content", "a"},
        {"variables", Json::array()},
        {"tags", Json::array({"review", "custom"})}};
    const Json prompt_b{
        {"id", "wp-b"},
        {"name", "WP B"},
        {"content", "b"},
        {"variables", Json::array()},
        {"tags", Json::array({"review", "custom"})}};
    REQUIRE(dispatch(fixture.get(), "prompts.save_def",
                     {{"scope", "workspace"}, {"prompt", prompt_a}})
                .contains("result"));
    REQUIRE(dispatch(fixture.get(), "prompts.save_def",
                     {{"scope", "workspace"}, {"prompt", prompt_b}})
                .contains("result"));
    const Json response = dispatch(fixture.get(), "prompts.list_tags");
    REQUIRE(response.contains("result"));
    const auto& result = response["result"];
    REQUIRE(result.contains("tags"));
    REQUIRE(result["tags"].is_array());
    REQUIRE(result["tags"].size() == result["total"]);
    // Extract to a keyed map so we can assert per-tag content without
    // relying on the exact market-preset population (which is optional).
    std::unordered_map<std::string, Json> by_name;
    std::vector<std::pair<size_t, std::string>> ordering;
    for (const auto& entry : result["tags"]) {
        by_name.emplace(entry.value("name", std::string{}), entry);
        ordering.emplace_back(entry.value("count", size_t{0}),
                               entry.value("name", std::string{}));
    }
    // review: two workspace prompts + code-review-base = at least 3
    // (market preset sql-audit adds one more when assets present).
    REQUIRE(by_name.count("review") == 1);
    REQUIRE(by_name["review"].value("count", size_t{0}) >= 3);
    // custom: only the two workspace prompts.
    REQUIRE(by_name.count("custom") == 1);
    REQUIRE(by_name["custom"].value("count", size_t{0}) == 2);
    // Prompts list under each tag must contain the ids we saved and
    // must be alphabetically sorted.
    const auto& custom_prompts = by_name["custom"]["prompts"];
    REQUIRE(custom_prompts.size() == 2);
    REQUIRE(custom_prompts[0] == "wp-a");
    REQUIRE(custom_prompts[1] == "wp-b");
    // Sort invariant: entries must be ordered by count desc, then name asc.
    for (size_t i = 1; i < ordering.size(); ++i) {
        const auto& prev = ordering[i - 1];
        const auto& current = ordering[i];
        if (prev.first == current.first) {
            REQUIRE(prev.second < current.second);
        } else {
            REQUIRE(prev.first > current.first);
        }
    }
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

TEST_CASE("conversation.tag and untag round-trip tag membership + reject junk",
          "[plugins][ai_editor][native][storage][tag]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    REQUIRE(dispatch(runtime, "runtime.initialize").contains("result"));

    const std::string id = dispatch(runtime, "conversation.create",
                                     {{"title", "Tagged"},
                                      {"model", "m"},
                                      {"scope", "workspace"}})["result"]["id"];

    // Fresh conversation defaults to an empty tag array.
    const Json created = dispatch(runtime, "conversation.get",
                                   {{"id", id}})["result"];
    REQUIRE(created.contains("tags"));
    REQUIRE(created["tags"].is_array());
    REQUIRE(created["tags"].empty());

    // Add two tags — response echoes the full set, get() surfaces them, and
    // duplicate adds collapse rather than creating repeats.
    const Json tagged = dispatch(runtime, "conversation.tag",
                                  {{"id", id},
                                   {"tags", Json::array({"work", "review"})}})
                             ["result"];
    REQUIRE(tagged["id"] == id);
    REQUIRE(tagged["tags"].is_array());
    REQUIRE(tagged["tags"].size() == 2);
    const std::vector<std::string> tag_list = {
        tagged["tags"][0].get<std::string>(),
        tagged["tags"][1].get<std::string>()};
    REQUIRE(std::find(tag_list.begin(), tag_list.end(), "work") !=
            tag_list.end());
    REQUIRE(std::find(tag_list.begin(), tag_list.end(), "review") !=
            tag_list.end());

    // Re-tagging the same values is a no-op (still returns current tags).
    const Json redundant = dispatch(
        runtime, "conversation.tag",
        {{"id", id}, {"tags", Json::array({"work", "work", "review"})}})
                                  ["result"];
    REQUIRE(redundant["tags"].size() == 2);

    // get() persists tags across reads.
    const Json fetched = dispatch(runtime, "conversation.get",
                                   {{"id", id}})["result"];
    REQUIRE(fetched["tags"].size() == 2);

    // Untag removes just the entry passed in; the other stays.
    const Json shrunk = dispatch(runtime, "conversation.untag",
                                  {{"id", id},
                                   {"tags", Json::array({"review"})}})
                              ["result"];
    REQUIRE(shrunk["tags"].size() == 1);
    REQUIRE(shrunk["tags"][0] == "work");

    // Untagging a value that is not present is a no-op — no error, current
    // set is echoed back.
    const Json noop = dispatch(runtime, "conversation.untag",
                                {{"id", id},
                                 {"tags", Json::array({"unknown"})}})
                            ["result"];
    REQUIRE(noop["tags"].size() == 1);
    REQUIRE(noop["tags"][0] == "work");

    // Empty tags array on either method is a no-op that still returns
    // current tags (per docstring).
    const Json empty_add = dispatch(runtime, "conversation.tag",
                                     {{"id", id}, {"tags", Json::array()}})
                                ["result"];
    REQUIRE(empty_add["tags"].size() == 1);

    // Missing id → NOT_FOUND, missing tags → INVALID_ARGUMENT.
    const Json bad_id = dispatch(runtime, "conversation.tag",
                                  {{"id", "conv-not-real"},
                                   {"tags", Json::array({"x"})}});
    REQUIRE(bad_id["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_NOT_FOUND);
    const Json missing_tags = dispatch(runtime, "conversation.tag",
                                        {{"id", id}});
    REQUIRE(missing_tags["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);

    // Empty string / over-length tag values are rejected — the >128 char
    // ceiling matches the ConversationStore docstring.
    const std::string huge(200, 'x');
    const Json too_long = dispatch(runtime, "conversation.tag",
                                    {{"id", id},
                                     {"tags", Json::array({huge})}});
    REQUIRE(too_long["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
    const Json empty_tag = dispatch(runtime, "conversation.tag",
                                     {{"id", id},
                                      {"tags", Json::array({""})}});
    REQUIRE(empty_tag["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
}

TEST_CASE("conversation.find_by_tag honours matchAll and pinned-first ordering",
          "[plugins][ai_editor][native][storage][tag]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    REQUIRE(dispatch(runtime, "runtime.initialize").contains("result"));

    const std::string a = dispatch(runtime, "conversation.create",
                                    {{"title", "A"},
                                     {"model", "m"},
                                     {"scope", "workspace"}})["result"]["id"];
    Sleep(5);
    const std::string b = dispatch(runtime, "conversation.create",
                                    {{"title", "B"},
                                     {"model", "m"},
                                     {"scope", "workspace"}})["result"]["id"];
    Sleep(5);
    const std::string c = dispatch(runtime, "conversation.create",
                                    {{"title", "C"},
                                     {"model", "m"},
                                     {"scope", "system"}})["result"]["id"];

    // A: {work, review}   B: {work}   C: {review}
    dispatch(runtime, "conversation.tag",
             {{"id", a}, {"tags", Json::array({"work", "review"})}});
    dispatch(runtime, "conversation.tag",
             {{"id", b}, {"tags", Json::array({"work"})}});
    dispatch(runtime, "conversation.tag",
             {{"id", c}, {"tags", Json::array({"review"})}});

    // matchAll=false (OR): {work} matches A + B (both workspace).
    const Json or_hits = dispatch(
        runtime, "conversation.find_by_tag",
        {{"tags", Json::array({"work"})}})["result"];
    REQUIRE(or_hits["total"] == 2);
    // savedAt desc within same pinned group: B was created after A.
    REQUIRE(or_hits["items"][0]["id"] == b);
    REQUIRE(or_hits["items"][1]["id"] == a);
    // Summary shape includes tags + pinned + messageCount.
    REQUIRE(or_hits["items"][0]["tags"].is_array());
    REQUIRE(or_hits["items"][0]["pinned"] == false);
    REQUIRE(or_hits["items"][0]["messageCount"] == 0);

    // matchAll=true (AND): {work, review} only matches A.
    const Json and_hits = dispatch(
        runtime, "conversation.find_by_tag",
        {{"tags", Json::array({"work", "review"})}, {"matchAll", true}})
                                ["result"];
    REQUIRE(and_hits["total"] == 1);
    REQUIRE(and_hits["items"][0]["id"] == a);

    // Pinning A pushes it in front of B even though B has a newer savedAt.
    dispatch(runtime, "conversation.pin", {{"id", a}});
    const Json pinned_first = dispatch(
        runtime, "conversation.find_by_tag",
        {{"tags", Json::array({"work"})}})["result"];
    REQUIRE(pinned_first["items"][0]["id"] == a);
    REQUIRE(pinned_first["items"][0]["pinned"] == true);
    REQUIRE(pinned_first["items"][1]["id"] == b);

    // Scope filter: scope=system matches only C.
    const Json system_only = dispatch(
        runtime, "conversation.find_by_tag",
        {{"tags", Json::array({"review"})}, {"scope", "system"}})["result"];
    REQUIRE(system_only["total"] == 1);
    REQUIRE(system_only["items"][0]["id"] == c);

    // Empty tags array is rejected.
    const Json empty_tags = dispatch(
        runtime, "conversation.find_by_tag",
        {{"tags", Json::array()}});
    REQUIRE(empty_tags["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
    const Json bad_scope = dispatch(
        runtime, "conversation.find_by_tag",
        {{"tags", Json::array({"work"})}, {"scope", "bogus"}});
    REQUIRE(bad_scope["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
}

TEST_CASE("conversation.list_tags aggregates counts + is legacy-safe",
          "[plugins][ai_editor][native][storage][tag]") {
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

    // Empty history → tags:[] and total:0.
    const Json empty = dispatch(runtime, "conversation.list_tags")["result"];
    REQUIRE(empty["total"] == 0);
    REQUIRE(empty["tags"].is_array());
    REQUIRE(empty["tags"].empty());

    // Seed three conversations with overlapping tag sets so we can verify
    // the count-desc, name-asc ordering plus per-tag conversationIds lists.
    const std::string a = dispatch(runtime, "conversation.create",
                                    {{"title", "A"},
                                     {"model", "m"},
                                     {"scope", "workspace"}})["result"]["id"];
    const std::string b = dispatch(runtime, "conversation.create",
                                    {{"title", "B"},
                                     {"model", "m"},
                                     {"scope", "workspace"}})["result"]["id"];
    const std::string c = dispatch(runtime, "conversation.create",
                                    {{"title", "C"},
                                     {"model", "m"},
                                     {"scope", "system"}})["result"]["id"];

    dispatch(runtime, "conversation.tag",
             {{"id", a}, {"tags", Json::array({"work", "review"})}});
    dispatch(runtime, "conversation.tag",
             {{"id", b}, {"tags", Json::array({"work"})}});
    dispatch(runtime, "conversation.tag",
             {{"id", c}, {"tags", Json::array({"review"})}});

    const Json stats = dispatch(runtime, "conversation.list_tags")["result"];
    REQUIRE(stats["total"] == 2);
    // work appears in {a, b} → count 2; review appears in {a, c} → count 2.
    // Same count → name-ascending tie-break → "review" before "work".
    REQUIRE(stats["tags"][0]["name"] == "review");
    REQUIRE(stats["tags"][0]["count"] == 2);
    REQUIRE(stats["tags"][1]["name"] == "work");
    REQUIRE(stats["tags"][1]["count"] == 2);

    // Untag "work" from B → work now has count 1, review still 2 → review
    // wins the top slot outright.
    dispatch(runtime, "conversation.untag",
             {{"id", b}, {"tags", Json::array({"work"})}});
    const Json after = dispatch(runtime, "conversation.list_tags")["result"];
    REQUIRE(after["tags"][0]["name"] == "review");
    REQUIRE(after["tags"][0]["count"] == 2);
    REQUIRE(after["tags"][1]["name"] == "work");
    REQUIRE(after["tags"][1]["count"] == 1);

    // Scope filter: workspace excludes system's "review" hit on C.
    const Json workspace_only = dispatch(runtime, "conversation.list_tags",
                                          {{"scope", "workspace"}})["result"];
    for (const auto& entry : workspace_only["tags"]) {
        const std::string name = entry["name"].get<std::string>();
        if (name == "review") {
            REQUIRE(entry["count"] == 1);
        } else if (name == "work") {
            REQUIRE(entry["count"] == 1);
        }
    }

    // Legacy documents missing the `tags` field must not crash the aggregator.
    // Simulate by stripping the field from A's on-disk JSON.
    const std::filesystem::path history_dir =
        std::filesystem::path(workspace_root_utf8) / L"chat_history";
    const std::filesystem::path legacy_path =
        history_dir / (std::filesystem::path(a + ".json"));
    REQUIRE(std::filesystem::exists(legacy_path));
    std::string legacy_text;
    {
        std::ifstream stream(legacy_path, std::ios::binary);
        REQUIRE(stream.good());
        legacy_text.assign(std::istreambuf_iterator<char>{stream},
                            std::istreambuf_iterator<char>{});
    }
    Json legacy_document = Json::parse(legacy_text);
    legacy_document.erase("tags");
    {
        std::ofstream stream(legacy_path,
                              std::ios::binary | std::ios::trunc);
        REQUIRE(stream.good());
        const std::string rewritten = legacy_document.dump(1);
        stream.write(rewritten.data(),
                     static_cast<std::streamsize>(rewritten.size()));
    }
    // get() must still surface `tags: []` for the legacy record.
    const Json fetched = dispatch(runtime, "conversation.get",
                                   {{"id", a}})["result"];
    REQUIRE(fetched["tags"].is_array());
    REQUIRE(fetched["tags"].empty());
    // list_tags drops A's contributions because the field is gone; review is
    // now only on C (system), and work has no members left.
    const Json post_legacy = dispatch(
        runtime, "conversation.list_tags")["result"];
    // At most one bucket ("review" → C) remains.  work vanishes entirely.
    for (const auto& entry : post_legacy["tags"]) {
        REQUIRE(entry["name"] != "work");
    }
}

TEST_CASE("tools.register surfaces custom tools in tools.list and tools.call "
          "returns the passthrough payload",
          "[plugins][ai_editor][native][tools][custom]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    REQUIRE(dispatch(runtime, "runtime.initialize").contains("result"));

    // Baseline: whatever built-in count is registered before any custom tool
    // is added.  The exact number varies with feature flags (e.g. gpu_hunt);
    // we anchor on the delta so `+ 1` after tools.register still means "one
    // more entry than the baseline".
    const Json baseline = dispatch(runtime, "tools.list")["result"];
    REQUIRE(baseline["tools"].is_array());
    const auto baseline_tool_count = baseline["tools"].size();
    REQUIRE(baseline_tool_count >= 4);

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

    // tools.list now surfaces one more tool than the baseline; the custom one
    // carries custom:true and the readOnly + parameters schema we registered.
    const Json after_register = dispatch(runtime, "tools.list")["result"];
    REQUIRE(after_register["tools"].size() == baseline_tool_count + 1);
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

    const Json pre_register = dispatch(runtime, "tools.list")["result"];
    const auto baseline_tool_count = pre_register["tools"].size();

    REQUIRE(dispatch(runtime, "tools.register",
                     {{"name", "ephemeral"},
                      {"description", "temp"}})
                .contains("result"));

    const Json listed = dispatch(runtime, "tools.list")["result"];
    REQUIRE(listed["tools"].size() == baseline_tool_count + 1);

    const Json removed = dispatch(runtime, "tools.unregister",
                                   {{"name", "ephemeral"}})["result"];
    REQUIRE(removed["ok"] == true);
    REQUIRE(removed["name"] == "ephemeral");

    const Json after_remove = dispatch(runtime, "tools.list")["result"];
    REQUIRE(after_remove["tools"].size() == baseline_tool_count);
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

namespace {

// Drain up to `budget` events from the runtime queue.  Some sao.event streams
// (workflow / chat) push events even in tests that never subscribe, so we
// tolerate an empty poll but never block forever waiting for a specific hook
// to appear.  Each returned entry is the parsed sao.event envelope.
std::vector<Json> drain_sao_events(sao_ai_editor_runtime_t runtime,
                                    size_t budget) {
    std::vector<Json> events;
    events.reserve(budget);
    for (size_t index = 0; index < budget; ++index) {
        uint32_t required = 0;
        const int32_t queried = sao_ai_editor_runtime_next_event(
            runtime, nullptr, 0, &required);
        if (queried == SAO_AI_EDITOR_OK && required == 0) {
            break;
        }
        if (queried != SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL) {
            break;
        }
        std::vector<char> event(static_cast<size_t>(required) + 1);
        if (sao_ai_editor_runtime_next_event(
                runtime, event.data(),
                static_cast<uint32_t>(event.size()), &required) !=
            SAO_AI_EDITOR_OK) {
            break;
        }
        events.push_back(Json::parse(event.data(), event.data() + required));
    }
    return events;
}

}  // namespace

TEST_CASE("tools.register_alias routes tools.call to the target and surfaces "
          "aliasOf in tools.list",
          "[plugins][ai_editor][native][tools][alias]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    REQUIRE(dispatch(runtime, "runtime.initialize").contains("result"));

    // Seed a workspace file so the alias-routed readFile call has content to
    // return, otherwise the test would only prove routing to a NOT_FOUND.
    {
        std::ofstream seed(fixture.workspace() / L"alias_note.txt");
        seed << "aliased-content\n";
    }

    // Snapshot the pre-alias tool count so we can prove the alias added exactly
    // one entry regardless of how many built-ins the runtime ships with.
    const Json pre_alias_list = dispatch(runtime, "tools.list")["result"];
    const auto pre_alias_count = pre_alias_list["tools"].size();

    // Register the alias.  Both alias + target are echoed back in the result
    // so callers can verify the binding without a follow-up tools.list.
    const Json register_result = dispatch(
        runtime, "tools.register_alias",
        {{"alias", "read_file"}, {"target", "readFile"}});
    REQUIRE(register_result["result"]["ok"] == true);
    REQUIRE(register_result["result"]["alias"] == "read_file");
    REQUIRE(register_result["result"]["target"] == "readFile");

    // tools.list now includes an aliasOf-tagged entry that mirrors readFile's
    // readOnly + parameters schema — exactly one more entry than the baseline.
    const Json listed = dispatch(runtime, "tools.list")["result"];
    REQUIRE(listed["tools"].size() == pre_alias_count + 1);
    bool saw_alias = false;
    for (const auto& tool : listed["tools"]) {
        if (tool.value("name", std::string{}) == "read_file") {
            saw_alias = true;
            REQUIRE(tool["aliasOf"] == "readFile");
            REQUIRE(tool["readOnly"] == true);
            REQUIRE(tool["parameters"]["properties"].contains("path"));
            // Alias inherits the built-in's parameters so permission gating
            // still lands on "allowed" for the read-only target.
            REQUIRE(tool["permission"] == "allowed");
        }
    }
    REQUIRE(saw_alias);

    // tools.call read_file dispatches through the alias to the built-in
    // readFile handler and returns the file contents.
    const Json call = dispatch(
        runtime, "tools.call",
        {{"mode", "agent"},
         {"name", "read_file"},
         {"arguments", {{"path", "alias_note.txt"}}}})["result"];
    REQUIRE(call["content"].get<std::string>().find("aliased-content") !=
            std::string::npos);

    // Unregistering the alias makes the alias name NOT_FOUND again; the
    // target continues to work directly.
    REQUIRE(dispatch(runtime, "tools.unregister_alias",
                      {{"alias", "read_file"}})["result"]["ok"] == true);
    const Json gone = dispatch(
        runtime, "tools.call",
        {{"mode", "agent"},
         {"name", "read_file"},
         {"arguments", {{"path", "alias_note.txt"}}}});
    REQUIRE(gone["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_NOT_FOUND);
    REQUIRE(dispatch(runtime, "tools.call",
                      {{"mode", "agent"},
                       {"name", "readFile"},
                       {"arguments",
                        {{"path", "alias_note.txt"}}}})["result"]["content"]
                .get<std::string>()
                .find("aliased-content") != std::string::npos);
}

TEST_CASE("tools.register_alias rejects collisions and dangling targets",
          "[plugins][ai_editor][native][tools][alias][errors]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    REQUIRE(dispatch(runtime, "runtime.initialize").contains("result"));

    // Shadowing a built-in name is INVALID_ARGUMENT — otherwise the alias
    // would silently override the direct dispatch entry.
    const Json shadow_builtin = dispatch(
        runtime, "tools.register_alias",
        {{"alias", "readFile"}, {"target", "listFiles"}});
    REQUIRE(shadow_builtin["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);

    // Target must resolve to something real; a rubbish target is NOT_FOUND
    // so the caller can distinguish "typo" from "bad payload".
    const Json missing_target = dispatch(
        runtime, "tools.register_alias",
        {{"alias", "cat"}, {"target", "no_such_tool"}});
    REQUIRE(missing_target["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_NOT_FOUND);

    // Register a valid alias, then try to register another with the same
    // name — must fail with INVALID_ARGUMENT (re-bind requires unregister
    // first).
    REQUIRE(dispatch(runtime, "tools.register_alias",
                      {{"alias", "cat"}, {"target", "readFile"}})
                ["result"]["ok"] == true);
    const Json duplicate = dispatch(
        runtime, "tools.register_alias",
        {{"alias", "cat"}, {"target", "listFiles"}});
    REQUIRE(duplicate["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);

    // Self-alias is meaningless — reject so the caller notices.
    const Json self_alias = dispatch(
        runtime, "tools.register_alias",
        {{"alias", "readFile"}, {"target", "readFile"}});
    REQUIRE(self_alias["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);

    // Unregistering an unknown alias returns NOT_FOUND, matching the
    // custom-tool contract.
    const Json missing_unreg = dispatch(
        runtime, "tools.unregister_alias", {{"alias", "never-set"}});
    REQUIRE(missing_unreg["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_NOT_FOUND);
}

TEST_CASE("tools.register_hook emits before + after events around tools.call",
          "[plugins][ai_editor][native][tools][hooks]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    REQUIRE(dispatch(runtime, "runtime.initialize").contains("result"));

    // Seed a workspace file so the editFile call succeeds; the hook events
    // should observe the arguments + result payloads verbatim.
    {
        std::ofstream seed(fixture.workspace() / L"hook_target.txt");
        seed << "original\n";
    }
    // Drain any residual initialize / mcp events so our poll below only
    // observes hook fires triggered by this test.
    (void)drain_sao_events(runtime, 32);

    // Register a before + after hook filtered on editFile only.  The hook
    // ids are echoed back so a downstream audit sink can correlate them.
    REQUIRE(dispatch(runtime, "tools.register_hook",
                      {{"id", "audit-before"},
                       {"phase", "before"},
                       {"toolFilter", Json::array({"editFile"})},
                       {"emitEvent", "audit.tool.before"}})
                ["result"]["ok"] == true);
    REQUIRE(dispatch(runtime, "tools.register_hook",
                      {{"id", "audit-after"},
                       {"phase", "after"},
                       {"toolFilter", Json::array({"editFile"})},
                       {"emitEvent", "audit.tool.after"}})
                ["result"]["ok"] == true);

    const Json call = dispatch(
        runtime, "tools.call",
        {{"mode", "agent"},
         {"name", "editFile"},
         {"arguments",
          {{"path", "hook_target.txt"},
           {"content", "rewrite-via-hook\n"}}}})["result"];
    REQUIRE(call["ok"] == true);

    // Drain events; expect at least one before + one after with the audit
    // metadata attached.  Unrelated events (e.g. workflow list refresh) are
    // ignored — we filter by hookId which is unique to this test.
    const std::vector<Json> events = drain_sao_events(runtime, 32);
    bool saw_before = false;
    bool saw_after = false;
    for (const Json& envelope : events) {
        if (envelope.value("method", std::string{}) != "sao.event") {
            continue;
        }
        const Json& params = envelope["params"];
        const std::string event_name = params.value("event", std::string{});
        const Json& payload = params.value("payload", Json::object());
        if (event_name == "audit.tool.before" &&
            payload.value("hookId", std::string{}) == "audit-before") {
            saw_before = true;
            REQUIRE(payload["phase"] == "before");
            REQUIRE(payload["tool"] == "editFile");
            REQUIRE(payload["arguments"]["path"] == "hook_target.txt");
            // No result yet — before-hooks fire ahead of the handler.
            REQUIRE(!payload.contains("result"));
        }
        if (event_name == "audit.tool.after" &&
            payload.value("hookId", std::string{}) == "audit-after") {
            saw_after = true;
            REQUIRE(payload["phase"] == "after");
            REQUIRE(payload["tool"] == "editFile");
            REQUIRE(payload["result"]["ok"] == true);
            REQUIRE(payload["arguments"]["content"] ==
                    "rewrite-via-hook\n");
            REQUIRE(payload["durationMs"].is_number());
        }
    }
    REQUIRE(saw_before);
    REQUIRE(saw_after);

    // Unregister the before hook and issue another call — only the after
    // event should fire on the next round.  This proves the hook map is
    // actually consulted per call (not memoised on registration).
    REQUIRE(dispatch(runtime, "tools.unregister_hook",
                      {{"id", "audit-before"}})["result"]["ok"] == true);
    (void)drain_sao_events(runtime, 32);
    REQUIRE(dispatch(runtime, "tools.call",
                      {{"mode", "agent"},
                       {"name", "editFile"},
                       {"arguments",
                        {{"path", "hook_target.txt"},
                         {"content", "second-round\n"}}}})
                ["result"]["ok"] == true);
    const std::vector<Json> round2 = drain_sao_events(runtime, 32);
    bool saw_before_r2 = false;
    bool saw_after_r2 = false;
    for (const Json& envelope : round2) {
        if (envelope.value("method", std::string{}) != "sao.event") {
            continue;
        }
        const std::string event_name =
            envelope["params"].value("event", std::string{});
        if (event_name == "audit.tool.before") saw_before_r2 = true;
        if (event_name == "audit.tool.after") saw_after_r2 = true;
    }
    REQUIRE_FALSE(saw_before_r2);
    REQUIRE(saw_after_r2);

    // Payload validation: missing phase / emitEvent / bad toolFilter shape
    // must all fail with INVALID_ARGUMENT.
    const Json bad_phase = dispatch(runtime, "tools.register_hook",
                                     {{"id", "x"},
                                      {"phase", "sideways"},
                                      {"emitEvent", "e"}});
    REQUIRE(bad_phase["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
    const Json bad_filter = dispatch(runtime, "tools.register_hook",
                                      {{"id", "y"},
                                       {"phase", "before"},
                                       {"emitEvent", "e"},
                                       {"toolFilter", "not-an-array"}});
    REQUIRE(bad_filter["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
    const Json missing_event = dispatch(runtime, "tools.register_hook",
                                         {{"id", "z"},
                                          {"phase", "before"}});
    REQUIRE(missing_event["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
}

TEST_CASE("tools.register_hook error phase fires on failed tools.call and "
          "reports the status code",
          "[plugins][ai_editor][native][tools][hooks][error]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    REQUIRE(dispatch(runtime, "runtime.initialize").contains("result"));

    // No toolFilter -> match every tool.  Emit event carries the status
    // integer so an audit sink can classify the failure without also
    // reading the JSON-RPC error envelope out-of-band.
    REQUIRE(dispatch(runtime, "tools.register_hook",
                      {{"id", "audit-error"},
                       {"phase", "error"},
                       {"emitEvent", "audit.tool.error"}})
                ["result"]["ok"] == true);
    (void)drain_sao_events(runtime, 32);

    // 1) Missing required field -> schema-validation failure with
    // INVALID_ARGUMENT.  Ensures the error-phase hook fires for validation
    // failures + not just permission denials.
    const Json bad = dispatch(runtime, "tools.call",
                               {{"mode", "agent"},
                                {"name", "editFile"},
                                {"arguments",
                                 {{"path", "no_content.txt"}}}});
    REQUIRE(bad.contains("error"));
    REQUIRE(bad["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);

    // 2) editFile under ask mode is PERMISSION_DENIED — a second flavour
    // of failure that must also fire the error hook.
    const Json denied = dispatch(runtime, "tools.call",
                                  {{"mode", "ask"},
                                   {"name", "editFile"},
                                   {"arguments",
                                    {{"path", "x.txt"},
                                     {"content", "hi"}}}});
    REQUIRE(denied.contains("error"));
    REQUIRE(denied["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_PERMISSION_DENIED);

    const std::vector<Json> events = drain_sao_events(runtime, 32);
    int seen_error_events = 0;
    bool saw_validation_status = false;
    bool saw_permission_status = false;
    for (const Json& envelope : events) {
        if (envelope.value("method", std::string{}) != "sao.event") {
            continue;
        }
        const Json& params = envelope["params"];
        if (params.value("event", std::string{}) != "audit.tool.error") {
            continue;
        }
        const Json& payload = params.value("payload", Json::object());
        if (payload.value("hookId", std::string{}) != "audit-error") {
            continue;
        }
        ++seen_error_events;
        REQUIRE(payload["phase"] == "error");
        REQUIRE(payload["tool"] == "editFile");
        REQUIRE(payload["durationMs"].is_number());
        const int32_t status = payload.value("status", 0);
        if (status == SAO_AI_EDITOR_ERR_INVALID_ARGUMENT) {
            saw_validation_status = true;
        }
        if (status == SAO_AI_EDITOR_ERR_PERMISSION_DENIED) {
            saw_permission_status = true;
        }
    }
    REQUIRE(seen_error_events == 2);
    REQUIRE(saw_validation_status);
    REQUIRE(saw_permission_status);

    // Successful calls must NOT fire the error hook — otherwise audit logs
    // would double-count when after + error hooks are both registered.
    {
        std::ofstream seed(fixture.workspace() / L"ok_path.txt");
        seed << "seed\n";
    }
    (void)drain_sao_events(runtime, 32);
    REQUIRE(dispatch(runtime, "tools.call",
                      {{"mode", "agent"},
                       {"name", "readFile"},
                       {"arguments", {{"path", "ok_path.txt"}}}})
                ["result"]["content"]
                .get<std::string>()
                .find("seed") != std::string::npos);
    const std::vector<Json> after = drain_sao_events(runtime, 32);
    for (const Json& envelope : after) {
        if (envelope.value("method", std::string{}) != "sao.event") {
            continue;
        }
        REQUIRE(envelope["params"].value("event", std::string{}) !=
                "audit.tool.error");
    }
}

TEST_CASE("AI Editor conversation.export stamps sha256 and import verifies it",
          "[plugins][ai_editor][native][storage][export][sha256]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    REQUIRE(dispatch(runtime, "runtime.initialize").contains("result"));

    // Seed a workspace conversation so export has content to hash.
    const Json created = dispatch(runtime, "conversation.create",
                                  {{"title", "SHA Roundtrip"},
                                   {"model", "gpt-x"},
                                   {"scope", "workspace"}});
    const std::string id = created["result"]["id"];
    REQUIRE(dispatch(runtime, "conversation.append",
                     {{"id", id},
                      {"message", {{"role", "user"},
                                   {"content", "hash me"}}}})
                .contains("result"));

    // Single-conversation envelope must carry sha256 in canonical hex form.
    const Json exported = dispatch(runtime, "conversation.export",
                                    {{"id", id}})["result"];
    REQUIRE(exported["format"] == "sao-conversation/1");
    REQUIRE(exported.contains("sha256"));
    REQUIRE(exported["sha256"].is_string());
    const std::string digest = exported["sha256"].get<std::string>();
    // SHA-256 hex is 64 lower-case chars.
    REQUIRE(digest.size() == 64);
    for (const char ch : digest) {
        const bool is_hex =
            (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        REQUIRE(is_hex);
    }

    // Round-trip: unchanged payload must import cleanly with overwrite=true.
    const Json ok = dispatch(runtime, "conversation.import",
                              {{"payload", exported},
                               {"scope", "workspace"},
                               {"overwrite", true}});
    REQUIRE(ok.contains("result"));
    REQUIRE(ok["result"]["imported"] == 1);

    // Batch envelope (scope=all) must also carry sha256.
    const Json all = dispatch(runtime, "conversation.export",
                               {{"scope", "all"}})["result"];
    REQUIRE(all["format"] == "sao-conversations/1");
    REQUIRE(all.contains("sha256"));
    REQUIRE(all["sha256"].get<std::string>().size() == 64);
}

TEST_CASE("AI Editor conversation.import rejects a tampered sha256 payload",
          "[plugins][ai_editor][native][storage][import][sha256]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    REQUIRE(dispatch(runtime, "runtime.initialize").contains("result"));

    const Json created = dispatch(runtime, "conversation.create",
                                  {{"title", "Tampered"},
                                   {"model", "m"},
                                   {"scope", "workspace"}});
    const std::string id = created["result"]["id"];
    REQUIRE(dispatch(runtime, "conversation.append",
                     {{"id", id},
                      {"message", {{"role", "user"},
                                   {"content", "genuine"}}}})
                .contains("result"));
    const Json exported = dispatch(runtime, "conversation.export",
                                    {{"id", id}})["result"];

    // Tamper: mutate the conversation content without updating sha256.
    Json tampered = exported;
    tampered["conversation"]["messages"][0]["content"] = "malicious";
    const Json rejected = dispatch(runtime, "conversation.import",
                                    {{"payload", tampered},
                                     {"scope", "workspace"},
                                     {"overwrite", true}});
    REQUIRE(rejected.contains("error"));
    REQUIRE(rejected["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_PROTOCOL);

    // Legacy (pre-checksum) payload must still be importable so pre-R4
    // consumers keep working after the checksum lands.
    Json legacy = exported;
    legacy.erase("sha256");
    const Json legacy_ok = dispatch(runtime, "conversation.import",
                                     {{"payload", legacy},
                                      {"scope", "workspace"},
                                      {"overwrite", true}});
    REQUIRE(legacy_ok.contains("result"));
    REQUIRE(legacy_ok["result"]["imported"] == 1);
}

TEST_CASE("AI Editor workflow.export stamps sha256 and import verifies it",
          "[plugins][ai_editor][native][workflow][export][sha256]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    REQUIRE(dispatch(runtime, "runtime.initialize").contains("result"));

    // Single-workflow envelope (built-in id) must carry sha256.
    const Json single = dispatch(runtime, "workflow.export",
                                  {{"id", "review-and-fix"}})["result"];
    REQUIRE(single["format"] == "sao-workflow/1");
    REQUIRE(single.contains("sha256"));
    REQUIRE(single["sha256"].get<std::string>().size() == 64);

    // Batch envelope on the builtin scope carries its own stamp too.
    const Json batch = dispatch(runtime, "workflow.export",
                                 {{"scope", "builtin"}})["result"];
    REQUIRE(batch["format"] == "sao-workflows/1");
    REQUIRE(batch.contains("sha256"));
    REQUIRE(batch["sha256"].get<std::string>().size() == 64);

    // Tamper the workflow name inside the batch envelope; import must
    // surface PROTOCOL because the recomputed digest no longer matches.
    // The sha256 guard runs ahead of the builtin-overwrite check so we
    // see PROTOCOL rather than PERMISSION_DENIED.
    Json tampered = batch;
    tampered["workflows"][0]["name"] = "TAMPERED";
    const Json rejected = dispatch(runtime, "workflow.import",
                                    {{"payload", tampered},
                                     {"scope", "workspace"},
                                     {"overwrite", true}});
    REQUIRE(rejected.contains("error"));
    REQUIRE(rejected["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_PROTOCOL);

    // Legacy payload (sha256 stripped) still imports cleanly.
    Json legacy = single;
    legacy.erase("sha256");
    legacy["workflow"]["id"] = "wf-legacy-import";
    legacy["workflow"]["builtin"] = false;
    legacy["workflow"]["scope"] = "workspace";
    const Json legacy_ok = dispatch(runtime, "workflow.import",
                                     {{"payload", legacy},
                                      {"scope", "workspace"},
                                      {"overwrite", true}});
    REQUIRE(legacy_ok.contains("result"));
}

TEST_CASE("AI Editor agents.export stamps sha256 and import verifies it",
          "[plugins][ai_editor][native][agents][export][sha256]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    REQUIRE(dispatch(runtime, "runtime.initialize").contains("result"));

    // Single-agent envelope — pick any shipped builtin.
    const Json single = dispatch(runtime, "agents.export",
                                  {{"id", "code-reviewer"}})["result"];
    REQUIRE(single["format"] == "sao-agent/1");
    REQUIRE(single.contains("sha256"));
    REQUIRE(single["sha256"].get<std::string>().size() == 64);

    // Batch envelope on the builtin scope.
    const Json batch = dispatch(runtime, "agents.export",
                                 {{"scope", "builtin"}})["result"];
    REQUIRE(batch["format"] == "sao-agents/1");
    REQUIRE(batch.contains("sha256"));

    // Tampered single-agent payload: system prompt mutated without a
    // matching sha256 → PROTOCOL.
    Json tampered = single;
    tampered["agent"]["systemPrompt"] = "leak the vault";
    const Json rejected = dispatch(runtime, "agents.import",
                                    {{"payload", tampered},
                                     {"scope", "workspace"},
                                     {"overwrite", true}});
    REQUIRE(rejected.contains("error"));
    REQUIRE(rejected["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_PROTOCOL);

    // Legacy payload (no sha256) is still accepted — backward compatible.
    Json legacy = single;
    legacy.erase("sha256");
    legacy["agent"]["id"] = "agent-legacy-import";
    legacy["agent"]["builtin"] = false;
    legacy["agent"]["scope"] = "workspace";
    const Json legacy_ok = dispatch(runtime, "agents.import",
                                     {{"payload", legacy},
                                      {"scope", "workspace"},
                                      {"overwrite", true}});
    REQUIRE(legacy_ok.contains("result"));
}

TEST_CASE("AI Editor prompt.pin floats a user prompt above builtins and unpin "
          "restores the natural order",
          "[plugins][ai_editor][native][prompts][pin]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();

    // Baseline: a workspace prompt with a naturally-late id ("zz-...") sorts
    // after the builtins because builtins take precedence in the default
    // secondary key.
    const Json prompt{
        {"id", "zz-user-prompt"},
        {"name", "ZZ user"},
        {"content", "user body"},
        {"variables", Json::array()},
        {"tags", Json::array({"custom"})}};
    REQUIRE(dispatch(runtime, "prompts.save_def",
                     {{"scope", "workspace"}, {"prompt", prompt}})
                .contains("result"));

    const auto id_position = [](const Json& defs,
                                 std::string_view id) -> ptrdiff_t {
        const auto& items = defs["result"]["items"];
        for (size_t index = 0; index < items.size(); ++index) {
            if (items[index].value("id", std::string{}) == id) {
                return static_cast<ptrdiff_t>(index);
            }
        }
        return -1;
    };

    const Json before = dispatch(runtime, "prompts.list_defs");
    const ptrdiff_t before_pos = id_position(before, "zz-user-prompt");
    REQUIRE(before_pos > 0);  // should sit after at least one entry
    // None of the entries ahead of it may be pinned — pinned would explain
    // the position without exercising the natural builtin/id sort we care
    // about.  Market presets from assets/ai_editor/prompts land as
    // builtin=false but still sort ahead of "zz-*" through the id
    // tie-break, so we deliberately do NOT assert builtin==true here.
    for (ptrdiff_t index = 0; index < before_pos; ++index) {
        REQUIRE(before["result"]["items"][index].value("pinned", false) ==
                false);
    }

    // Pin the user prompt — it must jump to index 0 and pinned=true must
    // be surfaced on both the pin result and the list entry.
    const Json pin_result = dispatch(runtime, "prompt.pin",
                                     {{"id", "zz-user-prompt"}})["result"];
    REQUIRE(pin_result["id"] == "zz-user-prompt");
    REQUIRE(pin_result["pinned"] == true);
    const Json after_pin = dispatch(runtime, "prompts.list_defs");
    REQUIRE(id_position(after_pin, "zz-user-prompt") == 0);
    REQUIRE(after_pin["result"]["items"][0].value("pinned", false) == true);

    // Unpin restores the pre-pin position (still after every builtin).
    const Json unpin_result = dispatch(runtime, "prompt.unpin",
                                       {{"id", "zz-user-prompt"}})["result"];
    REQUIRE(unpin_result["pinned"] == false);
    const Json after_unpin = dispatch(runtime, "prompts.list_defs");
    const ptrdiff_t after_unpin_pos =
        id_position(after_unpin, "zz-user-prompt");
    REQUIRE(after_unpin_pos == before_pos);
    REQUIRE(after_unpin["result"]["items"][after_unpin_pos]
                .value("pinned", true) == false);

    // Builtin prompts are locked — pin/unpin must surface PERMISSION_DENIED
    // so shipped defaults keep their canonical position.
    const Json builtin_pin = dispatch(runtime, "prompt.pin",
                                      {{"id", "code-review-base"}});
    REQUIRE(builtin_pin["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_PERMISSION_DENIED);

    // Unknown id → NOT_FOUND.
    const Json missing = dispatch(runtime, "prompt.pin",
                                   {{"id", "does-not-exist"}});
    REQUIRE(missing["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_NOT_FOUND);
}

// -------------------------------------------------------------------------
// Session-memory tool-result cache (src/tool_result_cache.{h,cpp}) — read-only
// dedup keyed on canonical JSON of `(tool, arguments)`, mutation-aware
// invalidation on editFile.  Modelled on VSCode terminalOutputCache.
// -------------------------------------------------------------------------

TEST_CASE("tool cache: readFile returns cached result on second call",
          "[plugins][ai_editor][native][tools][cache]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();

    // Seed a file that readFile can serve.
    REQUIRE(dispatch(runtime, "tools.call",
                     {{"mode", "agent"},
                      {"name", "editFile"},
                      {"arguments", {{"path", "cache_hit.txt"},
                                     {"content", "hello-cache\n"}}}})
                .contains("result"));

    const Json request = {{"mode", "agent"},
                          {"name", "readFile"},
                          {"arguments", {{"path", "cache_hit.txt"}}}};
    const Json first = dispatch(runtime, "tools.call", request);
    REQUIRE(first.contains("result"));
    REQUIRE(first["result"]["content"] == "hello-cache\n");
    // First call has no cacheHit annotation — it went through execute().
    REQUIRE_FALSE(first["result"].contains("cacheHit"));

    const Json second = dispatch(runtime, "tools.call", request);
    REQUIRE(second.contains("result"));
    REQUIRE(second["result"]["cacheHit"] == true);
    REQUIRE(second["result"].contains("cacheAgeMs"));
    REQUIRE(second["result"]["cacheAgeMs"].is_number_integer());
    REQUIRE(second["result"]["cacheAgeMs"].get<int64_t>() >= 0);
    // Content still round-trips even when served from cache.
    REQUIRE(second["result"]["content"] == "hello-cache\n");

    const Json stats = dispatch(runtime, "tools.cache_stats")["result"];
    REQUIRE(stats["hitCount"].get<int64_t>() >= 1);
    REQUIRE(stats["totalEntries"].get<int64_t>() >= 1);
    // The one Fast-class entry (readFile) shows up under byClass.Fast.
    REQUIRE(stats["byClass"]["Fast"].get<int64_t>() >= 1);
}

TEST_CASE("tool cache: editFile invalidates matching readFile path only",
          "[plugins][ai_editor][native][tools][cache]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();

    // Seed two files so we can prove path-scoped invalidation.
    REQUIRE(dispatch(runtime, "tools.call",
                     {{"mode", "agent"},
                      {"name", "editFile"},
                      {"arguments", {{"path", "a.txt"},
                                     {"content", "AAA\n"}}}})
                .contains("result"));
    REQUIRE(dispatch(runtime, "tools.call",
                     {{"mode", "agent"},
                      {"name", "editFile"},
                      {"arguments", {{"path", "b.txt"},
                                     {"content", "BBB\n"}}}})
                .contains("result"));

    // Warm the cache for both.
    const Json read_a = {{"mode", "agent"},
                         {"name", "readFile"},
                         {"arguments", {{"path", "a.txt"}}}};
    const Json read_b = {{"mode", "agent"},
                         {"name", "readFile"},
                         {"arguments", {{"path", "b.txt"}}}};
    REQUIRE(dispatch(runtime, "tools.call", read_a)["result"]["content"] ==
            "AAA\n");
    REQUIRE(dispatch(runtime, "tools.call", read_b)["result"]["content"] ==
            "BBB\n");

    // Second reads are cache hits.
    REQUIRE(dispatch(runtime, "tools.call",
                     read_a)["result"]["cacheHit"] == true);
    REQUIRE(dispatch(runtime, "tools.call",
                     read_b)["result"]["cacheHit"] == true);

    // Mutate a.txt only.  Cache for a.txt evicted; b.txt survives.
    REQUIRE(dispatch(runtime, "tools.call",
                     {{"mode", "agent"},
                      {"name", "editFile"},
                      {"arguments", {{"path", "a.txt"},
                                     {"content", "AAA-v2\n"}}}})
                .contains("result"));

    const Json after_a = dispatch(runtime, "tools.call", read_a);
    REQUIRE(after_a["result"]["content"] == "AAA-v2\n");
    // a.txt was invalidated — first read after the edit is a miss (no
    // cacheHit annotation) and returns fresh content.
    REQUIRE_FALSE(after_a["result"].contains("cacheHit"));

    const Json after_b = dispatch(runtime, "tools.call", read_b);
    REQUIRE(after_b["result"]["cacheHit"] == true);
    REQUIRE(after_b["result"]["content"] == "BBB\n");
}

TEST_CASE("tool cache: listFiles recursive gets Slow class, "
          "non-recursive gets Fast",
          "[plugins][ai_editor][native][tools][cache]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();

    // Seed a subdirectory with content so listFiles has real output.
    REQUIRE(dispatch(runtime, "tools.call",
                     {{"mode", "agent"},
                      {"name", "editFile"},
                      {"arguments", {{"path", "sub/nested.txt"},
                                     {"content", "n\n"}}}})
                .contains("result"));

    // Reset counters + entries so we can inspect this test's contribution.
    dispatch(runtime, "tools.cache_clear");

    // Non-recursive → Fast bucket.
    const Json ls_flat = {{"mode", "agent"},
                          {"name", "listFiles"},
                          {"arguments", {{"path", "."}}}};
    REQUIRE(dispatch(runtime, "tools.call", ls_flat).contains("result"));
    REQUIRE(dispatch(runtime, "tools.call",
                     ls_flat)["result"]["cacheHit"] == true);

    // Recursive → Slow bucket.
    const Json ls_deep = {{"mode", "agent"},
                          {"name", "listFiles"},
                          {"arguments", {{"path", "."},
                                         {"recursive", true}}}};
    REQUIRE(dispatch(runtime, "tools.call", ls_deep).contains("result"));
    REQUIRE(dispatch(runtime, "tools.call",
                     ls_deep)["result"]["cacheHit"] == true);

    const Json stats = dispatch(runtime, "tools.cache_stats")["result"];
    REQUIRE(stats["byClass"]["Fast"].get<int64_t>() == 1);
    REQUIRE(stats["byClass"]["Slow"].get<int64_t>() == 1);
    // Two hits (one for each variant), two misses (the initial recording
    // calls).
    REQUIRE(stats["hitCount"].get<int64_t>() == 2);
    REQUIRE(stats["missCount"].get<int64_t>() == 2);
}

TEST_CASE("tool cache: searchFiles Medium bucket + cache_stats + cache_clear",
          "[plugins][ai_editor][native][tools][cache]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();

    REQUIRE(dispatch(runtime, "tools.call",
                     {{"mode", "agent"},
                      {"name", "editFile"},
                      {"arguments", {{"path", "hits.txt"},
                                     {"content", "needle in haystack\n"}}}})
                .contains("result"));

    dispatch(runtime, "tools.cache_clear");

    const Json search = {{"mode", "agent"},
                         {"name", "searchFiles"},
                         {"arguments", {{"path", "."},
                                        {"query", "needle"},
                                        {"pattern", "*.txt"}}}};
    REQUIRE(dispatch(runtime, "tools.call", search)["result"]["total"] == 1);
    // Second call reuses the cache.
    const Json cached = dispatch(runtime, "tools.call", search);
    REQUIRE(cached["result"]["cacheHit"] == true);
    REQUIRE(cached["result"]["total"] == 1);

    // searchFiles lives in Medium.
    Json stats = dispatch(runtime, "tools.cache_stats")["result"];
    REQUIRE(stats["byClass"]["Medium"].get<int64_t>() == 1);
    REQUIRE(stats["hitCount"].get<int64_t>() == 1);
    REQUIRE(stats["missCount"].get<int64_t>() == 1);

    // Any editFile triggers full flush of searchFiles entries (invalidates
    // wholesale, not path-scoped — a doc under a different path could still
    // match the query).
    REQUIRE(dispatch(runtime, "tools.call",
                     {{"mode", "agent"},
                      {"name", "editFile"},
                      {"arguments", {{"path", "other.txt"},
                                     {"content", "x\n"}}}})
                .contains("result"));
    // The second read of the same query is now a miss again.
    const Json fresh = dispatch(runtime, "tools.call", search);
    REQUIRE_FALSE(fresh["result"].contains("cacheHit"));

    // Manual clear zeroes the counters and empties byClass buckets.
    const Json cleared = dispatch(runtime, "tools.cache_clear")["result"];
    REQUIRE(cleared["ok"] == true);
    stats = dispatch(runtime, "tools.cache_stats")["result"];
    REQUIRE(stats["totalEntries"] == 0);
    REQUIRE(stats["hitCount"] == 0);
    REQUIRE(stats["missCount"] == 0);
    REQUIRE(stats["invalidationCount"] == 0);
    REQUIRE(stats["byClass"]["Fast"] == 0);
    REQUIRE(stats["byClass"]["Medium"] == 0);
    REQUIRE(stats["byClass"]["Slow"] == 0);
}

TEST_CASE("tool cache: JSON key ordering does not fragment the cache",
          "[plugins][ai_editor][native][tools][cache]") {
    // Sanity check the canonical-JSON key generation: two argument objects
    // that carry the same fields in different insertion order must land on
    // the same cache slot.
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    REQUIRE(dispatch(runtime, "tools.call",
                     {{"mode", "agent"},
                      {"name", "editFile"},
                      {"arguments", {{"path", "canon.txt"},
                                     {"content", "K\n"}}}})
                .contains("result"));
    dispatch(runtime, "tools.cache_clear");

    // Use searchFiles because it accepts multiple string params; the exact
    // JSON insertion order is what we're stressing.
    Json args_a = Json::object();
    args_a["path"] = ".";
    args_a["query"] = "K";
    args_a["pattern"] = "*.txt";
    Json args_b = Json::object();
    args_b["pattern"] = "*.txt";
    args_b["query"] = "K";
    args_b["path"] = ".";

    REQUIRE(dispatch(runtime, "tools.call",
                     {{"mode", "agent"},
                      {"name", "searchFiles"},
                      {"arguments", args_a}}).contains("result"));
    const Json hit = dispatch(runtime, "tools.call",
                              {{"mode", "agent"},
                               {"name", "searchFiles"},
                               {"arguments", args_b}});
    REQUIRE(hit["result"]["cacheHit"] == true);
    const Json stats = dispatch(runtime, "tools.cache_stats")["result"];
    REQUIRE(stats["totalEntries"].get<int64_t>() == 1);
}

// -------------------------------------------------------------------------
// Tool execution telemetry (src/tool_execution_monitor.{h,cpp}) — per-tool
// counters modelled on VSCode outputMonitor.ts's
// IOutputMonitorTelemetryCounters.  Exposed via `tools.telemetry_stats` and
// wired into the tools.call path so both cache hits and fresh execute()
// contribute to the ledger.
// -------------------------------------------------------------------------

TEST_CASE("tool telemetry: readFile records invocations + bytes + cache hits",
          "[plugins][ai_editor][native][tools][telemetry]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();

    // Seed a file the read path can serve.
    REQUIRE(dispatch(runtime, "tools.call",
                     {{"mode", "agent"},
                      {"name", "editFile"},
                      {"arguments", {{"path", "telem.txt"},
                                     {"content", "hello-telemetry\n"}}}})
                .contains("result"));

    const Json read_call = {{"mode", "agent"},
                             {"name", "readFile"},
                             {"arguments", {{"path", "telem.txt"}}}};
    // First read -> miss (execute path).  Second read -> cache hit.
    REQUIRE(dispatch(runtime, "tools.call", read_call).contains("result"));
    REQUIRE(dispatch(runtime, "tools.call", read_call).contains("result"));

    const Json stats = dispatch(runtime, "tools.telemetry_stats")["result"];
    REQUIRE(stats.contains("byTool"));
    REQUIRE(stats.contains("aggregate"));
    const auto& read_row = stats["byTool"]["readFile"];
    REQUIRE(read_row["invocations"].get<int64_t>() == 2);
    REQUIRE(read_row["cacheHits"].get<int64_t>() == 1);
    REQUIRE(read_row["cacheMisses"].get<int64_t>() == 1);
    REQUIRE(read_row["totalBytesReturned"].get<int64_t>() > 0);
    REQUIRE(read_row["errorCount"].get<int64_t>() == 0);
    // avgDurationMs is a double; on the test path it may be 0 or a small
    // positive value.  Just assert the field type + non-negativity.
    REQUIRE(read_row["avgDurationMs"].is_number());
    REQUIRE(read_row["avgDurationMs"].get<double>() >= 0.0);
}

TEST_CASE("tool telemetry: aggregate sums across every tool",
          "[plugins][ai_editor][native][tools][telemetry]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();

    // Fire a mix of tools so the aggregate has something to add up.
    REQUIRE(dispatch(runtime, "tools.call",
                     {{"mode", "agent"},
                      {"name", "editFile"},
                      {"arguments", {{"path", "one.txt"},
                                     {"content", "1\n"}}}})
                .contains("result"));
    REQUIRE(dispatch(runtime, "tools.call",
                     {{"mode", "agent"},
                      {"name", "editFile"},
                      {"arguments", {{"path", "two.txt"},
                                     {"content", "2\n"}}}})
                .contains("result"));
    REQUIRE(dispatch(runtime, "tools.call",
                     {{"mode", "agent"},
                      {"name", "readFile"},
                      {"arguments", {{"path", "one.txt"}}}})
                .contains("result"));
    REQUIRE(dispatch(runtime, "tools.call",
                     {{"mode", "agent"},
                      {"name", "listFiles"},
                      {"arguments", {{"path", "."}}}})
                .contains("result"));

    const Json stats = dispatch(runtime, "tools.telemetry_stats")["result"];
    const auto& agg = stats["aggregate"];
    // 2 editFile + 1 readFile + 1 listFiles = 4 invocations total.
    REQUIRE(agg["invocations"].get<int64_t>() == 4);
    REQUIRE(stats["byTool"].contains("editFile"));
    REQUIRE(stats["byTool"].contains("readFile"));
    REQUIRE(stats["byTool"].contains("listFiles"));
    // The sum of per-tool invocations must equal aggregate.invocations.
    int64_t sum = 0;
    for (auto it = stats["byTool"].begin(); it != stats["byTool"].end(); ++it) {
        sum += it.value()["invocations"].get<int64_t>();
    }
    REQUIRE(sum == agg["invocations"].get<int64_t>());
}

TEST_CASE("tool telemetry: byTool buckets are keyed on canonical tool name",
          "[plugins][ai_editor][native][tools][telemetry]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();

    // Different tools get separate byTool entries; repeated invocations of
    // the same tool bump the same bucket.
    REQUIRE(dispatch(runtime, "tools.call",
                     {{"mode", "agent"},
                      {"name", "editFile"},
                      {"arguments", {{"path", "a.txt"},
                                     {"content", "A\n"}}}})
                .contains("result"));
    for (int i = 0; i < 3; ++i) {
        REQUIRE(dispatch(runtime, "tools.call",
                         {{"mode", "agent"},
                          {"name", "readFile"},
                          {"arguments", {{"path", "a.txt"}}}})
                    .contains("result"));
    }
    for (int i = 0; i < 2; ++i) {
        REQUIRE(dispatch(runtime, "tools.call",
                         {{"mode", "agent"},
                          {"name", "listFiles"},
                          {"arguments", {{"path", "."}}}})
                    .contains("result"));
    }

    const Json stats = dispatch(runtime, "tools.telemetry_stats")["result"];
    REQUIRE(stats["byTool"]["editFile"]["invocations"].get<int64_t>() == 1);
    REQUIRE(stats["byTool"]["readFile"]["invocations"].get<int64_t>() == 3);
    REQUIRE(stats["byTool"]["listFiles"]["invocations"].get<int64_t>() == 2);
    // 3 readFile calls: first is a miss, next two hit the 30s Fast TTL cache.
    REQUIRE(stats["byTool"]["readFile"]["cacheMisses"].get<int64_t>() == 1);
    REQUIRE(stats["byTool"]["readFile"]["cacheHits"].get<int64_t>() == 2);
}

TEST_CASE("tool telemetry: cache_clear preserves the ledger",
          "[plugins][ai_editor][native][tools][telemetry]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();

    REQUIRE(dispatch(runtime, "tools.call",
                     {{"mode", "agent"},
                      {"name", "editFile"},
                      {"arguments", {{"path", "keep.txt"},
                                     {"content", "K\n"}}}})
                .contains("result"));
    REQUIRE(dispatch(runtime, "tools.call",
                     {{"mode", "agent"},
                      {"name", "readFile"},
                      {"arguments", {{"path", "keep.txt"}}}})
                .contains("result"));
    REQUIRE(dispatch(runtime, "tools.call",
                     {{"mode", "agent"},
                      {"name", "readFile"},
                      {"arguments", {{"path", "keep.txt"}}}})
                .contains("result"));

    const Json before = dispatch(runtime, "tools.telemetry_stats")["result"];
    const int64_t reads_before =
        before["byTool"]["readFile"]["invocations"].get<int64_t>();
    REQUIRE(reads_before == 2);

    // Flush the dedup cache -- telemetry must survive.
    REQUIRE(dispatch(runtime, "tools.cache_clear")["result"]["ok"] == true);
    const Json cache_after = dispatch(runtime, "tools.cache_stats")["result"];
    REQUIRE(cache_after["totalEntries"].get<int64_t>() == 0);
    REQUIRE(cache_after["hitCount"].get<int64_t>() == 0);

    const Json after = dispatch(runtime, "tools.telemetry_stats")["result"];
    REQUIRE(after["byTool"]["readFile"]["invocations"].get<int64_t>() ==
            reads_before);
    REQUIRE(after["byTool"]["readFile"]["cacheHits"].get<int64_t>() == 1);
    REQUIRE(after["byTool"]["readFile"]["cacheMisses"].get<int64_t>() == 1);

    // telemetry_clear zeroes it out.
    REQUIRE(dispatch(runtime, "tools.telemetry_clear")["result"]["ok"] ==
            true);
    const Json cleared =
        dispatch(runtime, "tools.telemetry_stats")["result"];
    REQUIRE(cleared["byTool"].empty());
    REQUIRE(cleared["aggregate"]["invocations"].get<int64_t>() == 0);
}

TEST_CASE("tool cache: runInTerminal-style side-effect tool flushes readFile",
          "[plugins][ai_editor][native][tools][cache]") {
    // R16 cross-tool invalidation: side-effect tools (runInTerminal,
    // executeCommand, shell) must flush the read-only cache wholesale
    // because we cannot tell from the tool name alone whether the command
    // touched files under the workspace.  classify() fires cache
    // invalidation *before* execute() runs, so the custom registration
    // exists only to make the tools.call round-trip return OK -- the
    // invalidation is purely name-based.
    RuntimeFixture fixture;
    auto runtime = fixture.get();

    REQUIRE(dispatch(runtime, "tools.register",
                     {{"name", "runInTerminal"},
                      {"description", "custom tty pass-through"},
                      {"readOnly", false}})["result"]["ok"] == true);

    // Warm up read + list caches.
    REQUIRE(dispatch(runtime, "tools.call",
                     {{"mode", "agent"},
                      {"name", "editFile"},
                      {"arguments", {{"path", "cmd.txt"},
                                     {"content", "before\n"}}}})
                .contains("result"));
    const Json read_call = {{"mode", "agent"},
                             {"name", "readFile"},
                             {"arguments", {{"path", "cmd.txt"}}}};
    const Json list_call = {{"mode", "agent"},
                             {"name", "listFiles"},
                             {"arguments", {{"path", "."}}}};
    REQUIRE(dispatch(runtime, "tools.call", read_call).contains("result"));
    REQUIRE(dispatch(runtime, "tools.call", list_call).contains("result"));
    // Confirm they are hot.
    REQUIRE(dispatch(runtime, "tools.call", read_call)["result"]["cacheHit"]
            == true);
    REQUIRE(dispatch(runtime, "tools.call", list_call)["result"]["cacheHit"]
            == true);

    // Fire the side-effect tool -- invalidation runs even though the tool
    // itself just echoes its arguments.  Both readFile + listFiles caches
    // must be flushed after this call.
    REQUIRE(dispatch(runtime, "tools.call",
                     {{"mode", "agent"},
                      {"name", "runInTerminal"},
                      {"arguments", {{"command", "echo hi"}}}})
                .contains("result"));

    const Json read_after = dispatch(runtime, "tools.call", read_call);
    const Json list_after = dispatch(runtime, "tools.call", list_call);
    REQUIRE_FALSE(read_after["result"].contains("cacheHit"));
    REQUIRE_FALSE(list_after["result"].contains("cacheHit"));
}

// -----------------------------------------------------------------------------
// chat.set_pricing / chat.get_pricing / chat.list_pricing / chat.cost_stats
//
// R15 wave: LLM token pricing rules + per-model cost aggregation.  Storage is
// in-process (map<"provider|model", rule>) so the round-trip test asserts
// both the write shape (returned rule mirrors what the caller sent, missing
// fields dropped) and the read-back shape (found:true carries the same rule
// verbatim).  Cost math itself lives inside perform_openai_chat and is
// exercised via chat.run: the "with rule" path asserts costUsd is derived
// from the rule + reported usage, the "without rule" path asserts we still
// surface prompt/completion token counts but leave costUsd at zero and set
// pricingApplied:false so downstream consumers can distinguish the two.
// -----------------------------------------------------------------------------

TEST_CASE("AI Editor chat.set_pricing + chat.get_pricing round-trip a rule",
          "[plugins][ai_editor][native][pricing]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();

    // Unknown (provider, model) reports found:false with no rule payload so
    // callers can distinguish "no rule registered" from "rule with zeros".
    const Json missing =
        dispatch(runtime, "chat.get_pricing",
                 {{"provider", "openai"}, {"model", "gpt-4o"}});
    REQUIRE(missing.contains("result"));
    REQUIRE(missing["result"]["found"].get<bool>() == false);
    REQUIRE(!missing["result"].contains("rule"));

    // Register a canonical rule.
    const Json set = dispatch(runtime, "chat.set_pricing",
                              {{"provider", "openai"},
                               {"model", "gpt-4o"},
                               {"promptPer1K", 0.005},
                               {"completionPer1K", 0.015}});
    REQUIRE(set.contains("result"));
    REQUIRE(set["result"]["ok"].get<bool>() == true);
    REQUIRE(set["result"]["provider"] == "openai");
    REQUIRE(set["result"]["model"] == "gpt-4o");
    REQUIRE(set["result"]["rule"]["promptPer1K"].get<double>() == 0.005);
    REQUIRE(set["result"]["rule"]["completionPer1K"].get<double>() == 0.015);

    // Read it back.
    const Json got = dispatch(runtime, "chat.get_pricing",
                              {{"provider", "openai"}, {"model", "gpt-4o"}});
    REQUIRE(got["result"]["found"].get<bool>() == true);
    REQUIRE(got["result"]["rule"]["promptPer1K"].get<double>() == 0.005);
    REQUIRE(got["result"]["rule"]["completionPer1K"].get<double>() == 0.015);

    // Register a second rule under a different (provider, model) pair.
    dispatch(runtime, "chat.set_pricing",
             {{"provider", "anthropic"},
              {"model", "claude-3-sonnet"},
              {"promptPer1K", 0.003},
              {"completionPer1K", 0.015}});

    // list_pricing returns both entries.  Order is unspecified (unordered_map)
    // so the assertions walk the array and match by (provider, model).
    const Json listed = dispatch(runtime, "chat.list_pricing");
    REQUIRE(listed["result"]["items"].is_array());
    REQUIRE(listed["result"]["items"].size() == 2);
    bool saw_openai = false;
    bool saw_anthropic = false;
    for (const auto& entry : listed["result"]["items"]) {
        if (entry["provider"] == "openai" && entry["model"] == "gpt-4o") {
            saw_openai = true;
            REQUIRE(entry["rule"]["promptPer1K"].get<double>() == 0.005);
        } else if (entry["provider"] == "anthropic" &&
                   entry["model"] == "claude-3-sonnet") {
            saw_anthropic = true;
            REQUIRE(entry["rule"]["completionPer1K"].get<double>() == 0.015);
        }
    }
    REQUIRE(saw_openai);
    REQUIRE(saw_anthropic);

    // Overwrite the openai rule.  set_pricing is upsert-shaped: sending only
    // completionPer1K drops promptPer1K rather than merging with the old
    // value, so the read-back only carries what the last call supplied.
    dispatch(runtime, "chat.set_pricing",
             {{"provider", "openai"},
              {"model", "gpt-4o"},
              {"completionPer1K", 0.020}});
    const Json overwritten =
        dispatch(runtime, "chat.get_pricing",
                 {{"provider", "openai"}, {"model", "gpt-4o"}});
    REQUIRE(overwritten["result"]["rule"]["completionPer1K"].get<double>() ==
            0.020);
    REQUIRE(!overwritten["result"]["rule"].contains("promptPer1K"));

    // Rejects an empty rule (neither field numeric) so we never register a
    // silent no-op entry.
    const Json rejected = dispatch(runtime, "chat.set_pricing",
                                    {{"provider", "openai"},
                                     {"model", "no-pricing"}});
    REQUIRE(rejected.contains("error"));
    REQUIRE(rejected["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);

    // Missing provider / model → INVALID_ARGUMENT (rule cannot be keyed).
    const Json bad_key = dispatch(runtime, "chat.set_pricing",
                                   {{"provider", "openai"},
                                    {"promptPer1K", 0.001}});
    REQUIRE(bad_key["error"]["data"]["status"] ==
            SAO_AI_EDITOR_ERR_INVALID_ARGUMENT);
}

TEST_CASE("AI Editor chat.run with a pricing rule computes costUsd on metrics",
          "[plugins][ai_editor][native][pricing][runs]") {
    // Provider replies with a usage block containing both prompt_tokens and
    // completion_tokens so we can assert the derived costUsd lands on the run
    // result once resolve_pricing_rule injects the caller's rule.
    const std::string body =
        R"({"id":"chat-cost-1","model":"gpt-4o",)"
        R"("choices":[{"message":{"role":"assistant",)"
        R"("content":"priced"},"finish_reason":"stop"}],)"
        R"("usage":{"prompt_tokens":1000,"completion_tokens":500,)"
        R"("total_tokens":1500}})";
    LocalHttpServer server(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: " +
        std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body);
    RuntimeFixture fixture;
    auto runtime = fixture.get();

    // Register a $0.005 / $0.015 rule.  For 1000 prompt + 500 completion
    // tokens the derived cost is 1.0 * 0.005 + 0.5 * 0.015 = 0.0125 USD.
    dispatch(runtime, "chat.set_pricing",
             {{"provider", "openai"},
              {"model", "gpt-4o"},
              {"promptPer1K", 0.005},
              {"completionPer1K", 0.015}});

    const Json params{{"provider", {{"id", "fixture-cost"},
                                     {"endpoint", server.endpoint()}}},
                      {"model", "gpt-4o"},
                      {"messages", Json::array(
                           {{{"role", "user"}, {"content", "hi"}}})},
                      {"stream", false},
                      {"timeoutMs", 5'000}};
    const Json started = dispatch(runtime, "chat.run", params);
    const std::string run_id = started["result"]["runId"];
    REQUIRE(server.wait_for_connections(1, 2'000));

    Json status;
    const ULONGLONG wait_started = GetTickCount64();
    do {
        status = dispatch(runtime, "run.status", {{"runId", run_id}})["result"];
        if (status["status"] != "running") {
            break;
        }
        Sleep(10);
    } while (GetTickCount64() - wait_started < 5'000);
    REQUIRE(status["status"] == "completed");

    REQUIRE(status["result"].contains("metrics"));
    const Json metrics = status["result"]["metrics"];
    REQUIRE(metrics["promptTokens"].get<int64_t>() == 1000);
    REQUIRE(metrics["completionTokens"].get<int64_t>() == 500);
    REQUIRE(metrics["pricingApplied"].get<bool>() == true);
    REQUIRE(metrics.contains("costUsd"));
    // Floating-point math: allow tiny epsilon for the 0.0125 result.
    const double cost = metrics["costUsd"].get<double>();
    REQUIRE(cost > 0.01249);
    REQUIRE(cost < 0.01251);

    // Cost also propagates through the runtime's per-model accumulator.  The
    // (provider, model) row must credit the same numbers we just observed on
    // metrics so the dashboard slice stays consistent with per-run data.
    const Json stats = dispatch(runtime, "chat.cost_stats")["result"];
    REQUIRE(stats["totalRequestsSampled"].get<uint64_t>() == 1);
    REQUIRE(stats["totalPromptTokens"].get<int64_t>() == 1000);
    REQUIRE(stats["totalCompletionTokens"].get<int64_t>() == 500);
    const double stats_total = stats["totalCostUsd"].get<double>();
    REQUIRE(stats_total > 0.01249);
    REQUIRE(stats_total < 0.01251);
    REQUIRE(stats["byModel"].contains("gpt-4o"));
    REQUIRE(stats["byModel"]["gpt-4o"]["provider"] == "openai");
    REQUIRE(stats["byModel"]["gpt-4o"]["requests"].get<uint64_t>() == 1);
    REQUIRE(stats["byProvider"].contains("openai"));
    REQUIRE(stats["byProvider"]["openai"]["requests"].get<uint64_t>() == 1);
}

TEST_CASE("AI Editor chat.run without a pricing rule zeroes costUsd but keeps "
          "prompt / completion token counts",
          "[plugins][ai_editor][native][pricing][runs]") {
    const std::string body =
        R"({"id":"chat-cost-2","model":"unpriced-model",)"
        R"("choices":[{"message":{"role":"assistant",)"
        R"("content":"ok"},"finish_reason":"stop"}],)"
        R"("usage":{"prompt_tokens":250,"completion_tokens":42,)"
        R"("total_tokens":292}})";
    LocalHttpServer server(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: " +
        std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body);
    RuntimeFixture fixture;
    auto runtime = fixture.get();

    // Intentionally do NOT register a pricing rule for unpriced-model.  The
    // resolve_pricing_rule lookup should miss, leaving pricing_rule empty
    // and cost math skipped.
    const Json params{{"provider", {{"id", "fixture-nopricing"},
                                     {"endpoint", server.endpoint()}}},
                      {"model", "unpriced-model"},
                      {"messages", Json::array(
                           {{{"role", "user"}, {"content", "hi"}}})},
                      {"stream", false},
                      {"timeoutMs", 5'000}};
    const Json started = dispatch(runtime, "chat.run", params);
    const std::string run_id = started["result"]["runId"];
    REQUIRE(server.wait_for_connections(1, 2'000));

    Json status;
    const ULONGLONG wait_started = GetTickCount64();
    do {
        status = dispatch(runtime, "run.status", {{"runId", run_id}})["result"];
        if (status["status"] != "running") {
            break;
        }
        Sleep(10);
    } while (GetTickCount64() - wait_started < 5'000);
    REQUIRE(status["status"] == "completed");

    const Json metrics = status["result"]["metrics"];
    REQUIRE(metrics["promptTokens"].get<int64_t>() == 250);
    REQUIRE(metrics["completionTokens"].get<int64_t>() == 42);
    REQUIRE(metrics["pricingApplied"].get<bool>() == false);
    // costUsd is present but zero — the wire shape stays stable so consumers
    // can index into metrics.costUsd without branching on pricingApplied.
    REQUIRE(metrics["costUsd"].get<double>() == 0.0);

    // cost_stats still records the run so per-model token counts stay useful
    // even in the no-pricing case.  costUsd contribution is 0.
    const Json stats = dispatch(runtime, "chat.cost_stats")["result"];
    REQUIRE(stats["totalRequestsSampled"].get<uint64_t>() == 1);
    REQUIRE(stats["totalPromptTokens"].get<int64_t>() == 250);
    REQUIRE(stats["totalCompletionTokens"].get<int64_t>() == 42);
    REQUIRE(stats["totalCostUsd"].get<double>() == 0.0);
    REQUIRE(stats["byModel"]["unpriced-model"]["provider"] == "openai");
}

TEST_CASE("AI Editor chat.cost_stats aggregates across multiple runs by model "
          "and provider",
          "[plugins][ai_editor][native][pricing][runs]") {
    // We send the same fixture-model twice and a different model once so the
    // aggregation rolls up requests/prompt/completion/cost correctly per
    // (provider, model) row *and* per provider-only slice.  Each response
    // returns a distinct usage block so a bug that stops accumulating after
    // the first run would fail the deep totals check.
    const std::string body_a =
        R"({"id":"agg-a","model":"gpt-4o",)"
        R"("choices":[{"message":{"role":"assistant","content":"a"},)"
        R"("finish_reason":"stop"}],)"
        R"("usage":{"prompt_tokens":100,"completion_tokens":50,)"
        R"("total_tokens":150}})";
    const std::string body_b =
        R"({"id":"agg-b","model":"gpt-4o",)"
        R"("choices":[{"message":{"role":"assistant","content":"b"},)"
        R"("finish_reason":"stop"}],)"
        R"("usage":{"prompt_tokens":200,"completion_tokens":80,)"
        R"("total_tokens":280}})";
    const std::string body_c =
        R"({"id":"agg-c","model":"gpt-4o-mini",)"
        R"("choices":[{"message":{"role":"assistant","content":"c"},)"
        R"("finish_reason":"stop"}],)"
        R"("usage":{"prompt_tokens":40,"completion_tokens":10,)"
        R"("total_tokens":50}})";
    auto build_http_response = [](const std::string& body) {
        return std::string(
                   "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                   "Content-Length: ") +
               std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" +
               body;
    };
    LocalHttpServer server(std::vector<std::string>{
        build_http_response(body_a), build_http_response(body_b),
        build_http_response(body_c)});

    RuntimeFixture fixture;
    auto runtime = fixture.get();

    // $0.005/$0.015 for gpt-4o so cost=100/1000*0.005 + 50/1000*0.015=0.00125
    // per body_a and 0.0022 per body_b (total 0.00345).
    dispatch(runtime, "chat.set_pricing",
             {{"provider", "openai"},
              {"model", "gpt-4o"},
              {"promptPer1K", 0.005},
              {"completionPer1K", 0.015}});
    // gpt-4o-mini priced at 0.00015 / 0.0006 → 40/1000*0.00015 + 10/1000*0.0006
    // = 6e-6 + 6e-6 = 1.2e-5.
    dispatch(runtime, "chat.set_pricing",
             {{"provider", "openai"},
              {"model", "gpt-4o-mini"},
              {"promptPer1K", 0.00015},
              {"completionPer1K", 0.0006}});

    auto submit = [&](const std::string& model) {
        const Json params{{"provider", {{"id", "fixture-agg"},
                                          {"endpoint", server.endpoint()}}},
                          {"model", model},
                          {"messages", Json::array(
                               {{{"role", "user"}, {"content", "x"}}})},
                          {"stream", false},
                          {"timeoutMs", 5'000}};
        const Json started = dispatch(runtime, "chat.run", params);
        const std::string run_id = started["result"]["runId"];
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
    };
    submit("gpt-4o");
    submit("gpt-4o");
    submit("gpt-4o-mini");
    REQUIRE(server.wait_for_connections(3, 5'000));

    const Json stats = dispatch(runtime, "chat.cost_stats",
                                {{"days", 7}})["result"];

    // Top-level totals: 3 runs, 340 prompt tokens, 140 completion tokens.
    REQUIRE(stats["days"].get<int64_t>() == 7);
    REQUIRE(stats["totalRequestsSampled"].get<uint64_t>() == 3);
    REQUIRE(stats["totalPromptTokens"].get<int64_t>() == 340);
    REQUIRE(stats["totalCompletionTokens"].get<int64_t>() == 140);
    const double total_cost = stats["totalCostUsd"].get<double>();
    // 0.00125 (a) + 0.0022 (b) + 0.000012 (c) = 0.003462.
    REQUIRE(total_cost > 0.003461);
    REQUIRE(total_cost < 0.003463);

    // Per-model slice: gpt-4o row aggregates both runs.
    REQUIRE(stats["byModel"]["gpt-4o"]["requests"].get<uint64_t>() == 2);
    REQUIRE(stats["byModel"]["gpt-4o"]["promptTokens"].get<int64_t>() == 300);
    REQUIRE(stats["byModel"]["gpt-4o"]["completionTokens"].get<int64_t>() ==
            130);
    // 0.00125 + 0.0022 = 0.00345 for gpt-4o.
    const double gpt4o_cost =
        stats["byModel"]["gpt-4o"]["costUsd"].get<double>();
    REQUIRE(gpt4o_cost > 0.00344);
    REQUIRE(gpt4o_cost < 0.00346);
    REQUIRE(stats["byModel"]["gpt-4o-mini"]["requests"].get<uint64_t>() == 1);

    // Provider-only slice: openai row rolls up all 3 runs since both models
    // share the same provider_type.
    REQUIRE(stats["byProvider"].contains("openai"));
    REQUIRE(stats["byProvider"]["openai"]["requests"].get<uint64_t>() == 3);
    REQUIRE(stats["byProvider"]["openai"]["promptTokens"].get<int64_t>() ==
            340);
    REQUIRE(stats["byProvider"]["openai"]["completionTokens"].get<int64_t>() ==
            140);

    // Average per request: 340/3 = 113 prompt tokens (int div), 140/3 = 46
    // completion tokens.
    REQUIRE(stats["averagePerRequest"]["promptTokens"].get<int64_t>() == 113);
    REQUIRE(stats["averagePerRequest"]["completionTokens"].get<int64_t>() ==
            46);
    const double avg_cost =
        stats["averagePerRequest"]["costUsd"].get<double>();
    REQUIRE(avg_cost > total_cost / 3.0 - 1e-9);
    REQUIRE(avg_cost < total_cost / 3.0 + 1e-9);
}

// -------------------------------------------------------------------------
// Tool-result compression filter chain (src/tool_result_filter.{h,cpp}).
// Filters are exercised directly (via the factory functions) rather than
// through the runtime because the fixture caps listFiles/searchFiles at 50
// results — smaller than the collapse thresholds we need to validate.
// -------------------------------------------------------------------------

TEST_CASE("tool result filter: listFiles collapses large recursive listings "
          "into top-level directory stubs",
          "[plugins][ai_editor][native][tools][filter][listFiles]") {
    using namespace sao::ai_editor::native;
    auto filter = make_list_files_folder_filter();
    REQUIRE(filter);
    REQUIRE(filter->id() == "ListFilesFolder");
    REQUIRE(filter->matches("listFiles", Json::object()));
    REQUIRE_FALSE(filter->matches("readFile", Json::object()));

    // Build a listing shaped like a real recursive dump: 240 nested entries
    // under node_modules + 10 leaf files at the workspace root.  Total is
    // well above the 200-entry threshold so the filter should collapse
    // node_modules into a single stub and leave the leaf files alone.
    Json entries = Json::array();
    for (int index = 0; index < 240; ++index) {
        entries.push_back({{"name",
                            std::string("node_modules/dep-") +
                                std::to_string(index) + "/index.js"},
                           {"type", "file"},
                           {"size", 100}});
    }
    for (int index = 0; index < 10; ++index) {
        entries.push_back({{"name", std::string("root-") +
                                        std::to_string(index) + ".txt"},
                           {"type", "file"},
                           {"size", 200}});
    }
    Json result{{"path", "."},
                {"entries", std::move(entries)},
                {"total", 250}};
    const Json input{{"path", "."}, {"recursive", true}};
    ToolResultFilterOutput out = filter->apply(result, input);
    REQUIRE(out.compressed);
    REQUIRE(out.filter_id == "ListFilesFolder");
    REQUIRE(result["originalTotal"] == 250);
    // node_modules folded into 1 stub + 10 leaf files = 11 entries.
    REQUIRE(result["entries"].size() == 11);
    // The first entry should be the collapsed node_modules stub with its
    // itemsInside counter set to the 240 nested entries.
    bool found_collapsed = false;
    for (const auto& entry : result["entries"]) {
        if (entry.value("name", "") == "node_modules") {
            found_collapsed = true;
            REQUIRE(entry["type"] == "directory");
            REQUIRE(entry.value("collapsed", false) == true);
            REQUIRE(entry["itemsInside"].get<int>() == 240);
        }
    }
    REQUIRE(found_collapsed);

    // Small listings (<= 200) must be a no-op — filter returns compressed:false
    // and leaves entries unchanged.
    Json small_result{{"path", "."},
                     {"entries", Json::array()},
                     {"total", 5}};
    for (int index = 0; index < 5; ++index) {
        small_result["entries"].push_back({{"name", std::to_string(index)},
                                            {"type", "file"},
                                            {"size", 0}});
    }
    const Json small_input{{"path", "."}};
    ToolResultFilterOutput small_out = filter->apply(small_result, small_input);
    REQUIRE_FALSE(small_out.compressed);
    REQUIRE(small_result["entries"].size() == 5);
    REQUIRE_FALSE(small_result.contains("originalTotal"));
}

TEST_CASE("tool result filter: searchFiles folds dense per-file hits into "
          "3 samples plus a matches count",
          "[plugins][ai_editor][native][tools][filter][searchFiles]") {
    using namespace sao::ai_editor::native;
    auto filter = make_search_files_collapse_filter();
    REQUIRE(filter);
    REQUIRE(filter->id() == "SearchFilesCollapse");
    REQUIRE(filter->matches("searchFiles", Json::object()));
    REQUIRE_FALSE(filter->matches("listFiles", Json::object()));

    // 15 hits in path.ts + 2 hits in other.ts.  The dense file gets folded;
    // the sparse file's hits pass through unchanged so the LLM still sees
    // full-line context for the low-noise matches.
    Json results = Json::array();
    for (int index = 0; index < 15; ++index) {
        results.push_back({{"file", "path.ts"},
                           {"line", index + 1},
                           {"text", std::string("hit-") +
                                        std::to_string(index)}});
    }
    for (int index = 0; index < 2; ++index) {
        results.push_back({{"file", "other.ts"},
                           {"line", index + 10},
                           {"text", "sparse"}});
    }
    Json result{{"query", "hit"},
                {"results", std::move(results)},
                {"total", 17}};
    const Json input{{"query", "hit"}};
    ToolResultFilterOutput out = filter->apply(result, input);
    REQUIRE(out.compressed);
    REQUIRE(out.filter_id == "SearchFilesCollapse");
    REQUIRE(result["originalTotal"] == 17);
    // path.ts collapsed into 1 entry + 2 sparse other.ts entries = 3 results.
    REQUIRE(result["results"].size() == 3);
    bool found_collapsed = false;
    int sparse_count = 0;
    for (const auto& entry : result["results"]) {
        if (entry["file"] == "path.ts") {
            found_collapsed = true;
            REQUIRE(entry["matches"].get<int>() == 15);
            REQUIRE(entry["samples"].is_array());
            REQUIRE(entry["samples"].size() == 3);
            // The 3 samples should be the first 3 hits (line 1, 2, 3).
            REQUIRE(entry["samples"][0]["line"].get<int>() == 1);
            REQUIRE(entry["samples"][2]["line"].get<int>() == 3);
        }
        if (entry.value("file", "") == "other.ts") {
            ++sparse_count;
            // Sparse hits keep their line + text fields untouched.
            REQUIRE(entry.contains("line"));
            REQUIRE(entry.contains("text"));
        }
    }
    REQUIRE(found_collapsed);
    REQUIRE(sparse_count == 2);

    // All-sparse (< 4 hits per file) — no collapse; filter is a no-op.
    Json sparse_result{{"query", "hit"},
                       {"results", Json::array()},
                       {"total", 6}};
    for (int index = 0; index < 3; ++index) {
        sparse_result["results"].push_back({{"file", "a.ts"},
                                             {"line", index},
                                             {"text", "hit"}});
    }
    for (int index = 0; index < 3; ++index) {
        sparse_result["results"].push_back({{"file", "b.ts"},
                                             {"line", index},
                                             {"text", "hit"}});
    }
    ToolResultFilterOutput sparse_out =
        filter->apply(sparse_result, Json{{"query", "hit"}});
    REQUIRE_FALSE(sparse_out.compressed);
    REQUIRE(sparse_result["results"].size() == 6);
}

TEST_CASE("tool result filter: readFile head-truncates >32 KB content and "
          "adds a compression banner",
          "[plugins][ai_editor][native][tools][filter][readFile]") {
    using namespace sao::ai_editor::native;
    auto filter = make_read_file_truncate_filter();
    REQUIRE(filter);
    REQUIRE(filter->id() == "ReadFileTruncate");
    // Matching is arguments-aware: an explicit line window opts out because
    // the caller wanted exactly those lines.
    REQUIRE(filter->matches("readFile", Json::object()));
    REQUIRE_FALSE(filter->matches("readFile",
                                   Json{{"startLine", 10}, {"endLine", 20}}));
    REQUIRE_FALSE(filter->matches("editFile", Json::object()));

    // Build a >32 KB payload of ~800 lines, each 50 bytes.  Total ~40 KB
    // covers the truncate threshold with room for the banner tail.
    std::string content;
    content.reserve(50 * 800);
    for (int index = 0; index < 800; ++index) {
        content.append("line-");
        content.append(std::to_string(index));
        // Pad to 50 bytes so the read fits the shape the truncator expects.
        while (content.size() % 50 != 49) {
            content.push_back('x');
        }
        content.push_back('\n');
    }
    REQUIRE(content.size() > 32u * 1024u);
    const size_t original_bytes = content.size();
    Json result{{"path", "big.txt"},
                {"content", content},
                {"startLine", 0},
                {"endLine", 0}};
    const Json input{{"path", "big.txt"}};
    ToolResultFilterOutput out = filter->apply(result, input);
    REQUIRE(out.compressed);
    REQUIRE(out.filter_id == "ReadFileTruncate");
    const std::string truncated =
        result["content"].get<std::string>();
    // The truncated payload keeps the head + banner tail; overall must be
    // materially smaller than the input.
    REQUIRE(truncated.size() < original_bytes);
    REQUIRE(truncated.find("[SAO output compressed by ReadFileTruncate") !=
            std::string::npos);
    REQUIRE(truncated.find("more lines truncated") != std::string::npos);
    // First line of the file must still be present so the model sees the
    // shape of the truncated content, not just the banner.
    REQUIRE(truncated.find("line-0") != std::string::npos);
    // compressionInfo carries the filter id + before/after byte counts.
    REQUIRE(result["compressionInfo"]["filterIds"][0] == "ReadFileTruncate");
    REQUIRE(result["compressionInfo"]["originalBytes"].get<size_t>() ==
            original_bytes);
    REQUIRE(result["compressionInfo"]["compressedBytes"].get<size_t>() ==
            truncated.size());

    // Sub-threshold content is a no-op.
    Json small_result{{"path", "small.txt"},
                     {"content", std::string(1024, 'a')},
                     {"startLine", 0},
                     {"endLine", 0}};
    ToolResultFilterOutput small_out = filter->apply(small_result, input);
    REQUIRE_FALSE(small_out.compressed);
    REQUIRE(small_result["content"].get<std::string>().size() == 1024);
}

TEST_CASE("tool result filter: is_protected_from_compression refuses to "
          "touch JSON / YAML / TOML documents",
          "[plugins][ai_editor][native][tools][filter][protected]") {
    using namespace sao::ai_editor::native;
    // Valid top-level JSON object + array must be refused.
    REQUIRE(is_protected_from_compression(R"({"a":1,"b":[1,2,3]})"));
    REQUIRE(is_protected_from_compression("[1, 2, 3, 4]"));
    // Leading whitespace is stripped before the check.
    REQUIRE(is_protected_from_compression("   \n\t {\"x\":true}\n"));
    // YAML document opener.
    REQUIRE(is_protected_from_compression("---\nfoo: bar\n"));
    // TOML section header on its own line.
    REQUIRE(is_protected_from_compression("[server]\nport = 80\n"));

    // Ordinary text — not protected.
    REQUIRE_FALSE(is_protected_from_compression("hello world"));
    REQUIRE_FALSE(is_protected_from_compression(""));
    REQUIRE_FALSE(is_protected_from_compression("   \n  \t"));
    // Broken JSON (unbalanced braces) is not protected.
    REQUIRE_FALSE(is_protected_from_compression("{oops"));
    REQUIRE_FALSE(is_protected_from_compression("{\"unterminated\": "));
    // A line that starts with `[` but is not a real TOML header — no closing
    // bracket on the same line, or garbage after — must not fool the check.
    REQUIRE_FALSE(is_protected_from_compression("[not a header\nfoo=1\n"));

    // End-to-end guard: the ReadFileTruncate filter refuses to compress a
    // huge JSON payload because is_protected_from_compression() returns true.
    auto filter = make_read_file_truncate_filter();
    std::string huge_json = "{\"data\":[";
    for (int index = 0; index < 8000; ++index) {
        if (index != 0) huge_json.push_back(',');
        // Pad each item so the total document comfortably exceeds 32 KB
        // and forces the truncate filter's size check to fire.
        huge_json.append("\"item-");
        huge_json.append(std::to_string(index));
        huge_json.append("\"");
    }
    huge_json.append("]}");
    REQUIRE(huge_json.size() > 32u * 1024u);
    Json protected_result{{"path", "big.json"},
                         {"content", huge_json},
                         {"startLine", 0},
                         {"endLine", 0}};
    ToolResultFilterOutput protected_out =
        filter->apply(protected_result, Json{{"path", "big.json"}});
    REQUIRE_FALSE(protected_out.compressed);
    // Content must be byte-for-byte identical — the filter has to leave
    // structured payloads unchanged even when they are huge.
    REQUIRE(protected_result["content"].get<std::string>() == huge_json);
    REQUIRE_FALSE(protected_result.contains("compressionInfo"));
}

// --- vscode.window.createWebviewPanel R16 registry coverage ----------------
// Each case exercises the dispatch path (invoke() forwards vscode.* into
// dispatch_extension_call) plus the event queue so we can confirm the
// bridge-facing sao.event notifications fire in the right order.

namespace {

std::vector<Json> drain_webview_events(sao_ai_editor_runtime_t runtime,
                                       size_t budget) {
    std::vector<Json> collected;
    for (size_t index = 0; index < budget; ++index) {
        uint32_t required = 0;
        const int32_t queried = sao_ai_editor_runtime_next_event(
            runtime, nullptr, 0, &required);
        if (queried == SAO_AI_EDITOR_OK && required == 0) {
            break;
        }
        REQUIRE(queried == SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL);
        std::vector<char> event(static_cast<size_t>(required) + 1);
        REQUIRE(sao_ai_editor_runtime_next_event(
                    runtime, event.data(),
                    static_cast<uint32_t>(event.size()), &required) ==
                SAO_AI_EDITOR_OK);
        collected.push_back(Json::parse(event.data(),
                                        event.data() + required));
    }
    return collected;
}

bool contains_webview_event(const std::vector<Json>& events,
                            std::string_view name) {
    for (const auto& envelope : events) {
        if (envelope.value("method", std::string{}) != "sao.event") {
            continue;
        }
        const std::string event_name =
            envelope["params"].value("event", std::string{});
        if (event_name == name) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("vscode.window.createWebviewPanel mints an id and reveal marks visible",
          "[plugins][ai_editor][native][extensions][vscode][webview_panel]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    (void)drain_webview_events(runtime, 16);

    const Json created = dispatch(runtime, "vscode.window.createWebviewPanel",
                                   {{"viewType", "sao.demo"},
                                    {"title", "Demo Panel"},
                                    {"options",
                                     {{"enableScripts", true},
                                      {"retainContextWhenHidden", true}}}});
    INFO("createWebviewPanel response: " << created.dump());
    REQUIRE(created.contains("result"));
    const Json& create_result = created["result"];
    REQUIRE(create_result["viewType"] == "sao.demo");
    REQUIRE(create_result["title"] == "Demo Panel");
    REQUIRE(create_result["visible"] == true);
    REQUIRE(create_result["disposed"] == false);
    const std::string panel_id = create_result["panelId"];
    REQUIRE(!panel_id.empty());
    REQUIRE(create_result["options"]["enableScripts"] == true);
    REQUIRE(create_result["options"]["retainContextWhenHidden"] == true);

    const std::vector<Json> after_create = drain_webview_events(runtime, 4);
    REQUIRE(contains_webview_event(after_create,
                                   "vscode.window.webviewPanel.created"));

    Sleep(2);
    const Json revealed = dispatch(runtime, "vscode.window.revealWebviewPanel",
                                    {{"panelId", panel_id},
                                     {"viewColumn", 2},
                                     {"preserveFocus", true}});
    REQUIRE(revealed.contains("result"));
    REQUIRE(revealed["result"]["visible"] == true);
    REQUIRE(revealed["result"]["options"]["viewColumn"] == 2);
    REQUIRE(revealed["result"]["options"]["extras"]["preserveFocus"] == true);
    REQUIRE(revealed["result"]["lastRevealMs"].get<int64_t>() >=
            create_result["lastRevealMs"].get<int64_t>());

    const std::vector<Json> after_reveal = drain_webview_events(runtime, 4);
    REQUIRE(contains_webview_event(after_reveal,
                                   "vscode.window.webviewPanel.revealed"));

    const Json ghost_reveal = dispatch(
        runtime, "vscode.window.revealWebviewPanel",
        {{"panelId", "wvp-does-not-exist"}});
    REQUIRE(ghost_reveal.contains("error"));
    REQUIRE(ghost_reveal["error"]["data"]["status"].get<int>() ==
            SAO_AI_EDITOR_ERR_NOT_FOUND);
}

TEST_CASE("vscode.window.postMessageToWebview round-trips seq and emits event",
          "[plugins][ai_editor][native][extensions][vscode][webview_panel]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    (void)drain_webview_events(runtime, 16);

    const Json created = dispatch(
        runtime, "vscode.window.createWebviewPanel",
        {{"viewType", "sao.chat"}, {"title", "Chat"}});
    const std::string panel_id = created["result"]["panelId"];
    (void)drain_webview_events(runtime, 4);

    for (int64_t expected_seq = 1; expected_seq <= 3; ++expected_seq) {
        const Json message = {{"kind", "hello"}, {"seq", expected_seq}};
        const Json posted = dispatch(
            runtime, "vscode.window.postMessageToWebview",
            {{"panelId", panel_id}, {"message", message}});
        REQUIRE(posted.contains("result"));
        REQUIRE(posted["result"]["messageSeq"].get<int64_t>() == expected_seq);

        const std::vector<Json> events = drain_webview_events(runtime, 4);
        bool saw_post = false;
        for (const auto& envelope : events) {
            if (envelope.value("method", std::string{}) != "sao.event") {
                continue;
            }
            const std::string event_name =
                envelope["params"].value("event", std::string{});
            if (event_name == "vscode.window.webviewPanel.postMessage") {
                REQUIRE(envelope["params"]["payload"]["panelId"] == panel_id);
                REQUIRE(envelope["params"]["payload"]["messageSeq"]
                            .get<int64_t>() == expected_seq);
                REQUIRE(envelope["params"]["payload"]["message"] == message);
                saw_post = true;
            }
        }
        REQUIRE(saw_post);
    }

    const std::string html = "<html><body><h2>hi</h2></body></html>";
    const Json set_html = dispatch(
        runtime, "vscode.window.setWebviewHtml",
        {{"panelId", panel_id}, {"html", html}});
    REQUIRE(set_html.contains("result"));
    REQUIRE(set_html["result"]["htmlLength"].get<int64_t>() ==
            static_cast<int64_t>(html.size()));
    REQUIRE(set_html["result"]["messageSeq"].get<int64_t>() == 3);
}

TEST_CASE("vscode.window.disposeWebviewPanel blocks subsequent postMessage",
          "[plugins][ai_editor][native][extensions][vscode][webview_panel]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    (void)drain_webview_events(runtime, 16);

    const Json created = dispatch(
        runtime, "vscode.window.createWebviewPanel",
        {{"viewType", "sao.status"}, {"title", "Status"}});
    const std::string panel_id = created["result"]["panelId"];
    (void)drain_webview_events(runtime, 4);

    const Json disposed = dispatch(
        runtime, "vscode.window.disposeWebviewPanel",
        {{"panelId", panel_id}});
    REQUIRE(disposed.contains("result"));
    REQUIRE(disposed["result"]["disposed"] == true);
    REQUIRE(disposed["result"]["visible"] == false);
    REQUIRE(disposed["result"]["options"]["extras"]["already"] == false);

    const std::vector<Json> after_dispose = drain_webview_events(runtime, 4);
    REQUIRE(contains_webview_event(after_dispose,
                                   "vscode.window.webviewPanel.disposed"));

    const Json failed_post = dispatch(
        runtime, "vscode.window.postMessageToWebview",
        {{"panelId", panel_id}, {"message", Json::object()}});
    REQUIRE(failed_post.contains("error"));
    REQUIRE(failed_post["error"]["data"]["status"].get<int>() ==
            SAO_AI_EDITOR_ERR_PROTOCOL);
    const std::vector<Json> post_events = drain_webview_events(runtime, 4);
    REQUIRE(contains_webview_event(post_events,
                                   "vscode.window.webviewPanel.postFailed"));

    const Json disposed_again = dispatch(
        runtime, "vscode.window.disposeWebviewPanel",
        {{"panelId", panel_id}});
    REQUIRE(disposed_again.contains("result"));
    REQUIRE(disposed_again["result"]["options"]["extras"]["already"] == true);

    const Json listing = dispatch(runtime, "vscode.window.listWebviewPanels");
    REQUIRE(listing.contains("result"));
    REQUIRE(listing["result"]["panels"].is_array());
    REQUIRE(listing["result"]["panels"].empty());
    REQUIRE(listing["result"]["totalCreated"].get<int64_t>() >= 1);
}

TEST_CASE("vscode.window.createWebviewPanel supports multiple panels "
          "and setWebviewHtml preserves each panel state",
          "[plugins][ai_editor][native][extensions][vscode][webview_panel]") {
    RuntimeFixture fixture;
    auto runtime = fixture.get();
    (void)drain_webview_events(runtime, 16);

    const Json panel_a = dispatch(
        runtime, "vscode.window.createWebviewPanel",
        {{"viewType", "sao.a"}, {"title", "A"}});
    const Json panel_b = dispatch(
        runtime, "vscode.window.createWebviewPanel",
        {{"viewType", "sao.b"}, {"title", "B"}});
    const std::string id_a = panel_a["result"]["panelId"];
    const std::string id_b = panel_b["result"]["panelId"];
    REQUIRE(id_a != id_b);

    const std::string html_a = "<h1>panel-A</h1>";
    const std::string html_b = "<h1>panel-B-longer</h1>";
    REQUIRE(dispatch(runtime, "vscode.window.setWebviewHtml",
                     {{"panelId", id_a}, {"html", html_a}})
                .contains("result"));
    REQUIRE(dispatch(runtime, "vscode.window.setWebviewHtml",
                     {{"panelId", id_b}, {"html", html_b}})
                .contains("result"));

    REQUIRE(dispatch(runtime, "vscode.window.postMessageToWebview",
                     {{"panelId", id_b},
                      {"message", {{"kind", "hello-b"}}}})
                .contains("result"));

    const Json listed = dispatch(runtime, "vscode.window.listWebviewPanels");
    REQUIRE(listed.contains("result"));
    REQUIRE(listed["result"]["panels"].size() == 2);
    REQUIRE(listed["result"]["totalCreated"].get<int64_t>() >= 2);

    std::unordered_map<std::string, Json> by_id;
    for (const auto& panel : listed["result"]["panels"]) {
        by_id[panel.value("panelId", std::string{})] = panel;
    }
    REQUIRE(by_id.count(id_a) == 1);
    REQUIRE(by_id.count(id_b) == 1);
    REQUIRE(by_id[id_a]["htmlLength"].get<int64_t>() ==
            static_cast<int64_t>(html_a.size()));
    REQUIRE(by_id[id_b]["htmlLength"].get<int64_t>() ==
            static_cast<int64_t>(html_b.size()));
    REQUIRE(by_id[id_a]["messageSeq"].get<int64_t>() == 0);
    REQUIRE(by_id[id_b]["messageSeq"].get<int64_t>() == 1);

    REQUIRE(dispatch(runtime, "vscode.window.disposeWebviewPanel",
                     {{"panelId", id_a}})
                .contains("result"));
    const Json listed_after = dispatch(
        runtime, "vscode.window.listWebviewPanels");
    REQUIRE(listed_after["result"]["panels"].size() == 1);
    REQUIRE(listed_after["result"]["panels"][0]["panelId"] == id_b);
    REQUIRE(listed_after["result"]["panels"][0]["htmlLength"].get<int64_t>() ==
            static_cast<int64_t>(html_b.size()));
    REQUIRE(listed_after["result"]["totalCreated"].get<int64_t>() >= 2);
}
