// csmini_common.h — shared internals for the csmini engine.
//
// csmini is a zero-dependency tree-walking interpreter for the C# subset real
// plugins use (mirrors pymini 1:1 — see plugins/pymini/README.md).  Everything
// here is in-tree only — never exported across a plugin ABI.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sao::plugins::csmini {

// ── interned strings ──────────────────────────────────────────────────────
// Identifier-heavy code benefits from pointer-compare interning.
struct intern_pool {
    const std::string* get(std::string_view s) {
        std::lock_guard lock(mutex_);
        auto [it, _] = pool_.emplace(s);
        return &*it;
    }
  private:
    std::mutex mutex_;
    std::unordered_set<std::string> pool_;
};

// ── diagnostics ───────────────────────────────────────────────────────────
struct src_pos {
    uint32_t line = 0;
    uint32_t col = 0;
};

// Engine error surfaced to adapter / preflight; `features` lists the
// unsupported constructs encountered (prepopulated by the feature scanner).
struct cs_error {
    int32_t code = 0;              // sao_status-compatible code
    std::string kind;              // "SyntaxError"/"TypeError"/...
    std::string message;
    src_pos pos;
    std::string file;              // module file (utf8)
    std::vector<std::string> features; // unsupported-feature names
};

// ── interpreter global lock (the "GIL" analog) ────────────────────────────
// One recursive_mutex per interpreter instance; every eval path holds it via
// cs_guard. Native callbacks entering the interpreter re-acquire it.
class interpreter;
class cs_guard {
  public:
    explicit cs_guard(interpreter& i);
    ~cs_guard();
    cs_guard(const cs_guard&) = delete;
    cs_guard& operator=(const cs_guard&) = delete;
  private:
    interpreter& interp_;
};

} // namespace sao::plugins::csmini
