# cython: language_level=3
# cython: boundscheck=False
# cython: wraparound=False
# cython: initializedcheck=False
# cython: nonecheck=False
# cython: cdivision=True
# distutils: language=c++
"""High-throughput memory scan kernels for tools.mem_probe.

AVX2-accelerated 8/4-byte aligned value scans, masked-pattern scans, and
multi-needle hash-set scans. Falls back to scalar SSE2 baseline at runtime
when the host CPU lacks AVX2.

Public functions accept any buffer supporting the buffer protocol (bytes,
bytearray, memoryview); they never call Win32 APIs directly. The caller is
responsible for region iteration and ReadProcessMemory.
"""

from libc.stdint cimport uint8_t, uint16_t, uint32_t, uint64_t
from libc.string cimport memcmp, memchr
from libcpp.vector cimport vector
from libcpp.unordered_set cimport unordered_set


# ───────────────────────── CPU feature detection ─────────────────────────

cdef extern from *:
    """
    #include <stdint.h>
    #if defined(_MSC_VER)
        #include <intrin.h>
        static int sao_cpu_has_avx2(void) {
            int regs[4] = {0, 0, 0, 0};
            __cpuidex(regs, 7, 0);
            return (regs[1] & (1 << 5)) != 0;
        }
        static int sao_cpu_has_sse42(void) {
            int regs[4] = {0, 0, 0, 0};
            __cpuid(regs, 1);
            return (regs[2] & (1 << 20)) != 0;
        }
    #else
        #include <cpuid.h>
        static int sao_cpu_has_avx2(void) {
            unsigned int a, b, c, d;
            if (!__get_cpuid_count(7, 0, &a, &b, &c, &d)) return 0;
            return (b & (1 << 5)) != 0;
        }
        static int sao_cpu_has_sse42(void) {
            unsigned int a, b, c, d;
            if (!__get_cpuid(1, &a, &b, &c, &d)) return 0;
            return (c & (1 << 20)) != 0;
        }
    #endif
    """
    int sao_cpu_has_avx2() nogil
    int sao_cpu_has_sse42() nogil


# ───────────────────────── AVX2 intrinsics ─────────────────────────

cdef extern from "immintrin.h":
    ctypedef struct __m256i:
        pass

    __m256i _mm256_set1_epi64x(long long a) nogil
    __m256i _mm256_set1_epi32(int a) nogil
    __m256i _mm256_loadu_si256(const __m256i *mem_addr) nogil
    __m256i _mm256_cmpeq_epi64(__m256i a, __m256i b) nogil
    __m256i _mm256_cmpeq_epi32(__m256i a, __m256i b) nogil
    int _mm256_movemask_epi8(__m256i a) nogil
    void _mm256_zeroupper() nogil


# ───────────────────────── Module-level state ─────────────────────────

cdef int _HAS_AVX2 = sao_cpu_has_avx2()
cdef int _HAS_SSE42 = sao_cpu_has_sse42()


def cpu_features():
    """Return a dict describing detected CPU SIMD features.

    Used by the Python facade (cy_memscan.py) to decide between Cython AVX2,
    Cython scalar, and pure-Python fallback paths.
    """
    return {
        "avx2": bool(_HAS_AVX2),
        "sse42": bool(_HAS_SSE42),
    }


def force_disable_avx2():
    """Test hook: force scalar path even on AVX2 CPUs (for benchmarking)."""
    global _HAS_AVX2
    _HAS_AVX2 = 0


def force_enable_avx2():
    """Test hook: re-enable AVX2 if previously disabled."""
    global _HAS_AVX2
    _HAS_AVX2 = sao_cpu_has_avx2()


# ───────────────────────── Internal helpers ─────────────────────────

cdef inline list _vec_to_list(vector[Py_ssize_t] &v):
    cdef list out = []
    cdef size_t k
    cdef size_t n = v.size()
    for k in range(n):
        out.append(<Py_ssize_t>v[k])
    return out


# ───────────────────────── u64 aligned scan ─────────────────────────

