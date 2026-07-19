#include "sao/plugins/loader/plugin_deps.h"
#include "sao/plugins/loader/loader_status.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sao::plugins::loader {

struct deps_session_s {
    std::mutex mutex;
    deps_provider provider{};
    void* provider_session = nullptr;
    std::vector<std::wstring> attached_paths;
    bool operation_active = false;
    bool provider_session_closed = false;
    bool provider_released = false;
};

namespace {

namespace fs = std::filesystem;

struct deps_provider_record {
    deps_provider value{};
    bool present = false;
    size_t active_sessions = 0;
};

std::mutex g_deps_provider_mutex;
deps_provider_record g_deps_provider;
std::mutex g_deps_sessions_mutex;
std::unordered_map<deps_session_t, std::shared_ptr<deps_session_s>> g_deps_sessions;

constexpr size_t kDepsProviderMinimumSize = sizeof(deps_provider);
constexpr size_t kMaximumDepsCallbackNesting = 32;
thread_local std::array<deps_session_t, kMaximumDepsCallbackNesting> g_active_deps_sessions{};
thread_local size_t g_active_deps_session_depth = 0;

std::shared_ptr<deps_session_s> retain_deps_session(deps_session_t session) noexcept {
    if (session == nullptr)
        return nullptr;
    try {
        std::lock_guard lock(g_deps_sessions_mutex);
        const auto found = g_deps_sessions.find(session);
        return found == g_deps_sessions.end() ? nullptr : found->second;
    } catch (...) {
        return nullptr;
    }
}

class deps_session_lease {
  public:
    deps_session_lease() = default;
    ~deps_session_lease() {
        release();
    }

    deps_session_lease(const deps_session_lease&) = delete;
    deps_session_lease& operator=(const deps_session_lease&) = delete;

    int32_t acquire(deps_session_t session) noexcept {
        if (g_active_deps_session_depth == kMaximumDepsCallbackNesting)
            return SAO_PLUGINS_ERR_BUSY;
        auto retained = retain_deps_session(session);
        if (retained == nullptr)
            return SAO_ERR_HANDLE_INVALID;
        try {
            std::lock_guard lock(retained->mutex);
            if (retained->operation_active)
                return SAO_PLUGINS_ERR_BUSY;
            retained->operation_active = true;
            session_ = session;
            retained_ = std::move(retained);
            g_active_deps_sessions[g_active_deps_session_depth++] = session;
            return SAO_OK;
        } catch (...) {
            return SAO_ERR_OS_CALL_FAILED;
        }
    }

    deps_session_s& value() const noexcept {
        return *retained_;
    }

  private:
    void release() noexcept {
        if (retained_ == nullptr)
            return;
        if (g_active_deps_session_depth > 0 &&
            g_active_deps_sessions[g_active_deps_session_depth - 1] == session_) {
            g_active_deps_sessions[--g_active_deps_session_depth] = nullptr;
        }
        try {
            std::lock_guard lock(retained_->mutex);
            retained_->operation_active = false;
        } catch (...) {
        }
        retained_.reset();
        session_ = nullptr;
    }

    deps_session_t session_ = nullptr;
    std::shared_ptr<deps_session_s> retained_;
};

int32_t call_restore_path(deps_session_s& session, const wchar_t* path) noexcept {
    try {
        return session.provider.restore_path(session.provider.user_data, session.provider_session,
                                             path);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t restore_attached_paths(deps_session_s& session) noexcept {
    while (!session.attached_paths.empty()) {
        const int32_t status = call_restore_path(session, session.attached_paths.back().c_str());
        if (status != SAO_OK && status != SAO_ERR_HANDLE_INVALID)
            return status;
        session.attached_paths.pop_back();
    }
    return SAO_OK;
}

int32_t close_provider_session(deps_session_s& session) noexcept {
    if (!session.provider_session_closed) {
        int32_t status = SAO_ERR_OS_CALL_FAILED;
        try {
            status = session.provider.close_session(session.provider.user_data,
                                                    session.provider_session);
        } catch (...) {
            status = SAO_ERR_OS_CALL_FAILED;
        }
        if (status != SAO_OK && status != SAO_ERR_HANDLE_INVALID)
            return status;
        session.provider_session_closed = true;
    }
    if (!session.provider_released) {
        try {
            session.provider.release(session.provider.user_data);
            session.provider_released = true;
        } catch (...) {
            return SAO_ERR_OS_CALL_FAILED;
        }
    }
    return SAO_OK;
}

void release_provider_session_count() noexcept {
    try {
        std::lock_guard lock(g_deps_provider_mutex);
        if (g_deps_provider.active_sessions > 0)
            --g_deps_provider.active_sessions;
    } catch (...) {
    }
}

void erase_deps_session(deps_session_t session) noexcept {
    try {
        std::lock_guard lock(g_deps_sessions_mutex);
        g_deps_sessions.erase(session);
    } catch (...) {
    }
}

std::string trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(),
                                        [](unsigned char ch) { return std::isspace(ch) != 0; });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
                          return std::isspace(ch) != 0;
                      }).base();
    return first < last ? std::string(first, last) : std::string{};
}

