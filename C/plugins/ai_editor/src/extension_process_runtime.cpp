#include "extension_process_runtime.h"

#include "native_utils.h"
#include "sao/ai_editor/ai_editor_status.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE \
    ProcThreadAttributeValue(22, FALSE, TRUE, FALSE)
#endif

namespace sao::ai_editor::native {
namespace {

constexpr std::size_t kMaximumSessions = 128;
constexpr std::size_t kMaximumOutputBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaximumRequestBytes = 1024U * 1024U;
constexpr std::size_t kMaximumEventBytes = 5U * 1024U * 1024U;
constexpr std::size_t kMaximumDapMessageBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaximumDapBufferedBytes = 8U * 1024U * 1024U;
constexpr std::size_t kMaximumDapHeaderBytes = 16U * 1024U;
constexpr std::size_t kMaximumDapDepth = 64;
constexpr std::size_t kMaximumDapNodes = 16384;
constexpr std::size_t kMaximumQueuedEvents = 4096;
constexpr std::size_t kMaximumQueuedEventBytes = 32U * 1024U * 1024U;
constexpr std::size_t kMaximumQueuedWrites = 1024;
constexpr std::size_t kMaximumQueuedWriteBytes = 8U * 1024U * 1024U;
constexpr DWORD kStopWaitMilliseconds = 2000;
constexpr DWORD kConnectWaitMilliseconds = 10000;
constexpr uint64_t kMaximumSafeJsonInteger = UINT64_C(9007199254740991);

std::mutex& process_spawn_mutex() {
    static std::mutex mutex;
    return mutex;
}

class StandardHandleInheritanceGuard final {
  public:
    explicit StandardHandleInheritanceGuard(bool enabled) {
        if (!enabled) return;
        constexpr DWORD ids[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
        for (const DWORD id : ids) {
            const HANDLE handle = GetStdHandle(id);
            DWORD flags = 0;
            if (handle != nullptr && handle != INVALID_HANDLE_VALUE &&
                GetHandleInformation(handle, &flags)) {
                entries_[count_++] = {id, handle, flags};
                if ((flags & HANDLE_FLAG_INHERIT) != 0)
                    (void)SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);
                (void)SetStdHandle(id, nullptr);
            }
        }
    }

    ~StandardHandleInheritanceGuard() {
        for (std::size_t index = 0; index < count_; ++index) {
            (void)SetStdHandle(entries_[index].id, entries_[index].handle);
            if ((entries_[index].flags & HANDLE_FLAG_INHERIT) != 0) {
                (void)SetHandleInformation(entries_[index].handle,
                                           HANDLE_FLAG_INHERIT,
                                           HANDLE_FLAG_INHERIT);
            }
        }
    }

    StandardHandleInheritanceGuard(const StandardHandleInheritanceGuard&) = delete;
    StandardHandleInheritanceGuard& operator=(const StandardHandleInheritanceGuard&) = delete;

  private:
    struct Entry final {
        DWORD id{};
        HANDLE handle{};
        DWORD flags{};
    };

    std::array<Entry, 3> entries_{};
    std::size_t count_{};
};

class UniqueHandle final {
  public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    HANDLE get() const noexcept { return handle_; }
    HANDLE release() noexcept { return std::exchange(handle_, INVALID_HANDLE_VALUE); }
    void reset(HANDLE handle = INVALID_HANDLE_VALUE) noexcept {
        if (valid()) CloseHandle(handle_);
        handle_ = handle;
    }

  private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

struct PseudoConsoleApi final {
    using CreateFn = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
    using ResizeFn = HRESULT(WINAPI*)(HPCON, COORD);
    using CloseFn = void(WINAPI*)(HPCON);

    CreateFn create{};
    ResizeFn resize{};
    CloseFn close{};

    static const PseudoConsoleApi& instance() {
        static const PseudoConsoleApi api = [] {
            PseudoConsoleApi value;
            HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
            if (kernel != nullptr) {
                value.create = reinterpret_cast<CreateFn>(
                    GetProcAddress(kernel, "CreatePseudoConsole"));
                value.resize = reinterpret_cast<ResizeFn>(
                    GetProcAddress(kernel, "ResizePseudoConsole"));
                value.close = reinterpret_cast<CloseFn>(
                    GetProcAddress(kernel, "ClosePseudoConsole"));
            }
            return value;
        }();
        return api;
    }

    bool available() const noexcept { return create && resize && close; }
};

std::wstring quote_argument(std::wstring_view value) {
    if (value.empty()) return L"\"\"";
    const bool needs_quotes = value.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
    if (!needs_quotes) return std::wstring(value);
    std::wstring output;
    output.push_back(L'\"');
    std::size_t slashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        if (character == L'\"') {
            output.append(slashes * 2 + 1, L'\\');
            output.push_back(L'\"');
            slashes = 0;
            continue;
        }
        output.append(slashes, L'\\');
        slashes = 0;
        output.push_back(character);
    }
    output.append(slashes * 2, L'\\');
    output.push_back(L'\"');
    return output;
}

bool append_argument(std::wstring& command_line, std::string_view value) {
    if (!valid_utf8(value) || value.size() > 32768) return false;
    const std::wstring wide = utf8_to_wide(value);
    if (!value.empty() && wide.empty()) return false;
    if (!command_line.empty()) command_line.push_back(L' ');
    command_line += quote_argument(wide);
    return command_line.size() <= 32767;
}

bool valid_identity(std::string_view value) {
    return !value.empty() && value.size() <= 256 && valid_utf8(value) &&
           value.find('\0') == std::string_view::npos;
}

bool component_equal(const std::filesystem::path& left,
                     const std::filesystem::path& right) {
    std::wstring a = left.native();
    std::wstring b = right.native();
    std::transform(a.begin(), a.end(), a.begin(),
                   [](wchar_t c) { return std::towlower(c); });
    std::transform(b.begin(), b.end(), b.begin(),
                   [](wchar_t c) { return std::towlower(c); });
    return a == b;
}

bool path_within(const std::filesystem::path& root,
                 const std::filesystem::path& candidate) {
    auto r = root.begin();
    auto c = candidate.begin();
    for (; r != root.end(); ++r, ++c) {
        if (c == candidate.end() || !component_equal(*r, *c)) return false;
    }
    return true;
}

std::optional<std::filesystem::path> normalize_existing_directory(
    const std::filesystem::path& root, const nlohmann::json& value) {
    std::filesystem::path requested = root;
    try {
        if (value.is_string()) {
            std::string raw = value.get<std::string>();
            if (raw.rfind("file:///", 0) == 0) {
                raw.erase(0, 8);
                std::string decoded;
                decoded.reserve(raw.size());
                const auto hex = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return -1;
                };
                for (std::size_t index = 0; index < raw.size(); ++index) {
                    if (raw[index] == '%' && index + 2 < raw.size()) {
                        const int high = hex(raw[index + 1]);
                        const int low = hex(raw[index + 2]);
                        if (high < 0 || low < 0) return std::nullopt;
                        decoded.push_back(static_cast<char>((high << 4) | low));
                        index += 2;
                    } else {
                        decoded.push_back(raw[index]);
                    }
                }
                raw = std::move(decoded);
            }
            if (raw.find('\0') != std::string::npos) return std::nullopt;
            std::replace(raw.begin(), raw.end(), '/', '\\');
            const std::wstring wide = utf8_to_wide(raw);
            if (!raw.empty() && wide.empty()) return std::nullopt;
            requested = std::filesystem::path(wide);
        } else if (value.is_object()) {
            const std::string raw = value.value("fsPath", value.value("path", std::string{}));
            if (!raw.empty()) {
                if (raw.find('\0') != std::string::npos) return std::nullopt;
                const std::wstring wide = utf8_to_wide(raw);
                if (wide.empty()) return std::nullopt;
                requested = std::filesystem::path(wide);
            }
        } else if (!value.is_null()) {
            return std::nullopt;
        }
        if (requested.is_relative()) requested = root / requested;
        std::error_code error;
        const std::filesystem::path canonical_root =
            std::filesystem::weakly_canonical(root, error);
        if (error) return std::nullopt;
        const std::filesystem::path canonical =
            std::filesystem::weakly_canonical(requested, error);
        if (error || !path_within(canonical_root, canonical) ||
            !std::filesystem::is_directory(canonical, error) || error) {
            return std::nullopt;
        }
        return canonical;
    } catch (...) {
        return std::nullopt;
    }
}

struct CaseInsensitiveLess final {
    bool operator()(const std::wstring& left, const std::wstring& right) const {
        return _wcsicmp(left.c_str(), right.c_str()) < 0;
    }
};

bool build_environment(const nlohmann::json& options, std::vector<wchar_t>& output) {
    output.clear();
    const bool strict = options.value("strictEnv", false);
    std::map<std::wstring, std::wstring, CaseInsensitiveLess> values;
    if (!strict) {
        LPWCH raw = GetEnvironmentStringsW();
        if (raw == nullptr) return false;
        for (const wchar_t* cursor = raw; *cursor != L'\0';) {
            std::wstring entry(cursor);
            cursor += entry.size() + 1;
            const std::size_t split = entry.find(L'=', entry.front() == L'=' ? 1 : 0);
            if (split != std::wstring::npos) {
                values[entry.substr(0, split)] = entry.substr(split + 1);
            }
        }
        FreeEnvironmentStringsW(raw);
    }
    if (options.contains("env")) {
        if (!options["env"].is_object() || options["env"].size() > 1024) return false;
        for (const auto& [key, value] : options["env"].items()) {
            if (key.empty() || key.size() > 32767 || key.find('=') != std::string::npos ||
                key.find('\0') != std::string::npos || !valid_utf8(key)) {
                return false;
            }
            const std::wstring wide_key = utf8_to_wide(key);
            if (value.is_null()) {
                values.erase(wide_key);
                continue;
            }
            if (!value.is_string()) return false;
            const std::string text = value.get<std::string>();
            if (text.size() > 32767 || text.find('\0') != std::string::npos ||
                !valid_utf8(text)) {
                return false;
            }
            values[wide_key] = utf8_to_wide(text);
        }
    }
    std::size_t units = 1;
    for (const auto& [key, value] : values) {
        if (key.empty()) continue;
        if (key.size() > (std::numeric_limits<std::size_t>::max)() - value.size() - 2)
            return false;
        units += key.size() + value.size() + 2;
    }
    if (units > 1024U * 1024U) return false;
    output.reserve(units);
    for (const auto& [key, value] : values) {
        if (key.empty()) continue;
        output.insert(output.end(), key.begin(), key.end());
        output.push_back(L'=');
        output.insert(output.end(), value.begin(), value.end());
        output.push_back(L'\0');
    }
    if (output.empty()) output.push_back(L'\0');
    output.push_back(L'\0');
    return true;
}

std::optional<uint64_t> owner_generation(const nlohmann::json& params) {
    if (!params.contains("generation")) return uint64_t{0};
    if (!params["generation"].is_number_unsigned()) return std::nullopt;
    const uint64_t value = params["generation"].get<uint64_t>();
    if (value > kMaximumSafeJsonInteger) return std::nullopt;
    return value;
}

std::string json_string(const nlohmann::json& object, const char* first,
                        const char* second = nullptr) {
    if (object.contains(first) && object[first].is_string())
        return object[first].get<std::string>();
    if (second && object.contains(second) && object[second].is_string())
        return object[second].get<std::string>();
    return {};
}

} // namespace

