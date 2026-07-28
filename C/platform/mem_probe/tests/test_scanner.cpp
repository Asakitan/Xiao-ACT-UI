// test_scanner.cpp — Phase 4 focused: scanner dtype size + AVX2 detection.
// 真实跨进程扫描 gated (需要 target helper), 用 dtype+AVX2 API level check.

#include <catch2/catch_test_macros.hpp>

#include "sao/mem_probe/scanner.h"

TEST_CASE("memprobe_scanner_dtype_sizes_match_python", "[mem_probe][scanner]") {
    CHECK(sao_memprobe_dtype_size(SAO_MEMPROBE_DTYPE_U8) == 1);
    CHECK(sao_memprobe_dtype_size(SAO_MEMPROBE_DTYPE_U16) == 2);
    CHECK(sao_memprobe_dtype_size(SAO_MEMPROBE_DTYPE_U32) == 4);
    CHECK(sao_memprobe_dtype_size(SAO_MEMPROBE_DTYPE_U64) == 8);
    CHECK(sao_memprobe_dtype_size(SAO_MEMPROBE_DTYPE_I8) == 1);
    CHECK(sao_memprobe_dtype_size(SAO_MEMPROBE_DTYPE_I16) == 2);
    CHECK(sao_memprobe_dtype_size(SAO_MEMPROBE_DTYPE_I32) == 4);
    CHECK(sao_memprobe_dtype_size(SAO_MEMPROBE_DTYPE_I64) == 8);
    CHECK(sao_memprobe_dtype_size(SAO_MEMPROBE_DTYPE_F32) == 4);
    CHECK(sao_memprobe_dtype_size(SAO_MEMPROBE_DTYPE_F64) == 8);
}

TEST_CASE("memprobe_scanner_avx2_probe_reports_bool", "[mem_probe][scanner]") {
    const int avx = sao_memprobe_has_avx2();
    CHECK((avx == 0 || avx == 1));
}

TEST_CASE("memprobe_scanner_null_process_rejected", "[mem_probe][scanner]") {
    sao_memprobe_candidate_t out[4] = {};
    uint32_t count = 999;
    sao_status_t st = sao_memprobe_scan_first(nullptr, SAO_MEMPROBE_DTYPE_U32,
                                              0x1234u, nullptr, out, 4, &count);
    CHECK(st == SAO_STATUS_ERR_INVALID_ARGUMENT);
}
