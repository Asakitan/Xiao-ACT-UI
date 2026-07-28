// scanner.cpp — CE-style multi-frame memory scanner.
//
// Phase 4 (Python parity closure) — port of python/mem_probe/scanner.py.
// AVX2 SIMD accelerated u32/u64/i32/i64; scalar fallback for other dtypes.
// Region enumeration via sao_core_mem_enum_regions (VirtualQueryEx wrapper).

#include "sao/mem_probe/scanner.h"

#include "sao/core/memory.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__GNUC__) || defined(__clang__)
#include <immintrin.h>
#include <cpuid.h>
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
constexpr size_t kChunkBytes = 1u << 20; // 1 MiB per ReadProcessMemory
constexpr size_t kMaxRegionsSample = 8192;

bool has_avx2_cached() {
    static const bool cached = []() {
#if defined(_MSC_VER)
        int cpu[4] = {0, 0, 0, 0};
        __cpuidex(cpu, 7, 0);
        return (cpu[1] & (1 << 5)) != 0;
#elif defined(__GNUC__) || defined(__clang__)
        unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
        if (!__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx))
            return false;
        return (ebx & (1u << 5)) != 0;
#else
        return false;
#endif
    }();
    return cached;
}

// Portable ctz — msvc-safe. Declared before use so avx2 loop can call it.
inline int ctz32(int mask) {
#if defined(_MSC_VER)
    unsigned long idx = 0;
    _BitScanForward(&idx, static_cast<unsigned long>(mask));
    return static_cast<int>(idx);
#else
    return __builtin_ctz(static_cast<unsigned int>(mask));
#endif
}

bool region_scannable(const SaoMemRegion& r) {
    if ((r.state & MEM_COMMIT) == 0) return false;
    if ((r.protect & PAGE_GUARD) != 0) return false;
    if (r.protect == 0 || (r.protect & PAGE_NOACCESS) != 0) return false;
    return true;
}

uint32_t dtype_size_impl(sao_memprobe_dtype_t d) {
    switch (d) {
    case SAO_MEMPROBE_DTYPE_U8:  case SAO_MEMPROBE_DTYPE_I8:  return 1;
    case SAO_MEMPROBE_DTYPE_U16: case SAO_MEMPROBE_DTYPE_I16: return 2;
    case SAO_MEMPROBE_DTYPE_U32: case SAO_MEMPROBE_DTYPE_I32:
    case SAO_MEMPROBE_DTYPE_F32:                              return 4;
    case SAO_MEMPROBE_DTYPE_U64: case SAO_MEMPROBE_DTYPE_I64:
    case SAO_MEMPROBE_DTYPE_F64:                              return 8;
    }
    return 4;
}

// Scan a byte buffer for occurrences of `needle` (dtype_size bytes) at
// `align`-aligned offsets. Reports back absolute addresses via `emit`.
template <typename Emit>
void scan_chunk(const uint8_t* buf, size_t buf_len, uint64_t chunk_base,
                const void* needle, uint32_t dtype_size, uint32_t align,
                Emit&& emit) {
    if (buf_len < dtype_size) return;
    const size_t last = buf_len - dtype_size;
    for (size_t off = 0; off <= last; off += align) {
        if (std::memcmp(buf + off, needle, dtype_size) == 0) {
            emit(chunk_base + off);
        }
    }
}

#if defined(__AVX2__) || defined(_MSC_VER)
// AVX2 aligned u32 scan — 8 lanes per cycle. Aligned load; align must be 4.
template <typename Emit>
void scan_chunk_avx2_u32(const uint8_t* buf, size_t buf_len,
                         uint64_t chunk_base, uint32_t needle,
                         Emit&& emit) {
    if (!has_avx2_cached()) {
        scan_chunk(buf, buf_len, chunk_base, &needle, 4, 4, emit);
        return;
    }
    const size_t simd_end = (buf_len >= 32) ? (buf_len - 32) : 0;
    const __m256i target = _mm256_set1_epi32(static_cast<int32_t>(needle));
    size_t off = 0;
    for (; off <= simd_end; off += 32) {
        const __m256i v = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(buf + off));
        const __m256i eq = _mm256_cmpeq_epi32(v, target);
        int mask = _mm256_movemask_epi8(eq);
        while (mask != 0) {
            const int bit = ctz32(mask);
            emit(chunk_base + off + static_cast<size_t>(bit));
            mask &= mask - 1;
        }
    }
    for (; off + 4 <= buf_len; off += 4) {
        uint32_t v;
        std::memcpy(&v, buf + off, sizeof(v));
        if (v == needle) emit(chunk_base + off);
    }
}
#endif

} // namespace