struct ExtensionProcessRuntime::Impl final {
    using Json = ExtensionProcessRuntime::Json;

    enum class Kind : uint8_t { terminal, task, debug };

    struct Session final {
        std::recursive_mutex lifecycle_mutex;
        mutable std::mutex mutex;
        std::mutex write_mutex;
        std::condition_variable write_ready;
        Kind kind{Kind::terminal};
        std::string id;
        std::string terminal_id;
        std::string name;
        std::string owner_extension;
        uint64_t owner_generation{};
        Json definition = Json::object();
        Json configuration = Json::object();
        std::atomic<bool> running{true};
        std::atomic<bool> closing{false};
        std::atomic<bool> finished{false};
        std::atomic<bool> reader_done{false};
        std::atomic<bool> stderr_done{true};
        std::atomic<bool> writer_done{true};
        std::atomic<bool> monitor_done{true};
        std::atomic<uint64_t> sequence{0};
        bool extension_pty{};
        bool inline_debug{};
        std::atomic<bool> visible{false};
        std::atomic<bool> active{false};
        DWORD process_id{};
        DWORD exit_code{STILL_ACTIVE};
        COORD dimensions{80, 30};
        HPCON pseudo_console{};
        UniqueHandle process;
        UniqueHandle job;
        UniqueHandle read;
        UniqueHandle write;
        UniqueHandle stderr_read;
        std::atomic<SOCKET> socket{INVALID_SOCKET};
        bool winsock_started{};
        bool writer_stopping{};
        std::size_t queued_write_bytes{};
        std::deque<std::string> write_queue;
        std::thread reader;
        std::thread stderr_reader;
        std::thread writer;
        std::thread monitor;
        std::string output;

        ~Session() {
            if (pseudo_console) {
                const auto& api = PseudoConsoleApi::instance();
                if (api.close) api.close(pseudo_console);
                pseudo_console = nullptr;
            }
            const SOCKET value = socket.exchange(INVALID_SOCKET,
                                                  std::memory_order_acq_rel);
            if (value != INVALID_SOCKET) {
                ::shutdown(value, SD_BOTH);
                closesocket(value);
            }
            if (winsock_started) WSACleanup();
        }
    };

    struct QueuedEvent final {
        std::string kind;
        Json payload;
        std::size_t encoded_bytes{};
    };

    struct EventChannel final {
        explicit EventChannel(EventSink value) : sink(std::move(value)) {}

        std::mutex mutex;
        std::condition_variable ready;
        std::deque<QueuedEvent> queue;
        std::size_t queued_bytes{};
        bool stopping{};
        EventSink sink;
    };

    explicit Impl(std::filesystem::path root, EventSink sink)
        : workspace_root(std::move(root)),
          events(std::make_shared<EventChannel>(std::move(sink))) {
        const auto channel = events;
        event_thread = std::thread([channel] { dispatch_events(channel); });
    }

    ~Impl() { shutdown(); }

    mutable std::mutex mutex;
    std::filesystem::path workspace_root;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions;
    std::unordered_map<std::string, std::string> terminal_aliases;
    std::string active_terminal;
    uint64_t next_id{1};
    bool stopping{};

    std::shared_ptr<EventChannel> events;
    std::thread event_thread;

    static void dispatch_events(const std::shared_ptr<EventChannel>& channel) noexcept {
        for (;;) {
            QueuedEvent event;
            EventSink sink;
            {
                std::unique_lock lock(channel->mutex);
                channel->ready.wait(lock, [&] {
                    return channel->stopping || !channel->queue.empty();
                });
                if (channel->queue.empty() && channel->stopping) return;
                event = std::move(channel->queue.front());
                channel->queue.pop_front();
                channel->queued_bytes -= event.encoded_bytes;
                try {
                    sink = channel->sink;
                } catch (...) {
                }
            }
            try {
                if (sink) sink(event.kind, event.payload);
            } catch (...) {
            }
        }
    }

    static bool lossy_event(std::string_view kind, const Json& payload) {
        const std::string op = payload.value("op", std::string{});
        return (kind == "terminal" &&
            (op == "data" || op == "active" || op == "dimensions")) ||
               (kind == "tasks" && op == "output") ||
               (kind == "debug" && (op == "output" || op == "dap"));
    }

    static void emit_event(const std::shared_ptr<EventChannel>& channel,
                           std::string kind, Json payload) noexcept {
        try {
            const std::string encoded = payload.dump();
            if (encoded.size() > kMaximumEventBytes) return;
            std::lock_guard lock(channel->mutex);
            if (channel->stopping) return;
            while (channel->queue.size() >= kMaximumQueuedEvents ||
                   encoded.size() > kMaximumQueuedEventBytes -
                                        channel->queued_bytes) {
                const auto stale = std::find_if(
                    channel->queue.begin(), channel->queue.end(),
                    [](const QueuedEvent& event) {
                        return Impl::lossy_event(event.kind, event.payload);
                    });
                if (stale != channel->queue.end()) {
                    channel->queued_bytes -= stale->encoded_bytes;
                    channel->queue.erase(stale);
                } else if (lossy_event(kind, payload) || channel->queue.empty()) {
                    return;
                } else {
                    channel->queued_bytes -= channel->queue.front().encoded_bytes;
                    channel->queue.pop_front();
                }
            }
            channel->queued_bytes += encoded.size();
            channel->queue.push_back(
                {std::move(kind), std::move(payload), encoded.size()});
            channel->ready.notify_one();
        } catch (...) {
        }
    }

