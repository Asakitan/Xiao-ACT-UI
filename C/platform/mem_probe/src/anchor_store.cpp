// anchor_store.cpp — per-plugin JSON persistence of pointer chains.

#include "sao/mem_probe/anchor_store.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <shlobj.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
constexpr size_t kPluginIdMax = 64;
constexpr size_t kAnchorNameMax = 128;
constexpr size_t kModuleNameMax = 127;
std::mutex g_store_mutex;
std::atomic<uint64_t> g_temp_counter{0};

uint64_t lock_hash(std::wstring_view value) {
    uint64_t hash = 1469598103934665603ull;
    for (wchar_t character : value) {
        hash ^= static_cast<uint64_t>(character);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string lock_suffix(std::wstring_view value) {
    constexpr char kHex[] = "0123456789abcdef";
    const uint64_t hash = lock_hash(value);
    std::string result(16, '0');
    for (size_t i = 0; i < result.size(); ++i) {
        result[result.size() - i - 1] = kHex[(hash >> (i * 4)) & 0x0F];
    }
    return result;
}

#if defined(_WIN32)
std::wstring canonical_lowercase_directory(const fs::path& directory) {
    std::error_code error;
    fs::path canonical = fs::weakly_canonical(directory, error);
    if (error) {
        error.clear();
        canonical = fs::absolute(directory, error);
        if (error)
            return {};
        canonical = canonical.lexically_normal();
    }
    std::wstring value = canonical.native();
    for (wchar_t& character : value)
        character = static_cast<wchar_t>(std::towlower(character));
    return value;
}
#endif

class CrossProcessLock {
  public:
    explicit CrossProcessLock(const fs::path& file) {
#if defined(_WIN32)
        const std::wstring directory = canonical_lowercase_directory(file.parent_path());
        if (directory.empty())
            return;
        const std::string suffix = lock_suffix(directory);
        const std::wstring name =
            L"Local\\SAOAutoAnchor_" + std::wstring(suffix.begin(), suffix.end());
        mutex_ = CreateMutexW(nullptr, FALSE, name.c_str());
        if (mutex_ == nullptr)
            return;
        const DWORD result = WaitForSingleObject(mutex_, INFINITE);
        acquired_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
#else
        fs::path lock_file = file;
        lock_file += ".lock";
        std::error_code error;
        fs::create_directories(lock_file.parent_path(), error);
        if (error)
            return;
        fd_ = ::open(lock_file.c_str(), O_CREAT | O_RDWR, 0600);
        if (fd_ < 0)
            return;
        if (::flock(fd_, LOCK_EX) == 0) {
            acquired_ = true;
        } else {
            ::close(fd_);
            fd_ = -1;
        }
#endif
    }

    ~CrossProcessLock() {
#if defined(_WIN32)
        if (mutex_ != nullptr) {
            if (acquired_)
                ReleaseMutex(mutex_);
            CloseHandle(mutex_);
        }
#else
        if (fd_ >= 0) {
            if (acquired_)
                ::flock(fd_, LOCK_UN);
            ::close(fd_);
        }
#endif
    }

    bool acquired() const {
        return acquired_;
    }

  private:
#if defined(_WIN32)
    HANDLE mutex_ = nullptr;
#else
    int fd_ = -1;
#endif
    bool acquired_ = false;
};

size_t bounded_length(const char* value, size_t capacity) {
    if (value == nullptr)
        return capacity + 1;
    size_t length = 0;
    while (length < capacity && value[length] != '\0')
        ++length;
    return length;
}

bool valid_utf8(std::string_view value) {
    for (size_t i = 0; i < value.size();) {
        const uint8_t lead = static_cast<uint8_t>(value[i]);
        if (lead <= 0x7F) {
            ++i;
            continue;
        }
        if (lead >= 0xC2 && lead <= 0xDF) {
            if (i + 1 >= value.size())
                return false;
            const uint8_t b1 = static_cast<uint8_t>(value[i + 1]);
            if (b1 < 0x80 || b1 > 0xBF)
                return false;
            i += 2;
            continue;
        }
        if (lead >= 0xE0 && lead <= 0xEF) {
            if (i + 2 >= value.size())
                return false;
            const uint8_t b1 = static_cast<uint8_t>(value[i + 1]);
            const uint8_t b2 = static_cast<uint8_t>(value[i + 2]);
            if (b2 < 0x80 || b2 > 0xBF)
                return false;
            if (lead == 0xE0   ? (b1 < 0xA0 || b1 > 0xBF)
                : lead == 0xED ? (b1 < 0x80 || b1 > 0x9F)
                               : (b1 < 0x80 || b1 > 0xBF))
                return false;
            i += 3;
            continue;
        }
        if (lead >= 0xF0 && lead <= 0xF4) {
            if (i + 3 >= value.size())
                return false;
            const uint8_t b1 = static_cast<uint8_t>(value[i + 1]);
            const uint8_t b2 = static_cast<uint8_t>(value[i + 2]);
            const uint8_t b3 = static_cast<uint8_t>(value[i + 3]);
            if (b2 < 0x80 || b2 > 0xBF || b3 < 0x80 || b3 > 0xBF)
                return false;
            if (lead == 0xF0   ? (b1 < 0x90 || b1 > 0xBF)
                : lead == 0xF4 ? (b1 < 0x80 || b1 > 0x8F)
                               : (b1 < 0x80 || b1 > 0xBF))
                return false;
            i += 4;
            continue;
        }
        return false;
    }
    return true;
}

bool reserved_device_name(std::string_view value) {
    std::string base(value);
    const size_t dot = base.find('.');
    if (dot != std::string::npos)
        base.resize(dot);
    for (char& byte : base) {
        if (byte >= 'A' && byte <= 'Z')
            byte = static_cast<char>(byte - 'A' + 'a');
    }
    return base == "con" || base == "prn" || base == "aux" || base == "nul" ||
           (base.size() == 4 && (base.rfind("com", 0) == 0 || base.rfind("lpt", 0) == 0) &&
            base[3] >= '1' && base[3] <= '9');
}

bool valid_component(const char* value, size_t max_length) {
    const size_t length = bounded_length(value, max_length);
    if (length == 0 || length >= max_length || value == nullptr)
        return false;
    const std::string_view text(value, length);
    if (!valid_utf8(text) || reserved_device_name(text) || text == "." || text == ".." ||
        text.find("..") != std::string_view::npos || text.back() == '.' || text.back() == ' ')
        return false;
    for (unsigned char byte : text) {
        if (byte < 0x20 || byte == 0x7F || byte == '/' || byte == '\\' || byte == ':')
            return false;
    }
    return true;
}

fs::path path_from_utf8(std::string_view value) {
    std::u8string converted(value.size(), u8'\0');
    if (!value.empty())
        std::memcpy(converted.data(), value.data(), value.size());
    return fs::path(converted);
}

bool valid_chain(const sao_memprobe_ptr_chain_t& chain) {
    const size_t length = bounded_length(chain.module_name_utf8, sizeof(chain.module_name_utf8));
    return valid_component(chain.module_name_utf8, sizeof(chain.module_name_utf8)) &&
           length <= kModuleNameMax && chain.module_base_at_find != 0 && chain.static_offset >= 0 &&
           chain.deref_count <= 16;
}

bool valid_chain_v2(const sao_memprobe_ptr_chain_v2_t& chain) {
    sao_memprobe_ptr_chain_t v1{};
    std::memcpy(&v1, &chain, sizeof(v1));
    const size_t provenance_length =
        bounded_length(chain.provenance_utf8, sizeof(chain.provenance_utf8));
    return valid_chain(v1) && chain.struct_size == sizeof(sao_memprobe_ptr_chain_v2_t) &&
           chain.abi_version == SAO_MEMPROBE_PTR_CHAIN_V2_ABI_VERSION &&
           provenance_length < sizeof(chain.provenance_utf8) &&
           valid_utf8(std::string_view(chain.provenance_utf8, provenance_length)) &&
           (chain.validation_state == SAO_MEMPROBE_PTR_CHAIN_VALIDATION_CANDIDATE ||
            chain.validation_state == SAO_MEMPROBE_PTR_CHAIN_VALIDATION_VALIDATED);
}

bool resolve_plugin_dir(const char* plugin_id, fs::path* out_dir) {
    if (!valid_component(plugin_id, kPluginIdMax) || out_dir == nullptr)
        return false;
#if defined(_WIN32)
    wchar_t* appdata = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata) == S_OK &&
        appdata != nullptr) {
        *out_dir = fs::path(appdata) / L"SAOAuto" / L"plugins" /
            path_from_utf8(plugin_id);
        CoTaskMemFree(appdata);
        return true;
    }
#endif
    const char* env = std::getenv("APPDATA");
    if (env == nullptr || env[0] == '\0')
        return false;
    *out_dir = path_from_utf8(env) / L"SAOAuto" / L"plugins" /
        path_from_utf8(plugin_id);
    return true;
}

