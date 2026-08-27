// scanner.cpp — CE-style multi-frame memory scanner.

#include "sao/mem_probe/scanner.h"

#include "sao/core/memory.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__GNUC__) || defined(__clang__)
#include <cpuid.h>
#include <immintrin.h>
#endif

#ifndef PAGE_NOACCESS
#define PAGE_NOACCESS 0x01
#endif
#ifndef PAGE_GUARD
#define PAGE_GUARD 0x100
#endif
#ifndef MEM_COMMIT
#define MEM_COMMIT 0x1000
#endif

namespace {

constexpr uint32_t kDefaultTimeoutMs = 30'000;
constexpr uint64_t kDefaultScanBytes = 256ull * 1024ull * 1024ull;
constexpr uint32_t kDefaultReadCount = 20000u;

struct ScanBudget {
    uint64_t bytes_remaining;
    uint32_t reads_remaining;
    std::chrono::steady_clock::time_point deadline;
    bool exhausted = false;
    ScanBudget(uint64_t bytes, uint32_t reads, uint32_t duration_ms)
        : bytes_remaining(bytes), reads_remaining(reads),
          deadline(std::chrono::steady_clock::now() + std::chrono::milliseconds(duration_ms)) {}
    bool reserve(size_t bytes) {
        if (std::chrono::steady_clock::now() >= deadline || reads_remaining == 0 ||
            bytes > bytes_remaining) {
            exhausted = true;
            return false;
        }
        bytes_remaining -= bytes;
        --reads_remaining;
        return true;
    }
};
constexpr size_t kChunkBytes = 1u << 20;
constexpr size_t kMaxRegionsSample = 8192;

bool has_avx2_cached() {
    static const bool cached = []() {
#if defined(_MSC_VER)
        int cpu[4] = {0, 0, 0, 0};
        __cpuidex(cpu, 1, 0);
        if ((cpu[2] & (1 << 27)) == 0 || (cpu[2] & (1 << 28)) == 0)
            return false;
        const unsigned __int64 xcr0 = _xgetbv(0);
        if ((xcr0 & 0x6) != 0x6)
            return false;
        __cpuidex(cpu, 7, 0);
        return (cpu[1] & (1 << 5)) != 0;
#elif defined(__GNUC__) || defined(__clang__)
        return __builtin_cpu_supports("avx2") != 0;
#else
        return false;
#endif
    }();
    return cached;
}

bool region_scannable(const SaoMemRegion& r) {
    if ((r.state & MEM_COMMIT) == 0)
        return false;
    if ((r.protect & PAGE_GUARD) != 0)
        return false;
    if (r.protect == 0 || (r.protect & PAGE_NOACCESS) != 0)
        return false;
    return true;
}

bool checked_add(uint64_t base, uint64_t amount, uint64_t* out) {
    if (out == nullptr || base > std::numeric_limits<uint64_t>::max() - amount)
        return false;
    *out = base + amount;
    return true;
}

bool checked_region_end(const SaoMemRegion& region, uint64_t* out_end) {
    return checked_add(region.base_address, region.region_size, out_end);
}

uint32_t dtype_size_impl(sao_memprobe_dtype_t d) {
    switch (d) {
    case SAO_MEMPROBE_DTYPE_U8:
    case SAO_MEMPROBE_DTYPE_I8:
        return 1;
    case SAO_MEMPROBE_DTYPE_U16:
    case SAO_MEMPROBE_DTYPE_I16:
        return 2;
    case SAO_MEMPROBE_DTYPE_U32:
    case SAO_MEMPROBE_DTYPE_I32:
    case SAO_MEMPROBE_DTYPE_F32:
        return 4;
    case SAO_MEMPROBE_DTYPE_U64:
    case SAO_MEMPROBE_DTYPE_I64:
    case SAO_MEMPROBE_DTYPE_F64:
        return 8;
    }
    return 0;
}

bool valid_dtype(sao_memprobe_dtype_t d) {
    return static_cast<int>(d) >= static_cast<int>(SAO_MEMPROBE_DTYPE_U8) &&
           static_cast<int>(d) <= static_cast<int>(SAO_MEMPROBE_DTYPE_F64);
}

bool valid_predicate(sao_memprobe_predicate_t predicate) {
    return static_cast<int>(predicate) >= static_cast<int>(SAO_MEMPROBE_PRED_CHANGED) &&
           static_cast<int>(predicate) <= static_cast<int>(SAO_MEMPROBE_PRED_DECREASED);
}

struct NormalizedScanBounds {
    uint32_t max_addrs = 0;
    uint32_t max_duration_ms = 0;
    uint32_t align = 0;
    uint64_t max_scan_bytes = 0;
    uint32_t max_read_count = 0;
};

NormalizedScanBounds scan_bounds_v1(const sao_memprobe_scan_bounds_t* source) noexcept {
    NormalizedScanBounds target{};
    if (source != nullptr) {
        target.max_addrs = source->max_addrs;
        target.max_duration_ms = source->max_duration_ms;
        target.align = source->align;
    }
    return target;
}

bool scan_bounds_v2_to_internal(const sao_memprobe_scan_bounds_v2_t* source,
                                NormalizedScanBounds* target) {
    if (source == nullptr || target == nullptr ||
        source->struct_size < offsetof(sao_memprobe_scan_bounds_v2_t, _reserved) ||
        source->abi_version != SAO_MEMPROBE_SCAN_BOUNDS_V2_ABI_VERSION)
        return false;
    target->max_addrs = source->max_addrs;
    target->max_duration_ms = source->max_duration_ms;
    target->align = source->align;
    target->max_scan_bytes = source->max_scan_bytes;
    target->max_read_count = source->max_read_count;
    return true;
}

int64_t signed_value(sao_memprobe_dtype_t dtype, uint64_t raw) {
    switch (dtype) {
    case SAO_MEMPROBE_DTYPE_I8:
        return static_cast<int8_t>(raw & 0xFFu);
    case SAO_MEMPROBE_DTYPE_I16:
        return static_cast<int16_t>(raw & 0xFFFFu);
    case SAO_MEMPROBE_DTYPE_I32:
        return static_cast<int32_t>(raw & 0xFFFFFFFFu);
    case SAO_MEMPROBE_DTYPE_I64: {
        int64_t value = 0;
        std::memcpy(&value, &raw, sizeof(value));
        return value;
    }
    default:
        return 0;
    }
}

bool predicate_matches(sao_memprobe_dtype_t dtype, sao_memprobe_predicate_t predicate,
                       uint64_t current, uint64_t previous) {
    const bool is_float = dtype == SAO_MEMPROBE_DTYPE_F32 || dtype == SAO_MEMPROBE_DTYPE_F64;
    if (is_float) {
        if (predicate == SAO_MEMPROBE_PRED_CHANGED)
            return current != previous;
        if (predicate == SAO_MEMPROBE_PRED_UNCHANGED)
            return current == previous;
        if (dtype == SAO_MEMPROBE_DTYPE_F32) {
            const uint32_t current_bits = static_cast<uint32_t>(current);
            const uint32_t previous_bits = static_cast<uint32_t>(previous);
            float current_value = 0.0f;
            float previous_value = 0.0f;
            std::memcpy(&current_value, &current_bits, sizeof(current_value));
            std::memcpy(&previous_value, &previous_bits, sizeof(previous_value));
            if (std::isnan(current_value) || std::isnan(previous_value))
                return false;
            return predicate == SAO_MEMPROBE_PRED_INCREASED ? current_value > previous_value
                                                            : current_value < previous_value;
        }
        double current_value = 0.0;
        double previous_value = 0.0;
        std::memcpy(&current_value, &current, sizeof(current_value));
        std::memcpy(&previous_value, &previous, sizeof(previous_value));
        if (std::isnan(current_value) || std::isnan(previous_value))
            return false;
        return predicate == SAO_MEMPROBE_PRED_INCREASED ? current_value > previous_value
                                                        : current_value < previous_value;
    }
    if (dtype >= SAO_MEMPROBE_DTYPE_I8 && dtype <= SAO_MEMPROBE_DTYPE_I64) {
        const int64_t current_value = signed_value(dtype, current);
        const int64_t previous_value = signed_value(dtype, previous);
        switch (predicate) {
        case SAO_MEMPROBE_PRED_CHANGED:
            return current_value != previous_value;
        case SAO_MEMPROBE_PRED_UNCHANGED:
            return current_value == previous_value;
        case SAO_MEMPROBE_PRED_INCREASED:
            return current_value > previous_value;
        case SAO_MEMPROBE_PRED_DECREASED:
            return current_value < previous_value;
        }
    }
    switch (predicate) {
    case SAO_MEMPROBE_PRED_CHANGED:
        return current != previous;
    case SAO_MEMPROBE_PRED_UNCHANGED:
        return current == previous;
    case SAO_MEMPROBE_PRED_INCREASED:
        return current > previous;
    case SAO_MEMPROBE_PRED_DECREASED:
        return current < previous;
    }
    return false;
}

template <typename Emit>
bool scan_chunk(const uint8_t* buf, size_t buf_len, uint64_t chunk_base, const void* needle,
                uint32_t dtype_size, uint32_t align, Emit&& emit) {
    if (buf_len < dtype_size || align == 0)
        return true;
    const size_t last = buf_len - dtype_size;
    const size_t remainder = static_cast<size_t>(chunk_base % align);
    const size_t first = remainder == 0 ? 0 : align - remainder;
    for (size_t off = first; off <= last; off += align) {
        if (std::memcmp(buf + off, needle, dtype_size) == 0) {
            uint64_t address = 0;
            if (!checked_add(chunk_base, static_cast<uint64_t>(off), &address))
                return false;
            emit(address);
        }
        if (align > last - off)
            break;
    }
    return true;
}

#if defined(__AVX2__) || defined(_M_AVX2)
template <typename Emit>
bool scan_chunk_avx2_u32(const uint8_t* buf, size_t buf_len, uint64_t chunk_base, uint32_t needle,
                         Emit&& emit) {
    if (!has_avx2_cached()) {
        return scan_chunk(buf, buf_len, chunk_base, &needle, 4, 4, emit);
    }
    if (buf_len < 4)
        return true;
    const size_t remainder = static_cast<size_t>(chunk_base & 3ull);
    size_t off = remainder == 0 ? 0 : 4 - remainder;
    if (off > buf_len - 4)
        return true;
    const __m256i target = _mm256_set1_epi32(static_cast<int32_t>(needle));
    if (buf_len >= 32) {
        const size_t simd_end = buf_len - 32;
        for (; off <= simd_end; off += 32) {
            const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(buf + off));
            const __m256i eq = _mm256_cmpeq_epi32(v, target);
            const uint32_t lane_mask = static_cast<uint32_t>(_mm256_movemask_epi8(eq));
            for (uint32_t lane = 0; lane < 8; ++lane) {
                if ((lane_mask & (1u << (lane * 4))) != 0) {
                    uint64_t address = 0;
                    const uint64_t offset =
                        static_cast<uint64_t>(off) + static_cast<uint64_t>(lane) * 4;
                    if (!checked_add(chunk_base, offset, &address))
                        return false;
                    emit(address);
                }
            }
        }
    }
    for (; off <= buf_len - 4; off += 4) {
        uint32_t v = 0;
        std::memcpy(&v, buf + off, sizeof(v));
        if (v == needle) {
            uint64_t address = 0;
            if (!checked_add(chunk_base, static_cast<uint64_t>(off), &address))
                return false;
            emit(address);
        }
        if (off > (buf_len - 4) - 4)
            break;
    }
    return true;
}
#endif

} // namespace