    void emit(std::string kind, Json payload) noexcept {
        emit_event(events, std::move(kind), std::move(payload));
    }

    std::string allocate_id(std::string_view prefix) {
        std::lock_guard lock(mutex);
        if (next_id == 0 || next_id > kMaximumSafeJsonInteger) return {};
        return std::string(prefix) + std::to_string(next_id++);
    }

    std::shared_ptr<Session> find_locked(std::string_view id) const {
        auto found = sessions.find(std::string(id));
        if (found != sessions.end()) return found->second;
        auto alias = terminal_aliases.find(std::string(id));
        if (alias == terminal_aliases.end()) return {};
        found = sessions.find(alias->second);
        return found == sessions.end() ? std::shared_ptr<Session>{} : found->second;
    }

    std::shared_ptr<Session> find(std::string_view id) const {
        std::lock_guard lock(mutex);
        return find_locked(id);
    }

    static Json snapshot(const Session& session) {
        Json value;
        std::lock_guard lock(session.mutex);
        value = Json{{"id", session.id},
                     {"name", session.name},
                     {"running", session.running.load()},
                     {"processId", session.process_id == 0 ? Json(nullptr)
                                                          : Json(session.process_id)},
                     {"exitCode", session.running.load() ? Json(nullptr)
                                                         : Json(session.exit_code)},
                     {"extensionId", session.owner_extension},
                     {"generation", session.owner_generation}};
        if (!session.terminal_id.empty()) {
            value["terminalId"] = session.terminal_id;
            value["active"] = session.active.load();
            value["visible"] = session.visible.load();
            value["dimensions"] = Json{{"columns", session.dimensions.X},
                                        {"rows", session.dimensions.Y}};
        }
        if (session.kind == Kind::task) {
            value["executionId"] = session.id;
            value["task"] = session.definition;
        } else if (session.kind == Kind::debug) {
            value["sessionId"] = session.id;
            value["configuration"] = session.configuration;
            value["type"] = session.configuration.value("type", std::string{});
        }
        value["output"] = session.output;
        return value;
    }

    static Json event_snapshot(const Session& session) {
        Json value = snapshot(session);
        value.erase("output");
        return value;
    }

    static void append_output(const std::shared_ptr<EventChannel>& channel,
                              const std::shared_ptr<Session>& session,
                              std::string_view data, std::string_view stream) {
        if (data.empty()) return;
        std::lock_guard lock(session->mutex);
        if (session->finished.load()) return;
        session->output.append(data);
        if (session->output.size() > kMaximumOutputBytes) {
            session->output.erase(0, session->output.size() - kMaximumOutputBytes);
        }
        const uint64_t sequence = session->sequence.fetch_add(1) + 1;
        if (!session->terminal_id.empty()) {
            emit_event(channel, "terminal", Json{{"op", "data"},
                                                   {"id", session->terminal_id},
                                                   {"terminalId", session->terminal_id},
                                                   {"sequence", sequence},
                                                   {"stream", stream},
                                                   {"direction", "output"},
                                                   {"data", std::string(data)}});
        }
        if (session->kind == Kind::task) {
            emit_event(channel, "tasks", Json{{"op", "output"},
                                                {"executionId", session->id},
                                                {"terminalId", session->terminal_id},
                                                {"sequence", sequence},
                                                {"stream", stream},
                                                {"data", std::string(data)}});
        } else if (session->kind == Kind::debug && stream == "stderr") {
            emit_event(channel, "debug", Json{{"op", "output"},
                                                {"sessionId", session->id},
                                                {"sequence", sequence},
                                                {"category", "stderr"},
                                                {"output", std::string(data)}});
        }
    }

    static void finish(const std::shared_ptr<EventChannel>& channel,
                       const std::shared_ptr<Session>& session, DWORD exit_code,
                       std::string_view reason) {
        if (session->finished.exchange(true)) return;
        {
            std::lock_guard lock(session->mutex);
            session->running.store(false);
            session->exit_code = exit_code;
        }
        Json state = event_snapshot(*session);
        if (!session->terminal_id.empty()) {
            emit_event(channel, "terminal", Json{{"op", "exit"},
                                                   {"id", session->terminal_id},
                                                   {"terminalId", session->terminal_id},
                                                   {"code", exit_code},
                                                   {"reason", exit_code == ERROR_CANCELLED ? 3 : 2},
                                                   {"snapshot", state}});
        }
        if (session->kind == Kind::task) {
            emit_event(channel, "tasks", Json{{"op", "processEnd"},
                                                {"executionId", session->id},
                                                {"processId", session->process_id},
                                                {"exitCode", exit_code},
                                                {"reason", std::string(reason)}});
            emit_event(channel, "tasks", Json{{"op", "end"},
                                                {"executionId", session->id},
                                                {"exitCode", exit_code},
                                                {"reason", std::string(reason)},
                                                {"snapshot", state}});
        } else if (session->kind == Kind::debug) {
            emit_event(channel, "debug", Json{{"op", "end"},
                                                {"sessionId", session->id},
                                                {"exitCode", exit_code},
                                                {"reason", std::string(reason)},
                                                {"snapshot", state}});
        }
    }

    bool write_bytes(const std::shared_ptr<Session>& session,
                     std::string_view bytes) {
        if (bytes.empty()) return true;
        std::lock_guard lifecycle_lock(session->lifecycle_mutex);
        std::lock_guard lock(session->write_mutex);
        if (!session->running.load() || session->closing.load() ||
            session->writer_stopping || session->writer_done.load() ||
            !session->writer.joinable() ||
            session->write_queue.size() >= kMaximumQueuedWrites ||
            bytes.size() > kMaximumQueuedWriteBytes -
                               session->queued_write_bytes) {
            return false;
        }
        session->write_queue.emplace_back(bytes);
        session->queued_write_bytes += bytes.size();
        session->write_ready.notify_one();
        return true;
    }

    static bool write_transport(const std::shared_ptr<Session>& session,
                                std::string_view bytes) {
        const SOCKET socket = session->socket.load(std::memory_order_acquire);
        std::size_t offset = 0;
        if (socket != INVALID_SOCKET) {
            while (offset < bytes.size()) {
                const int amount = static_cast<int>((std::min)(
                    bytes.size() - offset,
                    static_cast<std::size_t>((std::numeric_limits<int>::max)())));
                const int written = send(socket, bytes.data() + offset, amount, 0);
                if (written <= 0) return false;
                offset += static_cast<std::size_t>(written);
            }
            return true;
        }
        if (!session->write.valid()) return false;
        while (offset < bytes.size()) {
            const DWORD amount = static_cast<DWORD>((std::min)(
                bytes.size() - offset,
                static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
            DWORD written = 0;
            if (!WriteFile(session->write.get(), bytes.data() + offset, amount,
                           &written, nullptr) || written == 0) {
                return false;
            }
            offset += written;
        }
        return true;
    }

    static void writer_loop(const std::shared_ptr<EventChannel>& channel,
                            const std::shared_ptr<Session>& session) {
        bool transport_ok = true;
        for (;;) {
            std::string bytes;
            {
                std::unique_lock lock(session->write_mutex);
                session->write_ready.wait(lock, [&] {
                    return session->writer_stopping ||
                           !session->write_queue.empty();
                });
                if (session->write_queue.empty()) {
                    if (session->writer_stopping) break;
                    continue;
                }
                bytes = std::move(session->write_queue.front());
                session->write_queue.pop_front();
                session->queued_write_bytes -= bytes.size();
            }
            if (!write_transport(session, bytes)) {
                transport_ok = false;
                break;
            }
        }
        {
            std::lock_guard lock(session->write_mutex);
            session->write_queue.clear();
            session->queued_write_bytes = 0;
        }
        session->writer_done.store(true, std::memory_order_release);
        if (!transport_ok && !session->closing.load()) {
            if (session->kind == Kind::debug) {
                emit_event(channel, "debug",
                           Json{{"op", "protocolError"},
                                {"sessionId", session->id},
                                {"message", "debug adapter transport write failed"}});
            }
            finish(channel, session, ERROR_BROKEN_PIPE,
                   "transport_write_error");
        }
    }

    static void emit_dap(const std::shared_ptr<EventChannel>& channel,
                         const std::shared_ptr<Session>& session, Json message) {
        const uint64_t sequence = session->sequence.fetch_add(1) + 1;
        emit_event(channel, "debug", Json{{"op", "dap"},
                                            {"sessionId", session->id},
                                            {"sequence", sequence},
                                            {"message", std::move(message)}});
    }

    static bool dap_json_within_budget(std::string_view text) {
        std::size_t depth = 0;
        std::size_t nodes = 0;
        bool in_string = false;
        bool escaped = false;
        bool in_primitive = false;
        for (const unsigned char byte : text) {
            const char c = static_cast<char>(byte);
            if (in_string) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    in_string = false;
                }
                continue;
            }
            const bool delimiter = c == ',' || c == ':' || c == ']' ||
                                   c == '}' || std::isspace(byte) != 0;
            if (in_primitive) {
                if (!delimiter) continue;
                in_primitive = false;
            }
            if (c == '"') {
                in_string = true;
                if (++nodes > kMaximumDapNodes) return false;
            } else if (c == '{' || c == '[') {
                if (++depth > kMaximumDapDepth ||
                    ++nodes > kMaximumDapNodes)
                    return false;
            } else if (c == '}' || c == ']') {
                if (depth == 0) return false;
                --depth;
            } else if (!delimiter) {
                in_primitive = true;
                if (++nodes > kMaximumDapNodes) return false;
            }
        }
        return !in_string && !escaped && depth == 0;
    }