bool resolve_anchor_file(const char* plugin_id, fs::path* out_file) {
    fs::path dir;
    if (!resolve_plugin_dir(plugin_id, &dir) || out_file == nullptr)
        return false;
    *out_file = dir / L"anchors.json";
    return true;
}

json chain_to_json(const sao_memprobe_ptr_chain_t& chain) {
    json result;
    result["module"] = chain.module_name_utf8;
    result["module_base_at_find"] = chain.module_base_at_find;
    result["static_offset"] = chain.static_offset;
    result["final_offset"] = chain.final_offset;
    result["deref_offsets"] = json::array();
    for (uint32_t i = 0; i < chain.deref_count; ++i)
        result["deref_offsets"].push_back(chain.deref_offsets[i]);
    return result;
}

json chain_to_json_v2(const sao_memprobe_ptr_chain_v2_t& chain) {
    sao_memprobe_ptr_chain_t v1{};
    std::memcpy(&v1, &chain, sizeof(v1));
    json result = chain_to_json(v1);
    result["struct_size"] = chain.struct_size;
    result["abi_version"] = chain.abi_version;
    result["process_pid_at_find"] = chain.process_pid_at_find;
    result["module_fingerprint"] = chain.module_fingerprint;
    result["anchor_address_at_find"] = chain.anchor_address_at_find;
    result["provenance"] = chain.provenance_utf8;
    result["validation_state"] = chain.validation_state;
    return result;
}

