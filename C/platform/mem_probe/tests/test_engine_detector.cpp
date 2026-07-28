// test_engine_detector.cpp — Phase 6 focused: engine detector null gate.

#include <catch2/catch_test_macros.hpp>

#include "sao/mem_probe/engine/adapter.h"

TEST_CASE("memprobe_engine_detect_null_returns_unknown",
          "[mem_probe][engine]") {
    CHECK(sao_memprobe_engine_detect(nullptr) == SAO_MEMPROBE_ENGINE_UNKNOWN);
}

TEST_CASE("memprobe_engine_open_null_returns_null", "[mem_probe][engine]") {
    CHECK(sao_memprobe_engine_open(nullptr, SAO_MEMPROBE_ENGINE_NATIVE) == nullptr);
}