cdef void _scan_u64_avx2(
    const unsigned char* p, Py_ssize_t n,
    uint64_t needle, Py_ssize_t max_hits,
    vector[Py_ssize_t] &hits,
) nogil:
    """8-byte aligned scan for u64 == needle, AVX2 path.

    Compares 4 u64 lanes per 32-byte chunk; matches yield byte offsets
    relative to p. Tail (last <32 bytes) handled with scalar loop.
    """
    cdef Py_ssize_t i = 0
    cdef Py_ssize_t end_simd = n - 31
    cdef __m256i v_needle = _mm256_set1_epi64x(<long long>needle)
    cdef __m256i v_chunk
    cdef __m256i v_eq
    cdef int mask
    while i < end_simd:
        v_chunk = _mm256_loadu_si256(<const __m256i*>(p + i))
        v_eq = _mm256_cmpeq_epi64(v_chunk, v_needle)
        mask = _mm256_movemask_epi8(v_eq)
        if mask != 0:
            if mask & 0x000000FF:
                hits.push_back(i)
                if hits.size() >= <size_t>max_hits:
                    _mm256_zeroupper()
                    return
            if mask & 0x0000FF00:
                hits.push_back(i + 8)
                if hits.size() >= <size_t>max_hits:
                    _mm256_zeroupper()
                    return
            if mask & 0x00FF0000:
                hits.push_back(i + 16)
                if hits.size() >= <size_t>max_hits:
                    _mm256_zeroupper()
                    return
            if mask & <int>0xFF000000:
                hits.push_back(i + 24)
                if hits.size() >= <size_t>max_hits:
                    _mm256_zeroupper()
                    return
        i += 32
    _mm256_zeroupper()
    while i + 8 <= n:
        if (<const uint64_t*>(p + i))[0] == needle:
            hits.push_back(i)
            if hits.size() >= <size_t>max_hits:
                return
        i += 8


cdef void _scan_u64_scalar(
    const unsigned char* p, Py_ssize_t n,
    uint64_t needle, Py_ssize_t max_hits,
    vector[Py_ssize_t] &hits,
) nogil:
    cdef Py_ssize_t i = 0
    while i + 8 <= n:
        if (<const uint64_t*>(p + i))[0] == needle:
            hits.push_back(i)
            if hits.size() >= <size_t>max_hits:
                return
        i += 8


def find_aligned_u64(const unsigned char[::1] buf not None,
                     unsigned long long needle,
                     Py_ssize_t max_hits=1_000_000):
    """8-byte aligned scan for `*(uint64_t*)(buf+i) == needle`.

    Returns list of byte offsets within buf. AVX2 used when available.
    """
    cdef Py_ssize_t n = buf.shape[0]
    if n < 8:
        return []
    cdef vector[Py_ssize_t] hits
    if _HAS_AVX2:
        with nogil:
            _scan_u64_avx2(&buf[0], n, <uint64_t>needle, max_hits, hits)
    else:
        with nogil:
            _scan_u64_scalar(&buf[0], n, <uint64_t>needle, max_hits, hits)
    return _vec_to_list(hits)


# ───────────────────────── u32 aligned scan ─────────────────────────

cdef void _scan_u32_avx2(
    const unsigned char* p, Py_ssize_t n,
    uint32_t needle, Py_ssize_t max_hits,
    vector[Py_ssize_t] &hits,
) nogil:
    cdef Py_ssize_t i = 0
    cdef Py_ssize_t end_simd = n - 31
    cdef __m256i v_needle = _mm256_set1_epi32(<int>needle)
    cdef __m256i v_chunk
    cdef __m256i v_eq
    cdef int mask
    cdef int lane
    while i < end_simd:
        v_chunk = _mm256_loadu_si256(<const __m256i*>(p + i))
        v_eq = _mm256_cmpeq_epi32(v_chunk, v_needle)
        mask = _mm256_movemask_epi8(v_eq)
        if mask != 0:
            # 8 lanes of 4 bytes; lane k matched if (mask >> (k*4)) & 0xF == 0xF
            for lane in range(8):
                if (mask >> (lane * 4)) & 0xF:
                    hits.push_back(i + lane * 4)
                    if hits.size() >= <size_t>max_hits:
                        _mm256_zeroupper()
                        return
        i += 32
    _mm256_zeroupper()
    while i + 4 <= n:
        if (<const uint32_t*>(p + i))[0] == needle:
            hits.push_back(i)
            if hits.size() >= <size_t>max_hits:
                return
        i += 4


cdef void _scan_u32_scalar(
    const unsigned char* p, Py_ssize_t n,
    uint32_t needle, Py_ssize_t max_hits,
    vector[Py_ssize_t] &hits,
) nogil:
    cdef Py_ssize_t i = 0
    while i + 4 <= n:
        if (<const uint32_t*>(p + i))[0] == needle:
            hits.push_back(i)
            if hits.size() >= <size_t>max_hits:
                return
        i += 4