bool chain_from_json(const json& value, sao_memprobe_ptr_chain_t& out) {
    if (!value.is_object() || !value.contains("module") || !value.contains("module_base_at_find") ||
        !value.contains("static_offset") || !value.contains("final_offset") ||
        !value.contains("deref_offsets"))
        return false;
    if (!value["module"].is_string() || !value["module_base_at_find"].is_number_unsigned() ||
        !value["static_offset"].is_number_integer() || !value["final_offset"].is_number_integer() ||
        !value["deref_offsets"].is_array())
        return false;
    const std::string module = value["module"].get<std::string>();
    if (!valid_component(module.c_str(), sizeof(out.module_name_utf8)) ||
        module.size() > kModuleNameMax)
        return false;
    const auto& offsets = value["deref_offsets"];
    if (offsets.size() > 16)
        return false;
    std::memset(&out, 0, sizeof(out));
    std::memcpy(out.module_name_utf8, module.data(), module.size());
    out.module_name_utf8[module.size()] = '\0';
    out.module_base_at_find = value["module_base_at_find"].get<uint64_t>();
    out.static_offset = value["static_offset"].get<int64_t>();
    out.final_offset = value["final_offset"].get<int32_t>();
    out.deref_count = static_cast<uint32_t>(offsets.size());
    for (uint32_t i = 0; i < out.deref_count; ++i) {
        if (!offsets[i].is_number_integer())
            return false;
        out.deref_offsets[i] = offsets[i].get<int32_t>();
    }
    return valid_chain(out);
}

bool chain_from_json_v2(const json& value, sao_memprobe_ptr_chain_v2_t& out) {
    sao_memprobe_ptr_chain_t v1{};
    if (!chain_from_json(value, v1))
        return false;
    std::memset(&out, 0, sizeof(out));
    std::memcpy(&out, &v1, sizeof(v1));
    if (!value.contains("struct_size") || !value["struct_size"].is_number_unsigned() ||
        !value.contains("abi_version") || !value["abi_version"].is_number_unsigned())
        return false;
    out.struct_size = value["struct_size"].get<uint32_t>();
    out.abi_version = value["abi_version"].get<uint32_t>();
    if (value.contains("process_pid_at_find")) {
        if (!value["process_pid_at_find"].is_number_unsigned())
            return false;
        out.process_pid_at_find = value["process_pid_at_find"].get<uint32_t>();
    }
    if (value.contains("module_fingerprint")) {
        if (!value["module_fingerprint"].is_number_unsigned())
            return false;
        out.module_fingerprint = value["module_fingerprint"].get<uint64_t>();
    }
    if (value.contains("anchor_address_at_find")) {
        if (!value["anchor_address_at_find"].is_number_unsigned())
            return false;
        out.anchor_address_at_find = value["anchor_address_at_find"].get<uint64_t>();
    }
    if (value.contains("validation_state")) {
        if (!value["validation_state"].is_number_unsigned())
            return false;
        out.validation_state = value["validation_state"].get<uint32_t>();
    }
    if (value.contains("provenance")) {
        if (!value["provenance"].is_string())
            return false;
        const std::string provenance = value["provenance"].get<std::string>();
        if (provenance.size() >= sizeof(out.provenance_utf8) || !valid_utf8(provenance))
            return false;
        std::memcpy(out.provenance_utf8, provenance.data(), provenance.size());
        out.provenance_utf8[provenance.size()] = '\0';
    }
    return valid_chain_v2(out);
}

