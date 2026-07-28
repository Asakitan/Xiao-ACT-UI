// test_anchor_store.cpp — Phase 5 focused: anchor JSON round-trip.

#include <catch2/catch_test_macros.hpp>

#include "sao/mem_probe/anchor_store.h"

#include <cstring>

TEST_CASE("memprobe_anchor_save_load_roundtrip", "[mem_probe][anchor_store]") {
    sao_memprobe_ptr_chain_t src{};
    std::strncpy(src.module_name_utf8, "target.exe", sizeof(src.module_name_utf8));
    src.module_base_at_find = 0x140000000ULL;
    src.static_offset = 0x1234;
    src.deref_count = 2;
    src.deref_offsets[0] = 0x10;
    src.deref_offsets[1] = 0x20;
    src.final_offset = 0x8;

    sao_status_t st = sao_memprobe_anchor_save("test_plugin", "hp_anchor", &src);
    CHECK(st == SAO_STATUS_OK);

    sao_memprobe_ptr_chain_t dst{};
    st = sao_memprobe_anchor_load("test_plugin", "hp_anchor", &dst);
    CHECK(st == SAO_STATUS_OK);
    CHECK(std::string(dst.module_name_utf8) == "target.exe");
    CHECK(dst.static_offset == 0x1234);
    CHECK(dst.deref_count == 2);
    CHECK(dst.deref_offsets[0] == 0x10);
    CHECK(dst.deref_offsets[1] == 0x20);

    st = sao_memprobe_anchor_delete("test_plugin", "hp_anchor");
    CHECK(st == SAO_STATUS_OK);
    st = sao_memprobe_anchor_load("test_plugin", "hp_anchor", &dst);
    CHECK(st == SAO_STATUS_ERR_NOT_FOUND);
}
