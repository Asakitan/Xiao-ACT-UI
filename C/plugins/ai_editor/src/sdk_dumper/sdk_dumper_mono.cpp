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
        sao_memprobe_engine_list_classes(adapter, offsets.data(), cap, &off_ct,
                                          names.data(), names.size(), &used);
        fs::path out = fs::path(cfg.output_dir) / "Mono-dump.cs";
        std::ofstream fs_out(out);
        fs_out << "// SAO Auto Mono SDK dump. PID=" << cfg.target_pid
               << " enumerated=" << off_ct << "\n";
        for (size_t i = 0; i < off_ct; ++i) {
            fs_out << "// symbol " << (names.data() + offsets[i]) << "\n";
        }
        sao_memprobe_engine_close(adapter);
        sao_core_process_close(h);
        r.ok = true;
        r.output_files.push_back(out.string());
        r.class_count = static_cast<uint32_t>(off_ct);
        return r;
    }
};
} // namespace
DumperBase* create_mono() { return new MonoDumper(); }
} // namespace sao::ai_editor::sdk_dumper