bool load_root(const fs::path& file, json* out_root) {
    if (out_root == nullptr)
        return false;
    if (!fs::exists(file)) {
        *out_root = json{{"chains", json::array()}};
        return true;
    }
    std::ifstream input(file, std::ios::binary);
    if (!input)
        return false;
    json root;
    try {
        input >> root;
    } catch (...) {
        return false;
    }
    if (!root.is_object() || !root.contains("chains") || !root["chains"].is_array())
        return false;
    std::set<std::string> names;
    for (const auto& entry : root["chains"]) {
        if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string() ||
            !entry.contains("chain"))
            return false;
        const std::string name = entry["name"].get<std::string>();
        if (!valid_component(name.c_str(), kAnchorNameMax) || !names.insert(name).second)
            return false;
    }
    *out_root = std::move(root);
    return true;
}

#if defined(_WIN32)
bool write_bytes(HANDLE handle, const std::string& bytes) {
    size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD amount = static_cast<DWORD>(
            std::min<size_t>(bytes.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(handle, bytes.data() + offset, amount, &written, nullptr) ||
            written != amount)
            return false;
        offset += written;
    }
    return FlushFileBuffers(handle) != FALSE;
}
#endif

bool write_atomic(const fs::path& file, const json& root) {
    std::error_code error;
    fs::create_directories(file.parent_path(), error);
    if (error)
        return false;
    const std::string bytes = root.dump(2);
#if defined(_WIN32)
    const std::wstring temp = file.wstring() + L".tmp-" + std::to_wstring(GetCurrentProcessId()) +
                              L"-" + std::to_wstring(GetCurrentThreadId()) + L"-" +
                              std::to_wstring(g_temp_counter.fetch_add(1));
    HANDLE handle = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    const bool wrote = write_bytes(handle, bytes);
    const bool closed = CloseHandle(handle) != FALSE;
    if (!wrote || !closed) {
        DeleteFileW(temp.c_str());
        return false;
    }
    if (ReplaceFileW(file.wstring().c_str(), temp.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH,
                     nullptr, nullptr) != FALSE)
        return true;
    if (GetLastError() == ERROR_FILE_NOT_FOUND &&
        MoveFileExW(temp.c_str(), file.wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE)
        return true;
    DeleteFileW(temp.c_str());
    return false;
#else
    fs::path temp = file;
    temp += ".tmp-" + std::to_string(g_temp_counter.fetch_add(1));
    auto remove_temp = [&]() {
        std::error_code cleanup_error;
        fs::remove(temp, cleanup_error);
    };
    std::ofstream output(temp, std::ios::binary | std::ios::trunc);
    if (!output) {
        remove_temp();
        return false;
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    if (!output) {
        output.close();
        remove_temp();
        return false;
    }
    output.close();
    if (!output) {
        remove_temp();
        return false;
    }
    fs::rename(temp, file, error);
    if (!error)
        return true;
    remove_temp();
    return false;
#endif
}

} // namespace

extern "C" sao_status_t SAO_CORE_CALL
sao_memprobe_anchor_save(const char* plugin_id_utf8, const char* anchor_name_utf8,
                         const sao_memprobe_ptr_chain_t* chain) {
    try {
        if (!valid_component(plugin_id_utf8, kPluginIdMax) ||
            !valid_component(anchor_name_utf8, kAnchorNameMax) || chain == nullptr ||
            !valid_chain(*chain))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        fs::path file;
        if (!resolve_anchor_file(plugin_id_utf8, &file))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        std::lock_guard lock(g_store_mutex);
        CrossProcessLock cross_process_lock(file);
        if (!cross_process_lock.acquired())
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        json root;
        if (!load_root(file, &root))
            return SAO_STATUS_ERR_READ_FAULT;
        bool found = false;
        for (auto& entry : root["chains"]) {
            if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string() ||
                !entry.contains("chain"))
                return SAO_STATUS_ERR_READ_FAULT;
            const std::string name = entry["name"].get<std::string>();
            if (!valid_component(name.c_str(), kAnchorNameMax))
                return SAO_STATUS_ERR_READ_FAULT;
            if (name == anchor_name_utf8) {
                entry["chain"] = chain_to_json(*chain);
                found = true;
                break;
            }
        }
        if (!found)
            root["chains"].push_back(
                json{{"name", anchor_name_utf8}, {"chain", chain_to_json(*chain)}});
        return write_atomic(file, root) ? SAO_STATUS_OK : SAO_STATUS_ERR_OS_CALL_FAILED;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}
extern "C" sao_status_t SAO_CORE_CALL sao_memprobe_anchor_load(
    const char* plugin_id_utf8, const char* anchor_name_utf8, sao_memprobe_ptr_chain_t* out_chain) {
    if (out_chain != nullptr)
        std::memset(out_chain, 0, sizeof(*out_chain));
    try {
        if (!valid_component(plugin_id_utf8, kPluginIdMax) ||
            !valid_component(anchor_name_utf8, kAnchorNameMax) || out_chain == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        fs::path file;
        if (!resolve_anchor_file(plugin_id_utf8, &file))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        std::lock_guard lock(g_store_mutex);
        CrossProcessLock cross_process_lock(file);
        if (!cross_process_lock.acquired())
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        json root;
        if (!load_root(file, &root))
            return SAO_STATUS_ERR_READ_FAULT;
        for (const auto& entry : root["chains"]) {
            if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string() ||
                !entry.contains("chain"))
                return SAO_STATUS_ERR_READ_FAULT;
            const std::string name = entry["name"].get<std::string>();
            if (!valid_component(name.c_str(), kAnchorNameMax))
                return SAO_STATUS_ERR_READ_FAULT;
            if (name == anchor_name_utf8) {
                sao_memprobe_ptr_chain_t candidate{};
                const auto& stored_chain = entry["chain"];
                if (stored_chain.is_object() && (stored_chain.contains("struct_size") ||
                                                 stored_chain.contains("abi_version"))) {
                    sao_memprobe_ptr_chain_v2_t candidate_v2{};
                    if (!chain_from_json_v2(stored_chain, candidate_v2) ||
                        candidate_v2.validation_state !=
                            SAO_MEMPROBE_PTR_CHAIN_VALIDATION_VALIDATED ||
                        std::strncmp(candidate_v2.provenance_utf8, "pointer_chain.validated",
                                     sizeof(candidate_v2.provenance_utf8)) != 0)
                        return SAO_STATUS_ERR_CAPABILITY_MISSING;
                    std::memcpy(&candidate, &candidate_v2, sizeof(candidate));
                } else if (!chain_from_json(stored_chain, candidate)) {
                    return SAO_STATUS_ERR_READ_FAULT;
                }
                *out_chain = candidate;
                return SAO_STATUS_OK;
            }
        }
        return SAO_STATUS_ERR_NOT_FOUND;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_CORE_CALL
sao_memprobe_anchor_save_v2(const char* plugin_id_utf8, const char* anchor_name_utf8,
                            const sao_memprobe_ptr_chain_v2_t* chain) {
    try {
        if (!valid_component(plugin_id_utf8, kPluginIdMax) ||
            !valid_component(anchor_name_utf8, kAnchorNameMax) || chain == nullptr ||
            !valid_chain_v2(*chain))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (chain->validation_state != SAO_MEMPROBE_PTR_CHAIN_VALIDATION_VALIDATED ||
            std::strncmp(chain->provenance_utf8, "pointer_chain.validated",
                         sizeof(chain->provenance_utf8)) != 0)
            return SAO_STATUS_ERR_CAPABILITY_MISSING;
        fs::path file;
        if (!resolve_anchor_file(plugin_id_utf8, &file))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        std::lock_guard lock(g_store_mutex);
        CrossProcessLock cross_process_lock(file);
        if (!cross_process_lock.acquired())
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        json root;
        if (!load_root(file, &root))
            return SAO_STATUS_ERR_READ_FAULT;
        bool found = false;
        for (auto& entry : root["chains"]) {
            if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string() ||
                !entry.contains("chain"))
                return SAO_STATUS_ERR_READ_FAULT;
            const std::string name = entry["name"].get<std::string>();
            if (!valid_component(name.c_str(), kAnchorNameMax))
                return SAO_STATUS_ERR_READ_FAULT;
            if (name == anchor_name_utf8) {
                entry["chain"] = chain_to_json_v2(*chain);
                found = true;
                break;
            }
        }
        if (!found)
            root["chains"].push_back(
                json{{"name", anchor_name_utf8}, {"chain", chain_to_json_v2(*chain)}});
        return write_atomic(file, root) ? SAO_STATUS_OK : SAO_STATUS_ERR_OS_CALL_FAILED;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_CORE_CALL
sao_memprobe_anchor_load_v2(const char* plugin_id_utf8, const char* anchor_name_utf8,
                            sao_memprobe_ptr_chain_v2_t* out_chain) {
    if (out_chain != nullptr)
        std::memset(out_chain, 0, sizeof(*out_chain));
    try {
        if (!valid_component(plugin_id_utf8, kPluginIdMax) ||
            !valid_component(anchor_name_utf8, kAnchorNameMax) || out_chain == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        fs::path file;
        if (!resolve_anchor_file(plugin_id_utf8, &file))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        std::lock_guard lock(g_store_mutex);
        CrossProcessLock cross_process_lock(file);
        if (!cross_process_lock.acquired())
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        json root;
        if (!load_root(file, &root))
            return SAO_STATUS_ERR_READ_FAULT;
        for (const auto& entry : root["chains"]) {
            if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string() ||
                !entry.contains("chain"))
                return SAO_STATUS_ERR_READ_FAULT;
            const std::string name = entry["name"].get<std::string>();
            if (!valid_component(name.c_str(), kAnchorNameMax))
                return SAO_STATUS_ERR_READ_FAULT;
            if (name == anchor_name_utf8) {
                sao_memprobe_ptr_chain_v2_t candidate{};
                if (!chain_from_json_v2(entry["chain"], candidate))
                    return SAO_STATUS_ERR_READ_FAULT;
                if (candidate.validation_state != SAO_MEMPROBE_PTR_CHAIN_VALIDATION_VALIDATED ||
                    std::strncmp(candidate.provenance_utf8, "pointer_chain.validated",
                                 sizeof(candidate.provenance_utf8)) != 0)
                    return SAO_STATUS_ERR_CAPABILITY_MISSING;
                *out_chain = candidate;
                return SAO_STATUS_OK;
            }
        }
        return SAO_STATUS_ERR_NOT_FOUND;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_CORE_CALL sao_memprobe_anchor_delete(const char* plugin_id_utf8,
                                                                 const char* anchor_name_utf8) {
    try {
        if (!valid_component(plugin_id_utf8, kPluginIdMax) ||
            !valid_component(anchor_name_utf8, kAnchorNameMax))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        fs::path file;
        if (!resolve_anchor_file(plugin_id_utf8, &file))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        std::lock_guard lock(g_store_mutex);
        CrossProcessLock cross_process_lock(file);
        if (!cross_process_lock.acquired())
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        json root;
        if (!load_root(file, &root))
            return SAO_STATUS_ERR_READ_FAULT;
        auto& chains = root["chains"];
        for (auto it = chains.begin(); it != chains.end(); ++it) {
            if (!it->is_object() || !it->contains("name") || !(*it)["name"].is_string() ||
                !it->contains("chain"))
                return SAO_STATUS_ERR_READ_FAULT;
            const std::string name = (*it)["name"].get<std::string>();
            if (!valid_component(name.c_str(), kAnchorNameMax))
                return SAO_STATUS_ERR_READ_FAULT;
            if (name == anchor_name_utf8) {
                chains.erase(it);
                return write_atomic(file, root) ? SAO_STATUS_OK : SAO_STATUS_ERR_OS_CALL_FAILED;
            }
        }
        return SAO_STATUS_ERR_NOT_FOUND;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}