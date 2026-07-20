#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "native_utils.h"

namespace sao::ai_editor::native {

class NativeRuntime;

// A Node.js child process supervised over stdio JSON-RPC 2.0.  One
// NodeRuntime owns one node.exe subprocess plus a background reader
// thread; requests carry a monotonic integer id and return futures.
//
// The VS Code extension host loads its Node.js shim via
// `boot_extension_host`.  The shim is expected to speak the
// `vscode.*` and `sao.host.*` methods this runtime advertises.
class NodeRuntime final : public std::enable_shared_from_this<NodeRuntime> {
public:
    struct BootOptions {
        std::string node_executable;        // absolute path to node.exe
        std::vector<std::string> node_args; // e.g. {"--experimental-vm-modules"}
        std::string entry_script;           // absolute path to shim/boot script
        std::vector<std::string> extra_args;
        std::vector<std::pair<std::string, std::string>> environment;
        std::string working_directory;
        uint32_t startup_ms = 15000;
    };

    NodeRuntime() = default;
    ~NodeRuntime();

    NodeRuntime(const NodeRuntime&) = delete;
    NodeRuntime& operator=(const NodeRuntime&) = delete;

    int32_t boot(const BootOptions& options);
    bool alive() const noexcept;
    int32_t request(std::string_view method, const Json& params,
                    uint32_t timeout_ms, Json& result);
    int32_t notify(std::string_view method, const Json& params);
    void set_native_runtime(NativeRuntime* runtime) noexcept {
        native_runtime_.store(runtime, std::memory_order_release);
    }
    void shutdown();

private:
    enum class State : uint8_t {
        closed,
        running,
        protocol_failed,
    };

    struct Pending {
        std::promise<Json> promise;
        std::future<Json> future;
        Pending() : future(promise.get_future()) {}
    };

    void reader_loop();
    void dispatch_loop();
    void stderr_loop();
    bool dispatch_message(Json message);
    void handle_request_from_node(Json message);
    void handle_notification_from_node(Json message);
    void fail_pending(std::string_view message, int32_t status);
    void fail_protocol(std::string_view message);
    bool send_framed(const std::string& payload);
    bool send_raw(const std::string& data);

    void* stdin_write_ = nullptr;     // HANDLE
    void* stdout_read_ = nullptr;     // HANDLE
    void* stderr_read_ = nullptr;     // HANDLE
    void* process_ = nullptr;         // HANDLE
    void* thread_ = nullptr;          // HANDLE

    std::atomic<bool> stopping_{false};
    std::atomic<State> state_{State::closed};
    std::thread reader_;
    std::thread dispatcher_;
    std::thread stderr_reader_;

    std::mutex dispatch_mutex_;
    std::condition_variable dispatch_ready_;
    std::deque<Json> dispatch_queue_;

    std::mutex write_mutex_;
    mutable std::mutex pending_mutex_;
    std::unordered_map<int64_t, std::shared_ptr<Pending>> pending_;
    std::unordered_set<int64_t> timed_out_request_ids_;
    std::atomic<int64_t> next_id_{1};

    std::atomic<NativeRuntime*> native_runtime_{nullptr};

    std::mutex shutdown_mutex_;
    bool shutdown_started_ = false;

    mutable std::mutex state_mutex_;
    std::string protocol_version_;
    Json server_info_;
    Json capabilities_;
    std::string stderr_tail_;
};

}  // namespace sao::ai_editor::native