extern "C" uint32_t SAO_CORE_CALL sao_memprobe_dtype_size(sao_memprobe_dtype_t d) {
    try {
        return dtype_size_impl(d);
    } catch (...) {
        return 0;
    }
}

extern "C" int SAO_CORE_CALL sao_memprobe_has_avx2(void) {
    try {
        return has_avx2_cached() ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

static sao_status_t scan_first_impl(
    sao_core_process_handle_t process, sao_memprobe_dtype_t dtype, uint64_t value,
    const NormalizedScanBounds* bounds, sao_memprobe_candidate_t* out_candidates,
    uint32_t cap, uint32_t* out_count) {
    try {
        if (out_count == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        *out_count = 0;
        if (process == nullptr || out_candidates == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (cap == 0)
            return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        if (!valid_dtype(dtype))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;

        const uint32_t dsz = dtype_size_impl(dtype);
        const uint32_t align = (bounds && bounds->align != 0) ? bounds->align : dsz;
        const uint32_t max_addrs = (bounds && bounds->max_addrs != 0) ? bounds->max_addrs : cap;
        const uint32_t timeout_ms =
            (bounds && bounds->max_duration_ms != 0) ? bounds->max_duration_ms : kDefaultTimeoutMs;
        ScanBudget budget(
            (bounds && bounds->max_scan_bytes) ? bounds->max_scan_bytes : kDefaultScanBytes,
            (bounds && bounds->max_read_count) ? bounds->max_read_count : kDefaultReadCount,
            timeout_ms);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

        std::vector<SaoMemRegion> regions(kMaxRegionsSample);
        size_t region_count = 0;
        if (sao_core_mem_enum_regions(process, regions.data(), regions.size(), &region_count) !=
            SAO_STATUS_OK)
            return SAO_STATUS_ERR_READ_FAULT;
        regions.resize(region_count);

        uint8_t needle[8] = {};
        std::memcpy(needle, &value, dsz);
        std::vector<uint8_t> chunk(kChunkBytes);
        std::vector<sao_memprobe_candidate_t> pending;
        pending.reserve(std::min<uint32_t>(cap, max_addrs));
        bool capacity_exhausted = false;
        bool timed_out = false;

        for (const auto& region : regions) {
            uint64_t region_end = 0;
            if (!checked_region_end(region, &region_end))
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            (void)region_end;
            if (!region_scannable(region))
                continue;
            uint64_t addr = region.base_address;
            uint64_t remaining = region.region_size;
            std::vector<uint8_t> carry(dsz > 1 ? dsz - 1 : 0);
            size_t carry_len = 0;
            while (remaining > 0) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    timed_out = true;
                    break;
                }
                const size_t want = static_cast<size_t>(std::min<uint64_t>(remaining, kChunkBytes));
                if (!budget.reserve(want)) {
                    timed_out = true;
                    break;
                }
                size_t got = 0;
                const sao_status_t read_status =
                    sao_core_mem_read(process, addr, chunk.data(), want, &got);
                if (got > want)
                    return SAO_STATUS_ERR_READ_FAULT;
                if (got == 0) {
                    carry_len = 0;
                    uint64_t next = 0;
                    if (!checked_add(addr, want, &next))
                        return SAO_STATUS_ERR_INVALID_ARGUMENT;
                    addr = next;
                    remaining -= want;
                    continue;
                }
                if (addr < carry_len)
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                std::vector<uint8_t> window(carry_len + got);
                if (carry_len != 0) {
                    std::memcpy(window.data(), carry.data(), carry_len);
                }
                std::memcpy(window.data() + carry_len, chunk.data(), got);
                const uint64_t window_base = addr - static_cast<uint64_t>(carry_len);
                auto emit = [&](uint64_t hit_addr) {
                    if (pending.size() >= max_addrs || pending.size() >= cap)
                        return;
                    pending.push_back(sao_memprobe_candidate_t{hit_addr, value});
                };
                if ((dtype == SAO_MEMPROBE_DTYPE_U32 || dtype == SAO_MEMPROBE_DTYPE_I32) &&
                    align == 4) {
#if defined(__AVX2__) || defined(_M_AVX2)
                    if (!scan_chunk_avx2_u32(window.data(), window.size(), window_base,
                                             static_cast<uint32_t>(value), emit))
                        return SAO_STATUS_ERR_INVALID_ARGUMENT;
#else
                    if (!scan_chunk(window.data(), window.size(), window_base, needle, dsz, align,
                                    emit))
                        return SAO_STATUS_ERR_INVALID_ARGUMENT;
#endif
                } else {
                    if (!scan_chunk(window.data(), window.size(), window_base, needle, dsz, align,
                                    emit))
                        return SAO_STATUS_ERR_INVALID_ARGUMENT;
                }
                if (dsz > 1) {
                    carry_len = std::min<size_t>(dsz - 1, window.size());
                    std::memcpy(carry.data(), window.data() + window.size() - carry_len, carry_len);
                } else {
                    carry_len = 0;
                }
                if (pending.size() >= cap && cap < max_addrs)
                    capacity_exhausted = true;
                if (capacity_exhausted || pending.size() >= max_addrs)
                    break;
                uint64_t next = 0;
                if (!checked_add(addr, got, &next))
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                addr = next;
                remaining -= got;
                if (read_status != SAO_STATUS_OK && got < want)
                    continue;
            }
            if (timed_out || capacity_exhausted || pending.size() >= max_addrs)
                break;
        }

        if (timed_out)
            return SAO_STATUS_ERR_TIMEOUT;
        const uint32_t written = static_cast<uint32_t>(pending.size());
        if (written != 0)
            std::memcpy(out_candidates, pending.data(), pending.size() * sizeof(pending[0]));
        *out_count = written;
        if (capacity_exhausted)
            return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        return SAO_STATUS_OK;
    } catch (...) {
        if (out_count != nullptr)
            *out_count = 0;
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_CORE_CALL sao_memprobe_scan_narrow(
    sao_core_process_handle_t process, sao_memprobe_dtype_t dtype, uint64_t value,
    const sao_memprobe_candidate_t* prev, uint32_t prev_count,
    sao_memprobe_candidate_t* out_candidates, uint32_t cap, uint32_t* out_count) {
    if (out_count == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_count = 0;
    try {
        if (process == nullptr || prev == nullptr || out_candidates == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (!valid_dtype(dtype))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (cap == 0)
            return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        const uint32_t dsz = dtype_size_impl(dtype);
        const uint64_t value_mask = dsz == sizeof(uint64_t) ? std::numeric_limits<uint64_t>::max()
                                                            : ((uint64_t{1} << (dsz * 8)) - 1u);
        ScanBudget budget(kDefaultScanBytes, kDefaultReadCount, kDefaultTimeoutMs);
        std::vector<sao_memprobe_candidate_t> pending;
        pending.reserve(cap);
        uint8_t buf[8] = {};
        for (uint32_t i = 0; i < prev_count; ++i) {
            uint64_t candidate_end = 0;
            if (!checked_add(prev[i].addr, dsz, &candidate_end))
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            (void)candidate_end;
            if (!budget.reserve(dsz)) {
                return SAO_STATUS_ERR_TIMEOUT;
            }
            size_t got = 0;
            if (sao_core_mem_read(process, prev[i].addr, buf, dsz, &got) != SAO_STATUS_OK ||
                got < dsz)
                continue;
            if (got > dsz)
                return SAO_STATUS_ERR_READ_FAULT;
            uint64_t current = 0;
            std::memcpy(&current, buf, dsz);
            if ((current & value_mask) != (value & value_mask))
                continue;
            if (pending.size() >= cap) {
                if (!pending.empty())
                    std::memcpy(out_candidates, pending.data(),
                                pending.size() * sizeof(pending[0]));
                *out_count = static_cast<uint32_t>(pending.size());
                return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
            }
            pending.push_back(sao_memprobe_candidate_t{prev[i].addr, current});
        }
        if (!pending.empty())
            std::memcpy(out_candidates, pending.data(), pending.size() * sizeof(pending[0]));
        *out_count = static_cast<uint32_t>(pending.size());
        return SAO_STATUS_OK;
    } catch (...) {
        if (out_count != nullptr)
            *out_count = 0;
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

static sao_status_t scan_pattern_impl(
    sao_core_process_handle_t process, const uint8_t* pattern, const uint8_t* mask,
    uint32_t pattern_len, const NormalizedScanBounds* bounds, uint64_t* out_addrs,
    uint32_t cap, uint32_t* out_count) {
    try {
        if (out_count == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        *out_count = 0;
        if (process == nullptr || pattern == nullptr || mask == nullptr || out_addrs == nullptr ||
            pattern_len == 0)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (cap == 0)
            return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        if (pattern_len > std::numeric_limits<size_t>::max() - kChunkBytes)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;

        const uint32_t max_addrs = (bounds && bounds->max_addrs != 0) ? bounds->max_addrs : cap;
        const uint32_t timeout_ms =
            (bounds && bounds->max_duration_ms != 0) ? bounds->max_duration_ms : kDefaultTimeoutMs;
        const uint32_t align = (bounds && bounds->align != 0) ? bounds->align : 1;
        ScanBudget budget(
            (bounds && bounds->max_scan_bytes) ? bounds->max_scan_bytes : kDefaultScanBytes,
            (bounds && bounds->max_read_count) ? bounds->max_read_count : kDefaultReadCount,
            timeout_ms);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

        std::vector<SaoMemRegion> regions(kMaxRegionsSample);
        size_t region_count = 0;
        if (sao_core_mem_enum_regions(process, regions.data(), regions.size(), &region_count) !=
            SAO_STATUS_OK)
            return SAO_STATUS_ERR_READ_FAULT;
        regions.resize(region_count);

        std::vector<uint8_t> chunk(kChunkBytes);
        std::vector<uint8_t> carry(pattern_len > 1 ? pattern_len - 1 : 0);
        std::vector<uint64_t> pending;
        pending.reserve(std::min<uint32_t>(cap, max_addrs));
        bool capacity_exhausted = false;
        bool timed_out = false;

        for (const auto& region : regions) {
            uint64_t region_end = 0;
            if (!checked_region_end(region, &region_end))
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            (void)region_end;
            if (!region_scannable(region))
                continue;
            uint64_t addr = region.base_address;
            uint64_t remaining = region.region_size;
            size_t carry_len = 0;
            while (remaining > 0) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    timed_out = true;
                    break;
                }
                const size_t want = static_cast<size_t>(std::min<uint64_t>(remaining, kChunkBytes));
                if (!budget.reserve(want)) {
                    timed_out = true;
                    break;
                }
                size_t got = 0;
                const sao_status_t read_status =
                    sao_core_mem_read(process, addr, chunk.data(), want, &got);
                if (got > want)
                    return SAO_STATUS_ERR_READ_FAULT;
                if (got == 0) {
                    carry_len = 0;
                    uint64_t next = 0;
                    if (!checked_add(addr, want, &next))
                        return SAO_STATUS_ERR_INVALID_ARGUMENT;
                    addr = next;
                    remaining -= want;
                    continue;
                }
                if (addr < carry_len)
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                std::vector<uint8_t> window(carry_len + got);
                if (carry_len != 0) {
                    std::memcpy(window.data(), carry.data(), carry_len);
                }
                std::memcpy(window.data() + carry_len, chunk.data(), got);
                const uint64_t window_base = addr - static_cast<uint64_t>(carry_len);
                const size_t last = window.size() >= pattern_len ? window.size() - pattern_len : 0;
                const size_t remainder = static_cast<size_t>(window_base % align);
                const size_t first = remainder == 0 ? 0 : align - remainder;
                for (size_t off = first; window.size() >= pattern_len && off <= last;) {
                    bool matched = true;
                    for (uint32_t p = 0; p < pattern_len; ++p) {
                        if (mask[p] != 0 && (window[off + p] & mask[p]) != (pattern[p] & mask[p])) {
                            matched = false;
                            break;
                        }
                    }
                    if (matched && (pending.size() >= max_addrs || pending.size() >= cap)) {
                        if (pending.size() >= cap && cap < max_addrs)
                            capacity_exhausted = true;
                        break;
                    }
                    if (matched) {
                        uint64_t hit_address = 0;
                        if (!checked_add(window_base, static_cast<uint64_t>(off), &hit_address))
                            return SAO_STATUS_ERR_INVALID_ARGUMENT;
                        pending.push_back(hit_address);
                    }
                    if (align > last - off)
                        break;
                    off += align;
                }
                if (pattern_len > 1) {
                    carry_len = std::min<size_t>(pattern_len - 1, window.size());
                    if (carry_len != 0) {
                        std::memcpy(carry.data(), window.data() + window.size() - carry_len,
                                    carry_len);
                    }
                } else {
                    carry_len = 0;
                }
                if (capacity_exhausted || pending.size() >= max_addrs)
                    break;
                uint64_t next = 0;
                if (!checked_add(addr, got, &next))
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                addr = next;
                remaining -= got;
                if (read_status != SAO_STATUS_OK && got < want)
                    continue;
            }
            if (timed_out || capacity_exhausted || pending.size() >= max_addrs)
                break;
        }

        if (timed_out)
            return SAO_STATUS_ERR_TIMEOUT;
        const uint32_t written = static_cast<uint32_t>(pending.size());
        if (written != 0)
            std::memcpy(out_addrs, pending.data(), pending.size() * sizeof(pending[0]));
        *out_count = written;
        if (capacity_exhausted)
            return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        return SAO_STATUS_OK;
    } catch (...) {
        if (out_count != nullptr)
            *out_count = 0;
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_CORE_CALL sao_memprobe_scan_predicate(
    sao_core_process_handle_t process, sao_memprobe_dtype_t dtype,
    sao_memprobe_predicate_t predicate, const sao_memprobe_candidate_t* prev, uint32_t prev_count,
    sao_memprobe_candidate_t* out_candidates, uint32_t cap, uint32_t* out_count) {
    if (out_count == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_count = 0;
    try {
        if (process == nullptr || prev == nullptr || out_candidates == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (!valid_dtype(dtype) || !valid_predicate(predicate))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (cap == 0)
            return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        const uint32_t dsz = dtype_size_impl(dtype);
        ScanBudget budget(kDefaultScanBytes, kDefaultReadCount, kDefaultTimeoutMs);
        std::vector<sao_memprobe_candidate_t> pending;
        pending.reserve(cap);
        uint8_t buf[8] = {};
        for (uint32_t i = 0; i < prev_count; ++i) {
            uint64_t candidate_end = 0;
            if (!checked_add(prev[i].addr, dsz, &candidate_end))
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            (void)candidate_end;
            if (!budget.reserve(dsz)) {
                return SAO_STATUS_ERR_TIMEOUT;
            }
            size_t got = 0;
            if (sao_core_mem_read(process, prev[i].addr, buf, dsz, &got) != SAO_STATUS_OK ||
                got < dsz)
                continue;
            if (got > dsz)
                return SAO_STATUS_ERR_READ_FAULT;
            uint64_t current = 0;
            std::memcpy(&current, buf, dsz);
            const bool keep = predicate_matches(dtype, predicate, current, prev[i].last_value);
            if (!keep)
                continue;
            if (pending.size() >= cap) {
                if (!pending.empty())
                    std::memcpy(out_candidates, pending.data(),
                                pending.size() * sizeof(pending[0]));
                *out_count = static_cast<uint32_t>(pending.size());
                return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
            }
            pending.push_back(sao_memprobe_candidate_t{prev[i].addr, current});
        }
        if (!pending.empty())
            std::memcpy(out_candidates, pending.data(), pending.size() * sizeof(pending[0]));
        *out_count = static_cast<uint32_t>(pending.size());
        return SAO_STATUS_OK;
    } catch (...) {
        if (out_count != nullptr)
            *out_count = 0;
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_CORE_CALL sao_memprobe_scan_first(
    sao_core_process_handle_t process, sao_memprobe_dtype_t dtype, uint64_t value,
    const sao_memprobe_scan_bounds_t* bounds, sao_memprobe_candidate_t* out_candidates,
    uint32_t cap, uint32_t* out_count) {
    const NormalizedScanBounds normalized = scan_bounds_v1(bounds);
    return scan_first_impl(process, dtype, value, &normalized, out_candidates, cap, out_count);
}

extern "C" sao_status_t SAO_CORE_CALL sao_memprobe_scan_pattern(
    sao_core_process_handle_t process, const uint8_t* pattern, const uint8_t* mask,
    uint32_t pattern_len, const sao_memprobe_scan_bounds_t* bounds, uint64_t* out_addrs,
    uint32_t cap, uint32_t* out_count) {
    const NormalizedScanBounds normalized = scan_bounds_v1(bounds);
    return scan_pattern_impl(process, pattern, mask, pattern_len, &normalized, out_addrs, cap,
                             out_count);
}

extern "C" sao_status_t SAO_CORE_CALL sao_memprobe_scan_first_v2(
    sao_core_process_handle_t process, sao_memprobe_dtype_t dtype, uint64_t value,
    const sao_memprobe_scan_bounds_v2_t* bounds, sao_memprobe_candidate_t* out_candidates,
    uint32_t cap, uint32_t* out_count) {
    if (out_count != nullptr)
        *out_count = 0;
    try {
        NormalizedScanBounds normalized{};
        if (!scan_bounds_v2_to_internal(bounds, &normalized))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        return scan_first_impl(process, dtype, value, &normalized, out_candidates, cap, out_count);
    } catch (...) {
        if (out_count != nullptr)
            *out_count = 0;
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_CORE_CALL sao_memprobe_scan_pattern_v2(
    sao_core_process_handle_t process, const uint8_t* pattern, const uint8_t* mask,
    uint32_t pattern_len, const sao_memprobe_scan_bounds_v2_t* bounds, uint64_t* out_addrs,
    uint32_t cap, uint32_t* out_count) {
    if (out_count != nullptr)
        *out_count = 0;
    try {
        NormalizedScanBounds normalized{};
        if (!scan_bounds_v2_to_internal(bounds, &normalized))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        return scan_pattern_impl(process, pattern, mask, pattern_len, &normalized, out_addrs, cap,
                                 out_count);
    } catch (...) {
        if (out_count != nullptr)
            *out_count = 0;
        return SAO_STATUS_ERR_UNKNOWN;
    }
}