def find_aligned_u32(const unsigned char[::1] buf not None,
                     unsigned long needle,
                     Py_ssize_t max_hits=1_000_000):
    """4-byte aligned scan for `*(uint32_t*)(buf+i) == needle`.

    Used by HP / MaxHP / value scanning paths.
    """
    cdef Py_ssize_t n = buf.shape[0]
    if n < 4:
        return []
    cdef vector[Py_ssize_t] hits
    cdef uint32_t n32 = <uint32_t>(needle & 0xFFFFFFFF)
    if _HAS_AVX2:
        with nogil:
            _scan_u32_avx2(&buf[0], n, n32, max_hits, hits)
    else:
        with nogil:
            _scan_u32_scalar(&buf[0], n, n32, max_hits, hits)
    return _vec_to_list(hits)


# ───────────────────────── Multi-needle u64 (hash set) ─────────────────────────

def find_aligned_u64_in_set(const unsigned char[::1] buf not None,
                            needles,
                            Py_ssize_t max_hits=1_000_000):
    """8-byte aligned scan for u64 values that are in `needles` (iterable).

    Returns list of (offset, matched_needle). Used by pointer_chain
    multi-target scan and refine multi-HP-frame union.
    """
    cdef Py_ssize_t n = buf.shape[0]
    if n < 8:
        return []
    cdef unordered_set[uint64_t] needle_set
    cdef uint64_t v
    cdef uint64_t lo = 0xFFFFFFFFFFFFFFFFULL
    cdef uint64_t hi = 0
    for x in needles:
        v = <uint64_t>(<unsigned long long>x)
        needle_set.insert(v)
        if v < lo:
            lo = v
        if v > hi:
            hi = v
    if needle_set.empty():
        return []

    cdef list out = []
    cdef Py_ssize_t i = 0
    cdef const unsigned char* p = &buf[0]
    cdef uint64_t cur
    with nogil:
        while i + 8 <= n:
            cur = (<const uint64_t*>(p + i))[0]
            # range pre-filter: most u64s in heap memory are far outside
            # any plausible target range; bypass hash lookup for those.
            if cur >= lo and cur <= hi and needle_set.count(cur):
                with gil:
                    out.append((i, int(cur)))
                    if len(out) >= max_hits:
                        return
            i += 8
    return out


# ───────────────────────── Masked pattern scan ─────────────────────────

cdef void _scan_pattern_masked(
    const unsigned char* p, Py_ssize_t n,
    const unsigned char* pattern, const unsigned char* mask, Py_ssize_t plen,
    Py_ssize_t anchor_idx, unsigned char anchor_byte,
    Py_ssize_t max_hits,
    vector[Py_ssize_t] &hits,
) nogil:
    """Find `pattern` in `p` honoring `mask` (0xFF=must match, 0=ignore).

    Strategy: memchr to the anchor byte (a position in pattern where mask
    is 0xFF), then verify the full masked pattern at that candidate.
    Mirrors fingerprint._masked_search but in C.
    """
    if plen == 0 or n < plen:
        return
    cdef Py_ssize_t pos = 0
    cdef const unsigned char* hit_ptr
    cdef Py_ssize_t i, start, k
    cdef int ok
    while pos < n:
        hit_ptr = <const unsigned char*>memchr(p + pos, anchor_byte, n - pos)
        if hit_ptr == NULL:
            return
        i = hit_ptr - p
        start = i - anchor_idx
        if start < 0:
            pos = i + 1
            continue
        if start + plen > n:
            return
        ok = 1
        for k in range(plen):
            if mask[k] == 0:
                continue
            if p[start + k] != pattern[k]:
                ok = 0
                break
        if ok:
            hits.push_back(start)
            if hits.size() >= <size_t>max_hits:
                return
        pos = i + 1