std::string distribution_name(std::string requirement) {
    const auto comment = requirement.find('#');
    if (comment != std::string::npos)
        requirement.resize(comment);
    requirement = trim(std::move(requirement));
    const auto extra = requirement.find('[');
    const auto version = requirement.find_first_of("<>=!~; ");
    const auto stop = std::min(extra == std::string::npos ? requirement.size() : extra,
                               version == std::string::npos ? requirement.size() : version);
    requirement.resize(stop);
    std::transform(requirement.begin(), requirement.end(), requirement.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return requirement;
}

bool dependency_present(const fs::path& root, std::string_view import_name) {
    std::error_code error;
    const auto package = root / fs::u8path(import_name);
    return fs::exists(package, error) || fs::exists(package.wstring() + L".py", error) ||
           fs::exists(package.wstring() + L".pyd", error) ||
           fs::exists(package.wstring() + L".dll", error);
}

} // namespace

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_deps_register_provider(const deps_provider* provider) {
    if (provider == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    if ((provider->abi_version >> 16u) != SAO_PLUGIN_DEPS_PROVIDER_ABI_VERSION_MAJOR ||
        provider->struct_size < kDepsProviderMinimumSize) {
        return SAO_PLUGINS_ERR_ABI_MISMATCH;
    }
    deps_provider copy{};
    std::memcpy(&copy, provider, std::min<size_t>(provider->struct_size, sizeof(copy)));
    if (copy.retain == nullptr || copy.release == nullptr || copy.create_session == nullptr ||
        copy.attach_path == nullptr || copy.restore_path == nullptr ||
        copy.close_session == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard lock(g_deps_provider_mutex);
        if (g_deps_provider.present)
            return SAO_PLUGINS_ERR_ALREADY_EXISTS;
        if (g_deps_provider.active_sessions != 0)
            return SAO_PLUGINS_ERR_BUSY;
        g_deps_provider.value = copy;
        g_deps_provider.present = true;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_deps_unregister_provider() {
    try {
        std::lock_guard lock(g_deps_provider_mutex);
        if (!g_deps_provider.present)
            return SAO_ERR_HANDLE_INVALID;
        if (g_deps_provider.active_sessions != 0)
            return SAO_PLUGINS_ERR_BUSY;
        g_deps_provider = {};
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_deps_attach(const char* plugin_id_utf8, const wchar_t* plugin_dir,
                        const deps_bootstrap_record* record, deps_session_t* out_session) {
    if (out_session != nullptr)
        *out_session = nullptr;
    if (plugin_id_utf8 == nullptr || plugin_id_utf8[0] == '\0' || plugin_dir == nullptr ||
        record == nullptr || out_session == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }

    deps_provider provider{};
    try {
        std::lock_guard lock(g_deps_provider_mutex);
        if (!g_deps_provider.present)
            return SAO_PLUGINS_ERR_UNSUPPORTED;
        provider = g_deps_provider.value;
        ++g_deps_provider.active_sessions;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }

    std::shared_ptr<deps_session_s> session;
    try {
        session = std::make_shared<deps_session_s>();
        session->provider = provider;
        session->attached_paths.reserve(record->added_paths.size());
    } catch (...) {
        release_provider_session_count();
        return SAO_ERR_OS_CALL_FAILED;
    }
    bool retained_provider = false;
    try {
        provider.retain(provider.user_data);
        retained_provider = true;
        deps_session_spec spec{};
        spec.struct_size = sizeof(spec);
        spec.plugin_id_utf8 = plugin_id_utf8;
        spec.plugin_dir = plugin_dir;
        int32_t status =
            provider.create_session(provider.user_data, &spec, &session->provider_session);
        if (status != SAO_OK || session->provider_session == nullptr) {
            if (session->provider_session == nullptr) {
                session->provider_session_closed = true;
            }
            const int32_t close_status = close_provider_session(*session);
            if (close_status != SAO_OK) {
                std::lock_guard lock(g_deps_sessions_mutex);
                g_deps_sessions.emplace(session.get(), session);
                *out_session = session.get();
                return close_status;
            }
            retained_provider = false;
            release_provider_session_count();
            return status == SAO_OK ? SAO_ERR_HANDLE_INVALID : status;
        }

        {
            std::lock_guard lock(g_deps_sessions_mutex);
            g_deps_sessions.emplace(session.get(), session);
        }
        for (const auto& path : record->added_paths) {
            session->attached_paths.emplace_back(path);
            status = provider.attach_path(provider.user_data, session->provider_session,
                                          session->attached_paths.back().c_str());
            if (status == SAO_OK)
                continue;
            const int32_t restore_status = restore_attached_paths(*session);
            if (restore_status != SAO_OK) {
                *out_session = session.get();
                return restore_status;
            }
            const int32_t close_status = close_provider_session(*session);
            if (close_status != SAO_OK) {
                *out_session = session.get();
                return close_status;
            }
            retained_provider = false;
            erase_deps_session(session.get());
            release_provider_session_count();
            return status;
        }
        *out_session = session.get();
        return SAO_OK;
    } catch (...) {
        if (session->provider_session != nullptr) {
            const int32_t restore_status = restore_attached_paths(*session);
            const int32_t close_status =
                restore_status == SAO_OK ? close_provider_session(*session) : restore_status;
            if (close_status != SAO_OK) {
                try {
                    std::lock_guard lock(g_deps_sessions_mutex);
                    g_deps_sessions.emplace(session.get(), session);
                    *out_session = session.get();
                } catch (...) {
                }
                return close_status;
            }
            retained_provider = false;
        } else if (retained_provider) {
            session->provider_session_closed = true;
            const int32_t close_status = close_provider_session(*session);
            if (close_status != SAO_OK) {
                try {
                    std::lock_guard lock(g_deps_sessions_mutex);
                    g_deps_sessions.emplace(session.get(), session);
                    *out_session = session.get();
                } catch (...) {
                }
                return close_status;
            }
            retained_provider = false;
        }
        if (retained_provider) {
            try {
                provider.release(provider.user_data);
            } catch (...) {
            }
        }
        erase_deps_session(session.get());
        release_provider_session_count();
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_deps_session_restore(deps_session_t session) {
    deps_session_lease lease;
    const int32_t lease_status = lease.acquire(session);
    if (lease_status != SAO_OK)
        return lease_status;
    return restore_attached_paths(lease.value());
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_deps_session_close(deps_session_t session) {
    deps_session_lease lease;
    const int32_t lease_status = lease.acquire(session);
    if (lease_status != SAO_OK)
        return lease_status;
    const int32_t restore_status = restore_attached_paths(lease.value());
    if (restore_status != SAO_OK)
        return restore_status;
    const int32_t close_status = close_provider_session(lease.value());
    if (close_status != SAO_OK)
        return close_status;
    erase_deps_session(session);
    release_provider_session_count();
    return SAO_OK;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_deps_ensure(
    const wchar_t* plugin_dir, bool allow_pip_install, deps_bootstrap_record* out_record) {
    if (plugin_dir == nullptr || out_record == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_record = deps_bootstrap_record{};
    try {
        const fs::path root(plugin_dir);
        std::error_code error;
        if (!fs::is_directory(root, error))
            return SAO_ERR_HANDLE_INVALID;
        std::vector<fs::path> search_roots;
        for (const auto* name : {L"libs", L"vendor", L"engine"}) {
            const auto path = root / name;
            if (fs::is_directory(path, error)) {
                search_roots.push_back(path);
                out_record->added_paths.push_back(fs::weakly_canonical(path, error).native());
            }
            error.clear();
        }
        const auto requirements = root / L"requirements.txt";
        if (!fs::is_regular_file(requirements, error))
            return SAO_OK;
        std::ifstream input(requirements);
        if (!input)
            return SAO_ERR_HANDLE_INVALID;
        std::string line;
        while (std::getline(input, line)) {
            auto distribution = distribution_name(line);
            if (distribution.empty() || distribution[0] == '-')
                continue;
            const auto map_iterator = dist_to_import_name_map().find(distribution);
            const auto import_name = map_iterator == dist_to_import_name_map().end()
                                         ? distribution
                                         : map_iterator->second;
            auto found = std::find_if(search_roots.begin(), search_roots.end(),
                                      [&import_name](const fs::path& path) {
                                          return dependency_present(path, import_name);
                                      });
            if (found == search_roots.end()) {
                out_record->deps_summary[distribution] = "missing";
                return allow_pip_install ? SAO_PLUGINS_ERR_UNSUPPORTED
                                         : SAO_PLUGINS_ERR_DEPENDENCY_MISSING;
            }
            out_record->deps_summary[distribution] =
                _wcsicmp(found->filename().c_str(), L"libs") == 0 ? "libs" : "vendor";
        }
        return SAO_OK;
    } catch (...) {
        *out_record = deps_bootstrap_record{};
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_deps_restore(const deps_bootstrap_record*) {}

const std::unordered_map<std::string, std::string>& dist_to_import_name_map() {
    // dist 名 → import 名 (对齐 python _IMPORT_NAME)。
    static const std::unordered_map<std::string, std::string> map_ = {
        {"pillow", "PIL"},
        {"pyyaml", "yaml"},
        {"beautifulsoup4", "bs4"},
    };
    return map_;
}

} // namespace sao::plugins::loader
