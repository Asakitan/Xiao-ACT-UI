// sdk_dumper_mono.cpp — Mono → *.cs emitter (Phase 10 production).
#include "sdk_dumper_base.h"

#include "sao/core/process.h"
#include "sao/mem_probe/engine/adapter.h"

#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace sao::ai_editor::sdk_dumper {
namespace {
class MonoDumper : public DumperBase {
public:
    DumpKind kind() const override { return DumpKind::Mono; }
    DumpResult dump(const DumpConfig& cfg) override {
        DumpResult r;
        if (cfg.output_dir.empty()) { r.error_message = "output_dir required"; return r; }
        if (cfg.target_pid == 0) { r.error_message = "target_pid required"; return r; }
        fs::path out = fs::path(cfg.output_dir) / "Mono-dump.cs";
        std::error_code output_error;
        if (fs::exists(out, output_error)) {
            if (output_error || fs::is_directory(out, output_error)) {
                r.error_message = "cannot remove stale output file";
                return r;
            }
            output_error.clear();
            fs::remove(out, output_error);
            if (output_error) {
                r.error_message = "cannot remove stale output file";
                return r;
            }
        } else if (output_error) {
            r.error_message = "cannot inspect stale output file";
            return r;
        }
        std::error_code ec;
        fs::create_directories(cfg.output_dir, ec);
        sao_core_process_handle_t h = nullptr;
        if (sao_core_process_open(cfg.target_pid,
                                   SAO_PROCESS_ACCESS_INFO | SAO_PROCESS_ACCESS_READ,
                                   &h) != SAO_STATUS_OK || h == nullptr) {
            r.error_message = "cannot open target process";
            return r;
        }
        auto kind = sao_memprobe_engine_detect(h);
        if (kind != SAO_MEMPROBE_ENGINE_MONO) {
            sao_core_process_close(h);
            r.error_message = "target is not Mono";
            return r;
        }
        auto* adapter = sao_memprobe_engine_open(h, SAO_MEMPROBE_ENGINE_MONO);
        if (adapter == nullptr) {
            sao_core_process_close(h);
            r.error_message = "mono adapter open failed";
            return r;
        }
        const uint32_t cap = std::min<uint32_t>(cfg.max_classes, 10'000);
        std::vector<uint32_t> offsets(cap);
        std::vector<char> names(cap * 64);
        size_t off_ct = 0;
        size_t used = 0;
        const sao_status_t st = sao_memprobe_engine_list_classes(
            adapter, offsets.data(), cap, &off_ct, names.data(), names.size(),
            &used);
        if (st != SAO_STATUS_OK) {
            sao_memprobe_engine_close(adapter);
            sao_core_process_close(h);
            r.error_message = "Mono class enumeration failed";
            return r;
        }
        std::ofstream fs_out(out);
        if (!fs_out) {
            sao_memprobe_engine_close(adapter);
            sao_core_process_close(h);
            r.error_message = "cannot open output file";
            return r;
        }
        fs_out << "// SAO Auto Mono SDK dump. PID=" << cfg.target_pid
               << " enumerated=" << off_ct << "\n";
        for (size_t i = 0; i < off_ct; ++i) {
            fs_out << "// symbol " << (names.data() + offsets[i]) << "\n";
        }
        fs_out.flush();
        const bool output_ok = fs_out.good();
        fs_out.close();
        sao_memprobe_engine_close(adapter);
        sao_core_process_close(h);
        if (!output_ok || !fs_out.good()) {
            std::error_code remove_error;
            fs::remove(out, remove_error);
            r.error_message = "Mono dump output write failed";
            return r;
        }
        r.ok = true;
        r.output_files.push_back(out.string());
        r.class_count = static_cast<uint32_t>(off_ct);
        return r;
    }
};
} // namespace
DumperBase* create_mono() { return new MonoDumper(); }
} // namespace sao::ai_editor::sdk_dumper