extern "C" uint32_t SAO_CORE_CALL sao_memprobe_dtype_size(sao_memprobe_dtype_t d) {
    return dtype_size_impl(d);
}

extern "C" int SAO_CORE_CALL sao_memprobe_has_avx2(void) {
    return has_avx2_cached() ? 1 : 0;
}

extern "C" sao_status_t SAO_CORE_CALL sao_memprobe_scan_first(
    sao_core_process_handle_t process,
    sao_memprobe_dtype_t dtype,
    uint64_t value,
    const sao_memprobe_scan_bounds_t* bounds,
    sao_memprobe_candidate_t* out_candidates,
    uint32_t cap,
    uint32_t* out_count) {
    if (process == nullptr || out_candidates == nullptr || out_count == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_count = 0;
    const uint32_t dsz = dtype_size_impl(dtype);
    uint32_t align = (bounds && bounds->align != 0) ? bounds->align : dsz;
    const uint32_t max_addrs = (bounds && bounds->max_addrs != 0) ? bounds->max_addrs : cap;
    const uint32_t timeout_ms = (bounds && bounds->max_duration_ms != 0)
                                    ? bounds->max_duration_ms
                                    : kDefaultTimeoutMs;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);

    auto handle = process;
    std::vector<SaoMemRegion> regions(kMaxRegionsSample);
    size_t region_count = 0;
    if (sao_core_mem_enum_regions(handle, regions.data(), regions.size(),
                                  &region_count) != SAO_STATUS_OK) {
        return SAO_STATUS_ERR_READ_FAULT;
    }
    regions.resize(region_count);

    // needle bytes (bit-cast the input for signed/float dtypes).
    uint8_t needle[8] = {};
    std::memcpy(needle, &value, dsz);

    std::vector<uint8_t> chunk(kChunkBytes);
    uint32_t written = 0;
    bool timed_out = false;
    for (const auto& r : regions) {
        if (!region_scannable(r)) continue;
        uint64_t addr = r.base_address;
        uint64_t remaining = r.region_size;
        while (remaining > 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                timed_out = true;
                break;
            }
            const size_t want = static_cast<size_t>(
                std::min<uint64_t>(remaining, kChunkBytes));
            size_t got = 0;
            if (sao_core_mem_read(handle, addr, chunk.data(), want, &got) !=
                    SAO_STATUS_OK ||
                got < dsz) {
                addr += want;
                remaining -= want;
                continue;
            }
            auto emit = [&](uint64_t hit_addr) {
                if (written >= max_addrs || written >= cap) return;
                out_candidates[written].addr = hit_addr;
                out_candidates[written].last_value = value;
                ++written;
            };
            if ((dtype == SAO_MEMPROBE_DTYPE_U32 ||
                 dtype == SAO_MEMPROBE_DTYPE_I32) && align == 4) {
#if defined(__AVX2__) || defined(_MSC_VER)
                scan_chunk_avx2_u32(chunk.data(), got, addr,
                                    static_cast<uint32_t>(value), emit);
#else
                scan_chunk(chunk.data(), got, addr, needle, dsz, align, emit);
#endif
            } else {
                scan_chunk(chunk.data(), got, addr, needle, dsz, align, emit);
            }
            if (written >= max_addrs || written >= cap) break;
            addr += got;
            remaining -= got;
        }
        if (timed_out || written >= max_addrs || written >= cap) break;
    }
    *out_count = written;
    if (written >= cap && written < max_addrs) return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    if (timed_out) return SAO_STATUS_ERR_TIMEOUT;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_CORE_CALL sao_memprobe_scan_narrow(
    sao_core_process_handle_t process,
    sao_memprobe_dtype_t dtype,
    uint64_t value,
    const sao_memprobe_candidate_t* prev,
    uint32_t prev_count,
    sao_memprobe_candidate_t* out_candidates,
    uint32_t cap,
    uint32_t* out_count) {
    if (process == nullptr || prev == nullptr || out_candidates == nullptr ||
        out_count == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_count = 0;
    const uint32_t dsz = dtype_size_impl(dtype);
    auto handle = process;
    uint32_t written = 0;
    uint8_t buf[8];
    for (uint32_t i = 0; i < prev_count; ++i) {
        size_t got = 0;
        if (sao_core_mem_read(handle, prev[i].addr, buf, dsz, &got) !=
                SAO_STATUS_OK ||
            got < dsz)
            continue;
        uint64_t current = 0;
        std::memcpy(&current, buf, dsz);
        // exact-match: 4-byte signed value bitcast may cross 32-bit boundary.
        if ((current & ((1ULL << (dsz * 8)) - 1ULL)) ==
            (value & ((1ULL << (dsz * 8)) - 1ULL))) {
            if (written >= cap) return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
            out_candidates[written].addr = prev[i].addr;
            out_candidates[written].last_value = current;
            ++written;
        }
    }
    *out_count = written;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_CORE_CALL sao_memprobe_scan_pattern(
    sao_core_process_handle_t process, const uint8_t* pattern,
    const uint8_t* mask, uint32_t pattern_len,
    const sao_memprobe_scan_bounds_t* bounds, uint64_t* out_addrs,
    uint32_t cap, uint32_t* out_count) {
    if (process == nullptr || pattern == nullptr || mask == nullptr ||
        out_addrs == nullptr || out_count == nullptr || pattern_len == 0)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_count = 0;
    const uint32_t max_addrs = (bounds && bounds->max_addrs != 0) ? bounds->max_addrs : cap;
    const uint32_t timeout_ms = (bounds && bounds->max_duration_ms != 0)
                                    ? bounds->max_duration_ms
                                    : kDefaultTimeoutMs;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    auto handle = process;
    std::vector<SaoMemRegion> regions(kMaxRegionsSample);
    size_t region_count = 0;
    if (sao_core_mem_enum_regions(handle, regions.data(), regions.size(),
                                   &region_count) != SAO_STATUS_OK)
        return SAO_STATUS_ERR_READ_FAULT;
    regions.resize(region_count);
    std::vector<uint8_t> chunk(kChunkBytes + pattern_len); // overlap for pattern spanning chunks
    uint32_t written = 0;
    bool timed_out = false;
    for (const auto& r : regions) {
        if (!region_scannable(r)) continue;
        uint64_t addr = r.base_address;
        uint64_t remaining = r.region_size;
        while (remaining > 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                timed_out = true;
                break;
            }
            const size_t want = static_cast<size_t>(
                std::min<uint64_t>(remaining, kChunkBytes));
            size_t got = 0;
            if (sao_core_mem_read(handle, addr, chunk.data(), want, &got) !=
                    SAO_STATUS_OK ||
                got < pattern_len) {
                addr += want;
                remaining -= want;
                continue;
            }
            const size_t last = got - pattern_len;
            for (size_t off = 0; off <= last; ++off) {
                bool matched = true;
                for (uint32_t p = 0; p < pattern_len; ++p) {
                    if (mask[p] == 0) continue;
                    if ((chunk[off + p] & mask[p]) != (pattern[p] & mask[p])) {
                        matched = false;
                        break;
                    }
                }
                if (matched) {
                    if (written >= max_addrs || written >= cap) break;
                    out_addrs[written++] = addr + off;
                }
            }
            if (written >= max_addrs || written >= cap) break;
            addr += got;
            remaining -= got;
        }
        if (timed_out || written >= max_addrs || written >= cap) break;
    }
    *out_count = written;
    if (timed_out) return SAO_STATUS_ERR_TIMEOUT;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_CORE_CALL sao_memprobe_scan_predicate(
    sao_core_process_handle_t process,
    sao_memprobe_dtype_t dtype,
    sao_memprobe_predicate_t predicate,
    const sao_memprobe_candidate_t* prev,
    uint32_t prev_count,
    sao_memprobe_candidate_t* out_candidates,
    uint32_t cap,
    uint32_t* out_count) {
    if (process == nullptr || prev == nullptr || out_candidates == nullptr ||
        out_count == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_count = 0;
    const uint32_t dsz = dtype_size_impl(dtype);
    auto handle = process;
    uint32_t written = 0;
    uint8_t buf[8];
    for (uint32_t i = 0; i < prev_count; ++i) {
        size_t got = 0;
        if (sao_core_mem_read(handle, prev[i].addr, buf, dsz, &got) !=
                SAO_STATUS_OK ||
            got < dsz)
            continue;
        uint64_t current = 0;
        std::memcpy(&current, buf, dsz);
        bool keep = false;
        switch (predicate) {
        case SAO_MEMPROBE_PRED_CHANGED:   keep = (current != prev[i].last_value); break;
        case SAO_MEMPROBE_PRED_UNCHANGED: keep = (current == prev[i].last_value); break;
        case SAO_MEMPROBE_PRED_INCREASED: keep = (current > prev[i].last_value); break;
        case SAO_MEMPROBE_PRED_DECREASED: keep = (current < prev[i].last_value); break;
        }
        if (keep) {
            if (written >= cap) return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
            out_candidates[written].addr = prev[i].addr;
            out_candidates[written].last_value = current;
            ++written;
        }
    }
    *out_count = written;
    return SAO_STATUS_OK;
}
