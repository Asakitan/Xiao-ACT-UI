// sdk_dumper_unreal.cpp — Unreal → *.h per-package emitter (Phase 10 production).
#include "sdk_dumper_base.h"

#include "sao/core/process.h"
#include "sao/mem_probe/engine/adapter.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace sao::ai_editor::sdk_dumper {
namespace {
class UnrealDumper : public DumperBase {
public:
    DumpKind kind() const override { return DumpKind::Unreal; }
    DumpResult dump(const DumpConfig& cfg) override {
        DumpResult r;
        if (!cfg.output_dir.empty()) {
            const fs::path out = fs::path(cfg.output_dir) / "Unreal-dump.h";
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
        r.error_message = "NOT_IMPLEMENTED: Unreal SDK dumper requires a version-specific GUObjectArray walk";
        return r;
    }
};
} // namespace
DumperBase* create_unreal() { return new UnrealDumper(); }
} // namespace sao::ai_editor::sdk_dumper