def find_pattern_masked(const unsigned char[::1] buf not None,
                        bytes pattern,
                        bytes mask,
                        Py_ssize_t max_hits=1_000_000):
    """Scan `buf` for occurrences of `pattern` under `mask`.

    `mask[i] == 0xFF` ⇒ position i must match pattern[i] exactly.
    `mask[i] == 0`    ⇒ position i is wildcard.
    Returns list of starting offsets within buf.
    """
    cdef Py_ssize_t n = buf.shape[0]
    cdef Py_ssize_t plen = len(pattern)
    if plen == 0 or len(mask) != plen or n < plen:
        return []
    # Find first byte in pattern with mask=0xFF as the memchr anchor.
    cdef Py_ssize_t anchor_idx = -1
    cdef Py_ssize_t i
    for i in range(plen):
        if (<const unsigned char>mask[i]) == 0xFF:
            anchor_idx = i
            break
    if anchor_idx < 0:
        # Mask is all-wildcard ⇒ every position would match; refuse.
        return []
    cdef unsigned char anchor_byte = <unsigned char>pattern[anchor_idx]
    cdef vector[Py_ssize_t] hits
    cdef const unsigned char* pat_ptr = <const unsigned char*>pattern
    cdef const unsigned char* mask_ptr = <const unsigned char*>mask
    with nogil:
        _scan_pattern_masked(
            &buf[0], n, pat_ptr, mask_ptr, plen,
            anchor_idx, anchor_byte, max_hits, hits,
        )
    return _vec_to_list(hits)


# ───────────────────────── Batch verify (narrow) ─────────────────────────

def narrow_u32_batch(const unsigned char[::1] packed not None,
                     unsigned long expected):
    """Given `packed` = concatenation of N×4-byte values, return a bytes
    object of length N where byte i is 1 iff packed[i*4:i*4+4] == expected.

    Used by refine._verify_lockstep / scanner.narrow inner loop. The caller
    is responsible for issuing the per-address ReadProcessMemory and packing
    results into `packed`; this kernel only does the comparison.
    """
    cdef Py_ssize_t n_bytes = packed.shape[0]
    cdef Py_ssize_t n = n_bytes // 4
    if n == 0:
        return b""
    cdef bytes result = bytes(n)
    cdef unsigned char* out = <unsigned char*><const unsigned char*>result
    cdef const uint32_t* in_p = <const uint32_t*>&packed[0]
    cdef uint32_t exp32 = <uint32_t>(expected & 0xFFFFFFFF)
    cdef Py_ssize_t i
    with nogil:
        for i in range(n):
            out[i] = 1 if in_p[i] == exp32 else 0
    return result


def narrow_u64_batch(const unsigned char[::1] packed not None,
                     unsigned long long expected):
    """8-byte version of narrow_u32_batch."""
    cdef Py_ssize_t n_bytes = packed.shape[0]
    cdef Py_ssize_t n = n_bytes // 8
    if n == 0:
        return b""
    cdef bytes result = bytes(n)
    cdef unsigned char* out = <unsigned char*><const unsigned char*>result
    cdef const uint64_t* in_p = <const uint64_t*>&packed[0]
    cdef uint64_t exp64 = <uint64_t>expected
    cdef Py_ssize_t i
    with nogil:
        for i in range(n):
            out[i] = 1 if in_p[i] == exp64 else 0
    return result


# ───────────────────────── Fingerprint v2 anchor scan ─────────────────────────

def find_aligned_u64_with_anchor(const unsigned char[::1] buf not None,
                                 unsigned long long anchor_value,
                                 Py_ssize_t anchor_off,
                                 Py_ssize_t fp_size,
                                 list expected_slots,
                                 Py_ssize_t max_hits=1_000_000):
    """Two-stage fingerprint v2 scan.

    Stage 1: 8-byte aligned scan for u64 == anchor_value (the most distinctive
    slot in the fingerprint).
    Stage 2: for each candidate, verify ALL expected_slots [(off_in_fp, u64), ...]
    by reading buf[stage1_offset - anchor_off + slot_off].

    Returns list of fp_base offsets (= match position - anchor_off).
    """
    cdef Py_ssize_t n = buf.shape[0]
    if n < fp_size:
        return []

    # Stage 1: anchor scan (reuse u64 kernel)
    cdef vector[Py_ssize_t] s1
    if _HAS_AVX2:
        with nogil:
            _scan_u64_avx2(&buf[0], n, <uint64_t>anchor_value, max_hits, s1)
    else:
        with nogil:
            _scan_u64_scalar(&buf[0], n, <uint64_t>anchor_value, max_hits, s1)
    if s1.empty():
        return []

    # Stage 2: build C array of (slot_off, expected_u64) for fast verify
    cdef Py_ssize_t n_slots = len(expected_slots)
    if n_slots == 0:
        # Just return stage 1 hits as fp_base offsets
        return [<Py_ssize_t>s1[i] - anchor_off
                for i in range(s1.size())
                if (<Py_ssize_t>s1[i] - anchor_off) >= 0
                and (<Py_ssize_t>s1[i] - anchor_off) + fp_size <= n]

    cdef vector[Py_ssize_t] slot_offs
    cdef vector[uint64_t] slot_vals
    slot_offs.reserve(n_slots)
    slot_vals.reserve(n_slots)
    for tup in expected_slots:
        slot_offs.push_back(<Py_ssize_t>tup[0])
        slot_vals.push_back(<uint64_t>(<unsigned long long>tup[1]))

    cdef list out = []
    cdef Py_ssize_t k, fp_base, j
    cdef Py_ssize_t s1_size = s1.size()
    cdef int ok
    cdef const unsigned char* p = &buf[0]
    for k in range(s1_size):
        fp_base = <Py_ssize_t>s1[k] - anchor_off
        if fp_base < 0 or fp_base + fp_size > n:
            continue
        ok = 1
        for j in range(n_slots):
            if (<const uint64_t*>(p + fp_base + slot_offs[j]))[0] != slot_vals[j]:
                ok = 0
                break
        if ok:
            out.append(fp_base)
            if len(out) >= max_hits:
                break
    return out


