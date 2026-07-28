// sdk_dumper_base.h — 4 game-engine SDK dumper abstraction.
//
// Phase 10 (Python parity closure) — port of python/ai_editor/sdk_dumper/*.py.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sao::ai_editor::sdk_dumper {

enum class DumpKind { IL2CPP, Mono, Unreal, Source };

struct DumpConfig {
    uint32_t target_pid = 0;
    std::string output_dir;      // 输出目录 (per-plugin working dir).
    uint32_t max_classes = 100'000; // safety cap
    uint32_t timeout_ms = 60'000;
};

struct DumpResult {
    bool ok = false;
    std::string error_message;
    std::vector<std::string> output_files;
    uint32_t class_count = 0;
};

class DumperBase {
public:
    virtual ~DumperBase() = default;
    virtual DumpKind kind() const = 0;
    virtual DumpResult dump(const DumpConfig& cfg) = 0;
};

DumperBase* create_dumper(DumpKind kind);

} // namespace sao::ai_editor::sdk_dumper
