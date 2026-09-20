// pymini_common.h — shared internals for the pymini engine.
//
// pymini is a zero-dependency tree-walking interpreter for the Python subset
// real plugins use (see docs/legacy-compat-design.md §"pymini scope").
// Everything here is in-tree only — never exported across a plugin ABI.
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

namespace sao::plugins::pymini {

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
struct py_error {
    int32_t code = 0;              // sao_status-compatible code
    std::string kind;              // "SyntaxError"/"ImportError"/...
    std::string message;
    src_pos pos;
    std::string file;              // module file (utf8)
    std::vector<std::string> features; // unsupported-feature names
};

// ── interpreter global lock (the "GIL") ──────────────────────────────────
// One recursive_mutex per interpreter instance; every eval path holds it via
// gil_guard. Native callbacks entering the interpreter re-acquire it.
class interpreter;
class gil_guard {
  public:
    explicit gil_guard(interpreter& i);
    ~gil_guard();
    gil_guard(const gil_guard&) = delete;
    gil_guard& operator=(const gil_guard&) = delete;
  private:
    interpreter& interp_;
};

} // namespace sao::plugins::pymini
