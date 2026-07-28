// perf_probe.cpp — Phase 14 (Python parity closure).
//
// Ring-buffer per section, background percentile compute on demand.
// Zero-cost when disabled: begin returns 0 immediately.

#include "sao/core/perf_probe.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr size_t kRingCap = 4096;

struct SectionRing {
    std::mutex mu;
    std::vector<uint64_t> samples_ns;
    uint64_t count = 0;
    uint64_t total_ns = 0;
    SectionRing() { samples_ns.reserve(kRingCap); }
};

struct Registry {
    std::mutex mu;
    std::unordered_map<std::string, SectionRing*> sections;
    std::atomic<bool> enabled{false};
};

Registry& registry() {
    static Registry r;
    return r;
}

SectionRing& get_section(const char* name) {
    auto& r = registry();
    std::lock_guard lock(r.mu);
    auto it = r.sections.find(name);
    if (it != r.sections.end()) return *it->second;
    auto* s = new SectionRing();
    r.sections[name] = s;
    return *s;
}

uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

} // namespace

extern "C" void SAO_CORE_CALL sao_perf_probe_set_enabled(int enabled) {
    registry().enabled.store(enabled != 0);
}

extern "C" int SAO_CORE_CALL sao_perf_probe_is_enabled(void) {
    return registry().enabled.load() ? 1 : 0;
}

extern "C" uint64_t SAO_CORE_CALL sao_perf_probe_begin(const char* /*section*/) {
    if (!registry().enabled.load()) return 0;
    return now_ns();
}

extern "C" void SAO_CORE_CALL sao_perf_probe_end(const char* section_utf8,
                                                  uint64_t begin_cookie) {
    if (!registry().enabled.load() || begin_cookie == 0 ||
        section_utf8 == nullptr)
        return;
    uint64_t elapsed = now_ns() - begin_cookie;
    SectionRing& s = get_section(section_utf8);
    std::lock_guard lock(s.mu);
    if (s.samples_ns.size() == kRingCap) {
        // rotate: drop oldest.
        s.samples_ns.erase(s.samples_ns.begin());
    }
    s.samples_ns.push_back(elapsed);
    ++s.count;
    s.total_ns += elapsed;
}

extern "C" sao_status_t SAO_CORE_CALL sao_perf_probe_get_stats(
    const char* section_utf8, sao_perf_probe_stats_t* out_stats) {
    if (section_utf8 == nullptr || out_stats == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    SectionRing& s = get_section(section_utf8);
    std::lock_guard lock(s.mu);
    if (s.samples_ns.empty()) {
        *out_stats = {};
        return SAO_STATUS_OK;
    }
    std::vector<uint64_t> sorted = s.samples_ns;
    std::sort(sorted.begin(), sorted.end());
    auto pct = [&](double p) -> uint64_t {
        size_t idx = static_cast<size_t>(sorted.size() * p);
        if (idx >= sorted.size()) idx = sorted.size() - 1;
        return sorted[idx];
    };
    out_stats->count = s.count;
    out_stats->p50_ns = pct(0.50);
    out_stats->p95_ns = pct(0.95);
    out_stats->p99_ns = pct(0.99);
    out_stats->max_ns = sorted.back();
    out_stats->total_ns = s.total_ns;
    return SAO_STATUS_OK;
}

extern "C" void SAO_CORE_CALL sao_perf_probe_reset(const char* section_utf8) {
    auto& r = registry();
    if (section_utf8 == nullptr) {
        std::lock_guard lock(r.mu);
        for (auto& [_, s] : r.sections) {
            std::lock_guard slock(s->mu);
            s->samples_ns.clear();
            s->count = 0;
            s->total_ns = 0;
        }
        return;
    }
    SectionRing& s = get_section(section_utf8);
    std::lock_guard lock(s.mu);
    s.samples_ns.clear();
    s.count = 0;
    s.total_ns = 0;
}