    static bool consume_dap_buffer(const std::shared_ptr<EventChannel>& channel,
                                   const std::shared_ptr<Session>& session,
                                   std::string& buffer) {
        for (;;) {
            const std::size_t header_end = buffer.find("\r\n\r\n");
            if (header_end == std::string::npos) {
                if (buffer.size() > kMaximumDapHeaderBytes) return false;
                return true;
            }
            if (header_end > kMaximumDapHeaderBytes) return false;
            std::string header = buffer.substr(0, header_end);
            std::string lower = header;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::optional<std::size_t> content_length;
            std::size_t line_begin = 0;
            while (line_begin <= lower.size()) {
                const std::size_t line_end = lower.find("\r\n", line_begin);
                const std::size_t line_limit = line_end == std::string::npos
                                                   ? lower.size()
                                                   : line_end;
                const std::string_view line(lower.data() + line_begin,
                                            line_limit - line_begin);
                const std::size_t colon = line.find(':');
                if (colon == std::string_view::npos || colon == 0) return false;
                std::string_view name = line.substr(0, colon);
                while (!name.empty() &&
                       (name.back() == ' ' || name.back() == '\t'))
                    name.remove_suffix(1);
                std::string_view value = line.substr(colon + 1);
                while (!value.empty() &&
                       (value.front() == ' ' || value.front() == '\t'))
                    value.remove_prefix(1);
                while (!value.empty() &&
                       (value.back() == ' ' || value.back() == '\t'))
                    value.remove_suffix(1);
                if (name == "content-length") {
                    if (content_length || value.empty()) return false;
                    std::size_t parsed_length = 0;
                    const auto parsed = std::from_chars(
                        value.data(), value.data() + value.size(), parsed_length);
                    if (parsed.ec != std::errc{} ||
                        parsed.ptr != value.data() + value.size() ||
                        parsed_length == 0 ||
                        parsed_length > kMaximumDapMessageBytes) {
                        return false;
                    }
                    content_length = parsed_length;
                }
                if (line_end == std::string::npos) break;
                line_begin = line_end + 2;
            }
            if (!content_length) return false;
            const std::size_t length = *content_length;
            const std::size_t body_offset = header_end + 4;
            if (buffer.size() - body_offset < length) return true;
            const std::string_view body(buffer.data() + body_offset, length);
            if (!dap_json_within_budget(body)) return false;
            Json message = Json::parse(body.begin(), body.end(), nullptr, false);
            if (!message.is_object()) return false;
            buffer.erase(0, body_offset + length);
            emit_dap(channel, session, std::move(message));
        }
    }

    static void stream_reader(const std::shared_ptr<EventChannel>& channel,
                              const std::shared_ptr<Session>& session, bool dap) {
        std::array<char, 16384> bytes{};
        std::string dap_buffer;
        bool protocol_ok = true;
        for (;;) {
            int count = 0;
            const SOCKET socket = session->socket.load(std::memory_order_acquire);
            if (socket != INVALID_SOCKET) {
                count = recv(socket, bytes.data(), static_cast<int>(bytes.size()), 0);
                if (count <= 0) break;
            } else {
                DWORD read = 0;
                if (!session->read.valid() ||
                    !ReadFile(session->read.get(), bytes.data(),
                              static_cast<DWORD>(bytes.size()), &read, nullptr) ||
                    read == 0) {
                    break;
                }
                count = static_cast<int>(read);
            }
            if (!dap) {
                append_output(channel, session,
                              std::string_view(bytes.data(), static_cast<std::size_t>(count)),
                              "stdout");
                continue;
            }
            dap_buffer.append(bytes.data(), static_cast<std::size_t>(count));
            if (dap_buffer.size() > kMaximumDapBufferedBytes ||
                !consume_dap_buffer(channel, session, dap_buffer)) {
                protocol_ok = false;
                break;
            }
        }
        session->reader_done.store(true, std::memory_order_release);
        if (!protocol_ok) {
            emit_event(channel, "debug", Json{{"op", "protocolError"},
                                                {"sessionId", session->id},
                                                {"message", "debug adapter emitted an invalid DAP frame"}});
            finish(channel, session, ERROR_INVALID_DATA, "protocol_error");
        } else if (session->process_id == 0 && !session->inline_debug) {
            finish(channel, session, 0, "transport_closed");
        }
    }

    static void stderr_reader_loop(const std::shared_ptr<EventChannel>& channel,
                                   const std::shared_ptr<Session>& session) {
        std::array<char, 4096> bytes{};
        for (;;) {
            DWORD read = 0;
            if (!session->stderr_read.valid() ||
                !ReadFile(session->stderr_read.get(), bytes.data(),
                          static_cast<DWORD>(bytes.size()), &read, nullptr) ||
                read == 0) {
                break;
            }
            append_output(channel, session,
                          std::string_view(bytes.data(), static_cast<std::size_t>(read)),
                          "stderr");
        }
        session->stderr_done.store(true, std::memory_order_release);
    }

    static void monitor_process(const std::shared_ptr<EventChannel>& channel,
                                const std::shared_ptr<Session>& session) {
        DWORD exit_code = ERROR_PROCESS_ABORTED;
        if (session->process.valid()) {
            (void)WaitForSingleObject(session->process.get(), INFINITE);
            (void)GetExitCodeProcess(session->process.get(), &exit_code);
        }
        if (session->reader.joinable() &&
            !session->reader_done.load(std::memory_order_acquire))
            (void)WaitForSingleObject(session->reader.native_handle(), 500);
        if (session->stderr_reader.joinable() &&
            !session->stderr_done.load(std::memory_order_acquire))
            (void)WaitForSingleObject(session->stderr_reader.native_handle(), 500);
        session->monitor_done.store(true, std::memory_order_release);
        finish(channel, session, exit_code,
               session->closing.load() ? "cancelled" : "process_exit");
    }

    void start_threads(const std::shared_ptr<Session>& session, bool dap) {
        const auto channel = events;
        if (session->write.valid() ||
            session->socket.load(std::memory_order_acquire) != INVALID_SOCKET) {
            {
                std::lock_guard lock(session->write_mutex);
                session->writer_stopping = false;
                session->write_queue.clear();
                session->queued_write_bytes = 0;
            }
            session->writer_done.store(false);
            session->writer = std::thread([channel, session] {
                writer_loop(channel, session);
            });
        }
        session->reader_done.store(false);
        session->reader = std::thread([channel, session, dap] {
            stream_reader(channel, session, dap);
        });
        if (session->stderr_read.valid()) {
            session->stderr_done.store(false);
            session->stderr_reader = std::thread([channel, session] {
                stderr_reader_loop(channel, session);
            });
        }
        if (session->process.valid()) {
            session->monitor_done.store(false);
            session->monitor = std::thread([channel, session] {
                monitor_process(channel, session);
            });
        }
    }

