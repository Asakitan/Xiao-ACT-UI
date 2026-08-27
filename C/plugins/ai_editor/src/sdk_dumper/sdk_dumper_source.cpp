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
        if (!cfg.output_dir.empty()) {
            const fs::path out = fs::path(cfg.output_dir) / "Source-netvars.dt";
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
        }
        r.error_message = "NOT_IMPLEMENTED: Source SDK dumper requires an engine-specific ClientClass walk";
        return r;
    }
};
} // namespace
DumperBase* create_source() { return new SourceDumper(); }
} // namespace sao::ai_editor::sdk_dumper
