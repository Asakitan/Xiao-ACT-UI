// sdk_dumper_source.cpp — Source engine .dt / netvar dump (Phase 10 skeleton).
#include "sdk_dumper_base.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace sao::ai_editor::sdk_dumper {
namespace {
class SourceDumper : public DumperBase {
public:
    DumpKind kind() const override { return DumpKind::Source; }
    DumpResult dump(const DumpConfig& cfg) override {
        DumpResult r;
        if (cfg.output_dir.empty()) { r.error_message = "output_dir required"; return r; }
        std::error_code ec;
        fs::create_directories(cfg.output_dir, ec);
        fs::path out = fs::path(cfg.output_dir) / "Source-netvars.dt";
        std::ofstream fs_out(out);
        fs_out << "// SAO Auto Source engine netvar dump. PID=" << cfg.target_pid << "\n"
               << "// Source netvar table walk requires engine-specific ClientClass "
                  "signature scan; deferred to live impl.\n";
        r.ok = true;
        r.output_files.push_back(out.string());
        return r;
    }
};
} // namespace
DumperBase* create_source() { return new SourceDumper(); }
} // namespace sao::ai_editor::sdk_dumper