    int32_t insert_session(const std::shared_ptr<Session>& session) {
        std::lock_guard lock(mutex);
        if (stopping || sessions.size() >= kMaximumSessions ||
            sessions.contains(session->id) ||
            (!session->terminal_id.empty() &&
             terminal_aliases.contains(session->terminal_id))) {
            return SAO_AI_EDITOR_ERR_BUSY;
        }
        sessions.emplace(session->id, session);
        if (!session->terminal_id.empty())
            terminal_aliases.emplace(session->terminal_id, session->id);
        return SAO_AI_EDITOR_OK;
    }

    int32_t prepare_owner(const Json& params, Session& session) {
        session.owner_extension = params.value("extensionId", std::string{});
        const auto generation = owner_generation(params);
        if (!generation) return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        session.owner_generation = *generation;
        if ((!session.owner_extension.empty() &&
             (!valid_simple_id(session.owner_extension) ||
              session.owner_generation == 0)) ||
            (session.owner_extension.empty() && session.owner_generation != 0)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        return SAO_AI_EDITOR_OK;
    }

    std::filesystem::path current_root() const {
        std::lock_guard lock(mutex);
        return workspace_root;
    }

    int32_t build_process_command(const Json& options, bool shell,
                                  std::wstring& command_line,
                                  std::filesystem::path& cwd,
                                  std::vector<wchar_t>& environment) const {
        const std::filesystem::path root = current_root();
        if (root.empty()) return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
        const auto directory = normalize_existing_directory(
            root, options.value("cwd", Json(nullptr)));
        if (!directory) return SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION;
        cwd = *directory;
        if (!build_environment(options, environment))
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;

        std::string executable = json_string(options, "command", "shellPath");
        Json arguments = options.value("args", options.value("shellArgs", Json::array()));
        if (shell) {
            std::array<wchar_t, 32768> comspec{};
            const DWORD length = GetEnvironmentVariableW(
                L"COMSPEC", comspec.data(), static_cast<DWORD>(comspec.size()));
            if (length == 0 || length >= comspec.size())
                return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
            const std::wstring shell_path(comspec.data(), length);
            std::string shell_command = options.value("commandLine", executable);
            if (shell_command.empty()) return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            std::wstring wide_command = utf8_to_wide(shell_command);
            if (wide_command.empty() && !shell_command.empty())
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            if (arguments.is_array()) {
                if (arguments.size() > 1024) return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                for (const Json& argument : arguments) {
                    if (!argument.is_string()) return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                    const std::string text = argument.get<std::string>();
                    const std::wstring wide = utf8_to_wide(text);
                    if ((!text.empty() && wide.empty()) || text.size() > 32768)
                        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                    wide_command.push_back(L' ');
                    wide_command += quote_argument(wide);
                }
            } else if (!arguments.is_null() && !arguments.is_string()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            } else if (arguments.is_string()) {
                const std::string raw = arguments.get<std::string>();
                if (!valid_utf8(raw) || raw.size() > 32768)
                    return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                wide_command.push_back(L' ');
                wide_command += utf8_to_wide(raw);
            }
            command_line = quote_argument(shell_path) + L" /d /s /c \"" +
                           wide_command + L"\"";
            return command_line.size() <= 32767 ? SAO_AI_EDITOR_OK
                                                 : SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL;
        }
        if (executable.empty()) return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        if (!append_argument(command_line, executable))
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        if (arguments.is_string()) {
            const std::string raw = arguments.get<std::string>();
            if (!valid_utf8(raw) || raw.size() > 32768)
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            command_line.push_back(L' ');
            command_line += utf8_to_wide(raw);
        } else if (arguments.is_array()) {
            if (arguments.size() > 1024) return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            for (const Json& argument : arguments) {
                if (!argument.is_string() ||
                    !append_argument(command_line, argument.get<std::string>())) {
                    return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                }
            }
        } else {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        return command_line.size() <= 32767 ? SAO_AI_EDITOR_OK
                                             : SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL;
    }

    static bool create_pipe(UniqueHandle& read_end, UniqueHandle& write_end,
                            bool host_reads, bool inheritable) {
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr,
                                     inheritable ? TRUE : FALSE};
        HANDLE read = nullptr;
        HANDLE write = nullptr;
        if (!CreatePipe(&read, &write, &security, 0)) return false;
        read_end.reset(read);
        write_end.reset(write);
        if (!inheritable) return true;
        HANDLE host = host_reads ? read : write;
        return SetHandleInformation(host, HANDLE_FLAG_INHERIT, 0) != FALSE;
    }

    int32_t spawn(const std::shared_ptr<Session>& session, const Json& options,
                  bool shell, bool conpty, bool dap) {
        std::lock_guard lifecycle_lock(session->lifecycle_mutex);
        if (session->closing.load()) return SAO_AI_EDITOR_ERR_IPC_CLOSED;
        std::wstring command_line;
        std::filesystem::path cwd;
        std::vector<wchar_t> environment;
        int32_t status = build_process_command(options, shell, command_line,
                                               cwd, environment);
        if (status != SAO_AI_EDITOR_OK) return status;

        UniqueHandle input_read;
        UniqueHandle input_write;
        UniqueHandle output_read;
        UniqueHandle output_write;
        UniqueHandle error_read;
        UniqueHandle error_write;
        if (!create_pipe(input_read, input_write, false, !conpty) ||
            !create_pipe(output_read, output_write, true, !conpty) ||
            (dap && !create_pipe(error_read, error_write, true, true))) {
            return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
        }

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        SIZE_T attribute_bytes = 0;
        DWORD creation_flags = CREATE_UNICODE_ENVIRONMENT |
                               EXTENDED_STARTUPINFO_PRESENT;
        const PseudoConsoleApi& pseudo_api = PseudoConsoleApi::instance();
        HPCON pseudo{};
        COORD pseudo_dimensions{80, 30};
        std::array<HANDLE, 3> inherited{};
        std::size_t inherited_count = 0;
        if (conpty) {
            if (!pseudo_api.available()) return SAO_AI_EDITOR_ERR_NOT_IMPLEMENTED;
            if (options.contains("dimensions") && options["dimensions"].is_object()) {
                const int columns = options["dimensions"].value("columns", 80);
                const int rows = options["dimensions"].value("rows", 30);
                if (columns < 1 || columns > 10000 || rows < 1 || rows > 10000)
                    return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                pseudo_dimensions = {static_cast<SHORT>(columns),
                                     static_cast<SHORT>(rows)};
            }
            if (FAILED(pseudo_api.create(pseudo_dimensions, input_read.get(),
                                         output_write.get(), 0, &pseudo))) {
                return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
            }
            InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
        } else {
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdInput = input_read.get();
            startup.StartupInfo.hStdOutput = output_write.get();
            startup.StartupInfo.hStdError = dap ? error_write.get() : output_write.get();
            inherited[inherited_count++] = input_read.get();
            inherited[inherited_count++] = output_write.get();
            if (dap) inherited[inherited_count++] = error_write.get();
            InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
            creation_flags |= CREATE_NO_WINDOW;
        }
        if (attribute_bytes == 0) {
            if (pseudo) pseudo_api.close(pseudo);
            return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
        }
        std::vector<std::byte> attributes(attribute_bytes);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
            attributes.data());
        if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0,
                                               &attribute_bytes)) {
            if (pseudo) pseudo_api.close(pseudo);
            return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
        }
        bool attribute_ok = true;
        if (conpty) {
            attribute_ok =
                UpdateProcThreadAttribute(startup.lpAttributeList, 0,
                                          PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                          pseudo, sizeof(pseudo), nullptr, nullptr) != FALSE;
        }
        if (!conpty) {
            attribute_ok = attribute_ok &&
                UpdateProcThreadAttribute(startup.lpAttributeList, 0,
                                          PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                          inherited.data(),
                                          inherited_count * sizeof(HANDLE),
                                          nullptr, nullptr) != FALSE;
        }
        if (!attribute_ok) {
            DeleteProcThreadAttributeList(startup.lpAttributeList);
            if (pseudo) pseudo_api.close(pseudo);
            return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
        }

        std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
        mutable_command.push_back(L'\0');
        PROCESS_INFORMATION process{};
        BOOL created = FALSE;
        {
            std::lock_guard spawn_lock(process_spawn_mutex());
            StandardHandleInheritanceGuard standard_handles(conpty);
            created = CreateProcessW(
                nullptr, mutable_command.data(), nullptr, nullptr,
                conpty ? FALSE : TRUE, creation_flags | CREATE_SUSPENDED,
                environment.data(), cwd.c_str(), &startup.StartupInfo, &process);
        }
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        input_read.reset();
        output_write.reset();
        error_write.reset();
        if (!created) {
            if (pseudo) pseudo_api.close(pseudo);
            return SAO_AI_EDITOR_ERR_LAUNCH_FAILED;
        }
        UniqueHandle thread(process.hThread);
        UniqueHandle process_handle(process.hProcess);
        UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job.valid() ||
            !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                                     &limits, sizeof(limits)) ||
            !AssignProcessToJobObject(job.get(), process_handle.get()) ||
            ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
            (void)TerminateProcess(process_handle.get(), ERROR_CANCELLED);
            if (pseudo) pseudo_api.close(pseudo);
            return SAO_AI_EDITOR_ERR_LAUNCH_FAILED;
        }
        {
            std::lock_guard lock(session->mutex);
            session->process_id = process.dwProcessId;
            session->dimensions = pseudo_dimensions;
            session->pseudo_console = pseudo;
        }
        session->process = std::move(process_handle);
        session->job = std::move(job);
        session->read = std::move(output_read);
        session->write = std::move(input_write);
        if (dap) session->stderr_read = std::move(error_read);
        try {
            start_threads(session, dap);
        } catch (...) {
            stop_one(session, true);
            return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
        }
        return SAO_AI_EDITOR_OK;
    }

    int32_t connect_debug_transport(const std::shared_ptr<Session>& session,
                                    const Json& descriptor) {
        std::lock_guard lifecycle_lock(session->lifecycle_mutex);
        if (session->closing.load()) return SAO_AI_EDITOR_ERR_IPC_CLOSED;
        const std::string kind = descriptor.value("kind", std::string{"executable"});
        if (kind == "inline") {
            session->inline_debug = true;
            return SAO_AI_EDITOR_OK;
        }
        if (kind == "server") {
            const std::string host = descriptor.value("host", std::string{"127.0.0.1"});
            const int port = descriptor.value("port", 0);
            if ((host != "127.0.0.1" && host != "localhost" && host != "::1") ||
                port < 1 || port > 65535) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            WSADATA data{};
            if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
                return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
            session->winsock_started = true;
            addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            addrinfo* addresses = nullptr;
            const std::string service = std::to_string(port);
            if (getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses) != 0) {
                WSACleanup();
                session->winsock_started = false;
                return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
            }
            SOCKET connected = INVALID_SOCKET;
            for (addrinfo* current = addresses; current; current = current->ai_next) {
                SOCKET candidate = socket(current->ai_family, current->ai_socktype,
                                          current->ai_protocol);
                if (candidate == INVALID_SOCKET) continue;
                if (connect(candidate, current->ai_addr,
                            static_cast<int>(current->ai_addrlen)) == 0) {
                    connected = candidate;
                    break;
                }
                closesocket(candidate);
            }
            freeaddrinfo(addresses);
            if (connected == INVALID_SOCKET) {
                WSACleanup();
                session->winsock_started = false;
                return SAO_AI_EDITOR_ERR_IPC_CONNECT_FAIL;
            }
            session->socket.store(connected, std::memory_order_release);
            try {
                start_threads(session, true);
            } catch (...) {
                stop_one(session, true);
                return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
            }
            return SAO_AI_EDITOR_OK;
        }
        if (kind == "namedPipe") {
            const std::string path = descriptor.value("path", std::string{});
            if (!valid_utf8(path) || path.size() > 32768 ||
                path.rfind(R"(\\.\pipe\)", 0) != 0) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            const std::wstring wide = utf8_to_wide(path);
            if (!WaitNamedPipeW(wide.c_str(), kConnectWaitMilliseconds))
                return SAO_AI_EDITOR_ERR_IPC_CONNECT_FAIL;
            HANDLE pipe = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE,
                                      0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe == INVALID_HANDLE_VALUE)
                return SAO_AI_EDITOR_ERR_IPC_CONNECT_FAIL;
            HANDLE duplicate = nullptr;
            if (!DuplicateHandle(GetCurrentProcess(), pipe, GetCurrentProcess(),
                                 &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                CloseHandle(pipe);
                return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
            }
            session->read.reset(pipe);
            session->write.reset(duplicate);
            try {
                start_threads(session, true);
            } catch (...) {
                stop_one(session, true);
                return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
            }
            return SAO_AI_EDITOR_OK;
        }
        if (kind != "executable") return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        Json options = descriptor;
        options["command"] = descriptor.value("command",
                                             descriptor.value("executable", std::string{}));
        options["args"] = descriptor.value("args", Json::array());
        if (descriptor.contains("options") && descriptor["options"].is_object()) {
            for (const auto& [key, value] : descriptor["options"].items())
                options[key] = value;
        }
        return spawn(session, options, false, false, true);
    }

    void stop_one(const std::shared_ptr<Session>& session, bool terminate) noexcept {
        if (!session || session->closing.exchange(true)) return;
        std::lock_guard lifecycle_lock(session->lifecycle_mutex);
        try {
            if (terminate && session->job.valid())
                (void)TerminateJobObject(session->job.get(), ERROR_CANCELLED);
            else if (terminate && session->process.valid())
                (void)TerminateProcess(session->process.get(), ERROR_CANCELLED);
            {
                std::lock_guard lock(session->write_mutex);
                session->writer_stopping = true;
                session->write_queue.clear();
                session->queued_write_bytes = 0;
            }
            session->write_ready.notify_all();
            const SOCKET socket = session->socket.load(std::memory_order_acquire);
            if (socket != INVALID_SOCKET) {
                ::shutdown(socket, SD_BOTH);
            }
            if (session->write.valid())
                (void)CancelIoEx(session->write.get(), nullptr);
            if (session->read.valid()) (void)CancelIoEx(session->read.get(), nullptr);
            if (session->stderr_read.valid())
                (void)CancelIoEx(session->stderr_read.get(), nullptr);

            const auto join_thread = [](std::thread& thread) -> bool {
                if (!thread.joinable()) return true;
                if (thread.get_id() == std::this_thread::get_id()) {
                    thread.detach();
                    return false;
                }
                (void)CancelSynchronousIo(thread.native_handle());
                if (WaitForSingleObject(thread.native_handle(),
                                        kStopWaitMilliseconds) == WAIT_OBJECT_0) {
                    thread.join();
                    return true;
                } else {
                    thread.detach();
                    return false;
                }
            };
            const bool writer_joined = join_thread(session->writer);
            const bool monitor_joined = join_thread(session->monitor);
            const bool reader_joined = join_thread(session->reader);
            const bool stderr_joined = join_thread(session->stderr_reader);
            if (writer_joined && monitor_joined && reader_joined &&
                stderr_joined) {
                HPCON pseudo_console = nullptr;
                {
                    std::lock_guard lock(session->mutex);
                    pseudo_console = session->pseudo_console;
                    session->pseudo_console = nullptr;
                }
                if (pseudo_console) {
                    const auto& api = PseudoConsoleApi::instance();
                    if (api.close) api.close(pseudo_console);
                }
                const SOCKET owned_socket = session->socket.exchange(
                    INVALID_SOCKET, std::memory_order_acq_rel);
                if (owned_socket != INVALID_SOCKET) closesocket(owned_socket);
                if (session->winsock_started) {
                    WSACleanup();
                    session->winsock_started = false;
                }
                session->write.reset();
                session->read.reset();
                session->stderr_read.reset();
                session->process.reset();
                session->job.reset();
            }
            DWORD exit_code = ERROR_PROCESS_ABORTED;
            {
                std::lock_guard lock(session->mutex);
                exit_code = session->exit_code;
            }
            finish(events, session,
                   terminate ? ERROR_CANCELLED : exit_code,
                   terminate ? "cancelled" : "disposed");
        } catch (...) {
            finish(events, session, ERROR_PROCESS_ABORTED, "cleanup_error");
        }
    }

    void erase_session(const std::shared_ptr<Session>& session) {
        std::lock_guard lock(mutex);
        if (!session) return;
        sessions.erase(session->id);
        if (!session->terminal_id.empty())
            terminal_aliases.erase(session->terminal_id);
        if (active_terminal == session->terminal_id) active_terminal.clear();
    }

    int32_t create_terminal(const Json& params, Json& result) {
        const Json options = params.value("options", Json::object());
        if (!options.is_object() || options.dump().size() > kMaximumRequestBytes)
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        auto session = std::make_shared<Session>();
        session->kind = Kind::terminal;
        session->id = json_string(params, "terminalId", "id");
        if (session->id.empty()) session->id = allocate_id("terminal-");
        session->terminal_id = session->id;
        session->name = options.value("name", params.value("name", std::string{"Terminal"}));
        if (!valid_identity(session->id) || !valid_identity(session->name))
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        int32_t status = prepare_owner(params, *session);
        if (status != SAO_AI_EDITOR_OK) return status;
        const std::string kind = params.value("kind", std::string{"shell"});
        session->extension_pty = kind == "extension";
        status = insert_session(session);
        if (status != SAO_AI_EDITOR_OK) return status;
        if (!session->extension_pty) {
            Json process_options = options;
            process_options["command"] = options.value("shellPath", std::string{});
            process_options["args"] = options.value("shellArgs", Json::array());
            if (process_options["command"].get<std::string>().empty()) {
                std::array<wchar_t, 32768> comspec{};
                const DWORD length = GetEnvironmentVariableW(
                    L"COMSPEC", comspec.data(), static_cast<DWORD>(comspec.size()));
                if (length == 0 || length >= comspec.size()) {
                    erase_session(session);
                    return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
                }
                process_options["command"] = wide_to_utf8(
                    std::wstring_view(comspec.data(), length));
            }
            status = spawn(session, process_options, false, true, false);
            if (status != SAO_AI_EDITOR_OK) {
                erase_session(session);
                return status;
            }
        }
        emit("terminal", Json{{"op", "open"},
                              {"id", session->terminal_id},
                              {"terminalId", session->terminal_id},
                              {"snapshot", event_snapshot(*session)}});
        result = snapshot(*session);
        return SAO_AI_EDITOR_OK;
    }

    int32_t send_terminal_text(const Json& params, Json& result) {
        const std::string id = json_string(params, "terminalId", "id");
        const auto session = find(id);
        if (!session || session->kind == Kind::debug)
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        std::lock_guard lifecycle_lock(session->lifecycle_mutex);
        if (session->closing.load() || !session->running.load())
            return SAO_AI_EDITOR_ERR_IPC_CLOSED;
        const std::string text = params.value("text", std::string{});
        if (!valid_utf8(text) || text.size() > kMaximumRequestBytes)
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        const bool newline = params.value("addNewLine", false);
        const std::string direction = params.value("direction", std::string{"input"});
        std::string payload = text;
        if (newline) payload += "\r\n";
        if (session->extension_pty) {
            const uint64_t sequence = session->sequence.fetch_add(1) + 1;
            emit("terminal", Json{{"op", "data"},
                                  {"id", session->terminal_id},
                                  {"terminalId", session->terminal_id},
                                  {"sequence", sequence},
                                  {"stream", params.value("stream", "stdin")},
                                  {"direction", direction},
                                  {"data", payload}});
        } else if (!write_bytes(session, payload)) {
            return SAO_AI_EDITOR_ERR_IPC_CLOSED;
        }
        result = Json{{"accepted", true}, {"terminalId", session->terminal_id}};
        return SAO_AI_EDITOR_OK;
    }

    int32_t set_terminal_visibility(const Json& params, bool visible,
                                    Json& result) {
        const std::string id = json_string(params, "terminalId", "id");
        const auto session = find(id);
        if (!session || session->terminal_id.empty())
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        std::lock_guard lifecycle_lock(session->lifecycle_mutex);
        if (session->closing.load()) return SAO_AI_EDITOR_ERR_NOT_FOUND;
        {
            std::lock_guard lock(mutex);
            const std::string previous = active_terminal;
            session->visible.store(visible);
            if (visible) {
                if (const auto old = find_locked(active_terminal); old && old != session)
                    old->active.store(false);
                active_terminal = session->terminal_id;
                session->active.store(true);
            } else if (active_terminal == session->terminal_id) {
                active_terminal.clear();
                session->active.store(false);
            }
            if (previous != active_terminal) {
                emit("terminal", Json{{"op", "active"},
                                      {"terminalId", active_terminal},
                                      {"previousTerminalId", previous}});
            }
        }
        result = snapshot(*session);
        return SAO_AI_EDITOR_OK;
    }

    int32_t resize_terminal(const Json& params, Json& result) {
        const std::string id = json_string(params, "terminalId", "id");
        const auto session = find(id);
        if (!session || session->terminal_id.empty())
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        std::lock_guard lifecycle_lock(session->lifecycle_mutex);
        if (session->closing.load()) return SAO_AI_EDITOR_ERR_NOT_FOUND;
        const Json dimensions = params.value("dimensions", params);
        const int columns = dimensions.value("columns", 0);
        const int rows = dimensions.value("rows", 0);
        if (columns < 1 || columns > 10000 || rows < 1 || rows > 10000)
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        const COORD size{static_cast<SHORT>(columns), static_cast<SHORT>(rows)};
        {
            std::lock_guard lock(session->mutex);
            if (session->pseudo_console) {
                const auto& api = PseudoConsoleApi::instance();
                if (!api.resize || FAILED(api.resize(session->pseudo_console, size)))
                    return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
            }
            session->dimensions = size;
        }
        emit("terminal", Json{{"op", "dimensions"},
                              {"terminalId", session->terminal_id},
                              {"dimensions", Json{{"columns", columns}, {"rows", rows}}}});
        result = snapshot(*session);
        return SAO_AI_EDITOR_OK;
    }

    int32_t dispose_terminal(const Json& params, Json& result) {
        const std::string id = json_string(params, "terminalId", "id");
        const auto session = find(id);
        if (!session || session->terminal_id.empty())
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        stop_one(session, true);
        erase_session(session);
        result = Json{{"disposed", true}, {"terminalId", id}};
        return SAO_AI_EDITOR_OK;
    }

    int32_t list_terminals(Json& result) const {
        Json terminals = Json::array();
        std::string active;
        {
            std::lock_guard lock(mutex);
            active = active_terminal;
            for (const auto& [id, session] : sessions) {
                if (!session->terminal_id.empty() && !session->finished.load())
                    terminals.push_back(snapshot(*session));
            }
        }
        result = Json{{"terminals", std::move(terminals)},
                      {"activeTerminalId", active}};
        return SAO_AI_EDITOR_OK;
    }

    int32_t execute_task(const Json& params, Json& result) {
        const Json task = params.value("task", params.value("definition", Json::object()));
        if (!task.is_object() || task.dump().size() > kMaximumRequestBytes)
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        const Json execution = task.value("execution", Json::object());
        if (!execution.is_object()) return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        auto session = std::make_shared<Session>();
        session->kind = Kind::task;
        session->id = json_string(params, "executionId", "id");
        if (session->id.empty()) session->id = allocate_id("task-");
        session->terminal_id = params.value("terminalId", session->id + "-terminal");
        session->name = task.value("name", std::string{"Task"});
        session->definition = task;
        if (!valid_identity(session->id) || !valid_identity(session->terminal_id) ||
            !valid_identity(session->name))
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        int32_t status = prepare_owner(params, *session);
        if (status != SAO_AI_EDITOR_OK) return status;
        const std::string kind = execution.value("kind", std::string{"shell"});
        session->extension_pty = kind == "custom";
        status = insert_session(session);
        if (status != SAO_AI_EDITOR_OK) return status;
        if (!session->extension_pty) {
            Json options = execution.value("options", Json::object());
            if (!options.is_object()) options = Json::object();
            options["command"] = execution.value("command", std::string{});
            options["args"] = execution.value("args", Json::array());
            if (execution.contains("commandLine"))
                options["commandLine"] = execution["commandLine"];
            status = spawn(session, options, kind == "shell", true, false);
            if (status != SAO_AI_EDITOR_OK) {
                erase_session(session);
                return status;
            }
        }
        const Json state = snapshot(*session);
        const Json event_state = event_snapshot(*session);
        emit("terminal", Json{{"op", "open"},
                              {"terminalId", session->terminal_id},
                      {"snapshot", event_state}});
        emit("tasks", Json{{"op", "start"},
                           {"executionId", session->id},
                           {"terminalId", session->terminal_id},
                           {"task", task},
                   {"snapshot", event_state}});
        if (session->process_id != 0) {
            emit("tasks", Json{{"op", "processStart"},
                               {"executionId", session->id},
                               {"processId", session->process_id}});
        }
        result = state;
        return SAO_AI_EDITOR_OK;
    }

    int32_t terminate_task(const Json& params, Json& result) {
        const std::string id = json_string(params, "executionId", "id");
        const auto session = find(id);
        if (!session || session->kind != Kind::task)
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        stop_one(session, true);
        result = snapshot(*session);
        erase_session(session);
        return SAO_AI_EDITOR_OK;
    }

    int32_t task_status(const Json& params, Json& result) const {
        const std::string id = json_string(params, "executionId", "id");
        const auto session = find(id);
        if (!session || session->kind != Kind::task)
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        result = snapshot(*session);
        return SAO_AI_EDITOR_OK;
    }

    int32_t list_task_executions(Json& result) const {
        Json executions = Json::array();
        std::lock_guard lock(mutex);
        for (const auto& [id, session] : sessions)
            if (session->kind == Kind::task) executions.push_back(snapshot(*session));
        result = Json{{"executions", std::move(executions)}};
        return SAO_AI_EDITOR_OK;
    }

    int32_t start_debug_session(const Json& params, Json& result) {
        const Json configuration = params.value("configuration", Json::object());
        const Json descriptor = params.value("descriptor", Json::object());
        if (!configuration.is_object() || !descriptor.is_object() ||
            configuration.dump().size() > kMaximumRequestBytes ||
            descriptor.dump().size() > kMaximumRequestBytes) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        auto session = std::make_shared<Session>();
        session->kind = Kind::debug;
        session->id = json_string(params, "sessionId", "id");
        if (session->id.empty()) session->id = allocate_id("debug-");
        session->name = configuration.value("name", std::string{"Debug"});
        session->configuration = configuration;
        if (!valid_identity(session->id) || !valid_identity(session->name))
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        int32_t status = prepare_owner(params, *session);
        if (status != SAO_AI_EDITOR_OK) return status;
        status = insert_session(session);
        if (status != SAO_AI_EDITOR_OK) return status;
        status = connect_debug_transport(session, descriptor);
        if (status != SAO_AI_EDITOR_OK) {
            erase_session(session);
            return status;
        }
        const Json state = snapshot(*session);
        emit("debug", Json{{"op", "start"},
                           {"sessionId", session->id},
                   {"snapshot", event_snapshot(*session)}});
        result = state;
        return SAO_AI_EDITOR_OK;
    }

    int32_t send_debug_message(const Json& params, Json& result) {
        const std::string id = json_string(params, "sessionId", "id");
        const auto session = find(id);
        if (!session || session->kind != Kind::debug)
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        if (!params.contains("message") || !params["message"].is_object())
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        if (session->inline_debug) {
            emit("debug", Json{{"op", "inlineInput"},
                               {"sessionId", id},
                               {"message", params["message"]}});
            result = Json{{"accepted", true}, {"sessionId", id}};
            return SAO_AI_EDITOR_OK;
        }
        std::string body = params["message"].dump();
        if (body.empty() || body.size() > kMaximumDapMessageBytes)
            return SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL;
        const std::string frame = "Content-Length: " + std::to_string(body.size()) +
                                  "\r\n\r\n" + body;
        if (!write_bytes(session, frame)) return SAO_AI_EDITOR_ERR_IPC_CLOSED;
        result = Json{{"accepted", true}, {"sessionId", id}};
        return SAO_AI_EDITOR_OK;
    }

    int32_t stop_debug_session(const Json& params, Json& result) {
        const std::string id = json_string(params, "sessionId", "id");
        const auto session = find(id);
        if (!session || session->kind != Kind::debug)
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        stop_one(session, true);
        result = snapshot(*session);
        erase_session(session);
        return SAO_AI_EDITOR_OK;
    }

    int32_t debug_status(const Json& params, Json& result) const {
        const std::string id = json_string(params, "sessionId", "id");
        const auto session = find(id);
        if (!session || session->kind != Kind::debug)
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        result = snapshot(*session);
        return SAO_AI_EDITOR_OK;
    }

    int32_t list_debug_sessions(Json& result) const {
        Json items = Json::array();
        std::lock_guard lock(mutex);
        for (const auto& [id, session] : sessions)
            if (session->kind == Kind::debug) items.push_back(snapshot(*session));
        result = Json{{"sessions", std::move(items)}};
        return SAO_AI_EDITOR_OK;
    }

    void retire_owner(std::string_view extension_id, uint64_t generation) noexcept {
        std::vector<std::shared_ptr<Session>> owned;
        {
            std::lock_guard lock(mutex);
            for (const auto& [id, session] : sessions) {
                if (session->owner_extension == extension_id &&
                    (generation == 0 || session->owner_generation == generation)) {
                    owned.push_back(session);
                }
            }
        }
        for (const auto& session : owned) {
            stop_one(session, true);
            erase_session(session);
        }
    }

    void shutdown() noexcept {
        std::vector<std::shared_ptr<Session>> active;
        {
            std::lock_guard lock(mutex);
            if (stopping) return;
            stopping = true;
            for (const auto& [id, session] : sessions) active.push_back(session);
        }
        for (const auto& session : active) stop_one(session, true);
        {
            std::lock_guard lock(mutex);
            sessions.clear();
            terminal_aliases.clear();
            active_terminal.clear();
        }
        {
            std::lock_guard lock(events->mutex);
            events->stopping = true;
            events->sink = {};
            events->queue.clear();
            events->queued_bytes = 0;
            events->ready.notify_all();
        }
        if (event_thread.joinable()) {
            if (event_thread.get_id() == std::this_thread::get_id())
                event_thread.detach();
            else if (WaitForSingleObject(event_thread.native_handle(),
                                         kStopWaitMilliseconds) == WAIT_OBJECT_0)
                event_thread.join();
            else
                event_thread.detach();
        }
    }
};