# ───────────────────────── Phase 4: Batch field unpack ─────────────────────────

def unpack_struct_fields(const unsigned char[::1] buf not None,
                         list field_specs):
    """Fast multi-field unpack from a single struct buffer.

    `field_specs` is a list of (offset, width) tuples; width ∈ {1,2,4,8}.
    Returns list of unsigned int values (caller does signed conversion if needed).

    Used by SELF / entity field reading: caller does ONE RPM to grab the whole
    object body, then this kernel rapidly unpacks all fields without Python
    int.from_bytes overhead per field.
    """
    cdef Py_ssize_t n = buf.shape[0]
    cdef Py_ssize_t n_fields = len(field_specs)
    cdef list out = [0] * n_fields
    cdef const unsigned char* p = &buf[0]
    cdef Py_ssize_t i, off, width
    cdef uint64_t val
    for i in range(n_fields):
        spec = field_specs[i]
        off = <Py_ssize_t>spec[0]
        width = <Py_ssize_t>spec[1]
        if off < 0 or off + width > n:
            out[i] = 0
            continue
        if width == 8:
            val = (<const uint64_t*>(p + off))[0]
        elif width == 4:
            val = <uint64_t>((<const uint32_t*>(p + off))[0])
        elif width == 2:
            val = <uint64_t>((<const uint16_t*>(p + off))[0])
        elif width == 1:
            val = <uint64_t>p[off]
        else:
            val = 0
        out[i] = int(val)
    return out


def unpack_array_fields(const unsigned char[::1] buf not None,
                        Py_ssize_t stride,
                        list field_specs,
                        Py_ssize_t n_elements):
    """Fast multi-entity field unpack from a packed buffer.

    Layout assumption: buf contains N consecutive structs, each `stride` bytes.
    For each struct, unpack `field_specs` = [(offset, width), ...].

    Returns a flat list: [entity0_field0, entity0_field1, ..., entity1_field0, ...].

    Used by entity_watcher fast-path: caller bulk-reads N entities into a
    contiguous buffer, then this kernel unpacks all (entity × field) values
    in one Cython call.
    """
    cdef Py_ssize_t n = buf.shape[0]
    cdef Py_ssize_t n_fields = len(field_specs)
    cdef list out = [0] * (n_elements * n_fields)
    cdef const unsigned char* p = &buf[0]
    cdef Py_ssize_t e, f, base, off, width
    cdef uint64_t val
    # Pre-extract field specs into C arrays for the hot inner loop
    cdef vector[Py_ssize_t] offsets
    cdef vector[Py_ssize_t] widths
    offsets.reserve(n_fields)
    widths.reserve(n_fields)
    for f in range(n_fields):
        spec = field_specs[f]
        offsets.push_back(<Py_ssize_t>spec[0])
        widths.push_back(<Py_ssize_t>spec[1])
    for e in range(n_elements):
        base = e * stride
        if base + stride > n:
            break
        for f in range(n_fields):
            off = base + offsets[f]
            width = widths[f]
            if off + width > n:
                val = 0
            elif width == 8:
                val = (<const uint64_t*>(p + off))[0]
            elif width == 4:
                val = <uint64_t>((<const uint32_t*>(p + off))[0])
            elif width == 2:
                val = <uint64_t>((<const uint16_t*>(p + off))[0])
            elif width == 1:
                val = <uint64_t>p[off]
            else:
                val = 0
            out[e * n_fields + f] = int(val)
    return out
