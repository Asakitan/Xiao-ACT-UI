// test_pointer_chain.cpp — Phase 5 focused: pointer chain resolve on null.

#include <catch2/catch_test_macros.hpp>

#include "sao/mem_probe/pointer_chain.h"

TEST_CASE("memprobe_ptr_chain_null_process_rejected", "[mem_probe][pointer_chain]") {
    sao_memprobe_ptr_chain_t out{};
    sao_status_t st = sao_memprobe_ptr_chain_backtrace(nullptr, 0xDEAD, nullptr, &out);
    CHECK(st == SAO_STATUS_ERR_INVALID_ARGUMENT);
}

TEST_CASE("memprobe_ptr_chain_resolve_null_process", "[mem_probe][pointer_chain]") {
    sao_memprobe_ptr_chain_t chain{};
    uint64_t addr = 0;
    sao_status_t st = sao_memprobe_ptr_chain_resolve(nullptr, &chain, &addr);
    CHECK(st == SAO_STATUS_ERR_INVALID_ARGUMENT);
}