ExtensionProcessRuntime::ExtensionProcessRuntime(std::filesystem::path workspace_root,
                                                 EventSink event_sink)
    : impl_(std::make_unique<Impl>(std::move(workspace_root),
                                   std::move(event_sink))) {}

ExtensionProcessRuntime::~ExtensionProcessRuntime() = default;

int32_t ExtensionProcessRuntime::create_terminal(const Json& params, Json& result) {
    return impl_->create_terminal(params, result);
}
int32_t ExtensionProcessRuntime::send_terminal_text(const Json& params, Json& result) {
    return impl_->send_terminal_text(params, result);
}
int32_t ExtensionProcessRuntime::show_terminal(const Json& params, Json& result) {
    return impl_->set_terminal_visibility(params, true, result);
}
int32_t ExtensionProcessRuntime::hide_terminal(const Json& params, Json& result) {
    return impl_->set_terminal_visibility(params, false, result);
}
int32_t ExtensionProcessRuntime::resize_terminal(const Json& params, Json& result) {
    return impl_->resize_terminal(params, result);
}
int32_t ExtensionProcessRuntime::dispose_terminal(const Json& params, Json& result) {
    return impl_->dispose_terminal(params, result);
}
int32_t ExtensionProcessRuntime::list_terminals(Json& result) const {
    return impl_->list_terminals(result);
}
int32_t ExtensionProcessRuntime::execute_task(const Json& params, Json& result) {
    return impl_->execute_task(params, result);
}
int32_t ExtensionProcessRuntime::terminate_task(const Json& params, Json& result) {
    return impl_->terminate_task(params, result);
}
int32_t ExtensionProcessRuntime::task_status(const Json& params, Json& result) const {
    return impl_->task_status(params, result);
}
int32_t ExtensionProcessRuntime::list_task_executions(Json& result) const {
    return impl_->list_task_executions(result);
}
int32_t ExtensionProcessRuntime::start_debug_session(const Json& params, Json& result) {
    return impl_->start_debug_session(params, result);
}
int32_t ExtensionProcessRuntime::send_debug_message(const Json& params, Json& result) {
    return impl_->send_debug_message(params, result);
}
int32_t ExtensionProcessRuntime::stop_debug_session(const Json& params, Json& result) {
    return impl_->stop_debug_session(params, result);
}
int32_t ExtensionProcessRuntime::debug_status(const Json& params, Json& result) const {
    return impl_->debug_status(params, result);
}
int32_t ExtensionProcessRuntime::list_debug_sessions(Json& result) const {
    return impl_->list_debug_sessions(result);
}
void ExtensionProcessRuntime::retire_owner(std::string_view extension_id,
                                           uint64_t generation) noexcept {
    impl_->retire_owner(extension_id, generation);
}
void ExtensionProcessRuntime::shutdown() noexcept { impl_->shutdown(); }

} // namespace sao::ai_editor::native
