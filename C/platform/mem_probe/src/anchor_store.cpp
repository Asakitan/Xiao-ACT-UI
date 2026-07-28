// anchor_store.cpp — per-plugin JSON persistence for pointer chains.
//
// Phase 5 (Python parity closure).
// Uses nlohmann::json for schema; atomic write via tmp-rename.

#include "sao/mem_probe/anchor_store.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <shlobj.h>
#endif

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

fs::path plugin_dir(const char* plugin_id) {
    if (plugin_id == nullptr) plugin_id = "unknown";
#if defined(_WIN32)
    wchar_t* appdata = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata) == S_OK &&
        appdata != nullptr) {
        fs::path base(appdata);
        CoTaskMemFree(appdata);
        return base / "SAOAuto" / "plugins" / plugin_id;
    }
#endif
    const char* env = std::getenv("APPDATA");
    if (env == nullptr) env = ".";
    return fs::path(env) / "SAOAuto" / "plugins" / plugin_id;
}

fs::path anchor_file(const char* plugin_id) {
    return plugin_dir(plugin_id) / "anchors.json";
}

json chain_to_json(const sao_memprobe_ptr_chain_t& c) {
    json j;
    j["module"] = c.module_name_utf8;
    j["module_base_at_find"] = c.module_base_at_find;
    j["static_offset"] = c.static_offset;
    j["final_offset"] = c.final_offset;
    j["deref_offsets"] = json::array();
    for (uint32_t i = 0; i < c.deref_count; ++i) {
        j["deref_offsets"].push_back(c.deref_offsets[i]);
    }
    return j;
}

bool chain_from_json(const json& j, sao_memprobe_ptr_chain_t& out) {
    if (!j.is_object()) return false;
    std::memset(&out, 0, sizeof(out));
    if (j.contains("module") && j["module"].is_string()) {
        std::string m = j["module"].get<std::string>();
        std::snprintf(out.module_name_utf8, sizeof(out.module_name_utf8), "%s",
                      m.c_str());
    }
    out.module_base_at_find = j.value("module_base_at_find", uint64_t{0});
    out.static_offset = j.value("static_offset", int64_t{0});
    out.final_offset = j.value("final_offset", int32_t{0});
    if (j.contains("deref_offsets") && j["deref_offsets"].is_array()) {
        uint32_t n = 0;
        for (const auto& x : j["deref_offsets"]) {
            if (n >= 16) break;
            out.deref_offsets[n++] = x.get<int32_t>();
        }
        out.deref_count = n;
    }
    return true;
}

json load_root(const fs::path& file) {
    if (!fs::exists(file)) return json::object({{"chains", json::array()}});
    std::ifstream in(file);
    if (!in) return json::object({{"chains", json::array()}});
    json j;
    try {
        in >> j;
    } catch (...) {
        return json::object({{"chains", json::array()}});
    }
    if (!j.is_object() || !j.contains("chains")) {
        return json::object({{"chains", json::array()}});
    }
    return j;
}

bool write_atomic(const fs::path& file, const json& j) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp);
        if (!out) return false;
        out << j.dump(2);
    }
    fs::rename(tmp, file, ec);
    return !ec;
}

} // namespace

extern "C" sao_status_t SAO_CORE_CALL sao_memprobe_anchor_save(
    const char* plugin_id_utf8, const char* anchor_name_utf8,
    const sao_memprobe_ptr_chain_t* chain) {
    if (plugin_id_utf8 == nullptr || anchor_name_utf8 == nullptr || chain == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    fs::path f = anchor_file(plugin_id_utf8);
    json root = load_root(f);
    // Replace-or-append.
    bool found = false;
    for (auto& entry : root["chains"]) {
        if (entry.value("name", "") == anchor_name_utf8) {
            entry["chain"] = chain_to_json(*chain);
            found = true;
            break;
        }
    }
    if (!found) {
        root["chains"].push_back(json{{"name", anchor_name_utf8},
                                       {"chain", chain_to_json(*chain)}});
    }
    return write_atomic(f, root) ? SAO_STATUS_OK : SAO_STATUS_ERR_OS_CALL_FAILED;
}

extern "C" sao_status_t SAO_CORE_CALL sao_memprobe_anchor_load(
    const char* plugin_id_utf8, const char* anchor_name_utf8,
    sao_memprobe_ptr_chain_t* out_chain) {
    if (plugin_id_utf8 == nullptr || anchor_name_utf8 == nullptr || out_chain == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    fs::path f = anchor_file(plugin_id_utf8);
    json root = load_root(f);
    for (const auto& entry : root["chains"]) {
        if (entry.value("name", "") == anchor_name_utf8) {
            if (!entry.contains("chain")) return SAO_STATUS_ERR_NOT_FOUND;
            if (!chain_from_json(entry["chain"], *out_chain))
                return SAO_STATUS_ERR_READ_FAULT;
            return SAO_STATUS_OK;
        }
    }
    return SAO_STATUS_ERR_NOT_FOUND;
}

extern "C" sao_status_t SAO_CORE_CALL sao_memprobe_anchor_delete(
    const char* plugin_id_utf8, const char* anchor_name_utf8) {
    if (plugin_id_utf8 == nullptr || anchor_name_utf8 == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    fs::path f = anchor_file(plugin_id_utf8);
    json root = load_root(f);
    json& arr = root["chains"];
    for (auto it = arr.begin(); it != arr.end(); ++it) {
        if (it->value("name", "") == anchor_name_utf8) {
            arr.erase(it);
            return write_atomic(f, root) ? SAO_STATUS_OK : SAO_STATUS_ERR_OS_CALL_FAILED;
        }
    }
    return SAO_STATUS_ERR_NOT_FOUND;
}
